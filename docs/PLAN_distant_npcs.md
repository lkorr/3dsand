# PLAN: distant NPCs — creatures that keep living past the residency window

Status: **PLANNED 2026-10-13, not started.** Owner request: "increase the range
at which NPCs can exist — we're limited by the paging window, which is very
frustrating depending on the size of the fight scene." Written for worktree
agents to execute package by package (§3). D0–D4 deliver the feature in single
player; D5–D9 are follow-ups, some gated on owner decisions (§5).

Line numbers below are as of `e7ab8ab`. They drift; **grep the symbol**, the
line is a hint.

Companion idea, NOT this plan: bias the window centre toward the fight.
`Stream::Update` centres on `interest.Primary()` exactly (`stream.cpp:325`); a
centre clamped between the player and the combat centroid (player kept ≥6
chunks inside) buys up to ~41 m of full fidelity on one side for about a day of
work. It stacks with this plan and touches none of its files.

## 0. Why (read from the code)

- **The window is 51.2 m on every axis, centred on the player**: `kWorldN` 512
  × 0.1 m (`world.h:27`), centred on the primary interest point with a 2-chunk
  hysteresis (`stream.cpp:19, 325-357`). ±25.6 m, vertical included.
- **A creature that leaves it stops existing as a creature.** `MobSystem::PreTick`
  (`mob.cpp:9579-9636`): origin more than `kPad = 16` voxels outside the window
  → `SaveOne` into `parkFn_` → `ChunkStore::DormantEntities` (region = 16
  chunks, `persist.cpp:1323-1335`, `chunkstore.h:39, 319`) → `ReleaseRig`.
  **Parked creatures do not tick** (`persist.h` "OUT OF SCOPE", `DESIGN.md:682`).
  They come back through `EntityParking::Unpark` (`persist.cpp:1382-1552`) when
  the window holds them again: 1 chunk inside (`kUnparkMarginChunks`), 2 per
  tick (`kUnparkPerCall`), after the fetch cache has their ground, and only if
  the 64-living cap has room.
- **Parking loses identity and mind.** The record has no id; `LoadOne` gets a
  fresh one from `Spawn` (`persist.cpp:1055`, `mob.cpp:4603`), so every target
  id naming it goes stale and every `Hash3(mobId ^ salt, …)` stream changes.
  `ai::Brain` is never saved except the target tail (`ai_behavior.h:1186`,
  `mob.h:7429`).
- **Outside the window there is no ground for a full creature.** Terrain
  collision is built from the fetch cache, and `RequestChunkFetch` refuses
  non-resident chunks (`world.cpp:1741`); `UpdateFall` does nothing on unknown
  ground (`mob.cpp:8444`). A creature past the edge free-falls (memory
  `gotcha-burning-npc-runs-off-the-window`). So "just widen `kPad`" is not an
  option.
- **The AI already does not need the world.** `ai::Think` (`ai_behavior.h:1441`)
  reads everything through `WorldView` / `SelfView` / `NavProbe` and says so:
  "no dependency on the rig, the physics or the animation" (`ai_behavior.h:977`).
  `lineOfSight` and `aimPart` are optional pointers (`PerceivesLine` skips a
  null one, `ai_behavior.cpp:~987`). Its cost is tiny: intent 0.3 ms for 64
  creatures (`DESIGN.md:13289`), ~5 µs each. The ~0.2 ms/creature of the
  64-brawl is ALL voxel and body work: stroke, stain, burn, shocks, Jolt
  (`PLAN_fight64_perf.md:720-726`).
- **The fighting does need the world.** Melee is geometry: `MeleeSweepDamage`
  casts against limb colliders and scales by tip speed and edge alignment
  (`melee.cpp:1485-1502, 1786`); `Mob::Damage` carves the lattice
  (`mob.cpp:13492-13710`); armour is a cover ray onto worn shells
  (`melee.cpp:2186`) then `ShellResponseOf` (`shellresponse.h:180`, pure);
  parries are blade-on-blade (`FindParry`). Health is per limb and blood is
  health (`DESIGN.md:9327`; `TotalHp` `mob.cpp:11711`; `DrainBlood`
  `mob.cpp:12001`; vital limbs `mob.cpp:13669`). None of that can run without a
  rig. A distant creature needs an **abstract** hit model.
- **Sight ranges are short.** Melee profiles see 22–64 voxels (2.2–6.4 m), the
  archer 160 (16 m), the shark 90 (`assets/mobs/behaviors.json`,
  `perception.sightRange`, world voxels per `ai_behavior.h:201`). The player
  is always at the window centre, ≥256 voxels from any distant creature, so in
  single player **a distant creature can never perceive the player**. Distant
  creatures interact with each other and, across a band ~16 m wide at the
  edge, with near creatures. That is the whole interaction surface.
