# Plan: voxel art pipeline, articulated mobs, and a laser cutting tool

Status: proposal, not yet reflected in DESIGN.md. Whoever implements this owns
updating DESIGN.md in the same commits (CLAUDE.md: a change that contradicts
DESIGN.md must update it or not happen).

---

## 1. What we're going for

Three deliverables, in dependency order:

1. **A voxel art pipeline.** Author models in MagicaVoxel, drop `.vox` files in
   `assets/prefabs/`, place them in the world. Covers statues, structures,
   props, and the source geometry for mobs.
2. **Articulated mobs.** A humanoid built from multiple Jolt bodies joined at
   limbs. It bleeds voxels when damaged, and severed limbs become free
   rigidbodies that eventually settle back into the CA grid as real voxels.
3. **A laser tool.** Player-held beam that cuts voxels out of the grid AND
   severs joints on mob/debris bodies — the test harness that proves #2 works.

The through-line: **handmade art becomes physical matter that the existing
destruction pipeline already knows how to break.**

### Why this decomposition works

The engine already has the hard half built. `DebrisSystem` (`src/phys/debris.h`)
takes voxels out of the grid, makes them a Jolt rigidbody carrying its voxel
payload, renders them instanced, and collides them against marching-cubes
terrain. A mob is a debris body that steers itself; a severed limb is a debris
body that stops steering. Bleeding is the particle system spawning liquid
voxels. Settling is `CellOp`s writing voxels back to the grid — the exact
inverse of the island-removal path in `debris.cpp:268`.

So most of this is **connecting existing systems**, not building new ones.

### The invariant that makes it all legal

`debris.h:20-22` states it: bodies are CPU-float gameplay state, deliberately
outside the hashed grid domain, and their grid interactions travel exclusively
through the op stream. **Mobs therefore get floats, joints, quaternions, and
animation without touching determinism rule #1.** The determinism boundary is
the MutationQueue, and everything below stays on the correct side of it.

Do NOT implement mobs as CA voxels. Locomotion via CA rules is capped at ≤1-cell
write reach, can't rotate, can't have limbs, and a mob that never settles keeps
its chunk permanently awake — violating rule #2. The `kMatMite` `wanders` flag is
fine for ambient bugs and is not a path to anatomy.

---

## 2. Hard constraints discovered during design

**Read these before writing code. Both will silently corrupt results.**

### 2.1 `DebrisVoxel` coordinates are `int8_t` — max 127 voxels from origin

```c
struct DebrisVoxel { int8_t x, y, z; uint8_t pad; uint16_t payload; };
```
(`src/phys/physics.h:30`)

Body-local coords are signed bytes. A prefab larger than ~127 voxels per axis
cannot be a single debris body. This is fine for mobs (a humanoid is ~20-40
voxels tall) but it means **large structures must go into the grid as `CellOp`s,
never through `CreateDebrisBody`**. Keep the two paths distinct.

Per-limb bodies help here: each limb is its own body with its own local origin,
so the byte range applies per limb, not per mob.

### 2.2 `BodyVoxInst` packs the body slot in bits 16..27 — 4096 slot ceiling, but `kMaxBodies = 200`

```c
struct BodyVoxInst { float lx, ly, lz; uint32_t packed; }; // bits 16..27 = body slot
```
(`src/phys/debris.h:25`, and `BuildInstances` at `debris.cpp:440`)

`kMaxBodies = 200`. **A multi-body mob consumes one body slot per limb.** A
9-limb humanoid is 9 slots. Twenty humanoids would be 180 slots and starve the
debris system of room for actual rubble.

Decide the budget explicitly. Recommended: raise `kMaxBodies` to 512 and add a
cheap accounting split (mob-reserved vs. debris-reserved) so a firefight can't
evict all mobs, or vice versa. `BuildInstances` already clamps at `kMaxBodies`
and `kMaxBodyVoxInstances`, so raising the cap is mostly a buffer-size change —
verify the 12-bit slot field still covers it (4096 max, so 512 is safe).

### 2.3 `Physics` has no joint API yet

`physics.h` exposes `CreateDebrisBody`, `CreateTerrainMesh`, `CreatePlayerBody`,
`RemoveBody`, `ApplyRadialImpulse`, `WakeNear`. **There is no joint/constraint
API.** Jolt supports these natively; the wrapper just doesn't expose them. This
is net-new work in `physics.{h,cpp}` and is the single largest unknown in the
plan.

