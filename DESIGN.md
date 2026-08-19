# 3D Falling Sand Simulator — Design Document

A first-person 3D falling-sand voxel game in the spirit of Noita: a fully simulated,
destructible micro-voxel world driven by simple per-voxel rules, with emergent
interactions between materials, rigidbody debris, and particles. Long-term: infinite
procedurally generated world, alchemy, spells, and online multiplayer.

**Primary sources informing this design:**
- Petri Purho, *Exploring the Tech and Design of Noita* (GDC 2019) — the 2D playbook:
  CA rules, chunking/dirty rects, checkerboard multithreading, rigidbody pipeline,
  particle ejection, streaming, and design lessons on emergent chaos.
- Burkelbear Games (*Grimorium*) devlogs — a working proof that this exact game works
  in 3D on a custom C++ engine with a **GPU-resident simulation**: 16³ chunks, atomics
  for race safety, hierarchical dirty flags, ~100M+ resident voxels in ~200 MB,
  bounded flood-fill island detection, marching-cubes collision.

---

## 1. Core Decisions (summary)

| Decision | Choice | Why |
|---|---|---|
| Simulation location | **GPU compute shaders** (CA, particles, worldgen) | Proven viable at 100M+ voxel scale; CPU cannot touch this throughput |
| What stays on CPU | Rigidbodies, gameplay, projectiles, networking, streaming | Needs branching logic, engine APIs, and authoritative game state |
| Voxel format | **16 bits: 12-bit material ID + 4-bit state** | 4,096 materials; ~200 MB for a 512³-scale resident region |
| Chunk size | **16³ voxels (4,096 voxels, 8 KB)** | Fine-grained dirty/sleep granularity; cheap streaming unit |
| Sim tick rate | **Fixed 30 Hz**, decoupled from render | Determinism of *timing*, halves sim cost vs 60, imperceptible for sand |
| Race handling in CA | **3×3×3 cell-coloring — 27 passes/tick** (deterministic by construction); atomics-CAS as an opt-in optimization | Same-color cells are ≥3 apart on every axis while movement reach is ≤1, so destination writes are provably disjoint: race-free AND bit-deterministic across GPUs — keeps lockstep networking and replay debugging viable (see §4 for why chunk-level checkerboarding alone is insufficient) |
| Materials | **Data-driven JSON → compiled to GPU lookup tables**, hot-reloadable | Moddability requirement; iteration speed |
| Rendering | **Ray traversal (DDA) over the voxel grid**, not meshing | Geometry changes every frame; meshing churn would dominate |
| Rigidbody physics | **Jolt Physics** + custom voxel-terrain collision via localized marching cubes | Don't write a rigidbody solver; Jolt is fast, free, battle-tested |
| Multiplayer model | **Determinism-first sim discipline now; lockstep vs. server-authoritative decided at M9** | A disciplined integer GPU sim CAN be bit-deterministic across machines — both models stay viable (§10) |
| Language / API | **C++20 + WebGPU (Dawn native / Emscripten browser) + WGSL** (see §12) | Browser technical demos + browser multiplayer are a product requirement; Vulkan cannot run in browsers, while Dawn runs on Vulkan natively — one codebase, zero shader rewrites for web |

---

## 2. Can this run entirely on the GPU?

**The simulation can. The game can't — and shouldn't.**

What runs on GPU (the expensive 99%):
- The cellular automaton itself: every powder/liquid/gas movement rule and every
  material reaction runs in compute shaders over dirty chunks. Voxel data lives in
  device-local memory and *never round-trips to the CPU per frame*.
- The ballistic particle system for loose voxels (splashes, ejecta) — particles need
  to collide against the grid, and the grid lives on the GPU.
- Procedural terrain generation (compute shaders fill chunk buffers in parallel).
- Rendering, which reads the same voxel buffers directly — zero upload cost.

What must stay on CPU:
- **Rigidbodies** (debris islands, props): narrow-phase physics is branchy,
  sequential, and needs to interact with gameplay code every frame.
- **Gameplay-relevant projectiles** (spells): they trigger game logic on hit.
- **Streaming, save/load, worldgen orchestration, UI, audio, networking.**

The critical interface problem is **CPU visibility into GPU state** (the player
controller and physics need to know what voxels are where) without stalls:
- Maintain a low-res **CPU mirror**: per-chunk occupancy/metadata flags (has-voxels,
  has-liquid, boundary-face occupancy bits) read back asynchronously each tick —
  kilobytes, not megabytes.
- For precise queries (player capsule, rigidbody contacts), read back **only the
  16³ chunks intersecting active colliders**, one tick latent, double-buffered.
  At 30 Hz sim, one tick of latency on terrain collision is invisible.
- All CPU→GPU writes (spells, explosions, brush edits, worldgen) are accumulated
  into a per-frame **MutationQueue** and uploaded as one batched transfer.
  This queue is a load-bearing design element: it is also the serialization format
  for saves, the replication stream for networking, and the replay log for debugging.