- **Ground exists on the CPU as a pure function.** `World::TerrainHeight(x, z,
  seed)` (`world.cpp:3191-3200`) is the exact twin of `genColumn().h`: map v2
  planes, relief noise, roads, site pads, pond bowls, rivers. It **excludes**
  trees, building/ruin geometry, `.svedit` layers and player edits
  (`world.cpp:3092-3095`, `world.h:5492-5521`). It costs ~25 hash3 + site
  scans (`world.cpp:3105-3110`). Water: `TerrainColumn().water`,
  `worldmap::WaterColumnAt`, `CurrentWorldMap().seaLevelY`. Modified chunks
  evicted from the window are RLE in `ChunkStore` (`stream.cpp:539-585`;
  `Get` + `RleDecodeChunk`), with no column-top helper yet.
- **Bodies are not clipped to the window when drawn.** Limbs are raster draws
  (`microbody.wgsl` OBB + brick march, `debris.wgsl` cubes). Neither clips to
  the window; fog matches terrain; outside the window a body is fully lit
  because the sun-shadow ray is window-clipped (`common.wgsl:7029`). And there
  is a ready draw-only path: **`RenderOnlyBody`** (`game/renderonly.h`,
  `BodyRegistry::SetRenderOnly`), one transform slot plus a SHARED micro
  model, no physics, walked LAST so it starves first. Today it draws arrows in
  flight. No mob LOD or impostor exists.
- **Arrows cannot cross the edge.** `ArrowFlights` expires a flight once its
  cell leaves the window (`arrows.cpp:307-313`), and nothing past the pad has
  a collider.
- **Precedent for this shape of work**: chunk tickets
  (`PLAN_chunk_tickets.md`) did the same for MATTER: a bounded, explicit,
  deterministic second tier outside the window. Its §1 non-goals name mobs as
  out of scope. This plan is that missing half.

## 1. Goals and non-goals

Goals
1. A creature that leaves the window keeps living out to a **distant radius**
   (`distant.radius`, proposed 1,280 voxels = 128 m) around the player. It
   walks, follows its behaviour profile, pursues and fights other creatures,
   bleeds and dies.
2. **Identity survives the tier change**: same mob id, same `ai::Brain`, same
   record. A fight that crosses the window edge does not restart.
3. A fight can **straddle the edge**: a near swordsman and a distant one can
   fight, and a distant archer can shoot into the window.
4. Distant creatures are **drawn** (cheaply) where the camera can see them.
5. Rules 1–3 hold. Deterministic (§2.12), every distant creature sleeps when
   idle (§2.11), and the only world mutations are near creatures' ordinary ops.
6. With `distant.enabled = false` the engine behaves exactly as today (park at
   the edge). That switch is the parity oracle for every package.

Non-goals (v1)
- **No matter interaction at distance.** No blood on the ground, no fire
  spreading, no stains, no carving, no footprints, no spells' voxel effects. A
  distant creature emits NO ops. (Rule 3 is untouched by construction.)
- **No physics.** No Jolt bodies, no ragdolls, no collision with buildings or
  trees (the ground is `TerrainHeight` plus edits, §2.4).
- **Not a bigger window and not a ticket.** Nothing here makes chunks resident.
- **Not the save format.** Distant creatures are saved as ordinary parked
  records (§2.10).
- **No multiplayer in v1** beyond "host only, clients see the old behaviour"
  (§2.13, D7).
- **No change to near-creature fidelity** unless the owner takes D8.

## 2. Design

### 2.1 Three tiers and the borders between them

