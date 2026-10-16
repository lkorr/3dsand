# PLAN: distant NPCs — creatures that keep living past the residency window

Status: **D0-D3 DONE on `distant-npcs` (2026-10-15): the tier is ON by
default. D5+D6 DONE on `dnpc-d56` (2026-10-16): activation, schedules at
distance, MOBS v13. D8, D9 next (§6 D3 and D5+D6 have what each must know).**
PLANNED 2026-10-13. AUDITED AND AMENDED 2026-10-15 (§7 lists the
twelve corrections; they are written into the sections below, so the body is
the truth and §7 is the changelog). Owner decisions §5 are RESOLVED. Execution
order and waves: §3. Owner request: "increase the range at which NPCs can
exist — we're limited by the paging window, which is very frustrating
depending on the size of the fight scene." Written for worktree agents to
execute package by package (§3). D0–D4 deliver the feature in single player;
D5, D6, D8, D9 are approved follow-ups; D7 (multiplayer) stays later.

Line numbers below are as of `e7ab8ab` (audit spot-checks at `79f8f21`). They
drift; **grep the symbol**, the line is a hint.

Companion idea, NOT this plan: bias the window centre toward the fight.
`Stream::Update` centres on `interest.Primary()` exactly (`stream.cpp:316`); a
centre clamped between the player and the combat centroid (player kept ≥6
chunks inside) buys up to ~41 m of full fidelity on one side for about a day of
work. **It does NOT simply stack with this plan** (audit A5): with the player
6 chunks from a face, a distant creature can be ~112 voxels away, inside the
archer's (200) and shark's (126) keep ranges. It is compatible only because
§2.7 keeps the player out of every distant actor list by construction; do not
remove that rule if the bias ever lands.

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
  is NOT always at the window centre: chunks are 16 voxels, the window shifts
  only once the player is `kHysteresis` = 2 chunks off centre, and one axis
  shifts per call (`stream.cpp:359-379`), so the player can sit 2–3 chunks off
  centre. A creature demoted at `kPad` past the face is therefore ≥ ~224
  voxels away in the worst case (`kWorldN/2 − 3·kChunk + kPad`), not 256. The
  longest keep range today is the archer's 160 × 1.25 = 200, so geometry alone
  keeps distant creatures blind to the player, but with a 24-voxel margin.
  **The design does not rely on that margin**: the player is never an entry
  in a distant creature's actor list (§2.7), so a distant creature cannot
  perceive the player by construction. Distant creatures interact with each
  other and, across a band ~16 m wide at the edge, with near creatures. That
  is the whole interaction surface.
- **Ground exists on the CPU as a pure function.** `World::TerrainHeight(x, z,
  seed)` (`world.cpp:3191-3200`) is the exact twin of `genColumn().h`: map v2
  planes, relief noise, roads, site pads, pond bowls, rivers. It **excludes**
  trees, building/ruin geometry, `.svedit` layers and player edits
  (`world.cpp:3092-3095`, `world.h:5492-5521`). It costs ~25 hash3 + site
  scans (`world.cpp:3105-3110`). Water: `TerrainColumn().water`,
  `worldmap::WaterColumnAt`, `CurrentWorldMap().seaLevelY`. Modified chunks
  evicted from the window are RLE in `ChunkStore` (`stream.cpp:539-585`;
  `Get` + `RleDecodeChunk`), with no column-top helper yet. **ChunkStore
  contents are NOT a function of the tick** (audit A3): `Stream::Update`
  harvests eviction readbacks non-blocking (`stream.cpp:321`,
  `pending_.front().map.Ready()`), so when a chunk's bytes reach the store
  depends on the GPU. A deterministic reader must first drain that chunk's
  pending eviction the way `RegenerateChunks` does (`stream.cpp:2347`,
  `while (pendingChunks_.count(key)) CompleteOldest(false)`). The fetch cache
  (`World::Cached`) is a different store and IS deterministic: it publishes at
  exactly `T − kSnapshotLatency` (4), blocking if it must (`world.h:5148`).
- **Ground that is not the surface.** Dungeons are carved UNDER the surface by
  worldgen (`sim/dungeon.h`; `Placed::boxMin/boxMax` are their world boxes,
  `dungeon::TwinCell` is the evaluator's exact CPU twin). Caves, upper floors,
  rooftops and bridges are not `TerrainHeight` either. The park rule is 3D,
  so climbing out of a dungeon parks everything left inside it. A distant
  tier that put every creature on `TerrainHeight` would teleport those
  creatures to the hillside above (audit A1). §2.4 classifies them instead.
- **Bodies are not clipped to the window when drawn.** Limbs are raster draws
  (`microbody.wgsl` OBB + brick march, `debris.wgsl` cubes). Neither clips to
  the window; fog matches terrain; outside the window a body is fully lit
  because the sun-shadow ray is window-clipped (`common.wgsl:7029`). And there
  is a ready draw-only path: **`RenderOnlyBody`** (`game/renderonly.h`,
  `BodyRegistry::SetRenderOnly`), one transform slot plus a SHARED micro
  model, no physics, walked LAST so it starves first. Today it draws arrows in
  flight. No mob LOD or impostor exists. **Models cost pool words, not just
  slots** (audit A7): one random-pool human packs ~66k words of limb bricks
  (`microbody.h`, the free-range note), and the shared micro-body pool is
  8 MiW (`kMicroBodyPoolWordsWorld`), which the `mob-cap64` brawl already
  fills with gore clones. `MicroBodyPack` adds a shared model at runtime and
  `MicroBodyReleaseShared` frees one.
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
- **No walking off the surface.** A creature that leaves the window
  underground, indoors or up on a structure is ANCHORED (§2.4): it thinks,
  turns and fights what reaches it, but does not move until promoted.
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
  outside the window, `mob.cpp:9789`), for a non-ghost creature, when
  `distant.enabled`, within `distant.radius` and `DistantMobs` has room. Move
  the id, `ai::Brain` (`Mob::ai_`), velocity and the `SaveOne` record into a
  `DistantMob`, classify its ground mode (§2.4: SURFACE or ANCHORED), then
  `ReleaseRig` exactly as parking does. **A corpse demotes too** (audit A11),
  as a dead `DistantMob` that is asleep forever: it costs nothing to keep, it
  is drawn lying (§2.9, after the living), and a battlefield you back away
  from stays littered instead of emptying at the edge. With no room or
  outside the radius, park as today.
- **DISTANT → NEAR (promote).** When the creature is inside the window by
  `kUnparkMarginChunks` (the PROMOTION LINE) with the SAME three waits
  `Unpark` has (budget, cap, ground known in the fetch cache), call
  `LoadOne(record, …, keepId)`, then apply the distant overlay (§2.2) and move
  the brain back. Candidates are served nearest-to-the-player first, ties by
  id. **While it waits it stays DISTANT and is HELD AT THE PROMOTION LINE**
  (audit A4): its drive refuses any step deeper into the window. It keeps
  thinking, turning, fighting what reaches it and drawing, so a wait is no
  longer a freeze, but no statue ever walks through the real voxel world.
  This matters most exactly when the 64-living cap is full, which is the big
  fight the owner asked for: the overflow queues at the edge instead of
  pouring into the window as a crowd of colliderless statues walking through
  walls on `TerrainHeight`.
