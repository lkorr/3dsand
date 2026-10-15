# PLAN: distant NPCs — creatures that keep living past the residency window

Status: **PLANNED 2026-10-13. AUDITED AND AMENDED 2026-10-15** (§7 lists the
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
| `distant-fight` | D2b | two squads fight distant: casualties > 0, twice-run digest equal (per-creature limb hp + origin bits + deaths, `mob-cap64-twice`-style), awake count → 0 after the fight, dead promoted as corpses on the ground |
| `distant-calibrate` | D2b | ASYMMETRIC matchups (armoured vs unarmoured, 4v3, sword vs zombie, archer vs melee), K seeds each, at full fidelity and distant: the stronger side's win rate agrees within `distantWinRateTol`, mean time-to-kill ratio inside `distantTtkRatioMin..Max` (±25%) |
| `distant-edge` | D3 | near vs distant melee pair across the edge: both take damage, the near wound is real (lattice changed, blood op emitted); a distant archer's arrow enters the window as an `ArrowFlight` and hits; a creature pacing the edge flips tier ≤ `distantMaxFlips` per 600 ticks |
| `distant-look` | D4a | a distant creature at 40 m covers > N pixels in a shot; render-only slots never exceed the free budget; the near crowd never loses a slot; baked pose words stay under `kDistantPoseWordsMax` and the words per pose are recorded |
| `distant-schedule` | D5 | a parked villager inside the radius activates, walks its schedule while distant, promotes at its scheduled place |
| `distant-save` | D5+D6 | save with K distant creatures → load → the same K exist (activated), every limb's hp preserved |
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

(empty)

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