| Tier | Lives in | Simulated by | Costs |
|---|---|---|---|
| NEAR | `MobSystem::mobs_` | everything (today's Mob) | rig, Jolt, bricks, ~0.2 ms/tick |
| DISTANT | `DistantMobs` (new, `src/game/distant.{h,cpp}`) | §2.5–2.8: Think + kinematic walk + abstract combat | ~5–10 µs/tick awake, ~0 asleep |
| PARKED | `ChunkStore::DormantEntities` | nothing | bytes |

Transitions (all decided in the authority tick, §2.12):

- **NEAR → DISTANT (demote).** The current park condition (origin > `kPad`
  outside the window, `mob.cpp:9579`), for a LIVING, non-ghost creature, when
  `distant.enabled`, within `distant.radius` and `DistantMobs` has room. Move
  the id, `ai::Brain` (`Mob::ai_`, `mob.h:5692`), velocity and the `SaveOne`
  record into a `DistantMob`, then `ReleaseRig` exactly as parking does. A
  corpse still parks as today (nothing to simulate). With no room or outside
  the radius, park as today.
- **DISTANT → NEAR (promote).** When the creature is inside the window by
  `kUnparkMarginChunks` with the SAME three waits `Unpark` has (budget, cap,
  ground known in the fetch cache), call `LoadOne(record, …, keepId)`, then
  apply the distant overlay (§2.2) and move the brain back. While it waits it
  stays DISTANT, so it keeps walking and drawing and **a wait is no longer a
  freeze**. That also fixes the "army pours in, 2 per tick" stall for free.
- **DISTANT → PARKED (retire).** Beyond `distant.radius + distant.hysteresis`
  (proposed +256 voxels): write the patched record (§2.10) into
  `DormantEntities`. The brain is dropped (today's rule) and the id is
  dropped.
- **PARKED → DISTANT (activate), D5.** Dormant LIVING records within
  `distant.radius` become distant creatures, with a per-tick budget (no rig, so
  16/tick is cheap). This is what makes a village or a war band 80 m away alive
  before you arrive. Until D5, only creatures that LEFT the window are distant.

Promotion and activation run **inside `TickAuthority`** (phase H, before
`mobs.PreTick`), not beside `EntityParking::Unpark` in `main.cpp:16488`.
`Unpark` sits outside the tick today, which is why `npc-catchup` has to call it
by hand (`selftest_npc.cpp:421-425`). A tier change outside the tick is exactly
the "harness stops where the game keeps going" trap CLAUDE.md warns about.

### 2.2 What a distant creature is

```cpp
struct DistantMob {               // src/game/distant.h (sketch, not final)
  uint64_t id;                    // the NEAR id, kept (2.3)
  int defIndex;
  std::string refId;              // Mob::RefId, for refs lookups (2.14)
  std::vector<uint8_t> record;    // SaveOne at demotion: everything NOT simulated here
                                  //   (lattices, gear, pack, wounds, burns)
  Vec3 origin; float heading;     // world voxels, Mob::origin_'s frame (min corner)
  Vec3 vel;
  ai::Brain brain;                // moved in, moved back
  ai::SelfView selfCache;         // size, speed, turnRate, faction, budgets: from the def at demotion
  // ---- health: the abstract body (2.6) ----
  ZoneHp zones[kZones];           // head, torso, armL, armR, legL, legR -> the def's authored limbs
  float bloodLossPerTick;         // from cuts taken while distant; decays
  uint16_t burnTicksLeft;         // carried in from a burning demotion
  // ---- combat ----
  DistantKit kit;                 // cached at demotion: strikes (cut/blunt/bite), reach,
                                  //   styles, shield, per-zone shell factor
  uint32_t strokeImpactTick;      // an abstract swing in progress
  uint32_t strokeStyle;
  // ---- life + sleep ----
  bool dead; uint8_t cause; uint32_t deathTick;
  bool asleep; uint32_t wakeTick;
};
```

The **record is the bridge for everything this tier does not simulate.** The
distant tier owns only an OVERLAY on it: origin, heading, zone hp, dead +
cause. Two functions carry the overlay, and they are the only two:

- `Mob::ApplyDistantOverlay(const DistantMob&)`, after `LoadOne` on promotion.
  It writes the zone hp back onto the authored limbs with `SpendHp`'s
  proportional spread inside each zone, and kills through the ordinary `Die()`
  if the overlay is dead.
- `PatchMobRecord(bytes, overlay)` for retire and save (§2.10). Built on the
  existing reader `MobRecord` (`mob.cpp:30773`).

**Round-trip is the D0 contract:** promote(demote(m)) reproduces every field
the record holds, bit for bit (`mob-save-delta`'s digest), plus the same id and
the same brain.

### 2.3 Identity: ids survive

`LoadOne` gains an optional `keepId`, and `Spawn` takes it instead of
`nextId_++`. That is safe because the distant tier HOLDS the id while the
creature is distant, nobody else can be issued it, and `nextId_` is saved
(`MOBG`, `persist.cpp:1028`). A save/load still drops ids (records carry none),
which is no change from today.

What keeps working because the id is kept:
- every `Hash3(mobId ^ salt, tick, i)` stream (`ai_behavior.h:74-76`);
- `Brain::targetId` on both sides of the edge;
- `CombatantRef`, melee events, archery/cast requests (`meleehit.h:82-93`,
  `mob_archery.h:80-106`);
- dialogue `Speaker{mobId, refId}` (`dialogue.h:268`).

### 2.4 The ground

`DistantGround` (in `distant.cpp`, one instance, CPU only) answers `GroundAt(x,
z) -> {y, water, kind}` from sources that are **pure functions of (seed, map,
ChunkStore contents)**. Never from a GPU readback: the far surface map would be
better ground (trees, buildings), but its arrival tick is scheduling-dependent
and that breaks rule 1.

1. `World::TerrainHeight(x, z, seed)`, the base.
2. Water: `TerrainColumn().water`, `worldmap::WaterColumnAt`, sea level. A
   walker in water above wading depth moves at a swim fraction. An aquatic
   profile is confined to water.
3. **Edited ground**: if the column's chunk is in `ChunkStore` (modified, then
   evicted), use the column top from the decoded chunk. Cache column tops per
   chunk (an LRU keyed by world chunk, invalidated by `ChunkStore::Put`). This
   is what makes a crater you dug and walked away from real to a distant
   creature.
4. `.svedit` layers (`WorldEdit::ResolvedCells`, `worldedit.h:125`) the same
   way, D1b, if the owner's maps lean on them for walls and terrain.

Known gap, accepted for v1: trees, buildings and ruins are not obstacles. A
distant creature can walk through a house 60 m away. D1 measures how often
this shows (a `--shot` frame of a distant patrol through Harrowby). If it is
bad, the cheap fix is a blocked-column mask from `sitePadAt`'s site footprints,
not a GPU readback.

Promotion snaps to the REAL ground: `Mob::GroundHeightAt` over the fetch cache,
searching up out of solid if `TerrainHeight` disagreed (a distant creature
"inside" a building pops onto its roof or out of its door rather than into a
wall). The landing error is bounded by a baseline threshold, like
`mobParkMaxDropVox`, and fall damage is suppressed on the promotion tick.

### 2.5 Movement and the AI

- **Think, as is**, with a `WorldView` whose `actors` is the distant actor list
  (§2.7), `lineOfSight = nullptr` (open-field sight: hills do not block at this
  tier, D1 records that as a known difference), `aimPart = nullptr`, and a
  `NavProbe` backed by `DistantGround`. A* (`ai_nav.h:147`) works over any
  probe, so distant creatures route around steep ground and water the same way
  near ones do. Cap distant A* at the near `navRadius`.
- **Steer, as is** (`mob.cpp:8367`, pure on mob state + `LocomotionDef`).
- **Drive**: `origin += heading-frame(driveScale, driveStrafe) × def.speed × dt`,
  then `y = ground`. Refuse a step whose rise exceeds the body's own climb
  budget (`SelfView` terrain budgets), so the planner and the drive refuse the
  same wall (`ai_behavior.h` explains why that matters).
- **Crowd spacing**: 2D separation over the spatial hash (§2.11), the same
  `spacingMul` the near crowd uses.
- **Schedules** (D5): a ref-carrying villager walks toward `ScheduledPlace`
  (`refs_npc.cpp:1140`) instead of being teleported there by the catch-up
  placer.

### 2.6 Abstract combat

Built from data the engine already has, calibrated against the real thing:

- **When a swing starts.** Think emits `attack` + `AttackRequest`.
  `PickAttackStyle(who = nullptr)` (`strokes.cpp:1397`) picks the style, and
  the impact is scheduled at the style's own impact timing from
  `attack_styles.json`.
- **At the impact tick**, if the target is still within `reach + slack`:
  - `hit` = roll < `calib.hitRate[style]` × (1 − the defender's block chance:
    shield / guard, from `calib`);
  - `zone` = sample `calib.zoneWeights[style]`;
  - `hp` = `strike[kind]` (the weapon item's `damage`, or the sidecar's natural
    weapon) × `calib.meanPower[style]` × `ShellResponseOf(the defender's worn
    shell on that zone, kind)`. `ShellResponseOf` is pure, `shellresponse.h:180`;
    the per-zone shell is cached in `kit` at demotion;
  - a cut adds `bloodLossPerTick += hp × calib.bleedPerHp`, which decays.
- **Death** uses the near rules on the abstract body: a vital zone at 0, blood
  total exhausted, or the burn cap (`Mob::BurnHealthCapFor`, `mob.cpp:12038`,
  pure).
- **RNG**: `rng::Hash3(id ^ kSaltDistant*, tick, i)`. The new salts go next to
  `ai_behavior.cpp:25-30`'s.
- **Ranged**: `Think`'s `shoot` → an abstract flight: impact after
  `distance / arrowSpeed` ticks, `hitRate` falling with distance and the
  target's speed (from `calib`), damage = the ammo's flat damage (35 today,
  `arrows.h`) × shell factor. Drawn as a `RenderOnlyBody` arrow on its arc
  (D4); `ArrowFlights` already draws exactly this.
- **Casting**: v1 drops `cast` at this tier (owner decision §5.3). A caster
  profile holds at its cast range.
- **Statuses carried across the edge**: a creature demoted while BURNING
  (common: the flee rule runs it off the window, memory
  `gotcha-burning-npc-runs-off-the-window`) keeps burning abstractly for
  `burnTicksLeft`, draining at `calib.burnHpPerTick` under the burn cap. If it
  is promoted still burning, re-light one limb (`IgniteLimb`) so the fire
  continues visibly. Bleeding carries the same way. Infection is re-derived on
  load already (`mob.h:5266`).

**Calibration is data, produced by a harness, not hand-tuned.**
`assets/mobs/distant_combat.json` holds the tables above per style (and per
weapon class where the data supports it). They are produced by
`SANDVOX_DISTANT_CALIB=<out.json> --selftest --gate mob-cap64`: the
full-fidelity brawl logs every strike's (style, weapon, zone struck, power,
hp, blocked/parried) and the harness reduces them to the tables. The gate
`distant-calibrate` (§4) then compares time-to-kill between a full-fidelity
squad fight and the same fight run distant, within a band stored in
`tests/baseline.json`. Re-running the harness after a combat change is how the
tier keeps up with melee tuning. Without it the two tiers drift apart silently.

### 2.7 The edge: near ↔ distant

The rule: **a pair resolves at the lower fidelity of its two members, and
damage lands through the receiver's own door.**

- **Who sees whom.** `MobSystem::PreTick`'s actor list (`mob.cpp:9343-9398`)
  gets the distant creatures within the edge band appended: max near sight
  range (160) + margin outside the window, in id order. Distant creatures get
  the near creatures within the same band in their list. Both read the other
  side's state AS OF THE START OF THE TICK (a snapshot each side publishes), so
  phase order cannot make the outcome order-dependent.
- **Near hits distant.** The near stroke plays (the player may be watching).
  At the stroke's impact tick, a near attacker whose target is distant resolves
  through §2.6's roll instead of the Jolt sweep (a hook where `StepStroke`
  would call `MeleeSweepDamage`). Damage goes to the distant zones.