- **DISTANT → PARKED (retire).** Beyond `distant.radius + distant.hysteresis`
  (proposed +256 voxels): write the patched record (§2.10) into
  `DormantEntities`. The brain is dropped (today's rule) and the id is
  dropped.
- **PARKED → DISTANT (activate), D5.** Dormant LIVING records within
  `distant.radius` become distant creatures, with a per-tick budget (no rig, so
  16/tick is cheap). This is what makes a village or a war band 80 m away alive
  before you arrive. Until D5, only creatures that LEFT the window are distant.

Promotion and activation run **inside `TickAuthority`** (phase H, before
`mobs.PreTick`). **`EntityParking::Unpark` moves there too, in D0** (audit
A12): it sits outside the tick today (`main.cpp:16654`), which is why
`npc-catchup`, `refs-*`, `mob-park` and `worldio` gates call it by hand
(`selftest_npc.cpp:424`, `selftest_refs.cpp:484`, `selftest_mob.cpp:12705`,
`selftest_worldio.cpp:2355`). A tier change outside the tick is exactly the
"harness stops where the game keeps going" trap CLAUDE.md warns about, and
promotion and unparking compete for the same cap and the same budget, so they
must be decided in one place in one order. Keep the gates' by-hand calls
working (they may become no-ops or explicit budget overrides) rather than
rewriting those gates.

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
  SteerState steer;               // heading, desiredHeading, turnVel, speedNow (2.5)
  uint8_t groundMode;             // SURFACE (walks on DistantGround) or ANCHORED (2.4)
  // ---- health: the abstract body (2.6) ----
  std::vector<float> limbHp;      // PER LIMB, parallel to the record's limbs, exactly
                                  //   Mob::limbs_[i].hp at demotion (bloodless kept, unspent)
  std::vector<uint8_t> limbRole;  // cached at demotion: vital? bloodless? which strike
                                  //   class targets it (head / torso / arm / leg / tail / fin ...)
  float bloodLossPerTick;         // from cuts taken while distant; decays
  uint16_t burnTicksLeft;         // carried in from a burning demotion
  // ---- combat ----
  DistantKit kit;                 // cached at demotion: strikes (cut/blunt/bite), reach,
                                  //   styles, shield, per-limb shell factor
  uint32_t strokeImpactTick;      // an abstract swing in progress
  uint32_t strokeStyle;
  // ---- life + sleep ----
  bool dead; uint8_t cause; uint32_t deathTick;
  bool asleep; uint32_t wakeTick;
};
```

**Health is per limb, not per zone** (audit A8). The near tier already keeps
one float `hp` per limb (`MobLimb::hp`; `TotalHp`, `SpendableHp`, `SpendHp` in
`mob.cpp:11931-12001`), so the distant tier keeps the same numbers. A six-zone
humanoid abstraction (head/torso/arms/legs) would have no mapping for the
snake, shark, piranha or the naga's tail, and the round trip would stop being
exact. Calibration (§2.6) samples a limb by its ROLE, cached at demotion.

The **record is the bridge for everything this tier does not simulate.** The
distant tier owns only an OVERLAY on it: origin, heading, per-limb hp, dead +
cause (+ the burn/bleed tail if §2.10 needs one). Two functions carry the
overlay, and they are the only two:

- `Mob::ApplyDistantOverlay(const DistantMob&)`, after `LoadOne` on promotion.
  It writes each limb's hp back DIRECTLY (no `SpendHp` spread, no carve: a
  carve would spend the hp a second time), applies the hp-zero rules the near
  tier applies (`HpZeroSevers`), and kills through the ordinary `Die()` if the
  overlay is dead. Wounds taken at distance are not carved (a known look gap;
  D2 may add a bounded cosmetic replay that carves without charging hp).
- `PatchMobRecord(bytes, overlay)` for retire and save (§2.10). Built on the
  existing reader `MobRecord` (grep `MobRecord`; it was `mob.cpp:30773`).

**Round-trip is the D0 contract:** promote(demote(m)) reproduces every field
the record holds, bit for bit (`mob-save-delta`'s digest), plus the same id and
the same brain, and save(demote(m)) → load gives the same creature as
save(m) → load (§2.10).

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
the tick)**. Never from a GPU readback that is not latency-fixed: the far
surface map would be better ground (trees, buildings), but its arrival tick is
scheduling-dependent and that breaks rule 1.

1. `World::TerrainHeight(x, z, seed)`, the base.
2. Water: `TerrainColumn().water`, `worldmap::WaterColumnAt`, sea level. A
   walker in water above wading depth moves at a swim fraction. An aquatic
   profile is confined to water.
3. **Edited ground**: if the column's chunk is in `ChunkStore` (modified, then
   evicted), use the walkable surface from the decoded chunk. **Drain that
   chunk's pending eviction first** (a `Stream` accessor that runs the
   `RegenerateChunks` loop, `while (pendingChunks_.count(key))
   CompleteOldest(false)`, then `Store().Get`): without it the bytes arrive
   on a GPU-timed tick and two runs see different ground (audit A3). The drain
   blocks at most once per evicted chunk, and only when a distant creature
   steps onto it. "Walkable surface" is the highest solid cell with headroom
   within the creature's step budget of its CURRENT y, not the column top: a
   stored chunk holds canopy too. Cache per chunk (an LRU keyed by world chunk,
   invalidated by `ChunkStore::Put`/`Erase`). This is what makes a crater you
   dug and walked away from real to a distant creature. If D1 runs long, this
   source may slip to D1b; v1 is then `TerrainHeight` + water, which is still
   deterministic.
4. `.svedit` layers (`WorldEdit::ResolvedCells`, `worldedit.h:125`) the same
   way, D1b, if the owner's maps lean on them for walls and terrain.

**Ground mode, decided once at demotion** (audit A1). A creature is SURFACE if
its feet are within `distant.surfaceSnapMaxVox` (proposed 24 = 2.4 m) of
`GroundAt` there AND its origin is outside every placed dungeon's world box
(`dungeon::Placed::boxMin/boxMax`, via the current world map). Otherwise it is
ANCHORED: Think still runs (it may turn, choose targets and strike what is in
reach), but its drive is zero and its y never changes, so a dungeon's
skeletons stay in the dungeon and a guard on a wall stays on the wall. An
anchored creature promotes where it stood. D1b may give dungeon interiors real
distant ground through `dungeon::TwinCell` (a floor search inside the box);
until then, anchored is the honest answer.

**A creature demoted in mid-air** (thrown, falling off the window's bottom
face) that classifies SURFACE lands on `GroundAt` with fall damage computed
from the drop and its vertical speed by the same thresholds
`Mob::ApplyFallDamage` uses (`fallDamageSpeed`, `fallDamageScale`,
`fallSplatSpeed`), spread over limbs as `SpendHp` spreads it, rather than a
free landing.

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
- **Steer, extracted, not "as is"** (audit A10). `MobSystem::Steer(Mob&,
  const MobDef&, float dt)` (`mob.cpp:8538`) reads and writes `Mob` fields
  (`heading_`, `desiredHeading_`, `turnVel_`, `speedNow_`, `turnCapL_/R_`,
  `ShieldTurnRate`, `skel_.loco`). D0 extracts the arithmetic into a pure
  free function over a small `SteerState` + `LocomotionDef` + the two turn
  caps, and `MobSystem::Steer` becomes a wrapper that copies in and out.
  **Guard: the `mob-cap64-twice` digest is bit-identical before and after the
  extraction** (record it from the unmodified tree first). The distant tier
  calls the same free function, so the two tiers cannot drift in how they
  turn.
- **Drive**: `origin += heading-frame(driveScale, driveStrafe) × def.speed × dt`,
  then `y = ground`. Refuse a step whose rise exceeds the body's own climb
  budget (`SelfView` terrain budgets), so the planner and the drive refuse the
  same wall (`ai_behavior.h` explains why that matters). Refuse any step that
  crosses the PROMOTION LINE inward while the creature waits (§2.1). An
  ANCHORED creature does not drive at all (§2.4).
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
  - `limb` = sample `calib.roleWeights[style]` over the defender's limb ROLES
    (head / torso / arm / leg / tail / fin / ...), then a live limb of that
    role (id-hashed pick); a role the defender lacks folds into torso;
  - `hp` = `strike[kind]` (the weapon item's `damage`, or the sidecar's natural
    weapon) × `calib.meanPower[style]` × `ShellResponseOf(the defender's worn
    shell on that limb, kind)`. `ShellResponseOf` is pure, `shellresponse.h:180`;
    the per-limb shell is cached in `kit` at demotion;
  - a cut adds `bloodLossPerTick += hp × calib.bleedPerHp`, which decays.
- **Death** uses the near rules on the abstract body: a vital limb at 0, blood
  total exhausted, or the burn cap (grep `Mob::BurnHealthCapFor`, pure).
- **RNG**: `rng::Hash3(id ^ kSaltDistant*, tick, i)`. The new salts go next to
  `ai_behavior.cpp:25-30`'s.
- **Ranged**: `Think`'s `shoot` → an abstract flight: impact after
  `distance / arrowSpeed` ticks, `hitRate` falling with distance and the
  target's speed (from `calib`), damage = the ammo's flat damage (35 today,
  `arrows.h`) × shell factor. Drawn as a `RenderOnlyBody` arrow on its arc
  (D4); `ArrowFlights` already draws exactly this.
- **Casting**: v1 drops `cast` at this tier (owner decision §5.3, resolved:
  hold). A caster profile holds at its cast range and does nothing; battlemages
  and fiends are sitting ducks at distance until an abstract spell hit exists.
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
`SANDVOX_DISTANT_CALIB=<out.json> --selftest --gate distant-calib-matrix`: a
MATRIX of full-fidelity duels and small skirmishes (weapon class × armour
class × creature kind, a few seeds each) that logs **every stroke started**,
not only the ones that landed: (style, weapon, outcome = miss / blocked /
parried / hit, limb role struck, power, hp, ticks from stroke start to
impact). Misses are what make `hitRate` computable at all (audit A9). A
mixed 64-brawl (`mob-cap64`) is the wrong source: crowding, flanking and
multiple attackers per target are baked into every number it yields. The
harness reduces the log to the tables.

The gate `distant-calibrate` (§4) then runs **asymmetric** matchups (armoured
vs unarmoured, 4v3, sword vs zombie, archer vs melee) both at full fidelity
and distant, and compares the full-fidelity WIN RATE and mean time-to-kill with
the distant ones, within bands stored in `tests/baseline.json`. A symmetric
4v4's winner is a coin flip at full fidelity, so "winner agrees" on equal
squads measures noise (audit A9). Re-running the harness after a combat change
is how the tier keeps up with melee tuning. Without it the two tiers drift
apart silently.

### 2.7 The edge: near ↔ distant

The rule: **a pair resolves at the lower fidelity of its two members, and
damage lands through the receiver's own door.**

- **Who sees whom.** `MobSystem::PreTick`'s actor list (`mob.cpp:9343-9398`)
  gets the distant creatures within the edge band appended: max near sight
  range (160) + margin outside the window, in id order. Distant creatures get
  the near CREATURES within the same band in their list. **Never a player**
  (audit A5): no avatar, local or remote, is ever an entry in a distant
  creature's actor list in v1, so "a distant creature cannot perceive the
  player" holds by construction, whatever the geometry (window hysteresis,
  the promotion-line wait, a future window bias). Both sides read the other
  side's state AS OF THE START OF THE TICK (a snapshot each side publishes),
  so phase order cannot make the outcome order-dependent.
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
  one out of the window simply becomes distant, which is consistent. A
  distant creature that cannot be promoted yet is held at the promotion line
  (§2.1), so it can still be struck across the band by near creatures (the
  near-hits-distant hook) and strike back. The gate bounds tier flips per
  creature per 600 ticks.

### 2.8 Death

A distant creature that dies becomes a dead `DistantMob`: asleep forever,
costing nothing, holding nothing. A near corpse that crosses the edge becomes
one too (§2.1). Its `rises_` booking is withdrawn exactly as parking withdraws
it (`mob.cpp:9830-9837`), and the record books it again on promotion.
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

A distant creature takes one `RenderOnlyBody` drawing a **baked pose model**:
a few whole-body micro models (idle, walk A, walk B, strike, lying) made by
posing the creature's limbs with its clips and merging them into ONE lattice.
The distant creature swaps between them by state and gait phase (a flipbook
statue). Frustum-culled on the CPU (the cube cull pattern in `main.cpp`'s
render path). Budget: one body slot each.

- **Pool words are the first limit, not slots** (audit A7). A full-resolution
  human is ~66k pool words; five poses for each of a dozen kinds would be
  ~4 MiW of the 8 MiW pool the near fight's gore clones need. So:
  - **downsample** the merged lattice to a coarse cell (target ~5 cm, i.e.
    20 cells/m; about one pixel per cell at 50 m on a 1080p, 60° view;
    measure and record the words per pose in `last_run.json`);
  - **bake lazily**, at first demotion of a given LOOK KEY, not at load:
    the key is (def, the worn/held item set, dye words), so a dressed villager
    and a naked one of the same def get different statues, and pool humans,
    which are each a runtime def, bake their own;
  - bake from the LIVE limbs at demotion (they are still there before
    `ReleaseRig`), so blood, char and lost limbs carry onto the statue;
  - hold baked poses in an LRU under a word budget (`kDistantPoseWordsMax`,
    proposed 1 MiW) via `MicroBodyPack` / `MicroBodyReleaseShared`; a
    creature whose key cannot be baked inside the budget draws as the def's
    pristine key if one exists, else not at all (counted, never fatal).
- **Slots are the second limit.** `kMaxBodySlots` 2048 is static_asserted
  ≥ 200 + 64×24 + 240 = 1,976 (`world.h:1003-1006`), so only ~72 slots are
  spare when the near crowd is full. Render-only bodies are walked LAST and
  starve first, so v1 draws the nearest distant creatures that fit (graceful,
  never steals a near body's slot), the living before corpses. **D4 raises
  `kMaxBodySlots` to the 4096 ceiling** (`BodyVoxInst` packs the slot in 12
  bits; +96 KiB VRAM): 72 drawn is too few for the fight this plan exists for.
  `kMaxMicroBodyModels ≥ kMaxBodySlots + 1024` (`world.h:2568`) then needs
  5120, or a restated assert, since a render-only body takes no owned clone;
  D4 decides and documents which. That is a `world.h` + GPU buffer change and
  needs a claim.
- **Articulated distant creatures** (real limbs posed by FK, no Jolt) look
  better and pop less at promotion, but each needs ~24 slots plus resident
  per-instance bricks. Owner decision §5.2, resolved: flipbook for v1.
- Render-only. Never hashed, never saved. Bodies cast no shadow
  (`microbody.wgsl:27-35`), and an out-of-window body is fully lit, so a
  distant creature in a valley's shade will look too bright. Known; D4 notes
  it.

### 2.10 Persistence

**No new save format.** At save, each distant creature is written as its
record patched with the overlay (`PatchMobRecord`), so a save is "everyone
distant parks". **This lands in D0, not D6** (audit A2): the `'MOBS'`
section's `saveRecords` (`persist.cpp`, `mobsSec.saveRecords`) iterates
`mobs.MobAt(i)` only, so the day the tier holds anyone, a save without this
silently drops every distant creature. D0 appends one patched record per
`DistantMob` there (living and dead), bucketed by origin like the rest, and
the `distant-roundtrip` gate asserts a save taken while creatures are distant
loads them back as parked records. On load, D5's activation brings the ones
within the radius back as distant. The brain and id are lost across
save/load, exactly as today. If the overlay grows fields the record cannot
express (blood loss rate, burn ticks), append a tail and bump `kSaveVersion`
11 → 12 (`mob.h:7764`), keeping 11 loadable (`PLAN_save_system.md:65-67`).

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
- Ground is a pure function (§2.4). No GPU readback feeds a distant decision
  unless its latency is fixed: the fetch cache is (K = 4); `ChunkStore` is NOT
  until the chunk's pending eviction is drained (§0, §2.4 item 3).
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
gates it could break (CLAUDE.md "Agent packages"; the owner's rule is "only
test the feature you implemented": no `--suite acceptance`, no bare
`--selftest`). World-hash moves are expected and pinned once, at the end of
the whole feature, not per package.

All packages integrate on the branch **`distant-npcs`** (worktree
`.claude/worktrees/distant-npcs`), which lands on main once at the end.
**Execution order (audit A6): D0 → D1 → D4 → D2 → D3, drawing BEFORE the
enable flip**, so there is never a build where invisible creatures fight at
the edge and archers shoot arrows into the window from nowhere. The parts of
D4 and D2 that do not need the seam start in parallel with D0:

| Wave | Packages (parallel within a wave) | Needs |
|---|---|---|
| 1 | **D0** seam · **D4a** pose bake + draw producer · **D2a** calibration harness + pure resolver | main |
| 2 | **D1** ground, walk, sleep (+ wire D4a's producer to the tier) | D0, D4a |
| 3 | **D2b** combat in the distant tick | D1, D2a |
| 4 | **D3** the edge + the enable flip | D2b |
| 5 | **D5+D6** activation, schedules, load · **D9** the player's arrows | D3 |
| 6 | **D8** cap overflow | D3 |

**D0: the seam and the audit (medium).** Create `src/game/distant.{h,cpp}`
(`DistantMobs`, `DistantMob`) and a `distant` tuning group (a new `Tuning`
member, `NO_WGSL` rows only, so no shader prelude change; batch EVERY row the
feature will need now: `enabled` (default FALSE until D3), `radius` (1280),
`hysteresis` (256), `idleStride`, `surfaceSnapMaxVox` (24),
`promoteMaxDropVox`). The demote branch in the park rule, for the living AND
the dead (§2.1). The SURFACE / ANCHORED classification (§2.4; the dungeon-box
test included). Promote inside `TickAuthority` with `Unpark`'s three waits,
nearest-first, AND move `Unpark` itself into the tick (§2.1). `LoadOne(keepId)`.
Per-limb hp overlay: `ApplyDistantOverlay` and `PatchMobRecord` (§2.2).
**Save: distant creatures written as patched records** in `'MOBS'`
`saveRecords` (§2.10). **Extract `Steer`** into a pure function, guarded by a
bit-identical `mob-cap64-twice` digest (§2.5). Distant creatures are FROZEN in
D0 (parity with parking, but identity kept). The §2.14 audit written into this
doc's §6, with the (a) sites fixed. Gate `distant-roundtrip`. Must stay green:
`mob-park` (runs with the switch off; add an arm with it on), `mob-save-delta`,
`npc-catchup`, `refs-npc-identity`, `mob-handoff`, `mob-cap64-twice`.

**D4a: pose bake and the draw producer (large; parallel with D0).** Pure of
the tier: input is a plain list of `DistantDrawItem{lookKey, pose, gait
phase, origin, heading}`, so it needs nothing from D0. The look-key bake from
LIVE limbs (posed by the creature's own clips, merged, DOWNSAMPLED, §2.9), the
pose LRU under `kDistantPoseWordsMax`, the flipbook selector, the
`RenderOnlyBody` producer, the CPU frustum cull, slot-budget accounting (the
living before corpses, nearest first), and `kMaxBodySlots` → 4096 with the
model-table assert resolved (claim `world.h`). Gate `distant-look` on
synthetic items. `--shot-frames` for the owner's look check.

**D2a: calibration harness and the pure resolver (large; parallel with D0).**
The stroke log at stroke START and at its outcome (miss / blocked / parried /
hit; §2.6), the duel-matrix gate `distant-calib-matrix` that writes it under
`SANDVOX_DISTANT_CALIB=<out.json>`, the reducer, the first
`assets/mobs/distant_combat.json` and its loader, and the pure, deterministic
resolver (`src/game/distant_combat.{h,cpp}`: kits, limb roles, the roll, shell
factor, bleed, burn, ranged flight time/hit falloff), unit-gated on synthetic
kits. Nothing here knows `DistantMobs` exists.

**D1: ground, walking, sleeping (medium).** `DistantGround` (base + water +
ChunkStore edits WITH the eviction drain, §2.4; `.svedit` and dungeon floors as
D1b if needed), the `NavProbe` over it, `Think` with the distant `WorldView`
(wander, flee and approach work; no attacks yet), extracted `Steer` + drive +
spacing, the promotion-line hold, the spatial hash, sleep and stride, mid-air
demotion landing. Promotion snap-to-real-ground. Wire D4a's producer to the
tier (statues follow the walkers). Gates `distant-walk`, `distant-sleep`. One
`--shot` frame of a distant patrol for the owner (no assertion on look).

**D2b: abstract combat in the tick (medium).** `DistantKit` and limb roles at
demotion, the resolver from D2a driven by `Think`'s `attack`/`shoot`, statuses
(bleed, burn), death and dead promotion. Gates `distant-fight`,
`distant-calibrate` (asymmetric matchups, §4). Measure the §2.11 cost targets
and record them in `last_run.json`.

**D3: the edge (medium).** The shared actor band (players excluded, §2.7),
near-hits-distant (the `StepStroke` hook), distant-hits-near (synthesized
contact through the ordinary door), arrows both ways (§2.7). Flip
`distant.enabled` default to TRUE here, and update `DESIGN.md` (a "Distant
creatures" section; the "Not done" line about parked creatures) and the
tuner's `ARCH_NODES`. Gate `distant-edge`.

**D5+D6: activation, schedules and load (medium).** PARKED → DISTANT within
the radius, with a budget (also on load: the parked records a save wrote come
back as distant). Villagers walk their schedules while distant
(`ScheduledPlace` as a goal, not a teleport). `npc-catchup` still passes
(catch-up remains the path for a record that was never distant). The version
bump only if a tail is needed. Gates `distant-schedule`, `distant-save`.

**D7: multiplayer (large, later).** §2.13. Not part of this execution.

**D8: cap overflow (owner decision §5.4: YES, after D3).** When the
64-living cap is full, the farthest near creature from the player (beyond a
minimum distance, `distant.overflowMinDistVox`, e.g. 150 = 15 m) demotes to
distant even inside the window, and the nearest distant one promotes into the
freed slot. This makes fights larger than 64 possible, at the cost of reduced
fidelity on the far side of the fight. In-window distant creatures read ground
from the fetch cache where they have it (deterministic, K = 4), else
`DistantGround`, and keep the players-excluded rule. Gate `distant-overflow`.

**D9: the player's reach (owner decision §5.5: YES).** The player's arrows
continue past the edge as abstract flights over `DistantGround` and hit
distant creatures through the D2a resolver's ranged branch. Spells stay
window-bound. Gate `distant-arrow`.

## 4. Gates

All runnable with `--gate <name>` alone, ticking through `support::RunTicks` /
`TickCursor` (`tickrig.h`). The rig pins the window, so creatures are moved
OUT of it by teleport (`selftest_npc.cpp:444-452`'s idiom), never by moving
the window. Thresholds go in `tests/baseline.json` (`BaselineNumber` /
`RecordObserved`), never in C++. Each is registered in its domain list and in
`kOrder` (`selftest.cpp:149-1409`); a gate missing from `kOrder` never runs.

| Gate | Package | Asserts |
|---|---|---|
| `distant-roundtrip` | D0 | demote → N ticks → promote: same id, same brain target, record digest equal (`mob-save-delta`'s), every limb's hp equal; a corpse round-trips dead; a creature demoted inside a dungeon box or 3 m above `TerrainHeight` classifies ANCHORED and promotes where it stood; a save taken while K creatures are distant loads K parked records back; `parked` did not increment inside the radius; with `enabled=false` the old park path runs |
| `distant-walk` | D1 | a distant creature walks ≥1,000 voxels over varied ground (the `peaks` map, as `altitude` does) and stays on `DistantGround`; refuses an over-budget climb; water handled; an ANCHORED creature does not move; a waiting creature never crosses the promotion line; a creature over a dug-and-evicted crater walks into it identically in two runs (the eviction drain); promotion lands within `distantPromoteMaxDropVox` of real ground with no fall damage; twice-run trace digest equal |
| `distant-sleep` | D1 | 200 idle distant creatures: awake count 0 within T ticks; µs/tick under `distantIdleMaxUs`; a creature walking past wakes exactly its neighbourhood |
| `distant-fight` | D2b | two squads fight distant: casualties > 0, twice-run digest equal (per-creature limb hp + origin bits + deaths, `mob-cap64-twice`-style), awake count → 0 after the fight, dead promoted as corpses on the ground; 128 engaged costs ≤ `distantEngaged128MaxUs` (a regression ceiling; the target is §2.11's 1.5 ms) with the per-phase attribution recorded |
| `distant-calibrate` | D2b | ASYMMETRIC matchups (armoured vs unarmoured, mace vs plate, 4v3, sword vs zombie, archer vs melee), K seeds each at distance against the table's full-fidelity rows: each side's win rate within `distantWinRateTol` (or 2 standard errors of the difference), mean time-to-kill ratio inside `distantTtkRatioMin..Max` (±25%, or within 2 standard errors of the full-fidelity mean) where full fidelity decided, and each side's hp left within `distantHpLeftTol`; a matchup listed in `distantCalibrate.knownDivergent` is measured and printed, not asserted (§6 D2b) |
| `distant-edge` | D3 | near vs distant melee pair across the edge: both take damage, the near wound is real (lattice changed, blood op emitted); a distant archer's arrow enters the window as an `ArrowFlight` and hits; a creature pacing the edge flips tier ≤ `distantMaxFlips` per 600 ticks |
| `distant-look` | D4a | a distant creature at 40 m covers > N pixels in a shot; render-only slots never exceed the free budget; the near crowd never loses a slot; baked pose words stay under `kDistantPoseWordsMax` and the words per pose are recorded |
| `distant-schedule` | D5 | a parked villager inside the radius activates, walks its schedule while distant (ref dormant), sleeps at its anchor, promotes at its scheduled place where it walked (no catch-up); a never-moved one takes the catch-up placer; twice-run identical |
| `distant-save` | D5+D6 | save with K distant creatures (one maimed + bleeding at distance, one burning, one armoured, a corpse) → load → one Unpark → the same K exist (activated), every limb's hp, sever, bleed, burn, stump and hand preserved; the maimed record loaded near keeps its sever and re-bleeds; a v12 record upgrades to v13 |
| `distant-calib-matrix` | D2a | the duel matrix runs at full fidelity and logs every stroke STARTED with its outcome; under `SANDVOX_DISTANT_CALIB` it writes the tables; the resolver's unit arm (synthetic kits) is twice-run identical and its hit rate / mean hp over 10k rolls match the table it was given |
| `distant-overflow` | D8 | 64 near + N distant with the cap full: the farthest near creature past `overflowMinDistVox` demotes and the nearest distant one promotes; never more than 64 living near; no player in any distant actor list; twice-run digest equal |
| `distant-arrow` | D9 | a player arrow loosed at a distant creature past the edge resolves through the ranged branch at the computed flight time; a miss lands on `DistantGround`; twice-run identical |

## 5. Risks and owner decisions

**Decisions: RESOLVED by the owner 2026-10-15** (accepted the audit's
recommendations):
1. **Radius and cap** (D0): **128 m (1,280 voxels) and 512.** The radius is
   bounded by CPU cost only (the ground works anywhere on the map).
2. **Look** (D4): **flipbook statues for v1.** Articulated (~24 slots each,
   resident bricks, FK posing) is not v1.
3. **Casters** (D2): **hold and do nothing at distance in v1.** An abstract
   spell hit (per-spell damage estimate) is a later package.
4. **Cap overflow** (D8): **yes**, after D3.
5. **The player's arrows** (D9): **yes.**
6. **Calibration band** (D2): **±25% time-to-kill**, plus win-rate agreement
   on asymmetric matchups (§4).

**Risks:**
- **Calibration rot.** Melee tuning changes, nobody re-runs the harness, and
  the tiers drift apart. Mitigation: `distant-calibrate` is a real gate, not
  advisory, and the harness is one command.
- **Promotion pop.** A statue → rig swap at 25 m, and a creature `TerrainHeight`
  placed inside a building. Mitigation: snap-to-real-ground with an up-search;
  statues baked from the creature's own live limbs (§2.9); the look check in
  D4a.
- **Sight-range assumption.** No longer load-bearing: players are excluded
  from distant actor lists by construction (§2.7). `LoadBehaviors` still logs
  (does not assert) any profile whose `sightRange × keepRangeScale` exceeds the
  geometric bound `kWorldN/2 − 3·kChunk + kPad` (224 today), because such a
  profile would see the player from the band if the exclusion were ever
  dropped. Today's maximum is 160 × 1.25 = 200.
- **Underground and indoor creatures.** Covered by ANCHORED (§2.4); the
  failure to watch for is a creature misclassified SURFACE inside a structure
  `TerrainHeight` does not know (a ruin's upper floor 2 m up passes the 2.4 m
  snap test). Promotion's up-search catches the landing; D1 measures how
  often it fires.
- **The shark.** The aquatic confinement rule needs water at distance:
  `WaterColumnAt` covers rivers and lakes, `TerrainColumn().water` covers ponds
  and the sea. Check that the two agree at shorelines before trusting D1.
- **Ref and spawner duplicates** (§2.14) are the most likely correctness bug.
  The D0 audit exists for that.
- **Merge traffic.** Waves 1 and 5 run packages in parallel that all touch
  `selftest.cpp`'s `kOrder` and `tests/baseline.json`; the orchestrator merges
  those by hand. Every package after D0 builds on `distant-npcs`, not main.
- **Hash moves**: none expected through D2 (no ops, and the switch is off).
  D3's flip may move gate digests that involve creatures crossing the edge. That
  is a notification, not a regression (CLAUDE.md rule 1): rebaseline once.

## 6. Deviations from this plan (filled in by the implementing packages)

### D0

Landed on `distant-npcs` 2026-10-15 (`71840cf1`). Code: `src/game/distant.{h,cpp}` (the
tier, the classifier, MobSystem's border code), `src/game/steer.{h,cpp}` (the
extracted steer), `MobSystem::PatchMobRecord` + `Mob::ApplyDistantOverlay` in
`mob.cpp` (beside the record reader they are built on), the promotion pass in
`EntityParking::Unpark` (`persist.cpp`), phase H in `session.cpp`. Gate
`distant-roundtrip` (§4, plus a steer oracle arm, a patch arm, a retire arm, a
switch-off arm and a full-tier arm; 3.7 s alone); `mob-park` gained arm G
(switch on). Verified with `--verify distant-roundtrip,mob-park,
mob-save-delta,npc-catchup,refs-npc-identity,mob-handoff,mob-cap64-twice`:
all pass.

**The API later packages build on** (`game/distant.h`, `game/mob.h`):
`DistantMob` (id, defName/defIndex hint, refId, record, origin, vel, `SteerState
steer`, `ai::Brain brain`, `DistantGround ground`, cached worldSize/speed/loco,
`limbHp` + `limbRole` (flags + rig tag) per RECORD limb, D2b fields
`bloodLossPerTick` / `burnTicksLeft` / `strokeImpactTick` / `strokeStyle`
written by nobody yet, dead/cause/deathSeq/deathTick, asleep/wakeTick,
`Overlay()`); `DistantMobs` (sorted by id; `Find`, `FindByRef`, `All`, `At`,
`Insert`, `Erase`, `Take`, `PromotionCandidates`, `MarkMoved` + the two scan
latches, `Stats`, `SetCapForTest`); `DistantRunTotals()`; `SteerStep`;
`ClassifyDistantGround` / `InPlacedDungeonBox`; on `MobSystem`: `Distant()`,
`DistantMut()`, `PromoteDistant`, `HasRoomForDistant`, `DistantRecord`,
`PatchMobRecord`, `LocateCreature` / `LocateByRef` / `IdInUse`,
`SetDistantRefId`, `BrainAnyTierMut`, `SetBehaviorAnyTier`; `LoadOne(...,
keepId)`; `Mob::ApplyDistantOverlay`, `Mob::ShieldTurnCap`.

**Deviations**

1. **"Near the player" is the WINDOW CENTRE** (radius test, retire test,
   promotion order). It is within ~3 chunks of the primary (2-chunk
   hysteresis), immaterial at 128 m, and -- unlike a player position -- the
   same answer in the game and in a gate whose window is pinned. D8's
   "farthest from the player" may want the player itself.
2. **`MobSystem::ParkFn` takes `(Vec3 origin, record)`**, not `(const Mob&,
   record)`: a retiring distant creature has no Mob. The only installer is
   `EntityParking::Bind`.
3. **Unpark in the tick = `TickAuthorityCtx::parking`** (session.h H3b): phase
   H calls it right before `mobs.PreTick`; main.cpp sets it and now Binds
   BEFORE the tick (so tick 1 already has a park function). Null in every
   harness, so the gates that own an `EntityParking` (mob-park, corpse-save,
   npc-catchup, refs-*, worldio) keep their by-hand calls unchanged.
   `distant-roundtrip` sets it on its rig.
4. **Promotion passes `catchUp = true` in D0.** A frozen distant villager is
   a parked one with an id, so it comes back where its schedule says, as an
   unpark does. D5 (distant villagers walk their schedules) must turn it off
   for creatures that walked. The steer state is restored only when the
   creature came back where the overlay put it.
5. **Promotion loads the PATCHED record** (dead block included), then runs
   `ApplyDistantOverlay`. So a creature that dies at distance (D2b) comes back
   through `LoadOne`'s dead path (`spawnDead_` + `EnterLoadedDead`: no cry,
   ragdoll from the record pose) -- §2.8's second option. `ApplyDistantOverlay`
   still has the `Die()` branch for an overlay that is dead on a body that
   loaded alive, and the hp-zero rules (`HpZeroSevers` -> `Sever`, base limbs,
   `DamageCtx{}`); with D0's frozen numbers neither ever fires. D2b picks the
   cause it passes.
6. **The brain moves back BEFORE `ApplyDistantOverlay`** (plan: after), so a
   `Die()` inside the overlay acts on the creature's own brain.
7. **The tier does not hold its def** against `EvictUnusedDefs`: 512 distant
   pool humans must not pin the near crowd's runtime slots and pool words.
   `DistantMob` caches worldSize / speed / LocomotionDef at demotion;
   `defIndex` is only a hint (valid while that slot still has `defName`);
   `LoadOne` re-resolves by name at promotion. D4a's look keys may want a
   pinned def per baked look instead.
8. **SURFACE probes the footprint's centre column at the feet** (plan: the
   origin): `|feet - (TerrainHeight + 1)| <= surfaceSnapMaxVox` and that cell
   outside every placed dungeon box of `CurrentWorldMap().dungeons`.
9. **Demotion proves the overlay first**: the record patched with the
   overlay read off the live Mob must give back the record's own bytes, or the
   creature parks instead (counted `failed`, logged). Cheap (one copy + one
   parse per leaver) and it pins `SaveOne`'s limb walk to the tier's.
10. **Retire rescans** on a window move, the tier's latch (`MarkMoved`, D1's
    drive) or a change of radius + hysteresis (an F5); the switch turned off
    retires everyone at the next tick ("off = the old behaviour" holds at
    runtime too). Without a park function nothing retires.
11. **Nearest-first is the SERVICE order** (`PromotionCandidates`), not a
    promise about completion order: a nearer creature waiting on its ground
    does not block a farther one whose ground is known -- `Unpark`'s rule for
    records. Distant creatures are served before parked records.
12. **`Unpark`'s return / `lastCall` / `maxCall` count promotions too**;
    `Stats::unparked` stays records only; the tier counts its own.
13. **`SteerStep(state, loco, defSpeed, shieldRateCap, capMulL, capMulR, dt)`**:
    the shield's ceiling is passed as a number (`Mob::ShieldTurnCap`, +inf
    with none), because `ShieldTurnRate` is `min(freeRate, cap)`;
    `MobSystem::Steer` is the wrapper. Guarded by the oracle arm (100k random
    states, every bit) rather than a pinned `mob-cap64-twice` digest, which
    does not exist (that gate compares two arms of one run).
14. **Velocity** is stored (heading x speedNow, plus fall / lunge carry when
    airborne) for D1's mid-air landing, not restored at promotion.
15. **No save-format change** (`kSaveVersion` stays 11): the overlay fits the
    record. `MobSystem::SaveState` (the legacy whole-section writer) still
    writes the near crowd only, as it did for parked creatures.
16. **Not added in D0** (left to their packages): `ai::SelfView selfCache`,
    `DistantKit`, the distant `WorldView`/`NavProbe`, sleep/wake logic.
17. `check_invariants.py`'s tuning-consumer allowlist names the three rows
    batched ahead of their readers (`idleStride`, `promoteMaxDropVox`,
    `overflowMinDistVox`); each leaves when its package reads it.
18. `DESIGN.md`'s parking paragraph says Unpark runs inside the tick now,
    plus three lines on the seam; the full section stays D3's.

**§2.14, the audit** (every call site outside `src/test/` of `FindMobById`,
`FindCombatantById`, `CreatureWithId`, `FindCreature`, `MobBrain(Mut)`,
`FindMobByRef`, `IsAlive`, `RemoveMob`, `SetMobBehavior`, and every stored mob
id / ref lookup). The near lookups are UNCHANGED (they mean "a Mob with a rig
in `mobs_`"); class (a) sites now ask the either-tier queries.

(a) MUST SEE DISTANT -- fixed:

| Site | Was | Now |
|---|---|---|
| `LoadOne` duplicate-ref refusal | near only: a parked record for a ref whose body is distant unparked a second body | either tier, except the distant creature coming back under `keepId` |
| `LoadOne` / `AdoptDeadAvatar` player-corpse id probe | near only | `IdInUse` (either tier) |
| `refs_dungeon.cpp TickSpawner` slot occupancy | distant = missing -> dead after 600 ticks -> duplicate spawn | distant alive = occupied (`missing = 0`) |
| `TickSpawner` dead-rename loop | a distant corpse kept the slot name | renamed in the tier (`SetDistantRefId`; the patched record carries it); uniqueness probed across both tiers |
| spawner + npc `deactivate` (Edited/Deleted) | `RemoveMob` missed a distant body | removes the distant one too |
| npc `activate` ("already standing") | near only | `LocateByRef` |
| `MobSystem::RemoveMob` (demon `Depart`, the refs above) | near only | also erases from the tier |
| `demon.cpp DemonTick` | `IsAlive` false -> LiveDemon erased (binding, circle, pact lost) | kept while alive in either tier; a DISTANT demon is not re-judged (its circle is not in the window) |
| `DemonUnbind` / `AimAtSummoner`, `demon_seals Release`, `demon_talk Present` | one-time profile / brain writes missed a distant demon | `SetBehaviorAnyTier` / `BrainAnyTierMut` |
| `demon_talk` DMNS save | a distant bound demon not saved | saved; def name + origin from the tier |
| `demon_talk ContractTick` restores | near bodies only | also matches a distant body (strictly nearer) |
| `demon_cast MobCastTick` | `IsAlive` false -> caster latched `gone` for good | gone only if gone in both tiers; distant = frozen (no regen) |
| `ApplyAnnounce` / `ApplyHandoff` (net) | -- | a peer's creature replaces a distant copy of its id (unreachable in D0: the tier is host-only, host-band ids) |
| `SetBehaviors` (R reload) | -- | distant brains re-pointed by their own profile's name |
| `persist.cpp 'MOBS' saveRecords` | `mobs_` only (audit A2) | + every distant creature's patched record |
| `MobSystem::Reset(rewindIds)` | -- | clears the tier before ids can be re-issued |

(a) BY CONSTRUCTION, no code change: `ServiceRising` (`FindCreature` of a
corpse to raise) -- a rising is booked only on the dead, and a corpse's
booking is withdrawn at demotion and travels in its record (re-booked by
`EnterLoadedDead` at promotion), so no booking ever names a distant id. D2b's
distant deaths must keep it so (book into the overlay, never `rises_`).

(b) MUST NOT SEE DISTANT (~140 sites): every rig / limb / body / damage /
equip / carve / coat / shock / arrow-embed / pierce / render accessor
(`CreatureWithId`, `FindCreature`, `FindMob` and their wrappers in
`mob.cpp`), `LoadOne`'s and the handoff's re-lookups after a spawn, the
archers' draws, the casters' bodies, `refs_dungeon Bodies` (plates and
triggers), and main.cpp's shot harnesses. Unchanged.

(c) INDIFFERENT (~66 sites): AI target resolution (`FindCombatantById`,
`StepStroke`, swim / archery tracking -- the target is simply not there this
tick), net melee in flight (ghosts only), demon malice / gaze / talk-range /
per-tick brain reads (a distant demon is frozen, which is the intended
behaviour), dialogue `TickSession` (a speaker past the window is out of talk
range anyway), `refs_npc StepResident` (the resident's plan resets on return,
exactly as an unpark resets it today; D5 replaces this), `usePoint` / `onUse`
(the player is within 7 m), `refs.cpp LiveMobOfRef` (no callers), debug UI and
harness printouts. Unchanged.

Test-side note: `dungeon-entities`, `refs-npc-identity`, `npc-catchup` count
bodies per ref over `MobAt` and `demon-contract` / `demon-malice` use
`!IsAlive` as "gone"; all run with the switch off, so they still mean what
they meant. D3's flip should revisit them.

### D4a (branch `dnpc-d4a`, 2026-10-15)

Landed: `src/game/distant_draw.{h,cpp}` (bake, pose cache, producer, test
hook), `BodyRegistry::SetRenderOnly({lists...})` + `RealSlots()`,
`kMaxBodySlots` 4096, gate `distant-look`, the `distantDraw` block in
`build/last_run.json`, the frame-loop wiring in `main.cpp`.

**The API D1 calls** (all on the `DistantDraw distantDraw` main.cpp owns;
D1 must thread a pointer to it into `TickAuthorityCtx` to bake from the tick):
- `DistantBake Bake(const Mob&, MicroBodySet&)` -- at DEMOTION, **before
  `ReleaseRig`** (it reads the live limbs, bricks, gear and skeleton). Store
  `.key` (a `DistantLookKey`, plain data) on the `DistantMob`; for a corpse
  use `.restY` as the item's `origin.y` (a corpse's `origin_` follows its
  pelvis's rest corner, below the ground when it lies flat). Not
  thread-safe: call from the tick's serial part.
- `std::vector<DistantDrawItem>& Items()` -- every frame (or `SetItems`):
  `{look, state Idle/Walk/Strike, gaitPhase, origin (Mob::origin_ frame,
  y = what the statue rests on), heading, dead}`.
- `Produce(...)` is already called by the frame loop; D1 only fills items.
- `BakeDef(const MobDef&, MicroBodySet&)` -- the def's rest pose (the `bare`
  key); `Produce` also does this on demand from `mobs.Defs()`.

Deviations and decisions:
- **`kMaxMicroBodyModels` restated, not raised.** A render-only body owns no
  clone, so the owned term stays bounded by the REAL slots: new
  `kBodySlotsReal = 2048` (what the old static_assert sized) and
  `kDistantPoseModelsMax = 512` (statue records, enforced by the cache) in
  `world.h`; assert `4096 >= 2048 + 1024 + 512`. Neither constant reaches WGSL
  (not in the prelude; both shaders bind runtime-sized arrays), so no shader
  or `check_shaders.sh` change and no SPIR-V cache miss.
- **Baked on the CPU, packed lazily.** `Bake` builds every pose's downsampled
  cell list on the CPU (kept for `kDistantLooksMax` = 512 looks, LRU); a pose
  is `MicroBodyPack`ed only the first frame it is drawn, at most
  `kDistantPacksPerFrame` (8) per frame, and packed poses live in an LRU under
  `kDistantPoseWordsMax` (1 MiW). An evicted pose re-packs from the CPU copy;
  no live creature is needed. A pose is never packed if it would leave the
  pool under `kDistantPoolReserveWords` (512 KiW) or the model table under
  `kDistantModelReserve` (256 records): a statue goes without, never a wound.
- **The look key has a third part, a coarse DAMAGE signature** (per limb:
  gone; brick owned with remaining-volume and rewrite buckets; coat bucket).
  Without it a bloodied or one-armed creature would draw the first clean
  statue of its outfit, which contradicts "bake from the live limbs". The
  fallback chain is `full` -> `pristine` (same gear, damage 0) -> `bare` (the
  def alone, baked automatically by the first `Bake` and on demand).
- **Walk A/B legs are procedural.** No clip carries a leg (the runtime walks
  legs by gait IK), so the statue's stride is a hip/knee scissor on the leg
  chains (the gait's own leg groups pick which leads) plus the `walk` clip's
  arm swing, with the arm on the back-leg side forward.
- **Strike** = the punch clip that keys the weapon hand (else `bite`/`lunge`/
  ...) at the sampled time of its farthest forward reach; held items, worn
  shells and embedded arrows follow their host rigidly at the offset they
  have in the live rig.
- **Lying** = the corpse's real ragdoll pose when baked from a corpse (and a
  later corpse upgrades a look whose Lying was only the tipped idle); else
  the idle rotated onto its back. Every pose is GROUNDED (lowest cell at
  `origin.y`).
- **Downsample rule**: a 5 cm cell is solid if ANY source voxel lands in it
  (thin limbs, blades and hair survive); its material/colour is the weighted
  majority with exposed voxels at 16x interior ones (the cell shows skin, not
  the meat behind it); interior voxels are subsampled at the scale ratio; a
  coat survives when >= 1/4 of the cell's exposed voxels carry one (the
  statue then has a stain lattice: blood shows); bruises are dropped.
- **Colour**: a model has one art bank, a statue's paint comes from several
  (cast + wardrobe + dyes, which are baked into the colour since a
  render-only body has no dye word). The palette maps into the dominant
  bank: an entry within RGB distance^2 300, else a new entry (at most 96 per
  palette epoch, never the bank's last 32), else the nearest. Counted:
  `artAdded`, `artNearest`.
- **Epoch**: a replaced `MicroBodySet` (materials reload) drops every packed
  pose UNFREED and every CPU look (material ids may renumber). Looks come
  back from the next `Bake` or as `bare` on demand.
- **The owner's look check is written by the gate**, not a `--shot-frames`
  frame (RunShots draws the world only, no bodies):
  `build/distant_look.bmp` (a row 20..80 m out, all poses, the live creature
  at 6 m) and `build/distant_look_close.bmp` (the poses side by side).
  `SANDVOX_DISTANT_DRAW_TEST=<n>[:<def>,...]` stands `n` statues in a
  30..80 m ring in the windowed game, baked from the defs' rest poses (no
  creature spawned, no sim effect).
- Not done here: `DESIGN.md` and the tuner's `ARCH_NODES` (D3 per §3; D0 also
  holds `ARCH_NODES`). Cube-path limbs (skinScale 1: the test mob only) are
  not baked. Statues out of the window are fully lit (§2.9, known).

Measured (`--verify distant-look,mob-cap64`, both PASS; `build/last_run.json`):
- **Words per pose**, the human dressed (dyed tunic) and armed (sword): idle
  4,114, walk A/B 5,236, strike 3,690, lying 4,235 (~22.5k for all five;
  idle 11x34x22 cells -- the sword sets the depth). Naked: 1,122 / 2,601 /
  2,601 / 2,380 / 2,916 (~11.6k). No pose carried a blood lattice here. So the
  1 MiW budget holds ~45-90 fully packed looks, and far more in practice since
  a look only packs the poses being drawn. Plan §2.9 estimated far worse
  (~66k for the full-res human): the downsample is ~15-60x.
- **40 m**: one statue covers 180 px at 1920x1080 (threshold 80).
- **Slots**: 35 real + 7 arrows leave 4,054; 5,000 items -> 4,054 drawn,
  946 starved, living before dead, nearest first, and the near crowd's
  transforms / handles / micro instances byte-identical with and without.
- **Bake**: 3.2 ms naked, 7.7 ms dressed + armed, 10.6 ms worst (a def's bare
  look); colours: 0 added, 3 mapped to the nearest (the human's bank is
  within the 32-slot reserve).

Open issues for D1/D3:
- **In a full 64-brawl the POOL, not the budget, is the limit.** `mob-cap64`
  peaks at 8,371,749 of 8,388,608 pool words (gore clones), so the pack
  reserve refuses every new statue pose during such a fight (counted as
  `noFit`; already-packed poses keep drawing). That is the intended
  priority (gore first), but it means a big near fight can leave newly
  demoted creatures undrawn until it calms. If that shows, the lever is a
  pool share reserved for statues (the 1 MiW budget carved out of the pool
  rather than competing for it), which is a `world.h` sizing decision.
- An unwounded corpse shares its living look's key, so its real ragdoll pose
  becomes that look's Lying for every creature of the look (by design; the
  key's damage part is coarse). A held weapon stays in the hand when the
  idle is tipped onto its back (it points up).

### D2a (branch `dnpc-d2a`): calibration harness and the pure resolver

**What landed.** `src/game/stroke_log.{h,cpp}` (the log + its reducer),
`src/game/distant_combat.{h,cpp}` (vocabulary, table, loader/writer, kit,
resolvers), five hook lines in `mob.cpp` (`StartStroke`: OnStart;
`StepStroke`: BeforeSweep / OnSweep x2 / AfterSweep around the head and haft
`MeleeSweepDamage`, OnEnd at Finished and at the abort), the three salts
`kSaltDistantStrike/Arrow/Style` in `ai_behavior.cpp` beside the AI's (with a
`static_assert` that all nine are distinct), gate `distant-calib-matrix`
(`selftest_distant_calib.cpp`, kOrder after `infect-perf`), and the first
`assets/mobs/distant_combat.json`, written by the harness (8 rounds, 112
bouts, 4,936 strokes, 147 arrows; ~5 min).

**The API D2b drives** (all in `distant::`): at DEMOTION, before
`ReleaseRig`, `BuildDistantKit(mob, items)` and `CaptureDistantVitals(mob,
cal)`; once at load, `LoadDistantCombat(path, cal, log)` (builtin fallback if
absent). Per distant tick: `PickDistantStyle(kit, dist, id, tick)` when Think
says attack; at the style's `impactTicks` (kit; or the table's measured one)
`ResolveStrike(att, def, defVitals.limbHp, style, dist, {id, tick}, cal,
gear)` then `ApplyStrike(defVitals, defKit, out, cal)`; for `shoot`,
`ResolveArrow(...)` (impact after `flightTicks`) then `ApplyArrow`; every
tick `StepBleed` and `StepBurn`; `DeathTest` is the near rules. Every
resolver is pure (outcomes are returned, the caller applies them in its own
defined order).

**Deviations.**
1. **No axe or spear item exists** (items.json: sword, cleaver, shortsword,
   dagger, mace, bow). The cleaver (long, the heaviest edge) stands in for
   the axe and the dagger (short form, thrust-heavy) for the spear.
2. **`meanPower` is an EFFECTIVE multiplier** on `StrikeProfile::Total()`:
   life hp per hit on UNARMOURED flesh / strike total. It folds in the speed
   ramp, edge alignment, the carve's volume hp and repeat-strike falloff, so
   the resolver reproduces the measured mean without re-deriving any of them.
   The sweep's own 0..1 power is logged too (`Stroke::power`). Each style
   also carries `powerCv`; the resolver spreads hp by an exponential mixture
   with that CV (mean exact), so sever-sized blows still happen at distance.
3. **The log measures by DIFFERENCE**, not by instrumenting the wound model:
   the target's per-limb hp, shell hp, held-item hp, `HpLostBy` ledger and
   bleed budget are read before and after the stroke's sweeps. No resolver
   (cut / blunt / bite / parry / shield) was touched; off = one branch.
4. **Shell factor** = `ShellResponseOf(h, cause).passed + (1 - passed) x
   exposure[kind]`, where `exposure` (the gap rate: holes, a side the plate
   does not cover) is measured per kind. Cloth (hardness <= cutHardnessRef)
   passes an edge whole (melee.cpp's rule). Pierce uses the Blade row's
   `carve` ratio as its `passed` (the channel's own pricing, kerf.h). The
   kit caches each limb's hardest live worn shell by dominant material.
5. **Bleed** is a RATE (`DistantVitals::bleedRate`, hp/tick) decaying by
   `bleed.decay`; a blow owing B adds B(1 - decay). `perHp` per kind comes
   from the log (bleed budget opened x gore.bleedHpPerVoxel per life hp),
   times `drainShare` from a probe (one direct `Mob::Damage` on a passive
   dummy); `decay` from that probe's mean drain time. Fists pool into Blunt.
   Measured: cut 1.58 hp of blood per hp, blunt 0.08, bite 1.29, decay
   0.9866 (mean drain 73.5 ticks), drainShare 1.03.
6. **Burn costs nothing but the cap**: on a body the fire chars (rewrites)
   rather than carves, so measured burn hp/tick is ~0 and the whole cost is
   `BurnCapFor(burnFrac)` (gore's curve, copied at load; equal to
   `Mob::BurnHealthCapFor`, asserted). Measured 0.00086 burnt fraction per
   tick, 817 ticks to the burn death of an undoused human (fire does not
   gutter on a human: mob-burn's note).
7. **Matchups also carry `hpLeftA`/`hpLeftB`.** Full-fidelity fights mostly
   end UNDECIDED in 60 s (both sides disarmed or handless, bleeding slowly:
   59 "none of its styles is usable" lines in the run), so win rate and TTK
   score only 42 of 112 bouts and the 4v3 brawl none. Each side's TotalHp
   fraction at the end, decided or not, scores every bout; D2b's
   `distant-calibrate` should compare that damage race as well as the win
   rate and TTK (amend §4's assertions accordingly).
8. **Ranged hit = CONTACT** (a glance off plate is armour, not a miss); life
   hp per arrow = the target's TotalHp drop on the strike tick less that
   tick's blood loss (`hpCharged` is the struck slot's, a shell's for a
   cuirass). The knots are in-band (20 and 53 voxels: archers hold 35..80);
   the rate is flat past the last knot. Moving targets were hit no less often
   than still ones (they approach radially), so `speedPenalty` came out 0.
9. **Block chance is per defender guard class only** (none / blade /
   shield), additively smoothed. The shield measured 0.6%: the AI raises it
   only between its own strokes.
10. **Small samples are smoothed**: hitRate, roleWeights and meanPower take 4
    pseudo-samples of the pooled value; `byWeapon` rows from `minSamples` (8).
11. **Reach at impact** = style reach + the attacker's half depth (BeginStroke's
    slack) + `reachSlack` (2, not measured).
12. **The default gate is a subset** (the bouts marked `inDefault`, one round
    of 300 + 300 ticks, 9.3 s measured including the unit and log-neutral
    arms); it asserts that bouts DREW BLOOD (`minBoutsHurt`), not that they
    were decided, since a full-fidelity kill takes 600+ ticks.
    `SANDVOX_DISTANT_CALIB` runs all fifteen bouts for
    `distantCalib.calibRounds` rounds and writes the table. Re-running the
    harness reproduced every table value (one of 4,936 strokes flipped
    miss -> hit-other between the two processes; no reduced number moved).

**Headline numbers (the shipped table).** Pooled stroke hit rate 0.59 of
4,936 started (2,787 hits, 224 blocked/parried, 1,733 misses); meanPower
0.98. Per style: horizontal_r 0.61 / 0.97, horizontal_l 0.59 / 1.26,
diag_tr_bl 0.66 / 1.29, overhead 0.55 / 1.00 (a third of its hits on the
head), thrust 0.55 / 0.40, bite 0.67 / 0.45, bite_lunge 0.52 / 0.87,
snake_strike 0.66 / 0.41 (90% legs), snake_rear_strike 0.49 / 0.37 (67%
arms). Block: none 0.013, blade 0.087, shield 0.006. Armour exposure: cut
0.01, blunt 0.03, bite 0.11, pierce 0.0. Arrows: 500 vox/s, contact 0.80 at
20 voxels and 0.49 at 53, 1.59 x the flat 35 hp per bare contact. Matchups
(8 bouts each, 1,800-tick timeout; archer 5, 1,200): armoured vs unarmoured
winA 0.62 / winB 0 (hp left 0.99 / 0.16, TTK 605); duelist+mace vs plated
swordsman winA 0 / winB 0.75 (0.07 / 1.00, TTK 1,038); zombie vs swordsman
winA 0 / winB 0.38 (0.20 / 0.89, TTK 580); 4v3 swords never decided (0.56 /
0.54); archer vs swordsman winA 0.20 (1.00 / 0.49, TTK 782).

**Open.** Pierce `perHp` is the builtin 0.9 (arrows are not strokes; no
arrow-bleed probe). Infection (zombie rot, snake venom) is not modelled at
this tier. The player's own swings are not logged (no NPC stroke behind
them). ARCH_NODES / DESIGN.md are left to D3, which owns the feature's
documentation flip.

### D1 (branch `dnpc-d1`): ground, walking, sleeping

**What landed.** `DistantGround` (the ground SOURCE; D0's `DistantGround`
MODE enum is renamed `DistantGroundMode`, the field `DistantMob::groundMode`),
the distant tick `MobSystem::TickDistant`, the 2D spatial hash, sleep and the
idle stride inside `DistantMobs`, the mid-air landing (`LandDistant`), the
promotion snap (`SnapPromotionGround`, plus its chunks in `Unpark`'s ground
wait), `MobSystem::DemoteForTest` (a gate seam: the park rule's own demotion,
now), `FillDistantDrawItems` and the statue bake at demotion (D4a wired:
`mobs.SetDistantDraw(&distantDraw)` in `main.cpp`, `DistantDraw::Bake` in
`DemoteToDistant` before the caller's `ReleaseRig`, the frame loop fills
`distantDraw.Items()` from the tier unless the D4a test hook is on).
Engine seams: `Stream::SettledStoredChunk` / `EvictionPending` (the drain),
`ChunkStore::SetChangeFn` (one listener; Put / Erase / Clear / Unbind /
BindLoad), `worldmap::CurrentWorldMapSerial()`, phase H binds the stream every
tick (`mobs.SetDistantStream(&stream)`). Gates `distant-walk`,
`distant-sleep` (`selftest_distant.cpp`, kOrder after `distant-roundtrip`);
`build/last_run.json`'s `distant` block gains the walk's counters and
`tickUsPerCall`. No tuning row added; `idleStride` and `promoteMaxDropVox` are
read now (left the consumer allowlist).

**The API D2b / D3 build on.**
- WHERE IT RUNS: `MobSystem::TickDistant(tick, world, &actors_)`, called from
  the top of `MobSystem::PreTick` after the retire scan and the actor list,
  before the creature loop. Both tiers therefore read each other as of the
  START of the tick; a creature demoted this tick takes its first distant
  step next tick. `nearActors` may be null (a gate's CPU arm).
- ONE STEP (phase 1, id order, writes only the stepper): `SelfView` from the
  creature's cache (`stepUpCells` etc., `armed`/`canGuard`/`ammo` cached at
  demotion, hpFrac from `limbHp` over `hpFull`), a `GroundView` fan over
  DistantGround, a `WorldView` (actors = tick-start snapshots of the distant
  creatures in its hash reach + the near band; `lineOfSight`/`aimPart` null;
  `NavProbe` = `DistantNavCtx`), `ai::Think` -> `IntentOut`, then
  `SteerStep`, crowd spacing, the drive. **D2b plugs combat in right after
  `Think`**: the block `if (out.attack || out.shoot || out.cast)
  Note(combatHolds)` in `TickDistant` is where `PickDistantStyle` /
  `strokeImpactTick` scheduling and the abstract arrow go; the impact
  resolution belongs in a pass after phase 1 (reading tick-start snapshots
  of the defender, applying in id order), before phase 2's sleep test --
  which already refuses to sleep a creature with `strokeImpactTick != 0`.
- Snapshots: `DistantSnap` (centre, faction, hostile, keep, targetId, hpFrac,
  immersion, vel) -- eager for steppers, lazy for the rest. D3's near list
  wants the same struct the other way.
- Phase 2 sleeps (before the moves are filed, so a mover's wake is never
  slept through); phase 3 files the moves (`DistantMobs::Relocate`: refile +
  `WakeEntered`), sets the border latches, writes the sleep summary.
- Hash / wake: `DistantMobs::kCellVox` 64, `Neighbourhood`, `WakeEntered`,
  `Relocate` (the door a gate teleports through), `AnyAwakeHint` /
  `MarkAwakeHint` (a gate writing `asleep` by hand calls it).

**Deviations.**
1. **The ground mode enum is `DistantGroundMode`** (the brief's fix of D0's
   name clash); `ClassifyDistantGround` keeps its name.
2. **Edited ground is read per CELL**: a column's support search treats a
   stored chunk's cells as truth and every other chunk as procgen
   (`y <= TerrainHeight`). So a stored chunk next to a procgen one shows
   worldgen's trees / ruins only on the stored side (a seam), and a
   chunk-TICKET batch forced by the drain before its keep decision keeps all
   its chunks (stream.h, the known gap): those then read as stored, which is
   procgen-equal in content but not in model (trees count).
3. **The stored-chunk cache is a support MASK** (4,096 bits per chunk), not
   the words; liquid in a stored chunk is not read as water (water is the
   analytic columns only). A pond the player drained still reads full.
4. **Water = max(TerrainColumn().water, WaterColumnAt().water)**, and the two
   do disagree at shorelines (plan §5 risk, confirmed): the harness map's
   home lake has columns whose surface stands ~7 voxels ABOVE the adjacent
   dry bank on its south side, so a walker coming from that bank meets a
   wall and walks round. `distant-walk` S therefore starts its swimmer in the
   lake. A fish counts a column as water only where the surface is above the
   bed; it never left the water (`dry ticks 0`).
5. **Swimmers** (`DistantMob::canFloat` = CanSwimAfloat's predicate with the
   tread clip looked up on the RIG at demotion -- the pose resolves its swim
   table on its first animated tick, so a creature demoted before it ever
   animated read "cannot swim"): water reads as a floor at its surface to the
   planner, the line test and the fan (`DistantNavCtx`), the drive floats it
   at `kAfloatShare` of its height under the surface at
   `player.liquidSpeedScale`, climbs out onto a bank within a step of the
   SURFACE, and wades with the step-down budget extended by the water depth.
   A walker that cannot swim walks the bed. Fish: volumetric `Think` (no nav),
   confined to columns with >= max(2, height) of water, eased to the asked
   altitude inside it.
6. **Drops past the step-down budget are REFUSED** (the near tier lets a body
   step off and fall): the fan marks them blocked so steering sees them, and
   the planner's `StepTo` refuses them. DistantGround's `ProbeGround` never
   answers "unknown" (nothing is unknown at this tier), so a column past the
   reach reports its real height and `StepTo` refuses it, instead of near's
   "unknown = open".
7. **A stride must not overshoot its waypoint**: the drive cuts a step to end
   1 voxel short of the live A* waypoint. Without it the strider (4 ticks of
   travel per step, ~4 voxels) stepped past a waypoint the follower only
   retires within 1.9 voxels, turned back, and circled: 1,121 voxels walked
   for 458 of progress on `peaks`; with it 1,123 for 1,118.
8. **No profile = no mind**: a creature with no behaviour profile neither
   thinks nor turns at this tier (the near tier's legacy wander-and-avoid is
   not run) and sleeps at its first step. Every D0 gate's human has no
   profile, which is why `distant-roundtrip`'s frozen / byte-exact arms still
   hold unchanged.
9. **Only a creature that MOVED while distant is snapped at promotion**
   (`DistantMob::movedWhileDistant`: a step, a landing, a `Relocate`). One that
   never moved stands on the ground the near tier gave it, outside the window
   where nothing can have been edited since, and must come back byte for byte
   (D0's contract). The snap is over the footprint's 3x3 columns (the max,
   FootprintFooting's rule), up out of matter or down to the first support,
   within `promoteMaxDropVox`, from the FETCH CACHE -- and `Unpark` waits on
   those chunks (`DistantPromotionChunks`, in-window only) on top of its own
   footprint wait. A kinematic creature's fall bills no damage near (only a
   ragdoll arrest does), so "fall damage suppressed on the promotion tick" is
   true by construction; the gate asserts every limb's hp unchanged and
   `FallsBilled() == 0` over 30 ticks.
10. **The promotion line hold allows the crossing step**: a creature outside
    the line may step in (it must, to become a candidate); once inside, any
    substep that would put it DEEPER (the nearest face's distance) is refused;
    lateral and outward moves are free. Measured: 0.58 voxels over the line,
    then held.
11. **The mid-air landing happens on the first distant STEP** (`landPending`,
    set at demotion for an airborne SURFACE creature), not inside
    `DemoteToDistant`: the gate can then set `vel.y`, and the landing reads
    DistantGround like every other step. Corpses are not landed (they are
    asleep forever; a corpse falling off the window's bottom face lies where
    it left). A fall death sets `dead`, cause "fall", `deathSeq = ++deathSeq_`
    and books nothing in `rises_`.
12. **Sleep, precisely**: a stepped creature sleeps when it did not move, has
    no target, no threat, no landing pending, no stroke (D2b), no goal it can
    walk (a routine that is active and not holding -- ignored for an ANCHORED
    or immobile body), and no stranger of another faction is within the
    longer of the two keep ranges (+16) where either side is hostile. A
    wanderer resting between legs sleeps until `wanderPauseUntil`. Woken by a
    distant mover or a NEAR creature of the band entering its 3x3 cells
    ("entering" = a cell of the new neighbourhood that was not in the old
    one), or its clock. A demotion arrives awake.
13. **Near creatures in a distant creature's world**: the near actor list
    entries that are alive, not a player (`IsPlayerActorId`), not loose flesh,
    and within 192 voxels of the window boundary; then within the stepper's
    own keep / scent reach. Distant corpses are not actors (D2b may add
    carrion).
14. **Cost** (`distant-sleep`): 200 asleep cost 0.195 us a tick (the summary
    fast path: no creature is looked at); an AWAKE step averaged ~50 us in
    that gate (Think over a dense same-faction grid, the sleep test's 9x9-cell
    reach for the library's longest keep range, 216). 128 engaged at that
    rate would be ~6.4 ms against §2.11's 1.5 ms target: D2b should measure
    its own (engaged creatures skip the sleep test), and the work pool is the
    lever.
15. **The `--shot` frame of a distant patrol is not made**: `RunShots` draws
    the world only, no bodies (D4a's note), so no shot path can see a statue.
    D4a's `distant-look` writes its own BMPs from a gate; a patrol view would
    be the same machinery and is left to whoever wants the look check.
16. Gates drive the tier directly where the window cannot hold the ground
    (the `peaks` walk, the crater after a manual shift, the water arm):
    `DIRECT PHASE CALLS ON PURPOSE (W2-O)` with the reason in the file
    header; the promotion-line / snap and sleep arms run THE tick.

**Measured** (`--verify distant-walk,distant-sleep,distant-roundtrip,mob-park,
mob-cap64-twice`, all PASS, 23.8 s): `peaks` walk 1,123 voxels in 1,154
ticks (1,118 progress, heading 0 deg, y 9,129..9,182, 27 replans, 0 refusals,
on DistantGround every tick), twice-run digests equal; anchored origin
bit-identical over 200 ticks; landings: 2 m soft (no damage), 13.4 m/s billed
20 hp exactly (lround of the formula), 40 m/s splat dead of "fall"; promotion
line deepest 0.58 voxels, snap 3.70 voxels up onto the stone bump (bound 48),
hp drift 0, falls 0; crater dip 6.0 voxels, 1 eviction drained by the reader,
both runs' traces identical (run 2 after the harvest, cache cleared); shark 105
water refusals, 0 dry ticks; swimmer 222 swim steps at <= the water speed;
200 asleep after 4 ticks; 40 of 40 sleepers in reach woken, none outside.

**Open.** Trees, buildings and ruins are still not obstacles (the plan's v1
gap; the `sitePadAt` mask is the cheap fix if it shows). `.svedit` layers and
dungeon floors are D1b. Ticket batches and the shoreline disagreement (2, 4).
Distant villagers do not walk schedules (D5) and promotion keeps D0's
`catchUp = true` -- D5 must turn it off for creatures that walked
(`movedWhileDistant` is the flag to read). The awake step cost (14).

### D2b (branch `dnpc-d2b`): abstract combat in the tick

**What landed.** Distant creatures fight. `MobSystem::TickDistant` (distant.cpp)
starts swings and draws in phase 1 (writing only the stepper) and resolves
them in an **impact pass** between phase 1 and the sleep test: every swing
whose impact tick it is resolves against its defender's tick-start snapshot
(`distant::ResolveStrike`), every due bow looses (`ResolveArrow` -> a
`DistantArrowFlight` in `DistantMobs::Arrows()`), every arrow due lands; the
outcomes are then APPLIED in attacker-id order (swings and looses), then the
arrows in loose order, then the statuses (`StepBleed` / `StepBurn` /
`StepStumps`), then the grips. The kit, the vitals, the hands, the bow and
the sidearm are cached on `DistantMob` at demotion (`DemoteToDistant`, before
`ReleaseRig`); the calibration loads with the attack styles
(`MobSystem::SetAttackStyles`, so the load, the selftest boot and the R reload
all read it). Statues strike while a swing is in flight; arrows in the air are
drawn on their arc (`AppendDistantArrowBodies`, one line in `main.cpp`). Gates
`distant-fight` and `distant-calibrate` (`selftest_distant_calib.cpp`, kOrder
after `distant-sleep`), the `distant` block of `build/last_run.json` gains the
fight counters and the per-phase cost.

**What D3 hooks into.**
- *Distant hits near*: the impact pass's `strokeNoTarget` branch (target id
  not in the tier) is where a swing at a near creature of the edge band lands
  -- build the defender's kit with `distant::BuildDistantKit(mob)` (it works on
  any live Mob), roll `ResolveStrike`, apply through the near creature's own
  door. Same for `arrowsLost` (an arrow whose target crossed the edge: inject
  a real `ArrowFlight` instead).
- *Near hits distant*: a public "blow pending on distant id X" queue drained
  at the top of the impact pass, applied in the same id order, keeps the
  order-independence (the resolver is pure; only the apply order matters).
- *Snapshots*: `DistantSnap` + `actorAt` (the `ai::Actor` of each living
  distant creature, built once a tick from its tick-start state, with reach,
  bleeding and the swing's Windup / Cut / Recover) are local to `TickDistant`
  today; the edge band's near-side actor list wants exactly those, published
  before the near creature loop.

**Deviations and decisions.**
1. **Severs, stumps and the hit-size tail were added to the model** (they
   decide full-fidelity fights). D2a's log showed no heavy tail in a 300-tick
   sample, so a run with attribution (per-stroke severed limbs, per-bout death
   causes and limbs off) was taken: near, a cut hit on an arm takes a limb of
   the arm off 18% of the time, a leg 26%, a head 4% (decapitation); survivors
   carried 5-11 limbs off; organs (brain, heart) give head / torso hits a tail
   to 11-34x the mean. So the table gained `spread` (the hit-size inverse CDF
   per role, normalised by the style's mean) and `sever` (P(the struck limb
   comes off | kind, role), unarmoured; a shell scales it), the stroke log a
   severed mask, the resolver a `sever` outcome and `SeverLimb` (the subtree
   off, an open stump, the gout's owed blood; a head off is a death), and
   `StepBleed` an open-stump drain. The stump law (one clump per drip period,
   `gore.*`, 0.15 hp/tick, 24 hp owed) is the tuning's, copied at load; a stump
   probe in the harness measured 0.12-0.15 hp/tick, confirming it.
2. **D2a's matchups were measured under a throttled bleed.** Near drips go
   through a shared pot (`gore.bleedOpsPerTick`, 6 a tick per MobSystem,
   taken in turns): the matrix's ~30 bleeding creatures each bled at about a
   sixth of a lone fight's rate. The matrix now lifts the pot to its ceiling
   (64) for its duration -- the distant tier's bleeding is a creature's own --
   and the table was regenerated with **16 rounds** (`distantCalib.calibRounds`
   8 -> 16, ~11 min). The reference changed shape: full-fidelity bouts are
   mostly DECIDED now (armoured 15/16 at 1,145 ticks, mace-vs-plate 16/16 at
   920, the zombie dies 15/16 at 769, the 4v3 5/16), so D2a's "most bouts end
   undecided" was largely the pot. Arrow `damageFrac` moved 1.59 -> 2.58 in the
   new table (the strike tick's TotalHp drop; not investigated further).
3. **A ruined limb is still in the way** (`DistantKit::absorbRuined`, set on
   every kit the tier builds): near, an arm at hp 0 hangs there and the next
   blow aimed at it lands on it (`limb.hp -= amount`, which is what the log's
   `meanPower` / `roleWeights` were measured from); D2a's resolver moved such
   a blow onto the trunk. Limb hp may go negative, as near.
4. **The impact tick is the table's measured one** (mean stroke start ->
   first contact, which already carries the tempo jitter and the injured
   arms), the authored windup + half cut where a style has no row; the arm is
   busy until the authored end. A request on a busy arm, or one no style
   reaches, is dropped (`strokesRefused`), as `BeginStroke` drops it.
5. **Grips** (`melee.injuredArmDrop`): a weapon falls from a hand whose arm
   chain is below 15% (and above 0), and with the chain severed the weapon and
   the fist both go; the held styles re-arm with the other hand's weapon, else
   a fist (its reach the chain's live bone length, `StyleReachOn`'s fist term),
   else they go ("none of its styles is usable"). An archer out of arrows
   draws its sheathed sidearm (reach estimated: a quarter arm + the item's
   `reach`). The record keeps the item: a promotion's first `GripFails` drops
   it again.
6. **Bleed / burn across the edge: no `kSaveVersion` bump.** Promotion reads
   the `DistantMob` directly: `ApplyDistantOverlay` re-opens the owed blood
   (rate / (1 - decay)) on the limb the last cut landed on and severs, for
   real, every limb lost at distance; `PromoteDistant` re-lights 40 voxels of
   the biggest torso limb (`IgniteLimb`) on a creature still burning. A retire
   or a save loses bleed and burn exactly as a near park / save does (the
   record never carried them), and loses severs at distance (the record's limb
   comes back hanging at its hp) -- a known gap.
7. **An open stump does not keep a creature awake** (the brief said "awake
   while bleeding"): a stump bleeds until death -- thousands of ticks for a
   survivor -- so a creature whose only status is stumps sleeps with a clock
   at the tick it bleeds out (`StumpTicksLeft`), and whatever wakes it first
   pays the ticks it slept through in one sum (`StepStumps`: exact, since a
   constant drain spent proportionally leaves every limb at the same share).
   Decaying bleeds and burns keep it awake (they end in ~700-800 ticks).
8. **Sleep ignores sub-0.05-voxel jitter** (`kDistantSleepJitterVox`): two idle
   survivors inside each other's personal space were nudged ~1e-4 voxels a
   stride by the spacing push against ground that refused most of it, and
   "it moved" kept both awake for ever (a latent D1 case the fight exposed).
9. **Actor lists are capped at the 24 nearest** (+ itself; its target always
   kept), in id order: Think walks the list several times a step and a dense
   battle is otherwise O(N^2). **Neighbourhoods are cached per (cell, reach)
   per tick** (the hash does not change until phase 3).
10. **A reach of 0 at demotion takes the profile's floor**: `StyleReachOn`
    reads the live pose and answers 0 for a fist (1.2 for a mace) on a rig
    that has never been posed, and 0 means "unlimited" to the draw and the
    impact test. The gates pose their creatures 4 real ticks before demoting
    them (`MusterDistant`).
11. **Arrows**: resolved at the loose (hit, limb, hp: against the target's
    tick-start state), applied at the impact tick if the target is still a
    living distant creature; no spread and no sever on an arrow (pierce has
    no strokes in the log). The draw: the bow's windup + pull x the profile's
    `draw` + hold + jitter (`kSaltDistantArrow`, draw index 8).
12. **Deaths**: cause by the near names ("vital limb destroyed", "blood
    loss", "burnt past the death knot"), `deathSeq` from the near counter,
    asleep forever, the statue lying at its feet's height; nothing booked in
    `rises_` (infection is not modelled at this tier). A creature that walked
    and then died is snapped onto the real ground at promotion like the
    living (`DistantPromotionChunks` waits on its chunks).
13. **`distant-calibrate` compares against the table's rows** (it does not
    re-run full fidelity): win rate within 0.3 or two standard errors of the
    difference of the two proportions; TTK within +/-25% or two standard errors
    of the full-fidelity mean (with 16 bouts its SE is still 60-145 ticks);
    hp left within 0.2. `distantCalibrate.knownDivergent` lists the matchups
    measured and not asserted, with the reason in `baseline.json`.
14. `distant-roundtrip` zeroes the bleed rates right after its demotions:
    its creature [2] is carved before it leaves and would now bleed at
    distance, which is not what its byte-for-byte round trip is about.
15. Not done: severed limbs do not show on the statue (the look is baked at
    demotion); DESIGN.md's full section stays D3's.

**Measured** (`--verify distant-fight,distant-calibrate,distant-walk,
distant-sleep,distant-roundtrip,mob-cap64-twice,distant-calib-matrix,mob-park`:
all pass, 66.6 s; the cost quoted below from one `SANDVOX_RUN_EXCLUSIVE=1`
run of `distant-fight`).

`distant-fight`: four plated swordsmen and an archer against five unarmoured
swordsmen: decided at tick 1,022 (5 dead), the tier asleep at 1,238; 130
swings (83 hit, 12 blocked), 12 arrows (8 hit); twice-run digests equal; the 5
corpses promoted dead under their ids, cause kept, no voice, no fall billed,
lying with their lowest limb centre within 1.15 voxels of the ground.

`distant-calibrate` (32 seeds at distance vs 16 full-fidelity bouts):

| matchup | win (distant / full) | TTK ticks (distant / full +/- se) | hp left A, B (distant / full) | |
|---|---|---|---|---|
| armoured vs unarmoured | A 0.81 / 0.94 | 1,299 / 1,145 +/- 103 (1.13) | 0.99 / 1.00, 0.04 / 0.01 | agrees |
| mace vs plate | B 0.97 / 1.00 | 1,144 / 920 +/- 72 (1.24) | 0.00 / 0.00, 0.99 / 1.00 | agrees |
| sword vs zombie | B 1.00 / 0.94 | 957 / 769 +/- 93 (1.25) | 0.00 / 0.01, 0.97 / 0.90 | agrees (TTK at the band's edge) |
| 4v3 swords | A 0.41 / 0.31 | 1,225 / 1,387 +/- 60 (0.88) | 0.56 / 0.27, 0.13 / 0.24 | known divergent (the 4-side's hp) |
| archer vs melee | A 0.75 / 0.18 | 215 / 147 +/- 22 | 0.72 / 0.99, 0.17 / 0.42 | known divergent |

The archer gap is MOVEMENT, not the hit model: at distance the duelist closes
from 55 to ~10 voxels inside 60 ticks and the archer, holding its facing
through each draw, keeps loosing point-blank, where the full-fidelity archer
keeps its range (its shots' distance bins average 19 and 53 voxels) and is
hardly touched. The 4v3: win rate and TTK agree, the 4-side keeps too much
hp -- the leading hypothesis is frontage (near bodies block each other).

**Cost (section 2.11)**, `distant-fight` arm C, 64 a side in two 16 x 4 blocks,
~123 engaged steps a tick: **0.69 ms a tick** (5.6 us a step; exclusive run)
against the 1.5 ms target -- setup 10, self + ground fan 77, the actor list
358, Think 115, steer + spacing + drive 114, the impact pass + statuses 12,
sleep 1, file 5 us a tick. The first measurement (64 a side in 8 x 8 blocks, ~64 engaged a
tick) was 795 us a tick = 10 us a step, of which the actor list 5.9 and the
drive + spacing 4.2 us a step: walking, sorting and id-searching one
neighbourhood per creature. The per-cell cache took those to 3.2 and 1.0.
Non-exclusive runs of the same binary on the shared machine read 740-1,255
us a tick; the gate asserts only a 3,000 us regression ceiling. Not needed for the
target, so not done: the work pool (phase 1 writes only the stepper but
`DistantGround`'s caches and the lazy snapshots are shared state), and the
actor list's remaining cost (the lazy snapshot build of ~128 neighbours a
tick, ~half of it).

**Open.** The two known divergences above. Arrow damage (`damageFrac` 2.58)
and pierce bleeding are flat (no spread, no sever). Severs are lost on a
retire / save. Statues keep severed limbs. Infection (zombie rot, venom) is
not modelled at distance.

### D3 (branch `dnpc-d3`): the edge, and the flip

**What landed.** `distant.enabled` is ON by default (tuning row, `tuning.json`,
schema text, `tuning_params.js` regenerated). The edge (§2.7), all in the tick:

- **The shared band.** `MobSystem::PublishDistantBand` (distant.cpp) builds the
  near tier's view of the distant creatures within `kDistantEdgeBandVox` (192,
  moved to distant.h) of the window BOX -- 0 inside it, so one held at the
  promotion line is in -- from the tier's tick-start state, BEFORE `TickDistant`
  steps; PreTick appends it to `actors_` AFTER `TickDistant` (which must read
  the near creatures only). At most `kDistantBandMax` (128), nearest first,
  ties by id, then id order; membership cached against the tier's new MOVE
  GENERATION (`DistantMobs::MoveGen`, bumped by Insert / Erase / Take / Clear /
  Relocate and a death) and the window, so a still tier costs a walk of the
  members. The snapshot is ONE function now (`FillDistantSnap` /
  `DistantActorOf`), used by the distant lists and the published band alike.
  The other direction is D1's band, unchanged, plus the witness:
  `Stats::playerActorsSeen` counts (and drops) a player-band id at the point a
  distant actor list is built.
- **Near hits distant.** `NpcStroke::edgeQueued` + a hook at the top of
  `StepStroke`: a stroke whose target is not a near creature but a living
  distant one queues `QueueEdgeBlow` at its FIRST CUTTING TICK (the blade still
  sweeps for real). The queue (`NearEdgeBlow`: the attacker's kit at the cut,
  `BuildDistantKit(..., withBody = false)`, the stroke's style matched by form
  or base or name) is drained at the top of the next impact pass in
  (attacker id, tick) order and rolled with `ResolveStrike` against the
  defender's tick-start state; hits join the distant blows' apply.
- **Distant hits near.** In the impact pass, a swing whose target is a near
  creature of the band is rolled against that creature's LIVE body -- kit +
  base-limb hp read once per defender per tick, before anything applies
  (`absorbRuined`, as near) -- and a hit goes through `ApplyEdgeBlow`
  (melee.h/.cpp): contact = a ray from 24 voxels out on the attacker's side at
  THAT limb's collider, stepping past anything else on the line (8 casts
  max; the limb centre if none reaches it); the sweep's `WornShellAlong`
  cover retarget (cloth passed by an edge); `BuildStrikeParts` (a "thrust"
  style is a stab along the travel, any other swing a cut across it; the
  held item's edge length / half width / heft / carve bonus, a natural
  weapon's fist floor) + `ResolveOnCreature`; a `StrikeEvent` for the audio.
  No coat exchange (no attacker lattice). Applied after the distant blows,
  in attacker-id order. `TickDistant` gained `spawns` (null in a CPU arm:
  rolled, counted `edgeFarUnapplied`).
- **Arrows both ways.** `ArrowFlight` carries an `ArrowShot` (shooter,
  target) and its loose point; the NPC archers fill it. *Out*: a flight that
  leaves the window (mid-step, or the window moved off it) at a living distant
  creature is handed over (`QueueEdgeArrowOut`, an `ArrowEvent::PastEdge`) and
  rolled at the next impact pass by `ResolveArrow` over the whole shot's
  distance, landing after the flight time left (exit -> target at its exit
  speed). A near archer's draw tracks a distant target through
  `MobSystem::EdgeActor` (the published entry). *In*: a distant archer's loose
  at a near creature of the band is a REAL flight -- aimed as `mob_archery`
  aims (shoulder, aimRaise, `LeadPoint`, `ArrowHoldover`, the profile's spread
  x the brain's aim tune on `kSaltDistantArrow`), flown by
  `ArrowStepOutside` (arrows.cpp: ArrowFlights' own integrator, half-voxel
  samples, DistantGround as the ground), queued at the first in-window sample
  (`TakeEdgeArrowsIn`) and injected by `MobArcheryTick` with
  `ArrowFlights::Inject` at that exact fixed-point state. *Converted*: an
  abstract arrow whose target is a living NEAR creature (promoted mid-flight;
  D2b's `arrowsLost`) becomes an inbound flight from its arc's state that
  tick. Inbound arrows are drawn at their real state.
- **The thrash bound.** `kDistantMaxFlips` (4) per `kDistantFlipWindowTicks`
  (600), constexpr in distant.h. Demotion cannot wait, so Unpark's promotion
  pass gets a fourth wait (`DistantFlipBudgetOk`: fewer than max - 1 flips in
  the window, room kept for the demotion after; `Stats::flipWaits`); a waiting
  creature is held at the line. Flips are logged per id in
  `MobSystem::tierFlips_` (the last four ticks; trimmed; cleared by Reset);
  `PromoteDistant` takes the tick.
- **Gates.** `distant-edge` (new, `selftest_distant_calib.cpp`, kOrder after
  `distant-calibrate`); `DistantSwitchScope` (support.h) pins the switch for a
  scope. Per-gate decisions below. `last_run.json`'s `distant` block gains the
  edge counters.

**The flip, gate by gate** (every gate that binds a park function; the rest
cannot demote anyone -- the demote branch is inside `if (parkFn_)`):

| Gate | Decision | Why |
|---|---|---|
| `mob-park` | arms A-F PINNED OFF; G (switch on) unchanged | A-F are the park path (record bytes in the bucket, unpark budget/cap/ground); the ~90 m teleport is inside the radius, so with the tier on they would demote |
| `corpse-save` | PINNED OFF | P is the corpse's MOBS v6 record and its rising through park/unpark; the distant corpse round trip is `distant-roundtrip`'s |
| `npc-catchup` | PINNED OFF | the PARKED record's schedule catch-up (D5's note: "catch-up remains the path for a record that was never distant"); distant villagers walking schedules is `distant-schedule` (D5) |
| `refs-npc-identity` | A-E pinned off; NEW arm C2 with the switch ON | C keeps the park-path claim; C2: the villager demotes (distant, nothing parked), the ref goes dormant, and on return the ref does not spawn a duplicate and the PROMOTION brings back the same mob id |
| `item-park` | sees the tier (unpinned) | its claims are ground items (ITMS); the headless corpse now demotes instead of parking, which it does not assert on |
| `distant-*` | unchanged | each sets the tier explicitly |
| `dungeon-entities`, `demon-contract`, `demon-malice`, `mob-handoff` | unchanged, by construction | none binds a park function, so nothing they spawn can become distant; their `MobAt` counts and `!IsAlive`-as-gone still mean what they meant. A future arm that binds parking must count with `LocateCreature` / `LocateByRef` |

**Deviations.**
1. **A near creature does not walk out of the window.** Measured: a duelist
   on a Goto 40 voxels past the -Z face stopped with its centre one voxel
   outside -- the near drive refuses steps onto ground it cannot sense (the
   fetch cache has nothing past the face). So near -> distant happens when the
   WINDOW moves (or a body is thrown), not by walking; a near/distant pair
   meets across the face and stays split. §2.7's "a near creature chasing a
   distant one out of the window simply becomes distant" holds only for a
   window move. `distant-edge` T drives the outward crossing through the park
   rule's own door (`DemoteForTest`) for that reason.
2. **The near band is published, not shared in place**: the plan said
   "PreTick's actor list gets the distant creatures appended"; they are
   appended AFTER TickDistant so the distant tick cannot read the tier back
   out of the near list, and built BEFORE it so both read tick-start state.
3. **Near hits distant lands a tick late**: queued during tick T's near
   loop, rolled at T+1's impact pass (the distant tick runs before the near
   loop). Deterministic; the roll keys on the stroke's own cut tick.
4. **Distant hits near is priced by the near model**: the roll decides hit,
   limb and size; the size enters as the sweep's power = (roll hp / shell
   factor) / strike total, clamped 0.15..1. Measured in the gate: the roll
   said 120.5 hp over 7 hits, the near door charged 112.6. The roll's `sever`
   is ignored for a near body (its lattice decides). A `Blocked` outcome does
   nothing to the near defender (no item wear, no flinch).
5. **Inbound arrows are aimed, not rolled**: the near tier's physics is the
   higher fidelity, so the distant archer's shot is a near archer's shot (aim,
   lead, holdover, spread) and the table's hit rate is not used. One
   documented skew: the injected flight steps a full tick from the boundary
   sample on the tick it enters (< 1 tick of extra travel).
6. **The thrash bound is a constexpr, not a tuning row** (an engine bound like
   `kUnparkMarginChunks`); `distantMaxFlips` in baseline.json is the gate's
   claim about it.
7. **Fixture finding (not fixed)**: a standing dummy's body box centre
   (`MobBodyBox`, what archers aim at -- `BoxCentre`, the actor list's centre)
   sat 1.3 voxels off its pelvis; a ray through the box middle along +x met no
   collider. `distant-edge` C aims at the root limb's centre.

**Measured** (`--verify distant-edge, distant-fight, distant-calibrate,
distant-walk, distant-sleep, distant-roundtrip, distant-look, mob-park,
mob-save-delta, mob-handoff, npc-catchup, refs-npc-identity, dungeon-entities,
demon-contract, demon-malice, mob-cap64-twice, corpse-save, item-park, archer,
arrow, duel, npc-strike`: all 22 PASS, 102.7 s; `distant-edge` 15.5 s alone
after its last fixture change). `distant-edge`, both runs identical (digest
3ab6af95ccb29304):
M: 15 near strokes queued at distant, 13 hit (distant hp 1,107 -> 81); 35
distant swings at near creatures, 7 hit, 7 applied through the door (0 ray
misses, 0 covered), near hp 1,107 -> 823, 6 limbs carved, 107.7 Blade hp,
310.7 voxels of blood. A: 6 inbound looses, all injected, 7 PierceAlongRay
hits on the near dummy, 0 grounded. C: 1 converted, 2 hits. B: 4 flights
handed out, 4 resolved, the distant dummy killed. T: 7 flips (ticks 5, 69, 84,
605, 620, 684, 699), at most 4 in 600, 610 flip-wait ticks. P: 0 player ids in
any distant list over the whole verify run, the zombie awake and on its
dummy all 900 ticks. `distant-fight` C unchanged: 668 us/tick for 128 engaged.

**For D5+D6.** Promotion still passes `catchUp = true`; turn it off for a
creature that walked (`movedWhileDistant`). Activation (parked -> distant) is
not a tier flip in `tierFlips_` -- decide whether it should be. `npc-catchup`
pins the switch off; `refs-npc-identity` C2 is the identity template for a
distant villager. Saves still drop bleed, burn and distant severs.
**For D8.** An overflow swap's promotion goes through Unpark, so the flip
bound applies to it -- keep it, or a full cap thrashes. In-window distant
creatures are published to the near band (box distance 0) and keep the
never-a-player rule (it is enforced where the list is built, not by
geometry). Near creatures do not walk out (deviation 1), so overflow
demotions inside the window are the only near -> distant path besides the
window.
**For D9.** `ArrowShot` is the hook: the player's bow (session.cpp phase G)
launches with it empty, so its flights still expire at the face.
`QueueEdgeArrowOut` needs a TARGET id (the roll is per defender) -- D9 must
pick one (e.g. the distant creature nearest the flight's line at the exit)
and a miss must land on DistantGround.

**Open.** Near casters can pick a distant target from the band and their casts
are refused (no abstract spell hit). The player's own melee cannot reach a
statue. A distant `Blocked` on a near shield does no item wear. The published
band's cost was not measured on its own (a walk of at most 128 members a
tick, the tier only when somebody moved). Ghosts (MP) are in the near band
but a distant blow on one is counted as no target (owned elsewhere) -- D7.

### D5+D6 (branch `dnpc-d56`): activation, schedules and load

**What landed.** Parked creatures inside the radius come back as distant ones,
distant villagers walk their days, and what happens at distance survives a
retire and a save.

- **Activation** (`MobSystem::ActivateParked`, distant.cpp; one call at the top
  of `EntityParking::Unpark`, so it runs in phase H in the same place and order
  as promotion and unparking). A dormant `'MOBS'` record BEYOND THE PARK PAD
  (more than 16 voxels outside the window box: where a leaving creature would
  demote, not the band a parked record waits in) and within `distant.radius`
  of the window centre (the record's origin) becomes a `DistantMob`: a fresh id
  (`nextId_++` in service order; records carry none), the record kept as the
  bridge, built by `MobSystem::DistantFromRecord` (below), then held to the
  same identity proof a demotion is (the record patched with the overlay just
  built must give back its own bytes, statuses included) and inserted awake (a
  corpse asleep). Served NEAREST FIRST (squared distance to the window centre),
  ties by region (the sorted region list) then record order; at most
  `kActivatePerCall` (12) a call, at most `kActivateDefBuildsPerCall` (1) whose
  def must be built (a pool body reads files); never the tier's last
  `kActivateReserve` (64) slots -- the near crowd's, whose creatures hold an
  identity and a mind a record has already lost.
- **Bounded (rule 2).** A scan runs only when the window moved, the store's
  new `EntityEpoch()` moved (Clear / Unbind / BindLoad / BindSave /
  RemoveAllEntityFiles: a LOAD), this system parked something since
  (`parkedTotal_`), the radius changed, the last call left work (budget, a def
  build), room freed after a scan that ran out of it, or every
  `kActivateStrideTicks` (600) as a backstop. It walks only the regions the
  store knows inside the radius's box: `ChunkStore::EntityRegionsAny` = the RAM
  buckets plus the `r_*.sve` files under the bound dir, listed ONCE per epoch
  (a 128 m radius spans ~1,300 regions; probing each file to learn it is absent
  is what this avoids). Measured over the whole verify run (14 gates): 18 scans.
- **Load = activation, no special path.** After `LoadWorld` the records outside
  the window are dormant (as before); the epoch moved, so the next Unpark scans
  and brings them back as distant. `distant-save` proves it with one Unpark.
- **Schedules at distance** (`world/refs_npc.cpp`). `StepResident` became
  `StepResidentBody` over a `ResidentBody` view (id, near Mob or null, foot,
  brain): ONE resident step for both tiers, one `Resident` per ref (the mob id
  survives the edge, so a villager mid-route keeps its route across it). A
  distant body opens no door (`DoorWait` becomes `Travel`, a leg's
  `doorBefore` is skipped -- DistantGround has no doors), strikes no pose
  (`SetActivity` is a rig's), and `CheckCloseBehind` waits for a near body.
  `StepDistantResidents` (the npc kind's tick, after the near loop) steps every
  living distant creature carrying an npc ref, in the tier's id order, ref
  dormant or not; at its anchor it HOLDS (`routine.hold`), so the tier's sleep
  test sleeps it; a patrol's pause sets `SetDistantRoutineClock` (a new
  `DistantMob::routineWakeTick` the sleep test honours); a SLEEPER is woken
  (`WakeDistant`) only when the schedule clock reaches a new minute and its row
  index (or the refs / schedules revision) changed. Awake ones are stepped on
  the tier's idle stride (every tick when engaged), the tick the tier thinks
  with the routine. With everything asleep and the minute unchanged the pass
  is three compares.
- **Promotion: catch-up only for the never-moved.** `PromoteDistant` passes
  `catchUp = !movedWhileDistant`; counted when the placer moved it
  (`promoteCatchUps`). `npc-catchup` (switch pinned off, the parked record's
  path) passes unchanged.
- **Persistence: MOBS v13** (`kSaveVersion` 12 -> 13; 12 loads). A STATUS TAIL
  as every record's LAST field: u32 flag, then (flag 1) bleed rate f32, last
  wound i32, burn ticks u32, burnt fraction f32, open stumps u32. Only the tier
  writes a 1 (`PatchMobRecord` from the overlay, which gained `limbAttached`
  and the five statuses); a near `SaveOne` writes 0. Limbs severed at distance
  need no field: `PatchMobRecord` rewrites a base limb the overlay calls
  detached as `kLimbSevered` (its lattice block dropped -- the form a maimed
  near creature saves in; a severed limb overlaid attached is refused).
  `LoadOne` applies a set tail to the living (`ApplyRecordStatus`: the owed
  blood through `Mob::ReopenOwedBlood` -- D2b's block, lifted out of
  `ApplyDistantOverlay` -- and the trunk re-lit through `RelightTrunk`, lifted
  out of `PromoteDistant`). A parked v12 record activates too: the tail is the
  last field, so v12 + a 0 flag IS v13 (`UpgradeRecordToCurrent`, proven by a
  parse). The parking / save consumers all name `kSaveVersion`, so the bump
  moved them together.

**Deviations and decisions.**
1. **The status tail is applied by `LoadOne`, for promotions too.** D2b
   re-opened the bleed in `ApplyDistantOverlay` and re-lit in `PromoteDistant`
   from the `DistantMob`; now the promotion's patched record carries them and
   the ONE door (`LoadOne`) applies them for a promotion, an unpark of a
   retired creature and a load. `relit` / `rebled` now count all three.
2. **Severs at promotion take the record path.** D2b cut limbs lost at
   distance off "for real" at promotion (`Sever`: the limb dropped as debris at
   the PROMOTION spot, a gout, `DrainBlood` billing hp a second time). Now the
   patched record says `kLimbSevered` and the load detaches it (stump reopened,
   nothing dropped, no second bill) -- the arm came off far away.
   `ApplyDistantOverlay`'s sever pass stays as a safety net and finds nothing.
   A held item in a hand the record severed (its gear still lists it) is
   unequipped at the end of `OverlayMobRecord` (a near record never lists one:
   `CaptureGear` skips a bodyless held slot).
3. **The demotion's identity probe leaves the statuses out** (a near SaveOne
   writes no status; the overlay now carries what `CaptureDistantVitals`
   read). `DistantRecord(d, out, withStatus)`; `distant-roundtrip` A's patch
   check asks `withStatus = false` -- the record-held fields, D0's claim.
4. **Activation is NOT a tier flip.** The thrash bound is about the window's
   edge (near <-> distant); a parked <-> distant cycle needs the window to move
   a whole hysteresis (256), and counting it would make an activated creature's
   first promotion wait.
5. **Corpses activate too**, as dead statues (cheap: no kit, asleep forever),
   resting on their lowest attached limb's transform. A player's corpse stays
   parked (its id band is LoadOne's to renumber).
6. **The record-built kit** (`DistantFromRecord`; what is exact and what is
   approximated is listed at the top of the activation code in distant.cpp).
   Exact: hp, severs, roles, vitality, the brain's five facts, statuses, what
   each hand holds, the quiver's arrows, the def's terrain budgets
   (`Mob::TerrainBudgetsOf` / `RestLegSpanOf`, lifted out of `BuildRig`),
   natural weapons' usability (`Mob::ChainForEffector`, now static and
   public, over the def's skeleton). Approximated: REACH on the rest pose
   (bones are rigid; the held-weapon term a quarter arm + the edge; the lunge
   at lungeScale 1 through `MobSystem::LungeClosesVox`, lifted out of
   `StyleReachOn`) -- measured 9.25 voxels for the duelist's sword style; the
   SHELL hardness from the shell's stored lattice or the item's cover (no fit
   resample); a GAUNTLET by its cover's limb; the STATUE is the def's bare look
   (`DistantDraw::KeyOfDef`, baked on demand by the producer). The grip rule
   (`ApplyGripRule`, lifted out of the impact pass) runs once on the result, so
   a hand cut off at distance lets go of what the gear says it held.
7. **Left parked** (counted `activateKept`): a record older than v12, one whose
   material table needs a remap, a player's corpse, one whose def no longer
   resolves or no longer fits it. Its unpark is the path that remaps and reads
   old versions. A record whose ref already has a body in either tier is
   DROPPED (`activateDropped`, logged: LoadOne's own rule).
8. **At distance a villager stands at wander and socialize rows** (no milling
   about the anchor, no turning to neighbours): what keeps a village 100 m
   away asleep. Patrols still walk their routes.
9. `MobSystem::MobRecord` moved from mob.cpp into mob.h (the activation reads
   one in distant.cpp); `distant::StyleTimingOf` is public.
10. Test seam `MobSystem::SeverDistantForTest` (the impact pass's own sever +
    grip rule) for `distant-save`.

**Measured** (`--verify distant-schedule, distant-save, distant-roundtrip,
distant-walk, distant-fight, npc-catchup, refs-npc-identity, mob-park,
mob-save-delta, corpse-save, mob-handoff, item-park, save-entities,
save-material-remap`: all 14 PASS on the first run, 99.8 s).
`distant-schedule` (2.0 s): two villagers parked ~90 m off with the switch off,
both activated 1 tick after it went on, buckets emptied; V1 walked 59.8 voxels
A -> B while distant (largest single-tick step 2.10, its ref dormant), arrived
after 123 ticks and slept the same tick, 0 steps over the next 60; promoted
1.94 voxels from B with x / z bit-identical to where it walked (the placer not
asked); V2, never moved, promoted through the catch-up placer; twice-run
digests equal (b025aefef4d7a681). `distant-save` (13.2 s, most of it the full
flush below): 5 distant (a duelist; one with its sword hand cut off at distance
and bleeding; one burning; one in iron armour; a corpse), 5 records saved; after
the load ONE Unpark activated all 5 (1 dead), every one exact by (def, origin):
every limb's hp bits, severs, bleed rate, burn ticks and fraction, stumps,
last wound, hands, life, the duelist's style list; the maimed record loaded
straight near keeps its sever and re-bleeds, the burning one re-lights; a v12
record upgrades to the same v13 bytes. Run totals: 18 activation scans, 9
activated, 1 room wait (distant-roundtrip F's cap of 2), 0 kept / dropped.
`distant-fight` C on this tree: 851 us/tick for 128 engaged (non-exclusive).

**Open.**
- An activated creature draws as its def's bare statue (no gear, no damage)
  until it is next demoted from a rig.
- A weapon a hurt hand dropped at distance is back in the hand after a retire
  or a save (the record keeps it -- D2b's own note); arrows loosed at distance
  are back in the quiver the same way.
- `distant-save` pays a FULL store flush (13 s): its fixture regenerates the
  world without `Stream::OnRegen`; calling it first would make the save a
  delta (distant-roundtrip's note).
- Rule 2 has a witness (no steps while asleep, the activation scan count) but
  no microsecond number for a sleeping village; `distant-sleep`'s 0.2 us a tick
  covers the tier, and the refs pass is a few compares on the same ticks.
- An active npc ref whose villager is distant pays `Distant().FindByRef` (a
  walk of the tier) each tick in the near loop.
- For D8: activation never fills the tier's last 64 slots, and an overflow
  swap's demotion is a demotion (the reserve is for it). For D9: nothing here
  touches arrows.

### D8+D9 (branch `dnpc-d89`): cap overflow, and the player's reach

**What landed.** *D8*: `MobSystem::OverflowSwaps` (distant.cpp), called from
ONE statement at the top of `EntityParking::Unpark` (before its promotion
pass, with Unpark's budget and, as a lambda, its ground wait for a distant
creature -- the footprint + snap chunks); `MobSystem::PlayerDistance`;
`MobSystem::DemoteNow` (the park rule's demotion wherever a creature stands;
`DemoteForTest` now calls it, and `DemoteToDistant` takes the flip tick);
`DistantMob::overflow`; THE PLAYER'S RING in the drive; the window's own
ground (`DistantGround::SetWindowSource`); the distant tick's near band
widened to the whole near crowd while a distant creature stands in the window
(`bandInWindow_`, counted by `PublishDistantBand`). *D9*: the body boxes
(`MobSystem::DistantBoxes`, published with the band; `DistantBoxAlong`), the
in-window box test in `ArrowFlights::Tick`, `QueueEdgeArrowBoxHit`, the
player's flight past the face (`QueueEdgeShotOut` -> an OUTBOUND
`DistantArrowFlight`, stepped in the impact pass), `distant::
ResolveArrowContact` + `DistantLimb::at / placed` (captured by
`BuildDistantKit`), `ArrowFlights::Inject(..., ticksLeft)`, two `ArrowEvent`
kinds (`DistantHit`, `Continued`), the gates' logs (`OverflowLog`,
`DistantShotEvents`). Gates `distant-overflow` and `distant-arrow`
(`selftest_distant_calib.cpp`, kOrder after `distant-edge`);
`build/last_run.json`'s `distant` block gains `overflow*`, `boxHits*`,
`shotsOut*` and `edgeArrowsOutInWindow`. No tuning row added
(`overflowMinDistVox` is read now and left `check_invariants.py`'s consumer
allowlist). Shared signatures changed: `DemoteToDistant` (private, one
defaulted parameter) and `ArrowFlights::Inject` (one defaulted parameter);
`Unpark` gained one statement and nothing else.

**THE OVERFLOW RULE (D8).**
- *When*: only while the living cap BINDS -- more living distant creatures
  over the promotion line than free slots. Otherwise it costs one scan, and
  Unpark's own pass serves everyone in D0's window-centre order.
- *Who comes in*: the waiting, NEAREST TO THE PLAYER first (3D, body centre to
  the nearest LIVING player actor's centre; the window centre with none -- D0
  deviation 1 answered), ties by id, each under Unpark's own waits (the flip
  bound, then its ground). A free slot is filled first.
- *Who goes out* (only when no slot is free): the FARTHEST near creature from
  the player that is at least `distant.overflowMinDistVox` (150) away and
  farther than the waiting one by `kDistantOverflowMarginVox` (24, the
  hysteresis), with room in the flip bound -- `DistantFlipBudgetOk`, the
  predicate a promotion passes: an OPTIONAL flip always leaves room for one
  forced flip after it, so kDistantMaxFlips holds whatever order optional and
  forced flips come in. Never a ghost, a body a player drives, one ragdolled,
  airborne or holding a severed piece. One whose brain TARGETS A PLAYER is the
  last choice (the order is: not on a player, then the farthest, then id): it
  stops seeing the player the moment it is distant. At most
  `kDistantOverflowSwapsPerTick` (1) a tick -- each swap is a statue bake
  (3-10 ms in the game) and a LoadOne, and a battle's front moves slower than
  thirty creatures a second. The waiting are taken nearest first, so once no
  near creature qualifies for one, none can for the rest (the pass stops).
- *How an overflow creature lives*: a distant creature INSIDE the window. It
  walks on the window's own ground where the fetch cache holds the chunk
  (buildings, trees, edits; else D1's sources), fights through D3's edge (the
  published band already held in-window creatures, at box distance 0), is a
  statue, never perceives a player (the list rule is untouched), and is HELD
  AT THE PLAYER'S RING instead of at the promotion line: no step may END
  within `overflowMinDistVox` of a player AND nearer to that player than it
  started. It may walk and fight anywhere in the window but the middle;
  reaching the ring it waits there, and the swap takes the nearest first.
  Ending a step outside the promotion line clears the flag (an ordinary
  distant creature from there, held at the line if it comes back).

**Deviations.**
1. **Distance is 3D** (centre to centre) for the swap and the ring alike, so
   the ring is a sphere: on a slope "15 m" includes the height. The gate
   places its fixture by `PlayerDistance` for that reason.
2. **The ring replaces the promotion line for overflow creatures only.** The
   line's "never deeper" would freeze a creature demoted deep in the window
   where it stood. Creatures that walked in (held at the line) keep D1's rule.
3. **The window's ground is for overflow creatures only**, set around each
   one's step and cleared after it, so no D1-D3 behaviour moves. A fetched
   chunk is decoded once per cache VERSION; liquid is not ground (as D1's
   stored chunks).
4. **The near band widens while ANY distant creature is in the window** --
   one held at the promotion line included. In every D1-D3 gate the near
   creatures were within the old band anyway.
5. **An overflow creature promotes through the ordinary `PromoteDistant`**:
   one that never moved comes back byte for byte, one that walked is snapped
   from the fetch cache. `catchUp` is still true (D5's open item covers it).
6. **A body box is the near tier's** (`MobBodyBox`: the axis-aligned world box
   from the origin, worldSize up) -- what near archers already aim at -- not
   the creature's turned footprint. A shot through the box's empty corner is
   a hit: generous by the box/body area ratio, accepted at statue fidelity.
   Corpses publish no box (an arrow passes a lying statue).
7. **Contact is geometric, the pricing is the table's.** `ResolveArrowContact`
   rolls no hit: the limb is the live one whose centre (captured in the body's
   frame at demotion: x right, y up from the feet, z forward) lies nearest the
   arrow's LINE, its role that limb's (a kit with no positions falls back to
   the height on the body); the hp is ResolveArrow's (flat damage x
   `damageFrac`, measured per CONTACT, x the shell factor), the bleed the
   table's Pierce rate, no sever. Draw speed is not in it (the table's arrows
   were full draws). Its one draw (the fallback's limb) is `Hash3(shooter ^
   kSaltDistantArrow, tick, 12)`: the arrow salt with a new index rather than a
   new salt (ai_behavior.cpp untouched).
8. **In-window box hits land a tick late** (queued in phase K, rolled at the
   next impact pass against the defender's tick-start state): D3's
   near-hits-distant rule. Ties along one segment go to the lower id.
9. **The player's flight past the face is < 1 tick late** (the part of the
   tick past the face is not flown -- D3's inbound skew the other way round),
   handed over at the last in-window sample (or where the window left it). A
   flight that comes back INTO the window (shot high over the face) is
   injected as a real flight again with the life it has left; its quiver coat
   does not survive the stretch outside (a plain arrow comes back in).
10. **A D3 hand-over at a target INSIDE the window is refused**
    (`edgeArrowsOutInWindow`): its box was in the flight's path all along, so
    a flight that left missed it, and rolling it again past the face would be
    a second chance. That NPC flight expires at the face as before D9.
11. **The wall's stealth noise moved after the box test** (an arrow a statue
    stopped never reached the wall). With no distant box in the window, the
    order of everything in `ArrowFlights::Tick` is what it was.
12. **The arrow ends in the statue** (no embedded arrows at this tier; counted
    `boxHits` / `shotsOutHits`). A miss outside the window lands and ends
    (nothing out there can hold an item).

**Measured** (`--verify distant-overflow, distant-arrow, distant-edge,
distant-fight, distant-roundtrip, mob-cap64, mob-cap64-twice, archer, arrow,
poison-arrow, stealth`: all 11 PASS, 86.7 s; `build/last_run.json`).

`distant-overflow` (15.1 s; both runs a5b6dab76075581b). S: 8 swaps at ticks
+10..+17 (the ground waits first) -- the 4 blues (228-231 voxels from the
player) and the four farthest dummies (220-222) out, the 8 distant dummies
(81-118) in; after S the farthest near creature stands at 219.0 and the
nearest waiting one at 220.4. F: 11 swaps -- the 8 reds (183) at +130..+137
for dummies at 213-219, then two blues that fought their way in to 187-189
(+202, +222), and at +413 a red that had chased out to 219 traded for a blue
at 194: the front moving, the near side following it. Across the tiers 28
near strokes at in-window distant blues (13 hit) and 13 distant swings at
near reds (5 through the door). Never more than 64 living near; no overflow
creature inside the ring (none tried: 0 ring holds); 0 player ids in a list;
at most 2 flips in 600 ticks; 0 last resorts.

`distant-arrow` (both runs 3d7504a0e46a652c). O: both flights continued at
the face; the hit came 5 ticks after the loose where the open flight enters
the box at 4 (the hand-over's < 1-tick lag), on limb 0 for 90.2 hp (35 x
`damageFrac` 2.58, bare), and the dummy's hp was lower on that very tick
(1107 -> 961 by the end, the bleed after it). M: landed 6 ticks after the
loose, outside the window, 0.03 voxels above TerrainHeight + 1. I: the
in-window statue's box was met on the first tick and applied on the next
(1107 -> 973).

Unchanged where no distant creature is involved, or none in the window:
`distant-edge` reproduced D3's own digest (3ab6af95ccb29304) and every
number of its record; `distant-fight` A reproduced D2b's (decided at 1,022,
quiet at 1,238, 130 swings / 83 hit / 12 blocked, 12 arrows / 8 hit; C 975
us/tick for 128 engaged in a NON-exclusive run against D2b's exclusive 668);
`mob-cap64`, `mob-cap64-twice`, `archer`, `arrow`, `poison-arrow`, `stealth`
pass.

**Open.** One swap a tick: a front that jumps (a dozen waiting creatures
suddenly nearest) catches up over a dozen ticks. The ring is a sphere round
every LOCAL player actor (D7 decides what it means across peers). In-window
statues walk through near bodies and are walked through (no colliders; D1's
soft spacing between distant ones only). A near creature on a player is only
the last resort, but it does go when nothing else qualifies. The player's
melee still cannot reach a statue and near casters' casts at one are still
refused (D3's open items).

## 7. Audit 2026-10-15: what changed and why

The 2026-10-13 plan was audited against the code at `79f8f21` (157 commits
later). Its factual base held: the park rule, `TerrainHeight`'s contract,
`ai::Think`'s views, `RenderOnlyBody`, the sight ranges, the body-slot budget,
the spawner's 600-tick rule and the fixed fetch latency all checked out. Twelve
corrections, now written into the sections above:

| # | Severity | Correction | Where |
|---|---|---|---|
| A1 | critical | Ground was `TerrainHeight` for everyone, so dungeon, cave and upper-floor creatures would be teleported to the surface on demotion. Now SURFACE / ANCHORED at demotion. | §0, §1, §2.4 |
| A2 | critical | Saves iterate live mobs only, so the D3 flip would drop every distant creature from saves until D6. The save write moves into D0. | §2.10, §3 |
| A3 | determinism | `ChunkStore` contents arrive on a GPU-timed tick (non-blocking eviction harvest). A reader must drain the chunk's pending eviction first. | §0, §2.4, §2.12 |
| A4 | design | Promotion waits let statues walk into the window, a crowd of them when the cap is full. Waiting creatures are now held at the promotion line, served nearest-first. | §2.1, §2.5, §2.7 |
| A5 | design | The player is ~224 voxels from a distant creature in the worst case, not 256, and the window-bias companion idea would break the bound outright. Players are now excluded from distant actor lists by construction. | header, §0, §2.7, §5 |
| A6 | ordering | D3 flipped the switch before D4 drew anyone. Order is now D0 → D1 → D4 → D2 → D3, with D4a and D2a in parallel with D0. | §3 |
| A7 | budget | Statues cost pool WORDS (~66k per human at full res), not just slots. Downsampled, lazily baked per look key from live limbs, LRU under a word budget; `kMaxBodySlots` → 4096 in D4. | §0, §2.9 |
| A8 | design | Six humanoid zones do not fit snakes, sharks, piranhas or the naga. Per-limb hp (the near model's own numbers), exact round trip. | §2.2, §2.6 |
| A9 | test design | A symmetric 4v4's winner is a coin flip; the calibration source must log misses. Duel matrix + asymmetric matchups. | §2.6, §4 |
| A10 | feasibility | `MobSystem::Steer` is not pure (reads/writes `Mob`). Extract it, guarded by a bit-identical `mob-cap64-twice` digest. | §2.5, §3 |
| A11 | consistency | Near corpses parked and vanished while distant-killed ones stayed drawn. Corpses demote too. | §2.1, §2.8 |
| A12 | structure | `Unpark` moves into `TickAuthority` with promotion, so both compete for one cap and one budget in one place. Plus: a mid-air demotion lands with fall damage instead of a free landing. | §2.1, §2.4 |