**Second GPU consequence — determinism is a choice, not a casualty.** A naive GPU
sim (scheduling-dependent atomics, float math, stateful RNG) is non-reproducible
across runs and hardware. But cross-GPU *bit-determinism* is achievable with
kernel discipline, and independent projects have demonstrated lockstep multiplayer
GPU voxel sims. The requirements (adopted as day-one rules, see §4 and §10):
- **Integer-only simulation state and math.** Float basic ops are IEEE-deterministic
  per-op, but shader compilers diverge across vendors (FMA contraction, fast-math,
  transcendental approximations). An all-integer CA sidesteps this entirely.
- **No scheduling-dependent outcomes**: conflict resolution via checkerboard passes
  or deterministic-priority two-phase resolve — never first-come-CAS. No
  subgroup/wave ops, no order-dependent reductions or append-buffer ordering
  leaking into sim state.
- **Stateless counter-based RNG**: hash(worldSeed, tick, cellCoords).
- Determinism pays even in single-player: bit-exact replays from input logs are
  the best bug-reproduction tool a chaos sim can have. Verify with a per-tick
  world-state hash (also the desync detector in multiplayer).

---

## 3. Voxel World Storage

### Voxel format (16 bits)
```
bits 0–11 : material ID (4,096 materials)
bits 12–15: state nibble — meaning is per-material:
            powders/solids → visual variant (color/texture jitter resolved in
              shader, stable as the grain moves)
            liquids → fullness (mass-conserving flow, see §4)
            burning things → burn-stage counter
```
Material ID 0 = air/empty. If we ever need more per-voxel state (temperature,
velocity fields), add an *optional sparse auxiliary layer* keyed by chunk — do not
grow the base voxel. 16 bpv is what makes 100M+ resident voxels affordable.

### Chunks
- **16³ voxels = 8 KB per chunk.**
- Resident region: a rolling N³-chunk cube centered on the player (initial target
  N = 32 → 512³ voxels ≈ 134M voxels ≈ 268 MB device memory; tune to hardware).
- **Toroidal addressing**: the resident array never shifts in memory. Moving the
  player one chunk over recycles the trailing plane of chunks in place (save to
  disk / load or generate incoming). No pointer chasing, no defragmentation —
  chunk (cx,cy,cz) always maps to slot (cx mod N, cy mod N, cz mod N).
- Per-chunk metadata (CPU-mirrored, few bytes each):
  - `dirty` — contains moving/reacting voxels; the only chunks the CA dispatches
  - `nonEmpty` — any voxels at all (empty-space skipping for rendering & raycasts)
  - `hasLiquid`, `hasLights` — cheap routing flags
  - `faceOccupancy[6]` — does any voxel touch each boundary face (island detection §7)
- **Hierarchical dirty flags**: a mip-style tree over chunk flags so the dispatcher
  skips whole sleeping regions without iterating chunks.

### Streaming (infinite world)
- Chunks leaving the resident cube are compressed (RLE — falling-sand worlds are
  extremely runny) and written to a region file; incoming chunks load from disk or
  are generated in compute.
- **Implemented (2026-08-19, v0.4):** toroidal addressing is live on all three
  axes. World coords are unbounded i32; every kernel works in world coords and
  masks to slots (sizes are powers of two, so `mod` is a bitmask even for
  negatives); the window origin rides TickParams/RenderParams. The 3×3×3 color
  lattice is computed in WORLD coords — coloring by slot would race at the
  toroidal wrap. v1 simplifications, on purpose: the evicted-chunk store lives
  in RAM behind a `ChunkStore` interface (region files can replace the map
  without touching callers; the .svx save serializes the store wholesale), and
  eviction readback is synchronous per shift (bounded: one 16×16 plane,
  save-worthy chunks only — fresh terrain evicts almost nothing). Eviction
  save-worthiness reads the snapshot's occupancy/dirty flags, which lag the GPU
  by the readback ring: activity that starts on the trailing plane in those
  last ~2 ticks can be lost on re-entry — accepted, revisit with async
  eviction. CPU-known writes (brush/explosions) mark chunks modified
  immediately to shrink that window.