- **Distant hits near.** The roll as above. On a hit, damage enters the near
  creature through its ORDINARY door (`Mob::Damage` / `CutLimb` / `BluntHit`
  through `FindCreature`, per `DESIGN.md` "One creature list") with a contact
  point synthesized on the struck limb's surface facing the attacker. The near
  creature gets a real wound, real blood and its real shell response. D3 picks
  the exact entry point.
- **Distant archer shoots near.** Inject a REAL `ArrowFlight` at the window
  boundary with the ballistic state the abstract flight would have there. The
  arrow then flies in and hits (or misses) physically, through the code that
  already exists. Deterministic: it is computed from state.
- **Near archer shoots distant.** The flight is drawn until it leaves the
  window, and its outcome is the abstract roll at the computed flight time
  (today it would expire at the edge, `arrows.cpp:307`).
- **Thrash.** Demote at 16 voxels outside and promote at 1 chunk inside: a
  32-voxel band, the same as parking today. A near creature chasing a distant
  one out of the window simply becomes distant, which is consistent. The gate
  bounds tier flips per creature per 600 ticks.

### 2.8 Death

A distant creature that dies becomes a dead `DistantMob`: asleep forever,
costing nothing, holding nothing. Its `rises_` booking is withdrawn exactly as
parking withdraws it (`mob.cpp:9620-9628`).
- **On promotion**: `LoadOne` → overlay → `Die(cause)` through the normal path,
  then the corpse settles. The observable contract: within N ticks of promotion
  it lies on the real ground with its gear and pack, and no fall-damage, scream
  or death-cue spam. D2 chooses between "Die then let the ragdoll settle for a
  few unrendered substeps" and "spawn lying via the `spawnDead_` path".