### 2.4 Everything CPU→grid goes through the MutationQueue

Rule #3, no exceptions. Laser cuts, blood landing, limb settling, prefab
placement — all become `BrushOp`/`CellOp`/`ExplosionOp`. The only sanctioned
bypasses are whole-world snapshot restores (worldgen, `LoadWorld`). If you find
yourself writing to `world.voxels` directly, stop.

### 2.5 Voxel word is 16 bits and must not grow

12-bit material + 4-bit state nibble, and the nibble is already used (liquid
fullness, palette variant). **Limb identity cannot live in the voxel word.**
This is why limbs are separate bodies rather than tagged voxels. Per-voxel limb
data lives in the body's CPU-side `std::vector<DebrisVoxel>`, where it's free.

---

## 3. Milestone A — voxel art pipeline

### A1. `.vox` loader (`src/sim/voxload.{h,cpp}`)

Parse MagicaVoxel `.vox` (RIFF-style chunks). Minimum: `MAIN`, `SIZE`, `XYZI`,
`RGBA`. For milestone B also: `nTRN`, `nGRP`, `nSHP` (the scene graph).

```c
struct PrefabVoxel { int16_t x, y, z; uint16_t payload; }; // material | state<<12
struct PrefabModel {
  std::string name;          // from nTRN node name, or filename
  IVec3 size;
  IVec3 pivot;               // from scene-graph transform
  std::vector<PrefabVoxel> voxels;
};
struct Prefab { std::vector<PrefabModel> models; /* + hierarchy */ };
```

Note `PrefabVoxel` uses `int16_t`, deliberately wider than `DebrisVoxel`'s
`int8_t` — prefabs may be large; only mob limbs need to fit in a body.

**Palette convention: palette index == material ID.** Index 1 = stone, 2 = wood,
3 = sand, matching `materials.json` order and the `kMat*` constants in
`world.h:29-37`. This makes modeling "paint with materials," which is what a
falling-sand engine needs. Do not colour-match RGB back to materials — lossy and
fragile.

Ship `assets/prefabs/palette.png`, generated from `materials.json` colours by a
small script, so MagicaVoxel displays correct material colours while editing.
Add a loader-time warning for any palette index with no matching material.

**Coordinate handling:** MagicaVoxel is Z-up; check the engine's convention and
convert once, in the loader. Get this wrong and every model is on its side.

### A2. Prefab placement (`src/game/prefab.{h,cpp}`)

```c
// Emits CellOps to stamp a model into the grid at a world offset.
// Splits across ticks when it exceeds kMaxCellOpsPerTick (65536).
class PrefabPlacer {
  void Place(const PrefabModel&, IVec3 at, int rotY, bool overwrite);
  void PreTick(std::vector<CellOp>& cellOps);  // drains pending, bounded
};
```

Follow `DebrisSystem::PreTick` (`debris.cpp:288`) as the reference — same
"append bounded ops, defer the rest to next tick" shape, and copy its
`CellIndexOf` helper (`debris.cpp:29`) for slot-index math. Remember the
world-coords-vs-slot-index invariant from CLAUDE.md.

Rotation: restrict to 90° steps around Y. Arbitrary rotation of grid voxels
requires resampling and looks bad; skip it.

Placement modes mirror `BrushOp.mode`: paint-into-air (decorations that respect
terrain) vs. overwrite (statues that clear their footprint).

### A3. In-game placement UI

Extend the existing brush/overlay: cycle prefabs, preview at the pick location
(`sim_pick.wgsl` already gives hit cell + previous empty cell), rotate, place.
Hot-reload prefabs on the same key as materials (R) if cheap.

### Milestone A acceptance

- `--selftest` still passes (placement is `CellOp`s; the hash must stay stable
  for an unchanged world).
- A prefab placed, saved, and reloaded round-trips identically (it's in the
  MutationQueue, so `.svx` gets it for free).
- Placement of a 64³ prefab spreads across ticks without dropping voxels.

---

## 4. Milestone B — articulated mobs

### B1. Joint API in `Physics`

Net-new. Expose Jolt constraints through the existing voxel-units wrapper:

```c
enum class JointType { Fixed, Hinge, Ball };
uint64_t CreateJoint(uint64_t bodyA, uint64_t bodyB, JointType,
                     Vec3 anchorVoxel, Vec3 axis,
                     float minAngle, float maxAngle);
void DestroyJoint(uint64_t joint);          // <-- this is dismemberment
bool  JointBreak(uint64_t joint, float impulseThreshold); // optional: auto-break
```

Keep the voxel-units convention: positions in voxels, converted to meters
internally (`physics.h:8-10`).

### B2. Mob definition format

`.vox` scene graph gives limb partition + pivots + hierarchy. Sidecar JSON gives
what `.vox` can't express:

```
assets/mobs/humanoid.vox      # one model per limb, nodes named head/torso/arm.L/...
assets/mobs/humanoid.json     # joints, limits, HP, severability, bleed material
```

```json
{
  "root": "torso",
  "bleed": { "material": "water", "rate": 3 },
  "limbs": [
    { "name": "head",  "parent": "torso", "joint": "ball",
      "hp": 20, "severable": true, "anchor": [0, 12, 0] },
    { "name": "arm.L", "parent": "torso", "joint": "ball",
      "hp": 15, "severable": true, "anchor": [-4, 9, 0] }
  ]
}
```

Use a real blood material rather than reusing water. Add `blood` to
`materials.json` (class liquid, `tags: ["organic"]`) — tags mean it inherits
reactions (burns, freezes) for free. Append at the end; never reorder.

### B3. `MobSystem` (`src/game/mob.{h,cpp}`)

Mirrors `DebrisSystem`'s lifecycle so it slots into the existing frame loop:

```c
class MobSystem {
  void Init(Physics*, World*, DebrisSystem*, const std::vector<MaterialDef>&);
  uint64_t Spawn(const MobDef&, IVec3 atVoxel);
  void PreTick(uint32_t tick, World&, std::vector<CellOp>& cellOps);
  void PostStep();
  void Sever(uint64_t mob, int limbIndex);   // laser / damage entry point
  void BuildInstances(std::vector<BodyVoxInst>& out);
};
```

Spawn: one `CreateDebrisBody` per limb (voxels already limb-partitioned by the
scene graph), then `CreateJoint` per the sidecar hierarchy.

**Rendering:** limbs are already `BodyVoxInst` + `BodyXformGpu`, identical to
debris. Simplest correct approach is for `MobSystem` to contribute instances
into the same buffers `DebrisSystem` fills, so `debris.wgsl` needs no change.
Decide early whether mob bodies live in `DebrisSystem::bodies_` (simpler
rendering, muddier ownership) or a parallel array that merges at build time
(cleaner, needs the slot index to stay globally unique — remember bits 16..27).
Recommend the parallel array with a shared slot allocator.

### B4. Locomotion

Start **kinematic**: drive limb transforms from keyframes in the sidecar,
switch the whole mob to dynamic ragdoll on death or when a load-bearing limb is
severed. Physics-motor walking is more emergent but much harder to make look
deliberate; defer it.

Keep mob AI dead simple for now (walk forward, turn on obstacle). This milestone
is about the body, not the brain.

### B5. Bleeding

On damage, spawn particles at the wound in the mob's blood material. Particles
are already integer fixed-point (24.8) and part of the deterministic sim
(`common.wgsl:129-142`), and they already reinsert into the grid when they
settle. **Spawning must go through the op stream, not a direct particle write** —
confirm how `sim_particle.wgsl` currently receives spawns and reuse that path.