- **Unloaded space is treated as solid and inert** so liquids can't drain off the
  edge of the loaded world (Burkelbear's solution; adopt it verbatim).
- Underground, darkness hides the streaming horizon. Overworld draw distance vs.
  fog is a known open problem — defer.

---

## 4. The Simulation (GPU cellular automaton)

Fixed 30 Hz tick. Each tick, dispatch compute over dirty chunks only, batched via
SSBO lists of chunk indices.

### Movement rules (Noita rules lifted to 3D)
- **Powder**: try the cell directly below. If blocked, try the four horizontally
  adjacent cells *of the below cell* in random order (RNG replaces 2D's left/right
  alternation and breaks the symmetry that would create perfect square pyramids).
  Else stay and clear the moving bit. This alone produces angle-of-repose piles.
- **Liquid**: powder rule + try the four laterally adjacent cells on its own level.
  Plus **fullness equalization**: liquid voxels carry fullness in eighths (the state
  nibble); a cell flows into a lateral neighbor holding ≥ 2 eighths less, RNG on
  ties. Mass-conserving, settles flat, no oscillation. Viscosity is cheap: lava
  uses quarters and only updates every 3rd tick.
- **Gas**: inverse powder (up, then up-diagonals, then lateral), plus decay chance.
- **Solid**: doesn't move; participates in reactions and structural checks only.
- **Density displacement**: a mover entering a cell occupied by a less-dense
  fluid swaps with it (oil floats on water; sand sinks through both).

### Race safety (and determinism)
Two GPU threads must never both claim the same destination cell — and the *winner*
of any conflict must not depend on GPU scheduling, or the sim stops being
reproducible (killing lockstep networking and replay debugging).
- **Why chunk-level checkerboarding is NOT enough (audit fix, 2026-08-19):** a
  2×2×2 *chunk*-parity checkerboard (3D analog of Noita's 4-pass scheme) makes
  simultaneously-updated chunks non-adjacent — but on a GPU all 4,096 cells
  *within* a chunk update in parallel, and two movers in the same chunk can claim
  the same destination. Noita never faces this because its CPU update is
  sequential inside each region. Cell-level 2×2×2 parity also fails: two
  same-parity cells 2 apart can both target the diagonal cell between them.
- Primary approach: **3×3×3 cell-coloring** — 27 compute passes per tick, pass k
  updating only cells with (x mod 3, y mod 3, z mod 3) == color k. Same-color
  cells are ≥3 apart on every axis and movement reach is ≤1 cell, so destination
  writes are provably disjoint: race-free, atomic-free, AND bit-deterministic
  (fixed pass order). Dispatch overhead is negligible (~µs per dispatch); total
  bandwidth is unchanged since each cell is processed exactly once per tick.
  Chunk-level dirty early-out still applies inside every pass. Within a pass,
  each cell's decisions use only the stateless RNG (hash of seed/tick/coords) —
  never thread or dispatch order.
- Alternative where more parallelism is needed: **two-phase propose/resolve** —
  pass 1 writes movement proposals, pass 2 resolves conflicts by a fixed priority
  rule (e.g., lowest source-cell index wins). Deterministic, full-width dispatch,
  costs a proposal buffer.
- **Atomics-CAS** (Burkelbear's approach — claim destination by CAS on the packed
  32-bit word, contention reportedly negligible) is the fastest option but
  first-come-first-served = scheduling-dependent = nondeterministic. Permissible
  only as an opt-in optimization if we ever formally abandon determinism; the
  kernel structure should keep the strategies swappable.

### Update hygiene
- **Single-buffered, in-place** (Noita's choice, for Noita's reasons: double
  buffering forces write-conflict resolution and full-world updates). An 8-bit
  **tick-stamp** per voxel (`tick & 0xFF`, written on arrival, compared before
  updating — no per-tick clearing needed) prevents a voxel that moved from being
  updated again by a later color pass in the same tick. v0 stores each voxel in
  a u32 word (16-bit voxel + 8-bit stamp + 8 spare) since WebGPU storage buffers
  address u32s; repacking to 16 bpv + separate stamp layer is an M2+ memory
  optimization.
- A voxel that moves at a chunk boundary **sets the neighbor chunk's dirty flag**.
- Anything that changes marks its chunk dirty; a chunk with zero movement and zero
  active reactions for a tick clears its dirty flag and sleeps.
- **Batch-move optimization** (later): detect columns/blocks of voxels falling with
  identical motion and move them as a group instead of cell-by-cell.

### Reactions (see §6 for authoring format)
Each dirty voxel rolls against its reaction table entries: probability is expressed
**per-mille per tick** (fire beside wood → 10‰ chance/tick to ignite). Three shapes:
- **Reaction**: self + neighbor material → products (acid + stone → gravel)
- **Emission**: self emits into an adjacent empty/weaker cell (ember emits fire)
- **Decay**: self → product after probabilistic time (ember → ash, steam → water)

Chained rules produce emergent behavior for free: acid → stone → gravel → sand is
an erosion system nobody explicitly wrote.

---

## 5. Particle System (voxels in flight)

Noita's "Bloody Zombies" technique, on GPU:
- When a voxel should fly (splash, explosion ejecta, rigidbody displacement), it is
  **removed from the grid** and appended to a GPU particle buffer: position,
  velocity, 16-bit voxel payload.
- Particles integrate ballistically each tick, DDA-stepping through the grid;
  on hitting a non-empty voxel they **reinsert into the grid** at the last empty
  cell (waking that chunk).
- This is what makes liquids splash instead of blob, and it's the standard
  mechanism whenever the grid must yield space (rigidbody pushes through water →
  water voxels eject as particles).
- Capacity: ring buffer, ~1–4M particles. Overflow policy: oldest cosmetic
  particles reinsert immediately instead of flying.

**Gameplay projectiles are a separate CPU system** (§8) — they carry game logic.

---

## 6. Material & Interaction Authoring (the moddable core)

This is the system the whole game grows out of, so it gets designed first-class.
Author in JSON, hot-reload at runtime, compile at load into flat GPU tables.

### `materials/*.json`
```json
{
  "id": "water",
  "class": "liquid",              // solid | powder | liquid | gas
  "density": 1000,                // displacement ordering
  "hardness": 0,                  // resistance to explosions/digging (§7)
  "viscosity": { "granularity": 8, "tickInterval": 1 },
  "flammability": 0,
  "colors": ["#2a6df4", "#2f74ff", "#2261d8"],   // variant palette
  "emission": 0,                  // light emission (future GI)
  "tags": ["wet", "conductive", "extinguisher"],
  "statusOnContact": ["wet"]
}
```

### `reactions/*.json`
```json
{ "self": "fire",  "neighbor": "tag:flammable", "chance": 10,
  "selfBecomes": "fire", "neighborBecomes": "ember" },

{ "self": "acid",  "neighbor": "stone", "chance": 4,
  "selfBecomes": "flammable_gas", "neighborBecomes": "gravel" },

{ "self": "ember", "decay": true, "chance": 6,
  "becomes": ["ash", "smoke", "nothing"] }
```
- `chance` is per-mille per 30 Hz tick.
- **Tags** are the anti-bloat mechanism: reactions target tags (`flammable`, `organic`,
  `meltable`) rather than enumerating materials, so adding a material means adding
  tags, not editing every reaction. This is the single most important guard against
  the N×M interaction explosion.
- Explicit pairs override tag rules when both match.

### Compilation to GPU
- Material properties → one SSBO array indexed by 12-bit ID.
- Reactions → per-material buckets: each material stores offset+count into a flat
  reaction array (entry: neighbor ID or tag mask, chance, products). The CA kernel
  scans the bucket of the current voxel only — O(rules-per-material), not O(4096²).
- Validation pass at load: unknown IDs, unreachable rules, density cycles → loud
  errors with file/line. Modders get real diagnostics, not silent breakage.

---

## 7. Destruction, Islands, and Rigidbodies

### Large-scale destruction
- **Explosions**: cast rays from the blast center to every voxel on the blast
  sphere's *surface*, DDA-traversing voxel by voxel. Compare each voxel's
  `hardness` against remaining blast power: harder stops the ray, softer flags the
  voxel. Remove all flagged voxels in one pass, ejecting a fraction as particles.
  Ray count scales O(r²) while destroyed volume is O(r³) — this is what makes big
  explosions affordable.
- Lasers/beams/black holes: chunk-level geometric intersection first, per-voxel
  tests only inside intersected chunks.

### Island detection (the hard problem — budget time for it)
Destroying support must let disconnected terrain fall. CA rules only see immediate
neighbors, so this needs an explicit connectivity pass:
- Every destruction event queues an island check around the affected region.
- **Support-loss triggers (2026-08-19):** explosions and brush erases are not the
  only ways support disappears — the CA itself removes it (fire burns a stem,
  ember decays to ash, acid dissolves rock, sand flows out from under a slab).
  `sim_step` sets a per-chunk *support-loss flag* (side-channel buffer, never fed
  back into voxel state) whenever a supporting voxel (solid/powder) vacates or
  transforms next to a solid; the flags ride the async readback, are cleared on
  consume, and become island-check events with a per-chunk cooldown. A solid
  component with powder directly below counts as *resting* (anchored) — without
  that rule every slab on a sand pile would convert to a rigidbody the moment a
  grain shifted.
- **Bounded 6-connected flood fill** outward from voxels adjacent to the removal.
  Meeting fronts merge. If a fill exceeds ~32,000 voxels (~8 chunks), abort and
  declare "not an island" — an unbounded check could collapse an entire dungeon
  level, which is both a perf and a *design* disaster (Noita's lesson: "the world
  is simulated too much" kills level design).
- **Chunk-face acceleration**: per-chunk `faceOccupancy` flags let the fill run
  per-voxel only in chunks touched by the destruction, then continue at
  chunk-face granularity beyond — orders of magnitude cheaper.
- Known accepted flaw: large floating sections can survive. Ship it; revisit.

### Rigidbodies
- Detected islands are **removed from the grid** and become rigidbodies:
  marching cubes (Paul Bourke tables) over the island voxels → collision mesh →
  simplification pass to merge triangles → hand to **Jolt**.
- Mass = Σ per-voxel material density. Bodies keep their voxel payload so they
  remain destructible: damaging a body re-runs marching cubes and can split it.
- **Two-way handoff**: islands under ~8 voxels don't become bodies — they convert
  to their powder-equivalent material and drop back into the CA as rubble.
  (Conversely, a body that settles and stops could optionally re-fuse into the
  grid — defer, Noita keeps bodies as bodies.)
- Unlike Noita, body voxels do **not** live in the grid each frame (too expensive
  in 3D); bodies are meshes that carve/displace grid voxels on contact, ejecting
  displaced material into the particle system.
- **Terrain collision for bodies**: localized marching cubes over the contact
  region to get sloped normals (not box faces), generated on demand, cached per
  chunk until the chunk dirties.
- Sleeping: settled bodies deactivate entirely until another body or force
  intersects their AABB (Jolt does this natively).

---

## 8. Player & Projectiles

- **Player**: capsule controller (Jolt character), colliding against on-demand
  localized marching-cubes terrain patches. Voxel-type queries drive traversal:
  liquids slow movement and swap jump→swim; standing in gas/liquid can apply
  status effects; some materials are absorbed on contact (Noita stain system —
  and remember its lesson: players will invent rules for anything you surface
  in the UI, so communicate statuses deliberately).
- **Projectiles** (spells, thrown things): no colliders — swept ray each frame
  (position + velocity look-ahead, anti-tunneling). Spell modifiers attach as
  **tags with per-frame logic** (material trail, AoE on hit, bounce, acceleration
  modifiers). On hit: run effect, or reflect off a local marching-cubes normal.
  This tag-composition structure is deliberately the seed of the Noita-style
  wand/spell system later.

---

## 9. Rendering

- **Ray traversal, not meshing.** Terrain geometry changes every tick; re-meshing
  churn would dominate. Raymarch the voxel grid directly (DDA through chunks,
  `nonEmpty` flags for empty-space skipping — the same flags the physics uses).
  The sim already lives in GPU memory, so the renderer reads it for free.
- Pipeline: fullscreen ray pass → G-buffer (albedo/normal/depth/material) →
  deferred lighting. Voxel face normals from the hit axis; optional smoothed
  normals for liquids.
- Variant nibble → palette jitter in-shader (stable per-grain color, no reshuffling
  as grains move — exactly why the variant lives in the voxel).
- Rigidbodies/debris: two options. v1 = raster their marching-cubes meshes,
  composited by depth against the raymarched terrain. Better (proven by the BFS
  project): **GPU-driven render of each body's bounding box, then raymarch the
  body's own voxel payload inside the box to the exact voxel hit** — debris stays
  voxel-crisp instead of marching-cubes-smooth, and reuses the terrain shading
  path. Adopt once bodies carry their voxel payloads (M6).
- Later: distance fog + LOD bricks for far terrain, emissive materials feeding a
  cheap GI (light propagation volumes or per-chunk flood lighting), volumetrics
  for gases. **None of this in v1** — flat lit voxels first.

## 10. Networking (design now, build later)

With the determinism discipline of §2/§4, **both** classic models are viable, and
we defer the final choice to M9. What we do *now* is keep both doors open — which
turns out to be nearly the same set of day-one rules either way.

**Option A — Lockstep (deterministic sim, inputs-only on the wire):**
- Every machine runs the identical sim; only player commands are exchanged.
  Bandwidth is tiny and independent of how much chaos is on screen — a huge win
  for a simulation game where chunk deltas would spike exactly when the fun peaks.
- Requirements: bit-deterministic GPU kernels (checkerboard/two-phase, integer
  math, counter RNG — §4), deterministic CPU physics (Jolt supports a
  cross-platform-deterministic build flag), fixed tick, per-tick state hash for
  desync detection.
- Costs to respect: late join needs a full state snapshot (resident region is
  ~hundreds of MB — needs streaming join or joins at checkpoints); every client
  simulates the full active set (min-spec bound by the slowest GPU; interest
  management can't reduce sim cost, only render cost); a single determinism bug
  desyncs everyone, so the per-tick hash check must exist from the first
  multiplayer build; all clients hold full world state (cheat visibility).

**Option B — Server-authoritative (chunk-delta replication):**
- One machine runs the real sim; clients render + predict. Each tick the server
  already knows exactly which chunks changed (the dirty system computes this).
  Compress deltas (XOR vs. last acked + RLE + LZ4), send only chunks within each
  client's **interest radius**.
- Tolerant of nondeterminism and client heterogeneity; drop-in join is trivial
  (stream the interest region). Cost: bandwidth scales with visible chaos, and
  the server GPU carries everyone's simulation.
- Client prediction: player movement reconciled (standard); cosmetic particles
  and purely visual CA effects run client-side without authority — divergence in
  a splash pattern self-corrects on the next delta.

**Do now, cheaply (serves both options):**
- Determinism-first kernels and integer sim math (§4) — cheap now, near-impossible
  to retrofit. This also buys bit-exact replay debugging in single-player.
- **All world mutations flow through the MutationQueue** (§2) — locally it feeds
  the GPU; under lockstep it's the command stream; under server-auth it feeds
  replication. Building every tool, spell, and explosion against this API from
  day one is the whole anti-tech-debt play.
- Fixed tick, versioned chunk serialization, entity IDs never raw pointers,
  gameplay separated from render, a headless build target, per-tick world hash.
- Punt entirely: netcode library choice, final model selection, anti-cheat.
- Browser note: web builds network via WebSocket/WebRTC (no raw UDP). Both
  options survive this — lockstep needs only ordered command delivery; server-
  authoritative streams chunk deltas over a DataChannel/WebSocket fine.

## 11. Performance Budget & Principles

Targets (mid-range desktop GPU, e.g. RTX 3060-class):
- 30 Hz sim / 60+ FPS render, resident cube ≈ 512³ voxels (~268 MB device memory).
- Sim cost must scale with **activity, not world size**: a fully settled world costs
  ~zero (hierarchical dirty tree returns nothing to dispatch).
- Per-tick CPU↔GPU traffic: metadata mirror + collider-region readbacks + mutation
  uploads, target < 1 MB/tick, always batched, always async (one tick latent).
- Instrument from day one: dirty-chunk count, particles alive, sim/render GPU ms,
  readback stalls. On-screen debug overlay. Burkelbear couldn't hold 30 FPS while
  screen-recording in early builds — expect the same wall, profile before adding.

Principles (Noita's lessons, taken as requirements):
- **Sleeping is the product.** Every system — CA, bodies, chunks, reactions —
  must have a "costs nothing when idle" state.
- **Bound every emergent process** (flood fills, reaction cascades, particle
  counts). Unbounded emergence is both the perf killer and the design killer.
- **Glitches must not kill the player** (Noita: wedged rigidbodies hurt enemies,
  never the player).
- **Communicate causality** (NetHack standard: the player should conclude
  "I wasn't careful," not "the game is buggy").

## 12. Tech Stack

**Adopted (2026-08-19, browser requirement): C++20 + WebGPU + WGSL — Dawn
(Google's WebGPU implementation, Vulkan backend) for native, Emscripten/WASM for
browser builds. Jolt Physics, GLFW, Dear ImGui (imgui_impl_wgpu), nlohmann/json,
CMake.**
- Browser technical demos and browser multiplayer are a product requirement.
  Vulkan cannot run in browsers; WebGPU is the browser compute API, and native
  WebGPU implementations run *on top of* Vulkan/D3D12/Metal. Writing against
  `webgpu.h`/WGSL once gives native + web from one codebase with zero shader
  rewrites — the anti-tech-debt choice.
- WebGPU constraints folded into this design: voxel data lives in **storage
  buffers** (no 16-bit / read-write 3D storage textures), ≤256 threads per
  workgroup, no push constants (small dynamic-offset uniform buffers instead),
  `mapAsync` readback (matches the async mirror pattern in §2 natively),
  buffer-binding size limits cap browser-build world size (native requests
  higher device limits; browser demos use a smaller resident cube).
- Accepted cost: a modest native-perf ceiling vs raw Vulkan (no subgroup ops,
  some API overhead through Dawn). Irrelevant at current scale; if it ever
  binds, the escape hatch is a native Vulkan backend behind the same engine
  interfaces — the WGSL kernels translate mechanically.
- Jolt: modern, MIT, excellent sleeping/activation model, easy mesh colliders,
  compiles to WASM (JoltPhysics.js proves it).

**Considered alternatives:**
- *Raw Vulkan + GLSL* (this doc's original choice): maximum native control, but
  the browser target would force maintaining a second host layer and a full
  GLSL→WGSL shader port — rejected as pure tech debt once web became a
  requirement.
- *Rust + wgpu*: memory safety and the same WebGPU portability; the ecosystem
  for this niche is thinner and the team codebase is C++. A defensible swap —
  the design above is API-agnostic.
- *Unity/Godot + compute shaders*: fastest first demo, but the engine fights you on
  memory residency, readback control, and headless server builds. Rejected for the
  long game.
- *Full-GPU architecture (no CPU gameplay/physics)* — proven by the BFS project
  (r/VoxelGameDev): enemy logic as a simplified GPU ECS (sorted component passes),
  physics as simplified position-based dynamics where every dynamic entity is a
  particle set. Eliminates the CPU↔GPU readback boundary entirely and makes
  whole-game determinism trivial (one domain). Rejected here because (a) PBD
  particle physics stacks poorly — its own author calls stacking "not ideal" —
  and convincing rigid debris is core to our fantasy; (b) GPU-side game logic is
  hostile to the moddable JSON/tag scripting that is this project's pillar, and
  far harder to debug. Worth revisiting if readback latency proves worse than
  expected (§14 risk 2) — it's the escape hatch.

## 13. Roadmap

Each milestone is playable/demoable. Don't start a milestone's "later" items early.

> **v0.4 (2026-08-19)** — M2 complete, M7 core complete, island-detection
> support-loss triggers. All selftest-gated (new: streamed-walk determinism +
> eviction/persistence roundtrip).
> **M2/M7 streaming:** toroidal residency on all three axes (§3) — world
> coords unbounded, window origin uniform, slot = chunk mod N by bitmask,
> unloaded space solid+inert at the window faces, color lattice in world
> coords. Streaming manager recenters one chunk per axis per tick with
> hysteresis; leaving planes RLE into the in-RAM `ChunkStore` (occupancy/
> modified-filtered), entering planes load from the store or generate on GPU
> (`worldgen.wgsl:list` over a slot list). Save format v2 = the store +
> window origin (chunk-coord-keyed), so streamed worlds round-trip.
> **M7 procgen:** worldgen rewritten as a pure function of world coords +
> seed (floor-div noise for negative coords, mirrored in C++): infinite
> hills, two cave bands carved as 2D-noise column spans (3D-threshold carving
> was rejected — it generates free-floating stone blobs that the island
> detector then correctly-but-endlessly converts to debris), lava pools on
> deep cavern floors, sparse ruin POIs per 256² tile, authored set pieces at
> their absolute coords (cave-free under pool rims: a breached rim drains the
> pool forever and the world never sleeps).
> **Debris:** GPU support-loss flags (§7) — the CA reports supporting-voxel
> removal next to solids (burnt stems, ember→ash, undermined slabs) through a
> side-channel buffer; CPU turns flags into cooldown-limited island checks.
> Powder-below now anchors islands (a slab resting on sand is supported).
> Fixes plants/structures left floating after fire or erosion.
>
> **v0.3 (2026-08-19)** — M5 complete, M6 complete, M2 save/load core. All
> selftest-gated (determinism hashes now cover explosions + particles).
> **M5:** GPU particle system per §5 — fixed-point integer state (24.8), double-
> buffered with indirect dispatch, ballistic DDA flight, reinsertion via a
> two-phase claim (atomicMax of a state-derived priority — order-independent,
> so the grid stays bit-deterministic; slot order never leaks into sim state).
> Explosions per §7: per-voxel occlusion DDA against material `hardness` (new
> JSON field), class-scaled ejecta. **Hard-won invariant: destruction kernels
> must be two-phase (mark reads pristine grid → apply writes)** — a
> single-phase version raced its own occlusion rays and broke determinism.
> Grenade projectile (G) + crosshair detonate (X) as the §8 projectile seed —
> CPU floats, but the grid only sees their ExplosionOps (MutationQueue). New
> render foundation: shared reversed-Z depth (raymarch writes frag_depth);
> particles/grenades/debris draw as instanced lit cubes composited exactly.
> **M6:** the full §7 debris pipeline — destruction events → bounded async
> region readback (≤64 chunks/tick through the readback ring) → CPU island
> detection (solid-only 6-connected components; touching the region boundary =
> anchored; >32k = abort) → islands leave the grid via exact-cell MutationQueue
> ops (`sim_mutate.wgsl:cells`) and become **Jolt** bodies (v5.3,
> CROSS_PLATFORM_DETERMINISTIC, greedy-merged box compounds, mass = Σ voxel
> density) carrying their voxel payload, rendered voxel-crisp as instanced
> cubes with the body pose. Sub-8-voxel islands crumble to their JSON `rubble`
> material. Terrain collision: localized marching cubes (Bourke tables) per
> chunk near live bodies, cached, invalidated from the dirty-flag snapshot,
> meshes as static Jolt bodies. Explosions impulse nearby bodies. Bodies are
> CPU gameplay state by design (§2): their grid effects flow only through the
> op stream. Selftest: pillar-blast scenario must produce ≥1 body that falls
> and sleeps.
> **M2:** versioned chunk-RLE world save/load (F9/F10, ~6x compression),
> selftest-verified: save → diverge 50 ticks → load → world hash restores
> exactly. Still open for M2/M7: toroidal residency + disk streaming beyond one
> resident cube + procgen — the next major phase; the chunk-granular file
> format and the chunk-fetch cache were built to serve it.
> Deferred consciously: player↔body collision, destructible bodies (re-split on
> damage), body re-fusion into the grid, particle↔media interactions.
>
> **v0.2 (2026-08-19)** — M3 complete + M2 core. Fullness liquids (state nibble =
> eighths, mass-conserving fall/equalize/split; fullness-1 films never spread, so
> pools settle flat and SLEEP — the v0 jiggle debt is paid). Dirty-chunk-list
> compaction + `DispatchWorkgroupsIndirect` for all 54 CA passes and the occupancy
> update (full-world scan only on hash ticks): per-tick sim cost now scales with
> activity, not world size. The DESIGN hierarchical dirty tree is intentionally
> replaced by a flat 4096-chunk compaction scan — the tree only pays at much larger
> residencies. Selftest gained a sleep assertion (settled world must be <32 active
> chunks; measures 4) and `--adapter low` for cross-vendor hash comparison (this
> machine exposes only the RTX 3060 Ti — still untested on a second vendor, risk #3
> stays open). Two hard-won invariants, now enforced in comments: the 3×3×3 color
> lattice is GLOBAL (chunk-local dispatch must offset by chunk coord since 16 ≡ 1
> mod 3), and reaction-driven growth must be decisively subcritical or chunks never
> sleep (see reactions.json stem note). M2 still open: disk streaming + toroidal
> residency.
>
> **v0.1 (2026-08-19)** — the M3 reaction system landed early, sandspiel-inspired:
> JSON reactions (pair/decay/emit rules with tag matching and direction filters)
> compiled to per-material GPU buckets, 31 materials, ~60 rules (fire/ember/ash,
> lava→glass melting, acid erosion chain, ice/snow, plant/seed/stem/flower growth,
> fungus, dust deflagration, sources/void, wandering mites), emissive rendering
> with volumetric fire, hot reload. Reactions run inside the 27-color passes
> (substep 0), write-reach ≤1, integer-only — determinism selftest still passes.
> Still deferred to full M3: fullness-based liquids, per-material viscosity beyond
> the tick-interval gate.
>
> **v0 (in progress, 2026-08-19)** — M0 + M1 + a deliberate slice of M3/M4 to get
> a walkable demo immediately: JSON material table (no reactions yet), **binary
> liquids** (no fullness nibble — accepted cost: pool surfaces jiggle and stay
> dirty, bounded by surface area; fullness lands in M3), and a simple AABB voxel
> character controller (fly/walk) driven by the async `mapAsync` chunk readback —
> which deliberately front-loads risk #2 (§14) instead of waiting for M4. Jolt +
> marching-cubes collision still arrives in M4. Browser (Emscripten) build is
> scaffolded but not a v0 exit criterion.

- **M0 — Skeleton** (foundation): window, camera, fixed-tick loop, 16³ chunk store
  with toroidal residency, hardcoded test world, raymarched rendering with
  empty-space skipping, debug overlay. *Exit: fly around a static 512³ world at 60 FPS.*
- **M1 — It falls**: GPU CA for one powder + one liquid + one gas, checkerboard
  passes, counter-based RNG, single-buffered rules from §4, brush tool to
  paint/erase voxels via MutationQueue, per-tick world hash proving determinism
  (same seed + same inputs → same hash, twice in a row). *Exit: dump 1M sand into
  a pool of water and it piles, displaces, settles at 30 Hz — reproducibly.*
- **M2 — It sleeps**: per-chunk dirty flags, hierarchical dispatch, neighbor waking,
  chunk streaming to/from disk, solid-boundary rule. *Exit: settled world costs ~0 ms;
  walk (fly) across a streamed world larger than residency.*
- **M3 — It's moddable**: JSON material/reaction pipeline, tag system, hot reload,
  validation diagnostics, fullness-based liquids, density displacement, 15–20
  starter materials (sand, water, oil, lava, acid, wood, fire, ember, ash, smoke,
  steam, stone, gravel, ice, flammable gas). *Exit: add a new material + reactions
  with zero engine recompile; acid erosion chain works.*
- **M4 — You're in it**: Jolt integration, capsule player controller, localized
  marching-cubes terrain collision, swim/slow traversal, first-person interaction
  (dig/place/throw). *Exit: walk through a cave, dig through a wall, wade through
  the water that floods in.*
- **M5 — It splashes and explodes**: GPU particle system, explosion ray destruction,
  projectiles with tag modifiers. *Exit: an explosion carves terrain, ejects debris
  particles that reinsert, splashes liquids.*
- **M6 — It collapses**: bounded flood-fill island detection with chunk-face
  acceleration, island → rigidbody conversion, destructible bodies, <8-voxel
  rubble handoff. *Exit: blow out a pillar, watch the ceiling section fall as a
  body, shatter it back into powder.*
- **M7 — It's a world**: compute-shader procgen (biomes, caves, surface), infinite
  streaming both axes + depth, points of interest. *Exit: endless explorable world.*
- **M8 — It's a game (vertical slice)**: enemies, health, a handful of spells built
  on projectile tags, alchemy v1 (reactions + status effects as content), death loop.
- **M9 — It's online (prototype)**: choose lockstep vs. server-authoritative (§10)
  based on measured determinism (per-tick hashes across two GPU vendors) and
  bandwidth data; headless build, 2–4 player LAN test. *Everything before this was
  built against the MutationQueue with deterministic kernels, so this milestone is
  plumbing, not surgery.*
- **Beyond**: GI/lighting, wand/spell crafting depth, persistent meta-progression,
  Steam networking, temperature layer, structural stress.

## 14. Risks (ranked)

1. **Island detection** — flagged by the one team that's done it as "hardest, not
   completely solved." Mitigation: bounded fills, chunk-face metadata, accept
   imperfection (M6, not M1).
2. **GPU↔CPU readback latency** for player/physics collision — the architecture
   stands or falls on the async mirror pattern. Prototype it in M0/M1, not M4.
3. **Determinism erosion** — one scheduling-dependent op, float sneaking into sim
   state, or a driver-divergent intrinsic silently breaks cross-GPU reproducibility.
   Mitigation: per-tick world hash asserted in CI-style test runs from M1; validate
   on two GPU vendors early, not at M9.
4. **Scope** — every system above is the *simple* version of itself on purpose.
   The Noita lesson: they shipped on rules a beginner could write; the magic is
   sleeping, bounding, and content, not clever kernels.
5. **Memory bandwidth** at large residency — 16 bpv and dirty-only dispatch are
   the levers; don't grow the voxel.

## 15. References

- GDC 2019: *Exploring the Tech and Design of Noita* — youtube.com/watch?v=prXuyMCgbTc
- 80.lv interview with Petri Purho on the Falling Everything engine
- Burkelbear Games (*Grimorium*) devlogs:
  - *Building a 3D Noita-Style Material Simulation From Scratch* — HN8rEaFEOXA
  - *Simulating Hundreds of Millions of Micro-Voxels in Real-Time* — BySRC4HwLYg
  - *Realistic voxel world destruction and rigidbody physics* — mWdlTZ_FoBc
- Paul Bourke, *Polygonising a Scalar Field* (marching cubes tables)
- Jolt Physics — github.com/jrouwe/JoltPhysics
- "BFS" — massively multiplayer voxel sandbox with cross-vendor deterministic
  lockstep GPU simulation (r/VoxelGameDev post 1tw2yen, dev: bonzajplc). Full-GPU
  ECS + PBD particle physics; global-grid raymarching with bbox-then-raymarch for
  dynamic objects; 8×8×8 node storage. Evidence that GPU lockstep works in practice.