- **On retire**: the patched record carries the dead flag + cause, and it parks
  as a corpse does today.
- The dead caps (`kMaxDeadMobs 12`, `kMaxDeadBodies 240`, `EvictDead`) apply on
  promotion only. Distant corpses hold no cap.

### 2.9 Rendering

A distant creature takes one `RenderOnlyBody` drawing a **per-def baked pose
model**. Each def gets a few whole-body micro models baked at load from its
rest lattice and clips: idle, walk A, walk B, strike, lying. The distant
creature swaps between them by state and gait phase (a flipbook statue), and
all creatures of one def share them. Gear: the def's default loadout only.
Frustum-culled on the CPU (`main.cpp:20963`'s cube cull pattern). Budget: one
body slot each.

- **Slots are the hard limit.** `kMaxBodySlots` 2048 is static_asserted
  ≥ 200 + 64×24 + 240 = 1,976 (`world.h:1003-1006`), so only ~72 slots are
  spare when the near crowd is full. Render-only bodies are walked LAST and
  starve first, so v1 draws the nearest distant creatures that fit (graceful,
  never steals a near body's slot). D4b raises `kMaxBodySlots` toward the 4096
  ceiling (`BodyVoxInst` packs the slot in 12 bits). That is a `world.h` + GPU
  buffer change and needs a claim.
- **Articulated distant creatures** (real limbs posed by FK, no Jolt) look
  better and pop less at promotion, but each needs ~24 slots plus resident
  per-instance bricks. That is owner decision §5.2, not v1.
- Render-only. Never hashed, never saved. Bodies cast no shadow
  (`microbody.wgsl:27-35`), and an out-of-window body is fully lit, so a
  distant creature in a valley's shade will look too bright. Known; D4 notes
  it.

### 2.10 Persistence

**No new save format.** At save, each distant creature is written into
`DormantEntities` as its record patched with the overlay (`PatchMobRecord`), so
a save is "everyone distant parks". On load, D5's activation brings the ones
within the radius back as distant. The brain and id are lost across save/load,
exactly as today. If the overlay grows fields the record cannot express (blood
loss rate, burn ticks), append a tail and bump `kSaveVersion` 11 → 12, keeping
11 loadable (`mob.h:7397-7472`, `PLAN_save_system.md:65-67`).

### 2.11 Rule 2: cost scales with activity

- **Spatial hash**: 2D cells of 64 voxels over distant creatures, rebuilt
  incrementally on cell change. It serves perception, spacing and the edge
  band.
- **Sleep**: a distant creature with no hostile within `sightRange ×
  keepRangeScale` + margin, no stroke in flight and no movement goal is asleep.
  It is not ticked. It wakes when a creature enters its 3×3 hash neighbourhood
  or at `wakeTick` (its wander pause or schedule row ends).
- **Stride**: an awake but unengaged creature (wandering, walking a schedule)
  ticks every `distant.idleStride` ticks, on phase `(tick + id) % stride`, and
  moves `stride × dt`. An engaged creature ticks every tick (combat cadence is
  in ticks).
- **Cap**: `kMaxDistantMobs` (constexpr in `distant.h`, proposed 512). It needs
  no GPU sizing; render slots are the separate budget in §2.9. When full,
  demotion falls back to parking (counted in `build/last_run.json`, like
  `opstream` refusals: a refusal is a number).
- **Targets, to be measured in D2**: 512 asleep ≈ 0; 128 engaged ≤ 1.5 ms per
  tick (CPU, one thread). If it is over, the work pool (`src/game/workpool.*`,
  merge in id order, `PLAN_fight64_perf.md:46-56`) is the lever, as it was for
  the near crowd.

### 2.12 Determinism

Mob state is not in `determinismHash`. That hash is the window's grid and the
gate never ticks mobs (`selftest_sim.cpp:339-369`). Mob determinism is held by
twice-run DIGEST gates (`mob-cap64-twice`, `npc-schedule`), and this tier does
the same (§4). The rules:

- Iterate distant creatures in **id order** (a sorted vector, never
  `unordered_map` iteration). Hash-cell queries return id-sorted results.
- All randomness is `Hash3(id ^ salt, tick, i)`. No wall clock, no frame
  count.
- Tier changes are decided in the authority tick from tick-start state. A
  promotion wait (ground fetched) uses the fetch cache's fixed T+K latency, as
  `Unpark` does today.
- Ground is a pure function (§2.4). No GPU readback feeds a distant decision.
- Distant creatures emit no ops, so `ops-replay` needs nothing. Near creatures
  hurt by distant ones emit ordinary ops through the ordinary doors.
- Floats: same binary, same order, same result. Far from the origin
  (x ≈ 6,000) one ulp is 0.0005 voxels (memory
  `gotcha-float-position-ulp-far-from-origin`), so gates compare twice-run
  digests bit-exact and never assert small position deltas.

### 2.13 Multiplayer (D7, not v1)

Authority is per chunk: the nearest peer whose window holds it
(`authority.h:19-35`). Parking is host/single-player only (`persist.h:338-349`).
v1 rule: distant creatures exist only where parking is enabled (the host). In
MP, "distant" must mean outside EVERY peer's window, so a distant creature
entering a client's window has to be promoted and handed off. That needs the
host to `LoadOne` without local ground (a handoff from the record, which is
the `persist.h` KNOWN GAP), plus a cheap distant pose stream (id, pos, heading,
state, a dozen bytes) so clients can draw distant creatures. D7 only after D0–D4
are solid.

### 2.14 Lookups that assume "not in `mobs_`" means "gone"

D0 audits every one and classifies it, the way tickets P0 audited addressing
(`docs/tickets_p0_audit.md`):
- **(a) must see distant creatures**: liveness and identity;
- **(b) must not**: matter, physics, bodies;
- **(c) indifferent.**

Start from `FindMobById`, `CreatureWithId`, `FindCreature`, `MobBrain(Mut)`,
and every `refId` lookup. Known (a)s:
- **Dungeon spawner slots**: a slot whose mob is missing for 600 ticks counts
  as dead (`refs_dungeon.cpp:39-42, 850-863`). A distant creature must not
  look dead, or the spawner makes a duplicate.
- **Village residents** re-find their mob by ref each tick
  (`refs_npc.cpp:580-597`).
- **Duplicate-ref refusal** in `LoadOne` (`mob.cpp:31244-31249`) must see
  distant refs.
- Demon bindings and contracts (`demon*.cpp`, `ContractTick`).
- Dialogue speakers.

## 3. Packages

Each package is one worktree agent. C++ packages build ONCE with the shared
cache, and each ends with ONE `--verify` naming its gates plus the existing
gates it could break (CLAUDE.md "Agent packages").

**D0: the seam and the audit (medium).** Create `src/game/distant.{h,cpp}`
(`DistantMobs`, `DistantMob`) and a `distant` tuning group (a new `Tuning`
member, `NO_WGSL` rows only, so no shader prelude change): `enabled` (default
FALSE until D3), `radius`, `hysteresis`, `idleStride`. The demote branch in the
park rule. Promote inside `TickAuthority` with `Unpark`'s three waits.
`LoadOne(keepId)`. `ApplyDistantOverlay` and `PatchMobRecord`. Distant
creatures are FROZEN in D0 (parity with parking, but identity kept). The
§2.14 audit written into this doc's §6, with the (a) sites fixed. Gate
`distant-roundtrip`. Must stay green: `mob-park` (runs with the switch off;
add an arm with it on), `mob-save-delta`, `npc-catchup`, `refs-npc-identity`,
`mob-handoff`.

**D1: ground, walking, sleeping (medium).** `DistantGround` (base + water +
ChunkStore edits; `.svedit` as D1b if needed), the `NavProbe` over it, `Think`
with the distant `WorldView` (wander, flee and approach work; no attacks yet),
`Steer` + drive + spacing, the spatial hash, sleep and stride. Promotion
snap-to-real-ground. Gates `distant-walk`, `distant-sleep`. One `--shot` frame
of a distant patrol for the owner (no assertion on look).

**D2: abstract combat, calibration, death (large).** `DistantKit` at
demotion, zones, the hit roll, ranged, statuses (bleed, burn), death and dead
promotion. The calibration harness (`SANDVOX_DISTANT_CALIB`) and the first
`assets/mobs/distant_combat.json`. Gates `distant-fight`, `distant-calibrate`.
Measure the §2.11 cost targets and record them in `last_run.json`.

**D3: the edge (medium).** The shared actor band, near-hits-distant (the
`StepStroke` hook), distant-hits-near (synthesized contact through the
ordinary door), arrows both ways (§2.7). Flip `distant.enabled` default to
TRUE here, rebaseline whatever moves, and update `DESIGN.md` (a "Distant
creatures" section; the `DESIGN.md:682` "Not done" line). Gate `distant-edge`.

**D4: drawing them (large).** Per-def baked pose models, the flipbook state
machine, `RenderOnlyBody` producer, frustum cull, slot budget accounting.
Distant arrows drawn on their arc. D4b (claim `world.h`): raise
`kMaxBodySlots` if the owner wants more than ~72 drawn with a full near crowd.
Gate `distant-look` (pixel probe: a distant creature at 40 m is drawn; slots
never exceed the budget). `--shot-frames` for the owner's look check.

**D5: activation and schedules (medium).** PARKED → DISTANT within the radius,
with a budget. Villagers walk their schedules while distant (`ScheduledPlace`
as a goal, not a teleport). `npc-catchup` still passes (catch-up remains the
path for a record that was never distant). Gate `distant-schedule`.

**D6: save (small).** Patched records at save, activation at load, the version
bump only if a tail is needed. Gate `distant-save`.

**D7: multiplayer (large, later).** §2.13.

**D8: cap overflow (owner decision §5.4).** When the 64-living cap is full,
the farthest near creature from the player (beyond a minimum distance, e.g. 15
m) demotes to distant even inside the window, and the nearest distant one
promotes into the freed slot. This makes fights larger than 64 possible, at
the cost of reduced fidelity on the far side of the fight. In-window distant
creatures would read ground from the fetch cache where they have it.

**D9: the player's reach (owner decision §5.5).** The player's arrows continue
past the edge as abstract flights over `DistantGround` and hit distant
creatures. Spells stay window-bound.

## 4. Gates

All runnable with `--gate <name>` alone, ticking through `support::RunTicks` /
`TickCursor` (`tickrig.h`). The rig pins the window, so creatures are moved
OUT of it by teleport (`selftest_npc.cpp:444-452`'s idiom), never by moving
the window. Thresholds go in `tests/baseline.json` (`BaselineNumber` /
`RecordObserved`), never in C++. Each is registered in its domain list and in
`kOrder` (`selftest.cpp:149-1409`); a gate missing from `kOrder` never runs.

| Gate | Package | Asserts |
|---|---|---|
| `distant-roundtrip` | D0 | demote → N ticks → promote: same id, same brain target, record digest equal (`mob-save-delta`'s), limb hp equal; `parked` did not increment inside the radius; with `enabled=false` the old park path runs |
| `distant-walk` | D1 | a distant creature walks ≥1,000 voxels over varied ground (the `peaks` map, as `altitude` does) and stays on `DistantGround`; refuses an over-budget climb; water handled; promotion lands within `distantPromoteMaxDropVox` of real ground with no fall damage; twice-run trace digest equal |
| `distant-sleep` | D1 | 200 idle distant creatures: awake count 0 within T ticks; µs/tick under `distantIdleMaxUs`; a creature walking past wakes exactly its neighbourhood |
| `distant-fight` | D2 | two squads fight distant: casualties > 0, twice-run digest equal (per-creature zone hp + origin bits + deaths, `mob-cap64-twice`-style), awake count → 0 after the fight, dead promoted as corpses on the ground |
| `distant-calibrate` | D2 | the same 4v4 at full fidelity and distant: mean time-to-kill ratio inside `distantTtkRatioMin..Max`, winner side agrees in ≥ `distantWinnerAgree` of K seeded runs |
| `distant-edge` | D3 | near vs distant melee pair across the edge: both take damage, the near wound is real (lattice changed, blood op emitted); a distant archer's arrow enters the window as an `ArrowFlight` and hits; a creature pacing the edge flips tier ≤ `distantMaxFlips` per 600 ticks |
| `distant-look` | D4 | a distant creature at 40 m covers > N pixels in a shot; render-only slots never exceed the free budget; the near crowd never loses a slot |
| `distant-schedule` | D5 | a parked villager inside the radius activates, walks its schedule while distant, promotes at its scheduled place |
| `distant-save` | D6 | save with K distant creatures → load → the same K exist (activated), zone hp preserved |

## 5. Risks and open owner decisions

**Decisions (answer before the package that needs them):**
1. **Radius and cap** (D0): 128 m and 512 proposed. The radius is bounded by
   CPU cost only (the ground works anywhere on the map). Beyond ~200 m, ask
   whether anyone will see them.
2. **Look** (D4): are flipbook statues acceptable at 25–100 m, or is
   articulated required? Articulated costs ~24 slots each (so `kMaxBodySlots`
   → 4096 and about 80 drawn max) plus resident bricks and FK posing.
3. **Casters** (D2): v1 distant casters hold and do nothing. The alternative
   is an abstract spell hit using a per-spell damage estimate (needs authoring
   per spell).
4. **Cap overflow** (D8): should fights larger than 64 demote the far side of
   a fight inside the window?
5. **The player's arrows** (D9): should they reach distant creatures?
6. **Calibration band** (D2): how close must distant combat match the real
   thing? ±25% time-to-kill proposed.

**Risks:**
- **Calibration rot.** Melee tuning changes, nobody re-runs the harness, and
  the tiers drift apart. Mitigation: `distant-calibrate` is a real gate, not
  advisory, and the harness is one command.
- **Promotion pop.** A statue → rig swap at 25 m, and a creature `TerrainHeight`
  placed inside a building. Mitigation: snap-to-real-ground with an up-search;
  the look check in D4.
- **Sight-range assumption.** "Distant creatures never perceive the player"
  holds only while every profile's `sightRange × keepRangeScale` < 256 voxels.
  `LoadBehaviors` asserts it, so a long-sighted profile fails loudly instead
  of silently never seeing the player. (Today's maximum is 160 × 1.25 = 200.)
- **The shark.** The aquatic confinement rule needs water at distance:
  `WaterColumnAt` covers rivers and lakes, `TerrainColumn().water` covers ponds
  and the sea. Check that the two agree at shorelines before trusting D1.
- **Ref and spawner duplicates** (§2.14) are the most likely correctness bug.
  The D0 audit exists for that.
- **Hash moves**: none expected through D2 (no ops, and the switch is off).
  D3's flip may move gate digests that involve creatures crossing the edge. That
  is a notification, not a regression (CLAUDE.md rule 1): rebaseline once.

## 6. Deviations from this plan (filled in by the implementing packages)

(empty)