Bound the bleed rate (rule #2). A mob bleeding every tick forever keeps chunks
awake; use a decaying wound budget so bleeding stops.

### B6. Severed limbs settling back to voxels

The payoff. On `Sever`: destroy the joint; the limb is now a free debris body,
which is already a fully-supported object.

Then add settle-back — new, and useful for *all* debris, not just limbs:

- when a body has been inactive (`Physics::IsActive` is false) for N ticks and
  is resting on terrain, convert its voxels back into the grid via `CellOp`s at
  its final transform, and `RemoveBody`.
- rounding body-local float positions to grid cells is a resampling step: two
  voxels may land in the same cell. Handle collisions deterministically (fixed
  priority, never first-write-wins — rule #1) and accept minor volume loss.
- **only settle when the body's rotation is near-axis-aligned**, or the
  resampling looks like mush. If it came to rest at an odd angle, either snap to
  the nearest 90° or leave it as a body.

This closes the loop: grid → body → grid.

### Milestone B acceptance

- Spawn a humanoid; it stands, is stable, doesn't jitter or explode.
- Sever an arm: it detaches, falls, tumbles, comes to rest, becomes grid voxels.
- Blood spawns, falls, pools, and stops (chunks return to sleep).
- `--selftest` passes: settled-world active chunks stay under 32, world hash
  still reproducible across two runs.

---

## 5. Milestone C — laser tool

Test harness for everything above, and a genuinely good toy.

### C1. Grid cutting

Reuse `sim_pick.wgsl`'s DDA. The laser is a ray with a short cut radius:
march it, emit `BrushOp`s (mode=overwrite, material=air) along the beam, or a
narrow `ExplosionOp` if you want heat/melt behaviour.

Nice touch that costs little: convert cut voxels to their molten/heated form
rather than air (stone → lava, sand → molten glass — both materials already
exist), so cutting looks like cutting.

Cutting a slab in half naturally triggers the existing support-loss detection
(`QueueSupportEvents`, `debris.h:55`), so **the severed half becomes debris with
no new code.** This is a good early proof that the pipeline works.

### C2. Body cutting and dismemberment

Ray-test the beam against mob/debris bodies. On hit:

- if the beam crosses a joint anchor within a tolerance → `Sever` that joint.
- if it crosses a limb body's middle → split the body: partition its voxels by
  which side of the cut plane they fall on, destroy the original body, create
  two new ones. Reuses `CreateDebrisBody` and mirrors the island-detection
  split logic.

Voxel-level ray-vs-body test: transform the ray into body-local space using the
inverse of `BodyTransform`, then test against the local voxel set.

### C3. Controls and feedback

Bind to a mouse button; render the beam with the existing `Sprite` path
(`world.h:68`) or a debug line. Emissive impact particles at the hit point.
Add active-mob / active-body counts to the overlay for debugging.

### Milestone C acceptance

- Cut a stone pillar; the top half falls as debris and settles.
- Cut a humanoid's arm off; it falls, settles, becomes voxels.
- Cut a fallen limb in half; both halves are independent bodies.
- `--selftest` passes.

---

## 6. Suggested sequencing and risk

| Step | Risk | Note |
|---|---|---|
| A1 `.vox` loader (XYZI only) | low | pure parsing, testable offline |
| A2/A3 placement + UI | low | `CellOp` path already proven by debris |
| C1 laser grid-cut | low | do this early — it exercises support-loss detection and is fun |
| A1b scene-graph parsing | medium | more `.vox` chunk types |
| B1 joint API | **high** | net-new Jolt surface, largest unknown |
| B3 mob spawn/render | medium | body-slot budget question (§2.2) |
| B6 settle-back | medium | resampling determinism needs care |
| C2 body cutting | medium | depends on B1 |
| B4 locomotion | medium | tune-heavy, easy to defer |

Recommended order: **A1 → A2 → C1 → A1b → B1 → B3 → C2 → B6 → B5 → B4.**

Getting the laser cutting grid voxels early (C1) is deliberate: it's a few hours
of work, it validates the pick/mutation path end to end, and it makes every
later milestone testable by hand.

### Run the gate

`--selftest` after every sim/shader/material change, and report the actual
output — never claim a sim change works without it. `bash scripts/check_shaders.sh`
validates WGSL in seconds without a full rebuild.

```bash
taskkill //F //IM sandvox.exe   # if the link step hits LNK1104
cmake --build build --config Release --target sandvox
./build/Release/sandvox.exe --selftest
```

---

## 7. Open questions for the implementer

1. **Body slot budget** (§2.2) — raise `kMaxBodies`, and how to split mob vs.
   debris reservations?
2. **Mob body ownership** — inside `DebrisSystem::bodies_`, or a parallel array
   with a shared slot allocator? (Recommend the latter.)
3. **Settle-back rotation policy** (§B6) — snap to 90°, or refuse to settle
   non-axis-aligned bodies?
4. **Does the particle system have a CPU spawn path today?** If not, adding one
   for blood must respect the op-stream discipline.
5. **Blood pooling vs. rule #2** — liquid blood that never evaporates keeps
   chunks awake. Add a slow decay reaction in `reactions.json` (subcritical, per
   the stem note).
