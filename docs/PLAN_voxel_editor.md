# PLAN: Voxel model editor, procedural animation, microvoxels

Status: in progress (2026-08-20). This doc is the shared contract for the
feature; implementation agents build against it. When a decision here changes,
update this file in the same commit. DESIGN.md gets a distilled section when
each piece lands.

## Goals

1. A 3D voxel model editor inside the tuner (new tab) for rapidly authoring
   creatures and prefabs, fully integrated with the engine's formats.
2. Procedural animation in the general case: define limbs → get locomotion,
   layered with hand-authored keyframe clips and frame-by-frame (flipbook)
   voxel animation with per-frame durations.
3. Limb definitions drive dismemberment (already partially in `mob.cpp`);
   the editor authors them.
4. Microvoxels: models at 2x/4x/8x finer resolution than world voxels —
   dynamic (creatures, rigidbodies) and static (flowers, grass, torches
   occupying ordinary world cells, like Minecraft torch models).
5. Examples: grass, foliage, flowers.

## Research conclusions (distilled)

Full research lives in the session transcript; the load-bearing conclusions:

- **Static vs dynamic microvoxels are different problems.** Static detail is
  render-time substitution keyed by material ID — zero per-instance storage,
  zero new world state, the CA still sees one normal 16-bit cell. Dynamic
  bodies are object-space micro grids with free float transforms, marched
  per-pixel inside a rasterized OBB (Teardown's model), depth-composited via
  the existing reversed-Z `frag_depth` convention.
- **Shadow rays must never iterate models.** V1: micro bodies cast no shadows
  (parity with the current cube path, which also casts none). Static micro
  cells are non-occluding for ray skipping; their shadow term stays coarse.
- **`.vox` cannot carry smooth rotation** (nTRN is a 24-rotation lattice) or
  timing. `.vox` = voxel payload + palette only (palette index == material
  ID, unchanged). Rig, pivots, clips, gait params, flipbook timing live in
  the JSON sidecar. `.vox` files stay openable in MagicaVoxel.
- **Animation = 5-stage CPU-float pipeline** (sample → blend → additive →
  flatten → IK → physics-blend → submit), a strict superset of the current
  single-sine swing in `mob.cpp:391-408`. IK is a post-process, never a
  blended layer. nlerp (accumulator-aligned) for N-way blends, not slerp.
- **Gait**: foot targets with step triggers (plant until drift > threshold,
  one leg-group swinging at a time), `sin(t*π)` step arcs scaled by speed,
  **body pose derived from foot average** (free slope handling on voxel
  terrain), 2x-frequency pelvis bob, progressive phase lag up the chain.
  Two-bone analytic IK for legs/arms (clamp acos inputs, explicit pole
  vector); FABRIK only for 3+ segment chains (tails).
- **Editor**: plain JS + vendored three.js (only dependency), single
  InstancedMesh cube view, hand-written DDA picking (~60 lines), hand-written
  .vox reader/writer (~300 lines; existing JS writer libs silently drop scene
  chunks). Aseprite-style timeline (per-frame ms inline, onion skin that
  wraps the loop). Parts are named voxel selections in ONE volume, not
  separate files.
- **Determinism boundary** (= existing `mob.h`/`debris.h` doctrine, restate in
  DESIGN.md): animation/IK/springs/ragdoll are float presentation state,
  never hashed. Grid contact only via MutationQueue at integer-quantized
  positions. Flipbook frame index is an integer function of tick. Micro brick
  pools must never be bound in any sim shader's bind group.

## File layout

```
assets/models/                 editor-authored generic models: <name>.vox + <name>.json
assets/mobs/                   unchanged location; sidecar schema EXTENDED (below)
assets/microvox/<name>.vox     static micro-brick assets, referenced from materials.json
assets/editor/editor.js        the editor (ES module); split further if >~3k lines
assets/editor/vox.js           .vox read/write (DataView, no deps)
assets/editor/vendor/          three.module.js + OrbitControls.js, pinned, vendored + LICENSE
docs/PLAN_voxel_editor.md      this file
```

## Data formats

### Static micro-detail (materials.json)

A material may declare a micro model. The world cell remains an ordinary
16-bit voxel of that material; only the renderer substitutes.

```jsonc
{ "name": "flower_poppy", "class": "solid", "tags": ["organic","flammable"],
  "micro": {
    "model": "microvox/flower_poppy.vox",  // relative to assets/
    "subdiv": 4,                            // 2|4|8 micro voxels per world-cell edge
    "frames": [ { "model": 0, "ticks": 20 },   // optional flipbook; model = .vox model index
                { "model": 1, "ticks": 20 } ],
    "yawVariants": true,                    // hash3(cell)-keyed quarter-turn yaw
    "jitter": true                          // hash3-keyed sub-cell XZ offset (render-only)
  } }
```

- Each `.vox` model in the file is one FRAME, dims exactly `subdiv³`
  (one world cell). Multi-cell plants are built compositionally: a prefab
  whose cells use different micro materials (stem cell + blossom cell).
- Loader packs all frames of all micro materials into one GPU brick pool
  (`array<u32>`, 4×8-bit palette indices per word; palette index == material
  ID so micro voxels shade through the existing material table) plus a
  per-material table `MicroBrick { base:u32, subdivLog2:u32, frameInfo:u32,
  flags:u32 }` sized `kMaterialSlots`. `base==0xFFFFFFFF` ⇒ no micro model.
- Flipbook selection is `tick`-derived integer math in WGSL (render-only).

### Model/mob sidecar JSON (superset of current mob schema)

Extends `assets/mobs/*.json`; all new fields optional — `dummy.json` keeps
working unchanged (swingAmp/swingPhase remain the no-IK fallback).

```jsonc
{
  "root": "torso", "speed": 5.0, "bleed": { "material": "blood", "perDamage": 2.0 },
  "skinScale": 8,                   // 1 = world voxels (cube path), 2|4|8 = microvoxels.
                                    // The ART's resolution. The COLLIDER's is
                                    // derived at load (MobDef::physScale) and
                                    // logged; it is never authored here.
                                    // "scale" is still read, and means both
                                    // lattices are equal (the pre-split key).
  "gait": {                         // NEW: procedural locomotion params
    "cadence": 2.2, "strideBias": 0.35, "leadTime": 0.2,
    "stepThreshold": 0.6, "stepDuration": 0.22, "stepHeight": 0.25,
    "rideHeight": 0.9, "groups": [["leg.L"],["leg.R"]],
    "bobAmp": 0.06, "bobFreqMul": 2.0, "swayAmp": 0.05, "rollAmp": 0.09,
    "spineCounter": 0.7, "phaseLag": 0.05
  },
  "limbs": [ { "name": "leg.L", "parent": "torso", "joint": "ball", "hp": 15,
      "anchor": [1.5,6.0,0.5], "severable": true, "vital": false,
      "axis": [1,0,0], "swingAmp": 0.5, "swingPhase": 0.0,
      "tag": "leg",                 // NEW: gait/chain queries go by tag, not name
      "severImpactSpeed": 14.0,     // NEW: fast hit severs regardless of hp
      "spring": null } ],           // NEW: non-null ⇒ jiggled, never keyed
  "chains": [ { "tag": "leg", "parts": ["leg.L"], "effector": "leg.L",
      "pole": [0,0,1], "solver": "twobone" } ],
  "clips": { "attack": { "durationMs": 480, "loop": false, "mode": "override",
      "mask": ["arm.L","arm.R"], "blendInMs": 80, "blendOutMs": 120,
      "tracks": { "arm.R": { "rot": [ { "t":0, "q":[0,0,0,1], "ease":"cubicOut" } ],
                              "pos": [ { "t":0, "v":[0,0,0] } ] } } } },
  "flipbooks": { "death": { "frames": [ { "part":"torso", "model":3, "durationMs":120 } ] } },
  "states": [                       // NEW: dismemberment locomotion, first match wins
    { "name": "crawl", "missing": ["leg.L","leg.R"],  // AND-ed predicates; also
      "clip": "crawl",              // "missingAny": [...] and "minChainsLost": N
      "speedScale": 0.35,           // walk-drive multiplier while active
      "disableGait": true,          // clip owns the pose: no gait/IK/swing/bob
      "bodyYOffset": 0.0 } ],       // body height vs ground while gait is off
  "editor": { "parts": { "leg.L": { "box": [[0,0,0],[2,5,2]] } } }  // NEW: editor-only,
}                                                // voxel-selection→part mapping; engine ignores
```

Conventions: quaternions in clips (fused keyframes, not per-channel Euler);
easing enum `instant linear quadIn quadOut quadInOut cubicIn cubicOut
cubicInOut`; times in integer ms; unique part names enforced at edit time.

**Wave 2a notes (as implemented, `src/game/anim.{h,cpp}` + `mob.cpp`):**

- `clips` is authored as `{ "rot": [...], "pos": [...] }` per track; the loader
  **fuses them by `t`** into one keyframe list, so a key may carry a rotation,
  a position, or both. `ease` is a property of the *outgoing* key.
- `chains[].effector` defaults to the last entry of `parts` when omitted.
  `solver` accepts only `"twobone"` for now; anything else warns and falls back
  (FABRIK for 3+ segment chains is still deferred, per the research note).
- `spring` gained `gain` and `maxAngle` alongside `halflife` — a bare halflife
  gives no way to bound the jiggle, and unbounded jiggle violates rule #2's
  "bound every emergent process".
- `gait.groups` name **parts**, and a chain is matched to a group by either its
  root part or its effector, so a group may list either the hip or the foot.
- Limbs are **topologically sorted at load** (parent before child). Positional
  limb indices are therefore not stable across sidecar edits — the selftest now
  resolves limbs by name, and callers should too.
- `severImpactSpeed` absent (or 0) means *infinite*, i.e. impact never severs.
- `MobSystem::Damage` gained a trailing `impactSpeed` argument (defaulted, so
  existing call sites are unchanged).
- Flipbook frames swap the **rendered** voxel set only; the Jolt shape stays the
  rest model, and instances rebuild only on an actual frame change.
- Clip rotations are authored **from vertical**: a torso key of 60° leaves the
  limb 30° above the ground. `--shot-mob <def>[:limb,...][@x,z]` prints every
  live limb's local/model/world +Y elevation (90 = upright, 0 = flat) next to
  the screenshots, which is how these angles are set — and how a pose bug gets
  localized to a pipeline stage instead of guessed at from an image.
- `states` (dismemberment locomotion): rules are evaluated **in authored order,
  first match wins**, so list the most-maimed state first ("both legs gone →
  crawl" before "a leg gone → limp"). `speedScale: 0` also suppresses the
  blocked-turn, so a fully immobilized mob doesn't pirouette against a wall. A rule with an empty predicate never
  matches. `minChainsLost` counts IK chains with **any** severed part — the same
  test the gait uses to drop a leg, so the rule flips exactly when the gait
  stops using it. Selection lives in `AnimSelectState` (anim.cpp, re-evaluated
  every frame); transitions crossfade for free (old loco clip blends out via
  the stopping fade, new one in over its `blendInMs`). While a `disableGait`
  state is active the whole procedural layer (gait, IK, legacy swing, pelvis
  bob) is suppressed and `bodyY` settles to the walk drive's ground contact
  plus `bodyYOffset`; the tail spring keeps running (jiggle is not gait).
  `speedScale` is read by the walk drive one tick behind selection.

## Engine architecture

### A. Static micro-detail (raymarch substitution) — build FIRST

In `trace()` in `raymarch.wgsl`, when the DDA lands on a solid cell whose
material has `MATF_MICRO`:

1. Compute entry ray in cell-local space, run a nested Amanatides–Woo DDA
   over the `subdiv³` brick (worst case ~3·subdiv steps, hard-capped).
2. Apply hash3(seed, 0, cellIndexW)-keyed quarter-turn yaw (a coordinate
   swizzle) and optional sub-cell jitter before marching. Render-only.
3. Hit ⇒ shade with the micro voxel's material (existing palette/material
   path), normal from the last-stepped axis. Miss ⇒ continue the WORLD DDA
   past the cell.
4. LOD: beyond `TUNE_MICRO_LOD_DIST` (cell ≲ 1px) skip the nested march and
   shade as a plain voxel. Cap nested evaluations per primary ray
   (`TUNE_MICRO_MAX_PER_RAY`, ~8); beyond the cap treat the cell as solid.

Constraints and gotchas:
- Micro materials must be **non-blocking in `occBlockers`** (like media), or
  chunk skipping terminates rays on mostly-air cells. Verify occupancy is
  render-only before touching `isRayBlocker`; the selftest determinism gate
  is the check (hash may move only if worldgen content changes — the gate is
  *determinism*, not a fixed baseline).
- Shadow rays: cheapest correct-looking option (skip micro cells entirely or
  march them coarsely) — pick by eye with `--shot`.
- New bindings (brick pool + micro table) go in the raymarch bind group;
  watch Dawn's 16-storage-buffers-per-stage layout limit.
- `MICRO_*` constants derived from world.h go in `ShaderConstantPrelude()`;
  look/feel knobs are `TUNE_*` (4-place contract, non-`sim.*` keys only).
- CPU loader: `src/sim/` beside voxload; hot-reloads with R alongside
  materials (micro table rides `UploadTables` or its own upload).

### B. Animation runtime (CPU, `src/game/`) — replaces mob.cpp:391-408

Per mob per frame (float, presentation-side):

```
1 SAMPLE   active clips → local Transform[] (nlerp fused keyframes, easing)
2 BLEND    override layers weight-normalized w/ per-part masks; restPose
           fallback below threshold 0.1 (ozz BlendingJob shape)
3 ADDITIVE applied AFTER normalize: q_out = q_base * nlerp(id, dq, w)
4 FLATTEN  parent-before-child linear LocalToModel (parts stored in that order)
5 IK       two-bone analytic on `chains` in model space (post-process):
           clamp d to [|L1-L2|+ε, L1+L2-ε], clamp acos args to [-1,1],
           atan2 form for root angle, explicit pole vector, fixed fallback
           vector near full extension
6 PHYSICS  per-part slerp vs Jolt pose (ragdoll blend weight)
7 SUBMIT   MoveKinematicBody / xform upload (existing path)
```

Gait (base layer): foot targets `hip + fwd·strideBias + vel·leadTime`,
snapped down via `GroundHeightAt`; unplant when drift > stepThreshold·legLen
AND no sibling in the same group swinging; swing = lerp XZ + `stepHeight·
sin(tπ)` arc over stepDuration; scale stride+lift by speed (no marching in
place). Body Y/normal from foot average + rideHeight. Pelvis bob at 2× step
frequency, sway/roll at 1×, spine counter-rotation, per-level phase lag.
Springs (Holden closed-form halflife) only on `spring`-tagged parts — a part
is keyed or jiggled, never both. Limb loss: stop scheduling that leg's steps;
body-from-feet re-centers automatically; IK weight → 0.

Dismemberment additions to the existing Sever/DetachLimb/Die path:
`severImpactSpeed` second threshold; severed parts hold their last pose for a
beat before going dynamic; on true sever `RemoveConstraint` (not just
disable) and re-enable collision between the severed parts (the
GroupFilterTable otherwise suppresses it forever). Jolt ragdoll GroupIDs must
be unique per mob INSTANCE across the whole PhysicsSystem.

### C. Dynamic microvoxel render pass

New pipeline in `Simulation` beside `bodyDraw_`, drawn between `DrawBodies`
and `DrawSprites`:

- Per micro body (mob limb or debris chunk with `scale>1`): raster its OBB
  (36 verts, **backfaces only** so an inside camera still draws), reusing
  `bodyXforms`. Fragment shader: transform ray to object space (unnormalized
  dir keeps t in world units), slab-test, fine DDA over the limb's brick in
  the shared pool, discard on miss, on hit write color + `frag_depth` with
  the exact `KNEAR / max(viewZ, KNEAR)` reversed-Z convention +
  `projectView` from common.wgsl. Hardware GreaterEqual depth test does the
  compositing against the world raymarch.
- Brick pool: per-mob-def limb bricks uploaded once at load (shared across
  instances; damage does not edit voxels in v1, so no copy-on-write).
- Physics stays on the existing debris path with voxel pitch scaled by
  1/physScale; `DebrisVoxel` int8 coords are in COLLIDER units, and the loader
  coarsens physScale until they fit rather than refusing the def. Settle-back into the world downsamples
  micro→world by majority-fill through ordinary CellOps.
- Slots without a micro model keep the cube path; `scale:1` mobs unchanged.
- Lighting v1: match or modestly improve the cube path (litColor + normal
  from DDA axis). Shadows: none cast (parity), stretch goal = coarse
  occupancy proxy stamped render-side.

**Wave 3 notes (as implemented, `src/sim/microbody.{h,cpp}` +
`assets/shaders/microbody.wgsl`; distilled into DESIGN.md §9):**

- Micro limb models live in their OWN pool + model table (`kMicroBodyPoolWords`,
  `kMaxMicroBodyModels` in `world.h`), separate from the static `microvox` pool.
  They are bound as a dedicated group 1 (`microBodyBGL_`) paired with the
  existing `renderBGL_` — 11 fragment layout entries, under Dawn's 16.
- The routing key is the **body**, carried as a `MicroBodyRef` parameter on
  `DebrisSystem::AdoptBody` (defaulted, so plain-debris callers are unchanged).
  A severed micro limb keeps micro rendering because `MobSystem` hands over what
  it already knows; there is no side table to keep in sync and no way for a
  description to outlive the body it describes.
- Rig geometry is converted SKIN -> WORLD once, at load (`anchorLocal *=
  1/skinScale`) and at spawn (`restOffset`, joint anchors, `MobDef::worldSize`).
  The animation runtime, the gait and `GroundHeightAt` are therefore completely
  scale-unaware.
- **Skin and collider are two lattices, not one.** This originally read "`limb.
  voxels` and `limb.size` stay in micro units — that is what the collider's
  `1/scale` pitch and the renderer's brick both want", and that coupling is
  what capped micro rigs at scale 4. It no longer holds. `MobDef::skinScale` is
  the AUTHORED art resolution (1/2/4/8); `MobDef::physScale` is DERIVED at load
  as the finest of {8,4,2,1} that fits both the DebrisVoxel int8 bound and the
  `kMaxPhysScale` cost ceiling, and is logged per def. `limb.voxels`/`limb.size`
  are in physScale units; `limb.skinVoxels` (int16) is the skin and the brick
  source, empty when the two coincide. The skin is AUTHORITATIVE and the
  collider is re-derived from it by majority fill (`phys/lattice.h`
  `DownsampleSkin`) — data flows skin -> collider and never back, which is what
  keeps all of it outside the hashed domain.
- `Physics::CreateDebrisBody{,Xf}` gained a trailing `voxelPitch` (default 1, so
  every existing call site is byte-identical). It scales the box halves, the
  centres, the convex radius AND the per-voxel volume feeding mass.
- Micro bodies are excluded from body burn and `SplitBody` (both mutate
  `b.voxels`, which the shared per-def brick cannot follow). Settle-back
  downsamples micro -> world by majority fill instead.
- The `.vox` int8 bound (120) applies to the DERIVED COLLIDER, not to the art:
  the loader coarsens `physScale` until the limbs fit, so the authored lattice
  is bounded only by int16. A def only fails when a limb exceeds 120 *world*
  voxels, which no collider resolution can represent; the loader says so
  explicitly in the error.
- The `--selftest` `micro body render` case renders the scale-2 critter twice,
  with and without the pass, and asserts a pixel-difference floor. That proves
  the pass drew, that its depth survived the reversed-Z test against the world,
  and (via the reported cube-instance count of 0) that limbs are not double-drawn.

### D. Editor (tuner "Models" tab)

- New nav button + `<section id="view-models">` + `renderModels()` with the
  early-return in `renderAll()` (and the gate at tuner.html:840).
- Server (`tuner_server.py`): `GET /api/models` (list assets/{models,mobs,
  microvox}), `GET/POST /api/model?path=...` restricted to those dirs with
  traversal guards; `.vox` POSTs are base64 or raw bytes. MIME entry for
  `.vox`. Keep the page fully functional in both pywebview and browser mode.
- View: three.js (vendored, import map, pinned version, `frustumCulled=false`
  InstancedMesh, OrbitControls); picking via JS grid DDA returning cell +
  face normal.
- Tools (v1): attach/erase/paint modes (T/R/E keys, instant toggle);
  voxel/box/face brushes; mirror X (multi-axis toggleable); palette grid fed
  from materials.json colors (click select, alt-click eyedropper); box
  select → move/delete/copy/paste → **Make Part**; per-stroke diff undo;
  placement on clicked face (hit+normal).
- Parts/rig: part = named voxel selection + parent + pivot (always-visible
  gizmo snapped to voxel/half-voxel) + joint params + severable/vital +
  sever threshold. Unique-name validation inline.
- Animation: Aseprite-model timeline — frame strip with inline per-frame ms,
  one-key duplicate, tags (walk/idle/attack), looped playback, onion skin
  (prev red / next blue, wrapping the loop, depth-test-no-write, only where
  current frame is empty). Flipbook frames = additional .vox models.
- Gait preview: pose the parts with the SAME parameters the sidecar exports
  (the preview renders exported data — never a parallel implementation).
- Micro-brick authoring: grid preset `subdiv³` with a ghost outline of the
  world cell; save to assets/microvox/.
- Round-trip assertion on every save: write .vox → read back → compare.

## Phasing (implementation waves)

| Wave | Work | Files | Gate |
|---|---|---|---|
| 1a | Static micro-detail engine + generated grass/foliage/flower assets + materials | src/sim, shaders, assets/microvox, materials.json, scripts | check_shaders + selftest + --shot |
| 1b (parallel) | Editor core: tab, three.js, vox.js, edit tools, palette, save/load routes | assets/editor, tuner.html, tuner_server.py | manual + round-trip assert |
| 2a | Animation runtime: pose pipeline, gait+IK, clips, springs, sever additions | src/game | selftest (walk test) |
| 2b (parallel) | Editor: parts/rig UI, timeline, onion skin, gait preview | assets/editor | manual |
| 3 | Dynamic micro render pass + micro mob demo — **DONE** (critter is scale 2) | src/sim, src/gpu, shaders, assets/mobs | selftest + --shot |
| 4 | Polish: DESIGN.md section, tuner knobs, example pass, perf check | — | full selftest |

Build discipline: `taskkill //F //IM sandvox.exe` before builds; engine waves
are serialized (shared build dir + hub files); editor waves touch disjoint
files and may run parallel to engine waves. `bash scripts/check_shaders.sh`
after any WGSL edit.
