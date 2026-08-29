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
| Language / API | **C++20 + WGSL on native Vulkan** — port complete, Dawn/WebGPU REMOVED 2026-08-22 (see §12 and docs/PLAN_vulkan_port.md) | Browser requirement dropped 2026-08-22. Vulkan unlocks sparse residency (measured: 83% of the voxel buffer is empty pages — a 1024³ window for less memory than 512³ dense), explicit sync/memory control, async queues. WGSL stays the authoring language via Tint→SPIR-V — zero shader rewrites, and Tint is why the Dawn *checkout* remains a dependency |

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

### Paged residency: the voxel buffer is a page POOL, not a dense array

**As of the Vulkan port's phase 7 (`docs/PLAN_page_table.md`), where a chunk's
voxels live is an indirection, not an address.** A flat `pageTable` of one u32
per chunk SLOT holds either a page index into a pooled physical buffer or a
SENTINEL: `EMPTY`, or `UNIFORM(material)` for a chunk whose 4,096 words are
identical. A settled default-seed world is 84.8% sky, so it costs **4,975 pages
= 77.7 MiB resident instead of 512 MiB dense** — a 6.6x reduction, and the
mechanism that makes growing the window downward into solid bulk affordable.

**Paged is the DEFAULT residency mode** (2026-08-23, after both modes passed
the full suite at the phase-7 close). `--residency dense` remains the identity
map and the only live differential oracle: same scenario, dense vs paged,
bit-identical hashes — that comparison is the first diagnostic for any
suspected paging bug. Paged mode defends its own snapshot cadence in
`SubmitTick` (per-tick drains through the post-worldgen settle window, then a
bounded-staleness fallback), because §3.2's mirror starves without snapshots
and the pool is sized for a tightened mirror, not an unsnapshotted one.

Three properties this rests on, all load-bearing:

- **The CA is unaware of it.** Every world-coordinate voxel access in every
  kernel already routed through `cellIndexW`, so the indirection lives in the
  shared accessors in `common.wgsl` (`voxWordAt` / `voxWordIndex` / `voxStore`)
  and no sim kernel's own code changed. A kernel that computes a `voxels[]`
  subscript by any other means bypasses the table and reads another chunk's
  memory.
- **The table is DERIVED DATA.** Not hashed, not persisted, not replicated;
  rebuilt from chunk contents on every load, stream-in and worldgen. Two
  different page assignments for the same logical world ARE the same world,
  which is why `--residency dense` (the identity map) and `--residency paged`
  produce bit-identical hash sequences — the gate that proves the whole thing.
- **An index used as an IDENTITY is the SLOT index; only a memory address is
  the PAGE index.** The world hash, every per-cell RNG key, and the particle
  claim lattice all key on the slot. Feeding a page index into any of them
  would make the simulation a function of allocation history.

A GPU kernel cannot allocate, so every page a kernel might write is
materialized from the CPU BEFORE the command buffer is submitted, driven by a
conservative CPU mirror of the dirty set. Writes are structurally incapable of
reaching an unmaterialized page: the only way to obtain a writable word index
returns a distinguished no-word value for a sentinel chunk, and `voxStore`
tests it before indexing. A sentinel write is therefore a counted no-op, never
a corrupted bystander — and the counter is asserted zero by every gate.

### Art colour: mob/prefab skins are painted, world voxels are not

A creature is `meat` everywhere — that is what the CA reacts to, what a severed
limb becomes when it lands in the grid, what a fire spreads through — but its
skin is *painted per voxel*: different colours for eyes, tongue, claws, belly.
Material and colour are therefore two independent facts about a skin voxel, and
they live in two separate channels.

**This does not touch the world voxel.** Art colour exists only on mob/prefab
skins, which are a different representation from the grid (`PrefabVoxel`,
`DebrisVoxel`, the micro brick pool — all CPU rigidbody + render data, outside
the hashed domain). The 16-bit world cell is unchanged, so the rule above still
holds and rule 1 is untouched by construction: nothing here can reach a hashed
cell. The `settle-back` gate asserts exactly that — a *painted* body dropped
into the world settles as its plain material.

Where it lives, end to end:

| Stage | Carrier |
|---|---|
| authoring | editor paints a colour grid parallel to the material grid |
| `.vox` | a second model per limb, `"<limb>.col"`, same cells, byte = art palette slot |
| palette | art slots occupy the .vox palette from **255 downward**; material IDs still run upward from 1 (`kArtPaletteBase` = 128) |
| load | `voxload.cpp` pairs `.col` with its limb, folds it into `PrefabVoxel::color`, and **drops** the layer so it is not a limb and does not widen the prefab AABB |
| micro skin | brick payload is **16 bpv**: low byte material, high byte art slot |
| cube skin | `DebrisVoxel::color` (was padding); 4 bits on the GPU instance — this path is coincident-resolution only, and real characters all use the micro path |
| GPU | a reserved run of the material table (`kArtPaletteBaseGpu`), same trick as the stain palette — no new buffer, no new binding |

Two consequences worth stating. **Opacity is a brush property, not stored
state**: painting at 40% mixes into whatever colour the voxel already shows and
stores the *result*, so glazes layer the way real paint does while a voxel stays
one opaque colour. And **the palette is per-document, merged at load**: colours
are deduplicated across mob defs, so 128 slots cover a whole cast.

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
  - `nonEmpty` — any voxels at all (empty-space skipping for rendering & raycasts).
    Implemented as a packed per-chunk word: low 16 bits = non-air count, high 16 =
    ray-blocker count (solid/powder/opaque-liquid), so media-blind rays (shadows)
    skip chunks that hold only gas/translucent liquid (see common.wgsl packOcc)
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
  toroidal wrap. Eviction readback is ASYNC (post-v0.4): the leaving plane's
  copy into a pooled staging buffer is submitted before the slots refill
  (queue order keeps it correct), the mapAsync lands ticks later, and a
  pending-eviction set force-completes any in-flight chunk that streams back
  in or gets saved — the frame never stalls on a shift. Eviction
  save-worthiness reads the snapshot's occupancy/dirty flags, which lag the GPU
  by the readback ring: activity that starts on the trailing plane in those
  last ~2 ticks can be lost on re-entry — accepted (the trailing plane is ≥6
  chunks behind the player; only self-propelled fronts can be there).
  CPU-known writes (brush/explosions) mark chunks modified immediately to
  shrink that window.
- **Region files (post-v0.4):** the `ChunkStore` groups chunks into 16³-chunk
  regions. Unbound it is pure RAM (unchanged); bound to a world directory
  (`world.svd/` = `meta.svm` + `r_<x>_<y>_<z>.svr`) regions lazy-load on Get,
  dirty regions write on Flush, and an LRU budget (64 regions in RAM) spills
  to disk — long journeys stream to disk instead of growing RAM, and saves
  are per-region instead of one monolithic file. A bound directory is a LIVE
  world store (Minecraft-style), not a snapshot: LRU spills may persist
  chunks between explicit saves, F9 = flush checkpoint, F10 = re-fill the
  window from the directory. Binding happens on first save/load and is one
  directory per session; regen detaches without deleting files, so the last
  explicit save survives until the next save overwrites it. The old
  monolithic `.svx` (SVX2) format is retired.
- **Save-format hardening + entity persistence (2026-08-22, worldio.h):**
  `meta.svm` is `'SVM4'` and now records the exact BIT PATTERN of
  `kVoxelMeters` and the full material NAME table alongside `kWorldN`/`kChunk`;
  a load refuses any mismatch and names the exact field (down to "material id
  12 was 'lava', build has 'acid'") — material ids are baked into every saved
  chunk, and a silent voxel-size or table change is world corruption with a
  green build. The directory also gains an optional `entities.sve`: a TLV
  container of independently VERSIONED sections (`DBRS` debris bodies, `MOBS`
  mob instances incl. sever/carve state, `AVTR` the player avatar), written
  before `meta.svm` so meta's completed-save guarantee covers it. Unknown
  section ids are skipped (forward compat); adding a persistable system means
  adding a section via `game/persist.cpp`, never changing the container — so
  no category of game state is structurally unable to persist. Micro bricks
  are NOT serialized: they are derived render state, re-packed on load from
  the authoritative voxel lattices (§3's "derived data must be
  reconstructible"). Jolt bodies reload at their saved transform with zero
  velocity, DEACTIVATED — a settled pile reloads settled (§11's sleep
  invariant holds from tick one); anything saved mid-flight lands where it
  was, accepted. Entity state is CPU-float gameplay state outside the hashed
  domain (§7), so the grid hash round-trip is unchanged.
- **Unloaded space is treated as solid and inert** so liquids can't drain off the
  edge of the loaded world (Burkelbear's solution; adopt it verbatim).
- Overworld draw distance beyond the window is handled by the render-only
  far-field cascades (§9) — the streaming horizon is no longer visible from
  the surface. Underground, darkness still hides it.

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
- **THE CA IS THE BULK-TRANSPORT TIER, and that is a ratified decision**
  (2026-08-25; `docs/RESEARCH_water_architecture.md` §7, merge a2e723e). The
  open question was whether these liquid rules should be DELETED in favour of
  MPM-only transport. They should not: an MPM-only 10×10×2 m lake is ~1.6M
  particles ≈ 126 ms/tick, and that ceiling belongs to the method, not to the
  implementation. So ownership is split BY STATE, not by material —
  **the CA moves water that is SETTLED, the solver moves water that has
  MOMENTUM**, and a cell is in exactly one of the two representations at a
  time, which is what makes two movers safe. The four defects that made the CA
  look like a dead end (a fullness-1 cell that could never spread, a
  `liquidEqualize` staircase that was a stable equilibrium, whole-cell
  4-direction descent, and a settled path that never re-marked its chunk) are
  fixed rather than routed around; the `ca-slope` gate holds the result at
  95.3% of a pour arriving down a stepped ramp with the box asleep.
- **LEVELLING is a separate problem from FLOWING, and needed two more rules**
  (2026-08-25). `ca-slope` asks whether water gets down a hill; it says nothing
  about the shape water rests in once it is somewhere flat, and the shape was a
  DOME. Lateral spread into air is halving so only the rim ever touches air, and
  `liquidEqualize` is 2 so no adjacent pair on a slope of one eighth per cell is
  ever unstable — `(8,7,6,5,4,3,2,1)` was a stable resting state. A splash on a
  pond therefore relaxed in place into a mound instead of dispersing, which is
  visible now that b799a58 draws partial cells at fullness height. Two rules,
  both in `sim_step.wgsl` with their termination arguments in full:
  - **`filmPressed`** — a film too thin to split moves WHOLE into an air
    neighbour when another lateral neighbour is thick enough to split. This
    frees the rim, and the dome then unwinds from the outside in. It is neutral
    in `SUM(f*f)`, so the gate is chosen to make it terminate: the cell it
    vacates is a face neighbour of the cell that justified the move, which can
    therefore split into the hole, and splitting strictly decreases `SUM(f*f)`.
  - **`bridgeLevel`** — a cell may equalize between TWO OF ITS OWN lateral
    neighbours. Write reach is unchanged (both are one cell away — the same
    licence `tryMove` spends on self and one neighbour), but the pair straddles
    the mediator, so on a slope of one eighth per cell it differs by 2 and the
    ordinary equalize threshold bites. It is an ordinary equalize, so it
    inherits the `SUM(f*f)` termination argument with nothing new to prove.
    Requiring the mediator to be this same liquid is what makes it physical
    (pressure crosses a connected body of water) and what removes the need for a
    diagonal crack check.
  - `sim.liquidMinFilm` is back to **1**, so the resting film is the thinnest
    representable and one placed voxel spreads to eight cells of one eighth.
  - **The limit, stated because it is irreducible at reach 1**: the joint
    fixpoint is a surface sloping one eighth per TWO cells. Halving that again
    needs a look 4 cells wide, and a mediator can only bridge cells inside its
    own write reach. A genuinely level wide surface needs a global pressure
    solve (the MPM has one) or a mark/apply flux pass
    (`RESEARCH_water_architecture.md` option B).
  - Gates: `ca-level-one` (one placed voxel → 8 cells of one eighth, no slack),
    `ca-level` (216 eighths → 135 wetted columns, one cell deep, ≤4 eighths
    anywhere; was 57 columns and 6 eighths), `ca-level-pond` (the reported case:
    the same blob on standing water).
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
- **The stain layer (2026-08-20)** claims 7 of those 8 spare bits: bits 24..27 a
  stain AMOUNT (1..15) and bits 28..30 a stain TYPE (1..7, 0 = unstained). Bit 31
  stays reserved for `kCellOpIfAir`, a transient CPU→GPU message flag that
  `sim_mutate` masks off before storing. Constants live in `world.h`
  (`kStain*`) and are mirrored in `common.wgsl` (`STAIN_*`).
  - This is the "extra per-voxel state" §3 anticipated, taken from the spare
    byte rather than from a sparse aux layer — at 134M resident voxels a
    byte-per-voxel side buffer would cost 134 MB for what fits in bits already
    being paid for. The aux-layer plan still stands for anything wider.
  - Type is a small PALETTE, not a material id (12 bits would not fit and the
    renderer only needs to know which of a handful of stain looks to paint).
    Slots are registered at load from the `stain` blocks in materials.json, and
    their colours are mirrored into reserved material-table entries at
    `kStainPaletteBase` so the renderer needs no new binding and stains
    hot-reload with R.
  - **Stain is sim state, so it is hashed.** `sim_occupancy` folds bits 24..30
    into the world hash alongside the material and state nibble; without that,
    `--selftest` could not tell a correctly-stained world from one where
    staining diverged across vendors (rule 1). The stamp byte stays excluded —
    it is scheduling bookkeeping, not state.
  - Every sim write that means "same voxel, moved or refilled" goes through
    `packVoxKeepStain` rather than `packVox`, or ordinary liquid flow scrubs
    the stain off the world.
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

### Day/night, and sunlight as a sim input (2026-08-20)

The world runs a day/night cycle, and sunlight is a real input to the CA:
exposed water evaporates in the sun, snow melts by day and water freezes at
night, plants only grow in daylight, fungus prefers the dark. That makes the
sun part of the *simulation*, not just the renderer, so it has to satisfy
rule 1 (bit-determinism). Three decisions follow from that, and each of them
is the reason a more obvious approach was rejected.

**1. The cycle is driven by the tick, never the clock.**
`DayPhaseForTick()` (world.h) maps the tick counter onto a 16-bit integer
phase — 0 = midnight, 0x8000 = noon. Same seed + same tick ⇒ same phase ⇒ same
world hash, on every machine. A wall-clock cycle would have been simpler and
would have made every daylight-gated reaction non-deterministic and
non-replayable. The renderer derives its float sun/moon vectors from the same
phase (`ComputeSkyState`), so what you see and what the sim does cannot drift.

**2. Sky exposure is a one-cell test, not a column walk.**
`seesSky()` looks at the single cell above and asks whether it is a ray
blocker. The natural implementation — march up until something blocks, so a
pond in a cave is properly "indoors" — **breaks determinism**, and it is worth
being precise about why, because the argument is easy to get backwards:

> The 3×3×3 colour lattice guarantees that two cells acting in the same pass
> are ≥3 apart and that *writes* reach ≤1 cell. It guarantees nothing about
> *reads*. A 48-cell probe column crosses dozens of cells that other threads
> in that same pass are legally writing, so whether the probe sees a cell
> before or after its update is scheduling-dependent.

That reproduced as a hash divergence at tick 1. The one-cell test stays inside
the guarantee. The cost is that "sky" means "nothing directly on top of me", so
water in a lit cave also evaporates — a content inaccuracy, not a correctness
one. If that distinction ever matters, the deterministic way to get it is a
separate mark/apply pass over a sky-exposure buffer, exactly as
`sim_explode.wgsl` does.

**3. Light-gated rules do not hold their chunk awake.**
The unconditional rules set `keepAwake` to mean "this neighbourhood is
reactive, look again next tick", which is right for chains that terminate
(fire burns out, growth stops). A light-gated rule has no terminus: it stays
matched for as long as the sun is in the right part of the sky, i.e. thousands
of ticks. Letting it keep its chunk awake pinned 292/32768 chunks active
against a budget of 32 — rule 2 violated by a rule that *looked* harmless.

Instead the phase change itself is the wake signal: `Simulation::
EncodeWakeAll()` re-dirties every chunk on the few ticks per in-game day where
daylight switches on or off. Between those boundaries, a chunk whose only
pending work is light-gated sleeps.

**What this rules out.** A permanent emitter still cannot settle at any chance
value — an overnight "leaves emit dew" rule was tried twice and abandoned,
because every leaf is a source and the steam it makes re-dirties its chunk
forever. Sunlight can *drive* processes that consume something finite; it
cannot be hooked to a rule that manufactures matter indefinitely. The failed
attempt is documented in `reactions.json` so it is not tried a third time.

### The sky is a solar system (2026-08-23; `sim/celestial.*`)

Everything above still holds — the *clock* is what changed. Where the sun's
position used to be a tilted great circle traced by a phase ramp, the planet
now runs a **Keplerian orbit** around its star, spins on a tilted axis, and
carries **two moons** on their own inclined, eccentric orbits. Seasons, lunar
phase, the beat between the two moons, and eclipses are all *consequences* of
that geometry rather than authored curves, which is the same "no closed-ended
systems" argument §6 makes about materials: the orbital elements are
`tuning.json` rows, so a different sky is a data edit.

**The renderer barely changed.** `ComputeSky()` fills the same `SkyState` the
sky shader already consumed, plus moon B and eclipse coverage; the starfield,
galactic band, nebulae, aurora and the moon's maria/terminator code are
untouched. What they receive is better numbers.

Four properties are load-bearing:

- **Pure function of the tick.** Every element is recomputed from epoch on
  every call — Kepler's equation from a mean anomaly is O(1), so there is no
  reason to integrate, and an integrator would make the sky an hour into a
  session depend on the frame rate that got there. Kepler is solved by a
  **fixed** six-step Newton iteration, never a convergence loop, for the same
  reason the CA forbids scheduling-dependent outcomes: a trip count that
  depends on the value is a way for two machines to disagree.
- **The sim still reads only the integer phase.** `TickParams.dayPhase` is
  unchanged, integer, and the only thing the CA sees. This file is float
  throughout and cannot reach a voxel. The two agree because both are driven
  by the same celestial tick.
- **One number per fact.** A moon's apparent size comes from its orbit
  (`dayNight.moonAngularRadius` scaled by distance) and the eclipse test and
  the drawn disc read that same number. The old render-side `moonRadius` knob
  was deleted rather than kept: two answers to "how big is the moon" is
  exactly §3's unowned-diverging-representation trap, and here it would have
  meant eclipses that do not line up with the discs you can see.
- **`sunAzimuth` is a rotation of the OBSERVER, not of the clock.** Folding it
  into the spin angle also rotates *time*: at 24° it moved noon 1.6 game-hours
  off `dayT` 0.5 and silently desynchronised the visible sun from the phase
  the reactions are gated on. It is applied as a yaw of the finished horizon
  vector.

**The `celestial` gate is the reason any of this is trustworthy**, because a
screenshot cannot tell a real solve from a plausible-looking ramp. It asserts
properties, each of which fails under a specific plausible bug: the solar
*year* closes to 0.000° of accumulated azimuth (a sidereal/solar sign error
overshoots by exactly two turns — the original bug, and locally invisible);
the sun peaks at `dayT` 0.5 within the equation of time; the seasonal swing is
2× the axial tilt and peaks at `90 - |lat - tilt|`; each moon hits its
*authored synodic* period (8.00 and 9.00 days — dropping the synodic→sidereal
conversion gives 8.7 and nobody notices for a week); the 8/9 phase pair does
not repeat inside 72 days; eclipses occur but stay under 0.3% of samples; and
a disengaged `CelestialClock` is bit-exactly the identity map. It also caught
a unit bug on the first run — moon radii read as radians instead of degrees,
a 97° moon that eclipsed the sun a third of the time.

### The dev time-scale slider, and why it moves the sim

The overlay's *time speed* slider scales a `CelestialClock` (`world.h`) that
feeds **both** the rendered sky and `TickParams.dayPhase`. Scaling only the
render would show a sun racing across a world that ignores it — useless for
tuning weather, which is the reason to want the control at all. So at 100×
water genuinely freezes and thaws while sand still falls at 30 Hz.

That makes the world hash a function of the slider, deliberately. Two things
keep it from touching rule 1:

- The clock is an **exact rational counter** (`scaleNum/scaleDen` with the
  remainder carried), never a float accumulator, so the integer path into
  `dayPhase` is unbroken and reverse time is the exact mirror of forward time.
- It is **disengaged until the slider first leaves 1.0×**, and while
  disengaged `SimTick()` returns the sim tick byte for byte. Every headless
  path — `--selftest`, `--shot`, `--frames`, every gate — is therefore
  structurally unable to observe the feature, and the pinned hash `7cfa2420`
  is unmoved.

The day/night wake handshake compares against the clock's *previous* value
rather than `tick - 1`: at 100× the clock jumps ~100 phase-ticks per sim tick,
and comparing to `tick - 1` would test a phase the world never occupied and
sail through several dawns without ever waking a chunk.

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
- **Reinsertion is the one dirty-writer the CPU cannot predict**, and that makes
  the live particle count a load-bearing *sim* quantity, not just a HUD number.
  `resolve` writes a voxel and marks a chunk dirty at a location chosen on the
  GPU, so "no CPU inputs this tick" does not imply "no work next tick" while
  anything is in flight. The CA's settled-tick skip (ROADMAP_scale.md §3.4,
  §3.2d) therefore requires its licensing snapshot to report **both**
  `activeChunks == 0` and `particleCount == 0`, and the page table's flight
  shell uses the same off condition (`PageTable::ApplyParticleShell`). Both are
  sound only because the snapshot's particle count is captured downstream of
  `resolve` in the same tick — a landing is never invisible to the snapshot of
  its own tick, and a particle that lands is still counted on the tick it lands.
  **A new GPU-side particle source must land inside that count**, or both
  mechanisms will reason a world settled while matter is still moving.

**Gameplay projectiles are a separate CPU system** (§8) — they carry game logic.

### MLS-MPM liquid (2026-08-22..23; `sim_fluid.wgsl` + `sim_fluid_seam.wgsl`, docs/PLAN_mpm_fluids.md)

The EXCITED state of liquid: an MLS-MPM particle solver (plan Phases 0+1)
plus the plan's §7 excite/settle SEAM (Phase 2, 2026-08-23) converting both
ways between settled fullness voxels and particles. The MLS-MPM core (Hu et
al. 2018) is ported to Q16.16 integer fixed point, P2G scattering through i32
`atomicAdd` (associative, so scheduling cannot move the sum), sparse 16³-node
grid blocks over exactly the chunks that hold particles, terrain boundary
conditions read live from the voxel buffer through `voxWordAt`, 6 substeps per
tick. The chunk→block MAP is built ONCE per tick (`PT_FLUIDMAP`), not once per
substep: displacement is CFL-capped at 2.7 cells/tick and `mark` pads its
support by 3, so the padded set is a superset of every substep's exact set.
The node ACCUMULATORS are still cleared per substep. Every fluid dispatch is
indirect off a GPU-owned count, so a world that has poured water and settled it
records the tables (the CPU count is monotone by design — recording must never
depend on readback timing) and costs ~0 ms.

THE SEAM (`sim_fluid_seam.wgsl`) is the only fluid code that writes voxels,
and it is INSIDE the hashed sim domain — deterministically. Per tick, around
the solver substeps: a slot-order ping-pong COMPACTION removes dead particles
and rebuilds the GPU-OWNED live count (`fluidArgsStage[7]` — settle kills and
excite births mean no CPU count can be authoritative; per-particle passes
dispatch indirectly); CPU spawn ops append; EXCITE converts disturbed settled
liquid to particles (one per fullness eighth, jittered sub-cell lattice via
`hash3`, hydrostatic pre-compression seeded into J from a per-column depth
scan so a reawakened lake holds its own weight instead of jello-popping);
after the substeps, SETTLE converts calm blocks back (per-slot max-speed
calm counters, `sim.fluidSettleTicks` consecutive ticks under
`sim.fluidSettleEps`, ≤16 blocks per tick with a COLUMN exclusion, per-column
segment-pooled bottom-up refill, all-or-nothing refusal per block — mass is
EXACT integer eighths in both directions, and stains ride the particle's attr
word round-trip). Excite triggers: (a) settled liquid with air below —
gated by `sim.fluidExciteMode`, **DEFAULT ON since WP5** (2026-08-25); (b)
progressive wake — a grid node at an active/settled interface moving above
`sim.fluidWakeSpeed` (4× the settle threshold: hysteresis) wakes the
neighbouring settled cell, always on, hash-safe because it needs existing
particles; (c) DIAGONAL FALL (WP3) — a settled cell resting on terrain with an
EMPTY lateral neighbour whose own below-cell is also empty, i.e. the water is
perched on a ledge and could fall diagonally. **Default OFF since WP5**
(`sim.fluidExcitePerch`), on the EXCITE side only — see below.

**WP5: THE FLIP, AND WHAT IT COSTS** (2026-08-25; world hash 7b01cfd8 →
58b27f33). `sim.fluidExciteMode` ships at 1, so world water responds to being
dug under. The plan's other half — deleting the CA's liquid movement — was
REJECTED; see §4's movement rules and `RESEARCH_water_architecture.md` §7.
Three things had to land with the flip:

- **THE BURST IS BOUNDED** (`sim.fluidExciteCeiling`, default 8,000
  particles; `sim.fluidExciteRate`, 4,096/tick). Excite is a per-cell trigger
  with no notion of "only wake what the disturbance can reach". Fixing the CA
  did NOT remove that, and the reason is worth remembering: while the CA
  drains a body, its partial descent leaves a transient gap under cells all
  over the body, so trigger (a) — "air below" — fires far beyond the hole. On
  the `worldlake` bench scene (worldgen's authored 347,832-voxel lake, 5×5
  shaft opened under a body that is provably asleep first) with the ceiling
  lifted to the pool: 352 → 1,916 → … → 262,144 live, the whole pool, held at
  ~70 ms/frame to end of run. That is the reported "it turns the whole lake
  into fluid".
- **REFUSAL IS GRACEFUL, and it is a property of the hybrid rather than of any
  fallback code.** Refused water is still SETTLED water, and the CA moves
  settled water. So the ceiling costs COVERAGE — how much of a body is
  visibly in motion — and not drain progress: measured on that same puncture,
  eighths delivered to the sealed chamber in 400 ticks are 73,672 / 72,996 /
  74,572 / 75,599 at ceilings 4k / 8k / 16k / 32k against 70,743 for the CA
  with no seam at all, i.e. FLAT, while frame p95 runs 24.9 / 27.1 / 28.8 /
  34.8 ms. The ceiling is a look knob above the point where it stops clipping
  bodies that were never going to burst.
- **THE PERCH TRIGGER IS OFF**, and that is a measured reversal of WP3's
  expectation, not a preference. It existed to unstick water the CA had parked
  on a slope, and the CA no longer parks water on slopes: with it on vs off,
  `pond68` produces 1,150 vs 1,150 excite candidates and `worldlake` 169,616
  vs 169,616 — byte-identical — for 24% more seam time. Worse, on the sealed
  ramp of the `ca-slope-hybrid` gate it prevents water from ever SETTLING (8
  settle commits over 400 ticks with it on, 369 with it off): anything settle
  produces, the perch trigger immediately takes back. That is the settle↔wake
  thrash WP3 warned about, arriving through the trigger rather than the wake
  speed. It stays behind a knob rather than being deleted because
  `settleCheck`'s stability veto must keep evaluating the FULL predicate —
  settle refusing more than excite takes is the safe direction of the
  hysteresis; the reverse oscillates.

KNOWN RED, recorded rather than fixed: `ca-slope-hybrid` finds a mass leak in
the SETTLE path (~0.4 eighths per settle commit) that is invisible on
large-body scenes because they barely settle. It is not excite (excited ==
emitted exactly), not reactions (consumed == 0), not an audit-bounds artifact,
and not the CA (the excite-off arm is exact). See `tests/BASELINE.md`.

**WP5b: THE FOUR THINGS THE FLIP EXPOSED** (2026-08-25; world hash
58b27f33 → dc666ada). Playing WP5 turned up four defects, and three of them
are one hole with three faces.

- **SETTLED LIQUID NOW HAS MASS IN THE SOLVER** (`sim.fluidSettledMass`,
  default 1.0). Until now `fluidSolid()` blocked solids and powders only and a
  fullness voxel scattered nothing into the node grid, so the two
  representations passed straight THROUGH each other: MPM water poured onto a
  basin filled to the rim fell to the BED. `clearGrid` now seeds every node
  inside a settled liquid cell with `fullness/8 × restDensity` of
  zero-velocity mass before P2G, which is the standard static-boundary
  treatment. Pressure then holds the pour up; the momentum divide in
  `gridUpdate` dilutes an impacting jet against static mass, which is a still
  pool's drag. It is deliberately NOT a prescribed-zero-velocity boundary —
  leaving node velocity as (real momentum / total mass) is what keeps the WAKE
  trigger alive at the point of impact, so a splash still excites the water it
  lands on. New `fluid-onwater` gate, which runs the same pour at
  `fluidSettledMass` 0 and 1 on the same build: the control converts 8,136 of
  the box's 8,144 eighths — the entire basin — and leaks particles out through
  the shell; the shipped arm peaks at 1,274 (a crater), craters 2 cells into an
  8-deep pool, and is fully back to settled voxels with exact mass.
  Second-order effects, both measured on `--fluid-bench basin`: settling is no
  longer self-disrupting (a chunk converting no longer drops the density that
  was holding its neighbours up), so `fluid(substep)` falls 4.26 → 1.44 ms and
  frame p50 14.4 → 5.8 ms, and wake candidates over 400 ticks fall 479 → 7
  because a still pool's nodes now read still.
- **THE EXCITE CEILING WAS BEING CHARGED AGAINST SPAWNS.**
  `sim.fluidExciteCeiling` documents itself as the standing size of the
  EXCITED region and explicitly exempts explicit spawns — but the budget
  subtracted the whole live pool, spawns included. A pour is a spawn, so the
  exemption was a fiction: any pour larger than the ceiling zeroed excite's
  budget permanently. Measured on `basin` (15,360 poured against an 8,000
  ceiling): 479 candidates, **0 emitted, 100% refused, for all 400 ticks** —
  the reported "water falling past a waterfall never becomes MPM". Excited
  particles now carry `FP_EXCITED` (attr bit 22), the compaction counts them,
  and the ceiling is charged against that; the pool is still bounded
  separately by `kFluidCap`. Same scene after: 0 slots refused.
- **THE SETTLE EXCLUSION IS A COLUMN RULE.** It used to refuse any pick within
  one chunk in ANY direction, so a pool spanning 2×2 chunks — the fluid lab's
  own 20×20 basin — could settle only one of them per tick, which is the
  reported "one quadrant stabilises first, then the rest in discrete steps".
  A column walk touches only its own (x, z) column, so the write hazard is
  exactly "same chunk column, within one chunk in y"; lateral neighbours may
  settle together because `settleCheck` and `settleCommit` are separate pass
  rows and the barrier between them means every stability probe in the tick
  reads pre-commit voxels, identically on every device. `basin` settle commits
  over 400 ticks: 7 → 12.
- **LIQUID HAS A MINIMUM FILM** (`sim.liquidMinFilm`), a split floor: a cell
  splits into air only when `f >= 2 × minFilm`, so both halves clear it.
  Same-liquid equalize is untouched (ponds still level) and descent is untouched
  (water still runs downhill at any fullness); a film too thin to split still
  steps off a one-voxel riser whole, which keeps terrace treads draining.
  `hill` basin capture is unmoved by the floor at 51.6% / 52.5% (excite on/off).
  It shipped at 2 to stop one placed voxel becoming eight cells of one eighth,
  and went **back to 1** on 2026-08-25 when the owner asked for the opposite —
  the flattest state an eighth-quantised lattice can hold is exactly one eighth
  per wetted cell. See "LEVELLING is a separate problem from FLOWING" in §4 for
  the two rules that landed with it; the cost of 1 is that a one-eighth film is
  below the solver's density support, so the thinner the resting film the more
  water the CA owns outright — which is why the excite seam grew a surface-step
  trigger in the same change.
- **THE SURFACE-STEP EXCITE TRIGGER** (`sim.fluidExciteStep`, default 2 cells).
  A splash landing on a pond satisfies none of the other triggers: there is
  water directly below it rather than air (not (a)), the cell below its lateral
  neighbour is that same water rather than a void (not (b)), and until something
  is already moving nearby there are no fast nodes to wake off. So it stayed CA,
  and the CA has no momentum — it relaxed in place into a mound. Trigger (d)
  takes a free-surface cell whose own water surface stands `exciteStep` whole
  CELLS above the surface in a lateral neighbour's column. Measured in cells
  between two water surfaces, not in eighths between two cells: plan §6's
  eighth-level trigger (c) was rejected twice because a settled column carries a
  couple of eighths of shot noise and bottom-packing puts a deeper column's top
  cell beside a shallower one's empty cell, so any eighth threshold is true at
  the surface of every pool that is not perfectly level. It also only looks over
  WATER (the neighbour column must hold liquid), so a puddle spreading across
  dry ground never fires it — this is a lakebed trigger, not a spill one. It is
  excite-side only, like `fluidExcitePerch`, and that is the unsafe direction of
  the hysteresis asymmetry; what holds it in practice is that the CA flattens a
  2-cell step by itself, so the configuration does not persist for the two sides
  to fight over. Measured on `ca-level-pond`: with the trigger off the seam
  NEVER fires on a splashed pond (0 excited); at 2 it converts 454 eighths and
  settles all 454 back, mass exact, box asleep 60 ticks later.

SETTLE IS EXCITE-STABLE BY CONSTRUCTION (WP3, plan §6 item 2). `settleCheck`
runs a second test after column feasibility: every cell the walk would write
must FAIL the geometric excite triggers, so a settled configuration can never
immediately satisfy one. It runs regardless of `sim.fluidExciteMode`, which is
the point — the reported "water clumps and settles on the hill instead of
flowing down" happens at mode 0, where nothing can re-excite it and the CA's
`liquidEqualize` then holds the staircase as a stable equilibrium forever. The
test is asked only at the BASE of each water column (the cell whose own below
is not this liquid), because "perched" is a statement about the ledge a body of
water stands on, not about its surface: asked at every level it is true of
every pool that is not level to one eighth, since the column walk bottom-packs
and a deeper column's top cell always overhangs a shallower neighbour's empty
one. The excite side applies the identical base restriction, so {cells excite
takes} ⊆ {cells settle refuses to create} and the pair cannot oscillate. That
is a SUBSET, not an equality, since WP5 turned `sim.fluidExcitePerch` off: the
veto keeps evaluating the full predicate while excite no longer acts on the
perch arm of it, and the asymmetry is deliberately in that direction only.
Settle refusing MORE than excite takes is safe — the water stays particles a
while longer. The reverse would let settle create a configuration excite
immediately tears up again. At mode 0 a base cell holding >= 2 eighths is
exempt — that is the CA's own
lateral-spread threshold (`sim_step`'s `LIQ_SPLIT_MIN`, i.e.
2 × `sim.liquidMinFilm`), so the CA will move it
and refusing would cause the freeze rather than prevent it.

THE VETO IS PER-COLUMN, NOT PER-BLOCK. Settle commits a 16³ block, and column
feasibility is still all-or-nothing across it (partial conversion of a column
is what would drop or invent mass). Excite-INSTABILITY is not: it is a property
of one column, and refusing the block for it over-reaches badly. Measured on
the sealed `fluid-excite` chamber, whose upper pool rests on an internal floor
with a carved 4×4 drain plug: ~16 columns at the plug lip are genuinely perched
and correctly vetoed, and they were refusing all 169 water columns of a flat
pool every tick, forever. `settleCheck` now publishes a per-column bit
(`SP_COLBAD`); `settleCommit` skips exactly those columns and `settleKill`
spares exactly their particles, so refused water stays particles and the ledger
balances column by column. The hysteresis guarantee survives the split: a
refused neighbour column keeps its particles, so `seamNeighbourState` reads its
excited eighths instead of its settled fill — nonzero either way, so the
predicate cannot tell "settled" from "refused". A block that loses columns to
the veto halves its calm counter, as a fully refused one does, so an awkward
pool gets a cooldown instead of re-running the whole window forever.

THE SOLVER'S FREE SURFACE IS NEVER AT REST, and every speed test in the seam
corrects for it. Pressure comes from density ≥ rest, so the top layer of any
pool has none, and `gridUpdate` adds `gravity / substeps` to it every substep
with nothing to cancel it. Velocity is overwritten from the grid each substep
(pure PIC+APIC), so it does not accumulate — it sits at exactly one substep of
gravity forever, 3.3 vox/s at the shipped defaults. Because the calm judgement
is a MAX over a chunk, one surface particle vetoed every pool in the engine,
and the wake trigger fired on every settled cell touching any fluid node.
`seamRestVy(vy) = min(|vy + gravity/substeps|, |vy|)` strips it in all three
tests. The `min()` is forced, not defensive: `gridUpdate` applies the terrain
BC *after* gravity, so a node resting on a floor reads exactly 0 while a
free-surface node reads exactly `−gravity/substeps`, and only the min maps both
to 0. This removes a systematic offset, NOT the threshold's dependence on
gravity — `sim.fluidSettleEps` still has to clear the genuine turbulence of the
scene, which scales with g (see `tuning.h` for the measured sweep).

Excite marks candidates in
voxel scratch bits 19..23 (set by detect, consumed the same tick by emit —
budget refusals restore the word), assigns emission offsets by a slot-order
scan (never first-come atomics), and refuses whole slots past the
`kFluidCap` budget — refused water simply stays settled and retries.
Materialization: the seam never tests the page table (readback-timing state —
a branch on it would be nondeterministic); instead excite writes only into
this tick's dirty chunks (§3-covered) and settle only into ≥8-tick-calm
fluid blocks, which the block-list snapshot readback has long since fed to
`PageTable::UpdateFluidChunks`. `pageFaults == 0` on every gate is the
tripwire.

A world that never spawns fluid is byte-identical to one before this system
existed (the pinned determinism gate proves 7cfa2420 stands). The fluid's own
bit-determinism is gated by `fluid-det`, which now also audits the seam:
twice-run particle-buffer + world-hash equality AND exact mass conservation
(spawned eighths == live fullness + settled voxel eighths — the 2026-08-23
run settles the whole 512-eighth pour back to basin voxels). Passing on the
RTX 3060 Ti; cross-vendor remains open exactly as it does for the CA itself.

Particles carry their IDENTITY in a packed attr word (32-word / 128 B struct,
power-of-two stride): material id (settle writes it back; splash droplets and
staining key on it), fullness eighths, and stain type/amount excited out of
the voxel's stain bits. The species id (0..3) survives as the grid's
mass-channel / render-colour grouping, derived from the material at excite
time.

ENVIRONMENT PARITY (Phase 2's §6 slice) runs through ONE per-cell bridge
buffer, `fluidCellScratch` (intent word from the seam, flags from the CA):

- REACTIONS: `doReactions` synthesizes an excited cell as a liquid neighbour
  of the particles' material (last tick's block map + node mass + intent),
  so authored PAIR rules — freezing, absorption, plants drinking — work
  against excited water with zero new authoring surface. A rule that TAKES
  the neighbour sets the cell's consume flag; the seam's `consumeApply`
  kills the whole cell bin the same tick (consumption granularity is the
  voxel-eighth, order-free, mass-exact — the fluid-react gate audits
  standing + live + consumed == placed). Transitions that PRODUCE matter
  write ordinary voxels into the (air) cell; every phase change crosses the
  seam through the voxel form (plan §6.6).
- CONTACT STAINING: `particleTick` scatters each particle's stain (carried
  attr stain beats the material's authored one) onto solid/powder face
  neighbours as intents; `stainApply` rolls `sim.fluidStainRate` per cell
  and merges with the CA's rules. Settled water then WASHES foreign stains
  exactly as CA water does — the fluid-stain gate observes both halves.
- SWIMMING: `mirrorFold` packs excited-fluid eighths for the 27 CPU-mirror
  chunks (one byte per cell, `TickParams.mirrorBase` = the readback's own
  clamp) into the snapshot; `World::FluidEighthsAt` folds it into the
  player's `kindAt` ahead of the voxel mirror, so `inLiquid`/submersion/the
  waterline frame see particles as water. Zeroed whenever no fluid is live.
- SOUND: a burst of excitement in one snapshot (>= 64 eighths) fires water's
  Impact cue at the last exciting chunk's centre — the Break-event
  precedent: presentation-only, driven from the readback, never in the
  hashed domain.
- RENDER SEAM: `fluidMassAt` (the one producer under the MPM isosurface)
  takes max(particle mass, settled-liquid fullness x rest), so the
  isosurface's boundary taps meet the voxel surface without a gap; the
  back-to-front stack still prefers a nearer CA liquid interface. Making the
  two LOOK like one body is a separate problem — see "The render seam: one
  lake, two representations" under the MPM fluid surface below.

KNOWN LIMITS (Phase 2): excite converts non-viscous liquids only
(moveEvery <= 1 — lava/blood stay CA until per-material fluid dynamics,
plan Phase 7); splash droplets from STAINED water carry the material, not
the carried stain; frontier neighbour-count scaling sees excited fluid as
air; a sealed, undamped pool at stock stiffness can churn indefinitely
(sim.fluidDamping defaults to 0 — the settle gates document the tuning that
calms adversarial geometry, and Phase 7 owns the defaults).

THIN FILMS ARE INERT IN BOTH REPRESENTATIONS (WP3 measurement, the open one).
A film shallower than about one cell gathers rho well below rest across the
solver's 3-cell B-spline support, so its EOS pressure is zero-clamped and the
MPM has no lateral driving force at all: it sits where it lands. The CA cannot
move it either below 2 eighths (`stepLiquid`'s lateral-spread gate). The hill
bench scene shows this directly — 440 ticks after the pour stops, a puddle
rests on every tread of the ramp and the catch-basin capture sticks at 51%.
Settling those puddles is NOT the fix (that is exactly the mid-slope freeze),
and the seam correctly refuses them; the fix is solver-side (a sub-rest
pressure floor, or Clavet near-pressure) or WP5's CA deletion. `--fluid-bench`
prints the split as `seam flow: ... N unstable`, which is how it was located.

SUBSTEPS ARE THE CFL BUDGET AND A TUNING KNOB (`sim.fluidSubsteps`, WP3).
`FLUID_VMAX` (0.45 cell/substep, expressed in cells/tick — also the fluid's
terminal velocity) and `FLUID_MARK_PAD` are DERIVED from it in common.wgsl, and
`EncodeTick` records the substep table that many times. The pseudo sound speed
sqrt(stiffness)/30 must fit under 0.45 cells/substep or the clamp converts
pressure work into silent energy loss — the "mushy under agitation" regime. At
the shipped stiffness of 14000 that is 8.7, hence the default of 9; at the
previous 6 the clamp engaged on ~575 of 600 bench ticks in every scene.

The solver (2026-08-22, second pass) follows grantkot's WebGPU MLS-MPM shape:
P2G is split into a mass/momentum scatter (`p2g1`) and a stress scatter
(`p2g2`), so pressure comes from the REAL local density sampled off the grid —
`stiffness * ((rho/rest)^power - 1)`, floored at `-cohesion` — rather than
from a per-particle volume ratio J (which saturated at its clamp and let a
small cavity swallow unbounded particles; the density EOS makes over-packing
eject instead). p2g2 also applies dynamic viscosity through the APIC C matrix
and the species terms: particles carry a species id (0..3), the grid carries
per-species mass channels, and `attractSame`/`attractDiff` add signed pulling
pressure per species — cohesion within a liquid, repulsion (layering) or
mixing between different liquids. Every solver constant is a `sim.fluid*`
tuning row (tuner section "MPM Fluid") in HUMAN units — gravity in voxels/s²,
stiffness/cohesion/attract in (vox/s)², viscosity in vox²/s, damping per
second — converted to Q16.16-per-tick integers at SHADER COMPILE TIME by the
const-eval block at the top of `sim_fluid.wgsl` (IEEE-exact folding, so
identical JSON yields identical solver constants everywhere; the kernel never
sees a runtime float). This is the one documented exception to "sim.* is
integer-only"; LoadTuning clamps the human values to ranges whose conversions
satisfy the kernel's i32 overflow audit. All fixed-point multiplies truncate
on the MAGNITUDE (round toward zero): flooring negative products biased every
force toward -x/-y/-z and the whole fluid crept along that diagonal on a flat
floor.

WATER, NOT GOO (WP2, 2026-08-24; docs/PLAN_fluid_overhaul.md §5). Three
structural fixes turned the solver's output from mucus into water:
- **Separate BC with tangential preservation** (`gridUpdate`): only the
  velocity component pointing INTO a solid is removed. A node whose own cell
  is solid used to be zeroed outright — but a particle sliding down a slope
  has in-solid nodes inside its 3³ support, so it lost tangential velocity
  every substep and water piled on inclines instead of sheeting down. Now an
  in-solid surface node keeps the axis components that run parallel to its
  exposed face (both neighbours solid = tangential under the face), removes
  only into-solid normal motion, and zeroes an axis outright only in the
  1-cell-wall ambiguous case (anti-tunneling). `sim.fluidFriction` (default
  0 = free-slip water) optionally decays the surviving tangential part —
  the mud/goo authoring knob.
- **CFL honesty**: stock stiffness 3600 (c = 60 vox/s = 0.33 cells/substep at
  6 substeps) under the 0.45 `FLUID_VMAX` cap, gravity 98.1 (real). The old
  5400-11500 range sat at/over the cap, and the clamp silently converted
  pressure work into energy loss — the "mushy under agitation" regime where
  tuning stops doing anything. `fluidArgs[FA_CLAMPED]` counts node-substeps
  the clamp truncates (zeroed per tick by the seam's compact scan, surfaced
  in `--fluid-bench`); it reads 0 across every lab scene at stock, and a
  persistent non-zero count means the stiffness/substep budget is dishonest
  again — fix it there, never with damping.
- **Zero tension by default**: cohesion/attractSame/attractDiff all default
  0, so the EOS floor is exactly `p >= 0` — negative-pressure terms are the
  classic sticky-ropes look and are now purely an authoring surface for
  other liquids. Viscosity defaults 0.1 vox²/s (references run 0.02-0.1
  grid units; a lab A/B at 0.5 moved nothing but the look toward syrup).

RENDERING (v3, 2026-08-23) — the fluid draws as a real water SURFACE, not as
particle cubes. Where Splash (matsuoka-601) filters depth sprites in screen
space, this engine has no sampled textures, so the same result is built the
way this renderer builds everything: `raymarch.wgsl`'s MPM FLUID SURFACE block
marches the solver's own node grid (last substep's mass field, read straight
from `fluidGrid`/`fluidBlockMap` — zero upload) as a trilinear isosurface.
Gradient normals, Schlick Fresnel, TRACED reflections and TRACED refraction
(the bent ray re-marches the world through `shadeSecondaryHit`, so the shore
genuinely bends at the surface), per-channel Beer-Lambert absorption derived
from the species colour, mass-weighted grid velocity driving churn foam and
sub-voxel shimmer, and a camera-submerged volumetric path. Cost is bounded in
three nested steps: `RenderParams.fluidLo/fluidHi` is the world AABB of live
fluid, so a ray that misses it pays one slab test and a ray that hits it marches
only the [enter, exit] span; chunk-stride skipping over the block map crosses
empty chunks; and the block map's second half is a per-chunk Y-OCCUPANCY mask
(gravity-fed fluid is a thin horizontal layer, and `mark`'s 3-cell pad means an
allocated block is routinely all air) so an empty y slab is skipped on one
buffer read. `RenderParams.fluidCount == 0`
(or `render.fluidSurface = 0`, which restores the old debug cubes via
`debris.wgsl:vsFluid`) skips every instruction of it. Tuner section "MPM
Fluid Look": iso, smoothing, IOR, clarity (metres), reflection/specular
gains, foam amount/speed, shimmer, per-species colours. Depth is written at
the fluid interface, so raster spray in front composites over it and debris
behind it is covered.

THE RENDER SEAM: ONE LAKE, TWO REPRESENTATIONS (2026-08-25). The virtual-mass
blend above lets the isosurface reach over SETTLED voxel water, which closes the
geometric gap — and hands this shade a body of water the CA owns, described by
completely unrelated coefficients (`waterAbsorb`/`waterScatter`, flat, versus
`(1.06 - depth-ramped species albedo)/clarity` with a lit squared-albedo
in-scatter). Measured over 2.5 m of pond that is (60,120,130) against
(142,159,177). So exciting one chunk of a lake used to REPAINT it: a
chunk-aligned rectangle of flat pale blue with a black rim, flickering as blocks
were allocated and freed. Repro: `--shot-fluid-pond`, which pours into a
generated pond and shoots a CA-only reference frame from the same camera.

The rule is that the shade must CONVERGE on the CA water's, continuously, as the
column it is looking through becomes settled water — never pick one model. Four
quantities carry the blend, all keyed off measurements the renderer already had:

- `caPath` = `RayHit.liqPath`, the settled liquid the PRIMARY ray crossed
  (bed-bounded, free). The column is `max(fh.thick, caPath)` and its settled
  share drives `caFrac`, which is SATURATING (`smoothstep(0.05, 0.55, ...)`) —
  the two coefficient sets describe the same substance, so a four-voxel excited
  film on a 26-voxel pond is a lake, not 15% fluid.
- absorption, in-scatter and the caustic web (`waterCaustics`, factored out of
  `shadeWater`) blend by `caFrac`; the CA branch derives non-water liquids
  exactly as `shadeWater` does, so a pour into an oil pond blends toward oil.
- TRACED REFRACTION FADES OUT by `caFrac`, and that is correctness, not thrift:
  `shadeSecondaryHit` carries no shadow or AO term, so a bed seen through the
  refracted ray came back brighter than the same bed two pixels away through the
  CA surface. It also removes a secondary march from every lake pixel — the
  `pond68` bench's fluid march went 1.63 -> 1.18 ms.
- `rippleSlope` (with `shadeWater`'s own screen-space damping, factored out as
  `waterRippleFootprint`) is added to the fluid normal for surfaces that are
  settled water or are live particles floating STILL on a settled column. A
  settled MPM slab has an exactly constant normal, so the whole slab sits at one
  specular angle and glints as a single sheet where the lake around it sparkles.
- CHURN FOAM GETS A THRESHOLD over a settled column. `churn` is linear in speed
  from zero, so a lake circulating at 3-4 vox/s wore a constant ~0.08 of foam
  across the whole excited footprint — a warm-white wash in a perfect rectangle,
  and the LAST thing standing after every colour term already matched to 2/255.
  A jet still ploughing in clears the threshold and keeps its whitewater; the
  simulated foam FIELD is never thresholded, because it is a real decaying
  quantity that has already earned its value.

ONE LIQUID INTERFACE PER PIXEL, and `caShadedLiquid` is the single flag that
enforces it. It replaced two independent tests that had to agree and did not:
- `fluidCellMarched` (deleted) asked whether the CA's liquid cell is inside the
  march's SAMPLING region, when the shade needed to know which interface is
  NEAREST. The Y-occupancy mask only has bits where node mass lives, so under a
  pour the CA's first liquid cell sits below the lowest marked slab, the test
  said "not marched", and `shadeWater` and `shadeMpmFluid` both painted a water
  surface on the same pixel. Ownership is now `mf.t <= h.liqT + 1` (one cell of
  slack: the iso crossing and the fullness plane are two definitions of one
  waterline).
- THE PANE. The march's empty-space skips classify chunks from the BLOCK MAP,
  but the field they skip through also holds settled water. A lake chunk with no
  block is skipped as empty, so the ray enters the next marched chunk ALREADY
  submerged, samples at or above iso, and the crossing bisection collapses onto
  the chunk face — a vertical pane of glass standing in the pond, with Fresnel
  and a glint on it, metres under the real surface. A ray that crossed a water
  surface first cannot meet another interface behind it, which `caShadedLiquid`
  already records.

Two smaller repairs in the same pass: `fluidSampleAt` gained the virtual-mass
term (settled water accumulates into species 0), without which a node carrying
only settled mass divided by the species floor and shaded BLACK — the dark rim
around every marched region that touched a pond; and the thickness walk is now
bounded by the SCENE, not by `fluidLo/fluidHi`, because the settled water in the
field extends arbitrarily far past the live particle blocks and clipping there
made a pour into a lake absorb like a puddle.

DRAW MODES — `render.fluidSurface` is a MODE, not a boolean, and it kept its
name because 0 and 1 still mean what they always did (no `tuning.json`
migration). `0` = one raster cube per particle, the solver-debug view and the
only mode drawn CPU-side; `1` = the smooth isosurface above; **`2` = VOXELIZED
at half a cell — the field quantized to a 2x2x2 sub-voxel lattice, which is one
sub-voxel per particle at rest density, and THE DEFAULT**; `3` = VOXELIZED on
the sim lattice, one cube per world cell. The voxelized modes exist to answer a
look question the isosurface cannot — *what does MPM water look like if it
still reads as VOXELS?* — and mode 3 in particular makes a pour
indistinguishable from CA water at a glance while the motion underneath is
still the full MPM solve.

Mode 2 is the default because this is a voxel engine: water that reads as
voxels is the house style, and the smooth Splash surface — which is the more
expensive march *and* the one that visually disagrees with every other surface
in the world — is the opt-in look rather than the assumed one. Half a cell, not
a whole one, because it is the resolution the solver already works at (8
particles per cell on a 2x2x2 lattice), so it is the finest quantization that
carries real information rather than interpolation.

They are strictly RENDER-ONLY: `fluidMarchBlocky` writes no voxel. The voxel
word is hashed, saved, deterministic state (rules 1 and 3), a per-frame float
occupancy decision has no business inside the world hash, and a half-size grid
is not representable there at all — the state nibble is *fullness*, not
occupancy of eight sub-cells. They sample the same `fluidFieldAt` the
isosurface marches (settled water included, via the virtual-mass blend), so all
three marched modes agree about where the water IS and differ only in how its
boundary is drawn; a sub-cell is filled when the field at its CENTRE is at or
above `fluidIso`, and at one sub-cell per voxel that centre IS the node, so
mode 3 is exactly "this cell holds >= iso of rest density" with no
interpolation blur. They inherit the three empty-space skips and the thickness
walk unchanged (so absorption and the depth gradient are identical across
modes, and an A/B compares surface SHAPE rather than colour), carry an exact
cube-face normal instead of the 4-tap gradient, and skip smoothing and shimmer
— both of which exist to round cubes off.

COARSE SEARCH, LOCAL REFINE is the whole performance story, and it was learned
the expensive way. The obvious implementation — a DDA that visits every
sub-cell and tests the field at its centre — measured **74.85 ms** of fluid
march on the `hill` bench against the smooth march's **8.90 ms**, an 8.4x
regression, because a DDA cannot stride: it pays 2 field samples per voxel (8
trilinear taps each) through exactly the empty space the smooth march crosses
1.25 cells at a time. So `fluidMarchBlocky` runs the smooth march's coarse loop
verbatim and only DDAs the sub-cell lattice across the single stride a crossing
is known to lie in (`fluidRefineSubCell`, bounded at 12 sub-cells). The refine
window is clamped to one stride back, which is load-bearing: the empty-space
skips set `tPrev` BEFORE teleporting `t`, so an unclamped refine would spend its
whole budget walking space the skip just proved empty.

ACCEPTED ARTEFACT, MECHANISM NOT ESTABLISHED: in the blocky modes some of the
ballistic spray droplets that the smooth march hides become visible, reading as
hard white sprite triangles over the pool. The droplets are a deliberate feature
drawn in every mode; they are what makes the difference visible, not the cause.
Since they are occluded by the depth this march writes, blocky and smooth depth
must disagree somewhere. Owner's call, 2026-08-24: ship it.

Three explanations were tried and all three are refuted — they are recorded in
the block comment above `fluidMarchBlocky` so nobody re-derives them: (1) the
far stride skipping isolated spray (the smooth march takes the same stride and
has no slivers); (2) refine failing and leaving a hole (a crossing-point
fallback changed pixels and removed no slivers); (3) the centre test landing
deeper than the smooth crossing (snapping the bisected crossing to its sub-cell
made it far worse — a regular grid of bright seams, because adjacent pixels snap
to different sub-cells). The exhaustive DDA showed far fewer, at 8.4x. Next
person: read the depth buffer before writing code.

SPLASH COUPLING — fast fluid particles at low density (spray, breaking
crests) shed `PFLAG_MICRO` droplets into the ballistic particle system from
`g2p` (bindings 6/7 of the fluid group are the particle write page + counts;
the appends land after `particleResolve`, so droplets fly next tick). Each
droplet carries the particle's OWN material (the attr word; the poured-species
table is the fallback) — so MPM blood spatters real stains through the
existing claim-hash stain path and MPM water is pure sparkle. Emission is
hash-keyed on particle state + tick (the fluid slot index is a stable
identity — assigned by the seam's deterministic slot-order compaction),
bounded by rate/speed/density thresholds (`sim.fluidSplash*`), droplet
lifetime, and `PARTICLE_CAP`. Live fluid holds `particlesActive` on (plus a
droplet-lifetime tail) so the spray integrates without an explosion ever
having happened.

Usage: the `mpm` tool (Tab; hold LMB to pour, keys 1-4 pick the species, U
clears — which now zeroes the GPU-owned count directly). `--shot-fluid` is
the look-iteration harness: worldgen, pour a pool + a falling stream with the
tool's own spawn shape, write `screenshot_fluid{,_top,_splash,_low}.bmp`. The
CPU keeps only a CONSERVATIVE live estimate (snapshot readback + spawns since
— drives record/skip, the draw count and the HUD; every kernel re-bounds
itself on the GPU count, and `vsFluid` collapses dead/stale slots). Hard
`kFluidCap` budget charged before emission on the CPU and enforced exactly by
the excite scan on the GPU; zero recorded work when no fluid exists and the
excite mode is off. Not persisted: saves and worldgen drop the particles
(they force-settle in spirit; the settle converter has usually already made
them voxels, which DO persist normally). The CA liquid movement rules are
still live — Phase 3 (deleting them, flipping `fluidExciteMode` default,
re-baselining the pinned hash) is a separate, later step.

**The fluid lab (2026-08-24; `src/lab/`, docs/PLAN_fluid_overhaul.md §4).**
The dedicated place fluid is looked at, tuned and benchmarked. `--lab
[basin|hill|faucet|pool|slosh]` runs the windowed game on a flat-slab world:
`TickParams.labMode` (the old padMb pad word) guards `genColumn` in
worldgen.wgsl — one branch covers full worldgen, streamed genList refills and
the far cascades; `World::TerrainHeight` mirrors it CPU-side; `kLabSlabY`
(world.h → prelude `LAB_SLAB_Y`) is the shared ground height. NOT a fork of
genChunk, and always 0 outside lab modes, so the pinned hash is a labMode=0
fact. Scenes are CellOp builds + `FluidSpawnOp` pours on fixed tick
schedules (the mpm tool's spawn shape; budgets charged before emission); L
re-runs the scene from the post-worldgen state without regenerating. The lab
forces `sim.fluidExciteMode=1` at runtime, watches tuning.json's mtime (~4
Hz) and applies changes through the F5 path; ImGui fluid-slider edits write
back into tuning.json (text-surgical, lab mode only, refused when the file
on disk is newer — last-writer-wins by mtime). `--fluid-bench
[scene|hill0|all]` is the headless twin: fixed camera, passtimer per-pass
GPU ms, frame percentiles, live/active-block curves, tick-of-settle and the
FA_* mass ledger (eighths in == standing + carried, day phase pinned) as
JSON + a per-scene screenshot. WP2–WP4 changes quote before/after from it;
the 2026-08-24 baselines live in the plan's §9.

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
- **`molten` (2026-08-19):** optional per-material product for the heat/laser
  melt brush (`BrushOp.mode == 2`): each cell in the brush converts to *its own*
  molten form (stone→lava, sand→molten glass, wood→fire; absent = vaporize).
  255-hardness matter is immune. Data, not code — the laser knows no material IDs.
- **Light conditions (2026-08-20):** any rule may additionally require a light
  environment, which is what lets sunlight drive the world (§4.5):
  ```json
  { "self": "water", "decay": true, "becomes": "steam",
    "needsSky": true, "when": "day", "minLight": 120, "chance": 1 }
  ```
  (the shipped rule adds `scaleByNeighbors` on top of this — see below)
  - `needsSky` — the cell must not be covered by a ray blocker.
  - `when: day|night` — gated on the tick-derived day phase.
  - `minLight: 0..255` — a floor on daylight strength, so "only near noon"
    is expressible without a second condition.

  These compile into the spare word of the 32-byte reaction entry, so the
  struct did not grow. Everything about them is integer and tick-derived,
  which is what keeps a sun-driven reaction inside the determinism rule.
- **Weather switches (2026-08-20):** a rule may name one named switch, and is
  dropped at COMPILE TIME when that switch is off:
  ```json
  { "self": "water", "decay": true, "becomes": "ice",
    "when": "night", "requires": "waterFreezes", "chance": 1 }
  ```
  The switches are booleans in `tuning.json`'s `weather` group
  (`waterFreezes`, `iceMelts`), resolved by `WeatherFlagEnabled` in
  `sim/materials.cpp`. This is deliberately **not** another `cond` bit: a
  switched-off rule never enters the GPU table at all, so it costs nothing per
  cell rather than being tested every tick and always failing (rule 2). The
  bucket flattening recomputes `reactOffset`/`reactCount` from the surviving
  rules, so nothing downstream needs to know a rule went missing — and a
  material whose every rule is off is skipped wholesale by the `reactCount > 0`
  dispatch guard.

  Two consequences worth stating plainly. **They change the world hash**, the
  same way editing `reactions.json` does — that is content, not divergence, but
  a lockstep session has to agree on them. And because the reaction table is
  built by `LoadAssets` (which F5 does not otherwise run), the F5 path now
  falls through into the materials reload so a switch applies on one keypress.
  An unknown switch name is a load ERROR, not a silent drop: silently
  compiling would make the switch look broken, and silently dropping would
  delete content.
- **Neighbour-count scaling (2026-08-20):** a decay rule's chance may scale with
  how many of its 6 face neighbours match a predicate, where a count of **zero
  forbids the rule outright**. This is what turns a uniform nucleation rule into
  a spreading frontier:
  ```json
  { "self": "water", "decay": true, "becomes": "ice",
    "needsSky": true, "when": "night", "chance": 1,
    "scaleByNeighbors": { "neighbor": "water", "invert": true, "scaleMax": 4.0 } }
  ```
  Water scaled by its count of *non*-water neighbours freezes shore-first: bank
  and surface cells count 2–3 and freeze fastest, every voxel that freezes is
  itself non-water and so raises its neighbours' odds, and water enclosed by
  water counts 0 and cannot freeze until the front reaches it. The ice creeps
  inward instead of speckling.

  Three constraints shaped the design:
  - The predicate **reuses `nbrMat`/`nbrTags`/`nbrClass`**, which a decay rule
    leaves unused. So the counted set speaks the vocabulary pair rules already
    use, no new field was needed for the 12-bit id, and the entry stayed 32
    bytes. Scaling is *rejected* on pair rules, whose neighbour fields already
    select their reacting partner.
  - **Rolls moved to a finer denominator** (`REACT_CHANCE_DEN`). The interesting
    rules are authored at chance 1–2 per-mille, where evaluating
    `(chance * q) / 4` in per-mille truncates 1.5× and 2.75× onto the same
    integer and collapses the 6-step ramp to 4 distinct rates. This changes the
    RNG-to-outcome mapping for *every* reaction, so it changed the world hash.
  - It is a **read-only 1-cell probe, integer throughout**. The colour lattice
    bounds *writes* to one cell, so a neighbourhood read stays inside its
    guarantee (§2) — this is the one place where "reads ≤ 1 cell" and "writes
    ≤ 1 cell" are worth keeping distinct in your head.

  The shape is invisible to the hash test, so `--selftest` gained a pond-freeze
  gate asserting *both* that the rim leads the middle by >2× and that no ice
  voxel ends up with zero non-water neighbours. Only the second is decisive: a
  rate comparison alone cannot separate "the gate works and the front crept
  down from the frozen surface" from "the gate is ignored", because both land
  at about the same percentage.
- **`minCount` — a floor on the count (2026-08-20):** the count-0 gate above has
  one blind spot, and evaporation walked straight into it. An open pond's whole
  top face has air above it, so every surface cell counts ≥1 and fires at the
  full base chance — which is why sun-driven evaporation boiled a lake off from
  the surface in seconds. `minCount` (1..4, in the two spare `cond` bits 18–19,
  stored biased by 1 so the default stays zero) raises the gate:
  ```json
  { "self": "water", "decay": true, "becomes": "steam",
    "needsSky": true, "when": "day", "minLight": 120, "chance": 1,
    "scaleByNeighbors": { "neighbor": "water", "invert": true,
                          "minCount": 4, "scaleMax": 3.0 } }
  ```
  At `minCount: 4` a water voxel must be *mostly out of the water* before the
  sun can take it: a flat pond surface (1 non-water neighbour) and a cell near a
  bank (2–3) are immune, a rim cell or thin sheet (4) goes at the base rate, and
  a lone droplet on stone (6) goes at 3×. Spread water dries, bodies of water do
  not — and a shallow puddle still dries edge-inward, because losing a rim cell
  exposes the next one.

  This makes freezing and evaporation *opposed frontiers over the same count*:
  freezing wants a small count (it spreads from the shore in), evaporation wants
  a large one (it retreats from the rim out). They are already exclusive by day
  phase, so they cannot fight.

  `--selftest` gained an `evaporation` gate that pins the rule from both ends in
  one scene: a pond whose surface must be **fully intact** after 2500 noon ticks,
  plus isolated droplets that must **all** be gone. Either assertion alone is
  weak — the first passes a rule that never fires, the second passes the old
  always-fires rule — and only together do they distinguish the two.

- **Staining (2026-08-20):** a liquid may mark the voxels it touches, and may
  eat what it marks. Authored per material, not per material PAIR — the same
  anti-N×M argument tags exist for:
  ```json
  { "id": "blood", "class": "liquid", ...,
    "stain": { "type": "blood", "color": "#4a0f0f",
               "amount": 5, "chance": 90, "consume": 6 } }
  ```
  - `type` names a stain palette slot (shared: two liquids naming the same
    stain get the same slot and the same look). `amount` is added per contact
    and saturates at 15, so repeated contact deepens a stain.
  - `chance` is per-mille per tick to stain one touching face neighbour;
    `consume` is per-mille that the stain then deletes that voxel to air,
    which is what lets blood slowly pit what it soaks.
  - Compiles into the two spare words of the 64-byte `Material` record
    (`stainPack` + `stainColor`), so the struct did not grow.
  - **Sleep discipline (rule 2) is the whole difficulty.** A pool of blood on
    stone is a PERMANENT condition, so a rule that stays awake "while touching
    something stainable" pins those chunks awake forever — the same trap the
    light-gated rules hit. `doStaining` therefore holds the chunk only while
    there is UNSATURATED surface left in reach; once everything touching is
    fully stained, the pool settles and sleeps. Reachable surface is finite and
    stain only ever increases, which is what makes it decisively subcritical.
  - Write reach is exactly one face neighbour, so it stays inside the colour
    lattice's guarantee, and both rolls come from the stateless hash.

- **Absorption and washing (2026-08-21):** two behaviours layered on staining,
  both authored as data, that turn "water marks the ground" into "ground drinks
  water until it cannot".
  ```json
  { "id": "grass", "class": "solid", ..., "absorb": { "capacity": 12 } }
  { "id": "water", "class": "liquid", ...,
    "stain": { "type": "wet", "amount": 15, "chance": 260, "washes": true } }
  ```
  - **Capacity is authored on the SUBSTRATE, not on the liquid.** This is the
    part worth defending: the liquid's `amount` is the per-contact STEP (how
    fast it soaks) and the ground's `capacity` is the CEILING (how much it
    holds). They are genuinely different axes — the same rain soaks into sand
    quickly but shallowly, into loam slowly but deeply — and the effective depth
    is `min(amount, capacity)`. Authoring the ceiling per material PAIR would be
    exactly the N×M explosion tags exist to avoid. Capacity 0 (every material
    predating this, all stone) means the liquid never soaks in and pools at once.
  - **Absorbing SPENDS the liquid**: one eighth of the source cell's fullness
    per successful contact, in the same units `stepLiquid` speaks, and the cell
    dies when it gives its last. Without that debit the puddle would stain the
    ground and then sit there full forever — which is not absorption, and was
    the behaviour before this. So a puddle on dry grass drains INTO the grass,
    and only once the ground beneath is saturated does water persist on top.
  - `washes` makes a liquid RINSE a foreign stain (step its amount down toward
    0) instead of overwriting it. Water over blood-soaked ground would otherwise
    relabel the blood as "wet" at full strength: the colour would change but the
    mess would never come out.
  - Both fit in `stainPack`'s spare bits (27..30 capacity, 31 washes), so the
    64-byte `Material` still did not grow.
  - **Sleep discipline (rule 2)** survives because every step is monotone toward
    a bounded fixed point: stain rises only to `min(amount, capacity)`, a washed
    stain falls only to 0, absorbed fullness falls only to 0. Nothing ever
    increases the work remaining, so `progress` latches false and a saturated
    puddle on saturated ground sleeps. This is also why saturation is TERMINAL
    and there is no drying-out rule: a cell that could both wet and dry would
    never reach a fixed point, and those chunks would never sleep again.
  - Absorption writes SELF, which the stain rule otherwise never does. Reach is
    still ≤1 cell so the lattice argument holds, and the write sets the substep
    stamp so the movement code cannot also move the cell and double-spend the
    eighth.
  - Fixing this surfaced a PRE-EXISTING rule-2 bug: the `moveEvery > 1`
    viscosity gate re-dirtied its chunk unconditionally on every off-tick, so
    a settled pool of ANY viscous liquid (lava included) could never sleep. It
    now re-dirties only if `canFlowAnywhere` says the cell has somewhere to go.
    It went unnoticed because the sleep selftest only ever settled water and
    powders, both `moveEvery == 1`.

### Compilation to GPU
- Material properties → one SSBO array indexed by 12-bit ID.
- Reactions → per-material buckets: each material stores offset+count into a flat
  reaction array (entry: neighbor ID or tag mask, chance, products). The CA kernel
  scans the bucket of the current voxel only — O(rules-per-material), not O(4096²).
- Validation pass at load: unknown IDs, unreachable rules, density cycles → loud
  errors with file/line. Modders get real diagnostics, not silent breakage.

### `materials/tuning.json` — look-and-feel parameters (2026-08-19)
Where `materials.json` says what a voxel *is*, `tuning.json` says how the engine
renders and moves it: sky/sun/fog, water and lava shading, AO and shadows,
tonemap, player speeds, Jolt body materials, debris budgets, the integer sim
constants, and worldgen shape. Edited with `assets/tuner.html` (Tuning tab),
which builds its whole UI from `assets/tuner_schema.js` — range, units and a
plain-English description per parameter.

Two delivery paths, because the values land in two places:

- **Shader params** are emitted as WGSL `const` declarations by
  `TuningWgslBlock()` (`sim/tuning.cpp`) and prepended by `LoadShader()`
  alongside `ShaderConstantPrelude()`. Shaders name these constants
  (`TUNE_MEDIA_ABSORB`, `TUNE_EXPOSURE_WHITE`, ...) instead of hardcoding
  literals, so **F5 recompiles the whole shading model against new values with
  no rebuild**. `ReloadShaders` already traps validation errors and keeps the
  old pipelines, so a bad value cannot take the renderer down.
- **CPU params** are plain fields on `Tuning`, read each frame by `player.cpp`,
  `camera.cpp`, `physics.cpp` and `main.cpp`. Same F5 applies them.

Why the prelude rather than a uniform: plumbing ~110 values through a UBO costs
a struct field and binding churn each, and burns uniform space; the prelude is
one codepath for arbitrarily many constants, and constant-folds in the compiled
shader exactly as the old literals did.

**Determinism (rule #1).** The `sim` group — `partGravity`, `partMaxVel`,
`airDensity`, `falloffPerCell`, the `eject*` per-mille set, `liquidEqualize`,
`wanderHopMask` — feeds voxel state. It is integer-only by construction (the
loader *rejects* a JSON float rather than truncating it, so a fractional value
is a loud error instead of a silent hash shift), and changing any of it changes
the world hash. The tuner marks that group in red and says so; `--selftest`
must be re-run to re-baseline. Everything outside `sim` is render- or CPU-side
and provably cannot perturb the hash — verified by changing sky colour and
exposure and watching the hash stay at `a0d20705`.

##### The terrain: an attenuated octave ladder (2026-08-26, overhaul package C)

The height function is five octaves of Q14 value noise, lacunarity 4,
persistence 1/4, so **every rung has the same amplitude-to-wavelength ratio of
0.5**. That uniformity is the design, not a coincidence: slope is what the CA
cares about — its angle of repose is exactly 1 voxel per column — and a ladder
whose rungs all share an A/W adds detail without adding slope.

| octave | cell | amplitude | |
|---|---:|---:|---|
| continental | 2048 vox / 204.8 m | 1024 / 102.4 m | faded out near the origin |
| range | 512 / 51.2 m | 256 / 25.6 m | faded out near the origin |
| hill | 128 / 12.8 m | 64 / 6.4 m | live everywhere |
| detail | 32 / 3.2 m | 16 / 1.6 m | live everywhere |
| grain | 8 / 0.8 m | 4 / 0.4 m | live everywhere |

Every octave is a **centred deviation** (`n - 8192`), so `worldgen.baseHeight`
is the world's *mean* height rather than its floor and there is as much room
below the datum for sea basins as above it for mountains. Measured over a 3,072
voxel transect from the origin: **y-351 .. y+607**, i.e. ~96 m of relief where
the pre-overhaul world had 5.4 m.

**Derivative attenuation is a rule-2 mechanism, not a look knob.** Each octave
is divided by `1 + fbmAtten·|g|²` against the gradient accumulated from the
octaves coarser than it (iq's trick). Without it the ladder sums five 0.5 slopes
into 2.5 and puts *the entire world* above the angle of repose, where nothing
loose can ever come to rest. With it the field saturates near slope 1.2 on
ridges and goes genuinely flat in valleys.

**The calm home area** fades only the two coarse octaves toward the world
origin, over `spawnPlainFade` past a Chebyshev radius of `spawnPlainR`. Fading
the whole deviation would pin spawn to a mathematically exact plane 64 m across
— which is not "calm", it is a dinner plate, and it would make the `terrain`
gate's per-voxel pass a test of a constant. The fade *width* is load-bearing: a
ramp of magnitude A over width W adds slope up to 1.5·A/W, so squeezing 640
voxels of coarse relief into a 300-voxel fade builds a cliff at exactly the
boundary. Pass A4 measures it; read that number rather than guessing it.

**The sediment wedge** is what makes the relief mean something to the sim rather
than only to the eye: low flat ground carries metres of loose dirt over gravel,
ridges carry bare rock. Thickness is
`(sedCeil − ground)·sedFraction/256 − sedStrip`, slope-gated and clamped to
`sedMax`, which stays under `caveBands`' 40-voxel shell so a cavern cannot
undercut it.

Dirt and gravel are **powders**, so the gate is the safety property. Two things
about it are not obvious and both were measured:

* It reads the **landform** gradient — accumulated through the hill octave, not
  the full one. `d(slope)/dcolumn` for an octave is ~`6·amp/cell²`, which for
  the *grain* octave is 96 Q8: the entire gate range in one column, turning a
  24-voxel wedge into a 24-voxel cliff wherever the fine noise crosses the
  threshold. Gated on the landform it thins over ~32 columns instead. 108
  chunks still awake at tick 120 became 8.
* With a **solid** grass skin at `y == h` the topmost grain sits at `h−1`, so it
  has a free down-diagonal exactly where a neighbouring column is 3+ voxels
  lower — which is the ground the gate has already taken the wedge to zero on.

`worldgen.sedSlope = 0` turns the wedge off, which makes
`--sweep worldgen.sedSlope=0,96` a one-invocation proof that the knob reaches
the kernel.

**Tree sizes are metre-true again.** `VOX_PER_M` in `worldgen.wgsl` was a
hardcoded 16 — correct when a voxel was 6.25 cm, and left behind when `world.h`
moved to `kVoxelMeters = 0.10`. For that whole interval every tree in the game
was 1.6× the metre size its own table documents (the "11.9 m" great oak was 19 m
of trunk). It reads `VOXELS_PER_M` from the prelude now, so the table's metres
are true at any voxel size.

##### The height contract (2026-08-26)

> `World::TerrainHeight(x, z, seed)` ≡ `genColumn(x, z, seed).h`, **exactly**, for
> all inputs.

The CPU mirror is not "roughly the terrain" and it is not "the topmost solid
voxel". It is the **ground**: the terrain octaves, the authored pool floors and
rims, the pond bowl carve, and the tarn berm — everything `landColumn()` in
`worldgen.wgsl` applies to `h`, in that order. Literal topmost-solid would
include canopy, ruin walls, grass tufts and the arena deck, and it cannot be
mirrored cheaply (it needs `treeAt`'s tile scan in a tick path); every one of
TerrainHeight's ~30 callers is asking where the ground is so it can stand
something on it.

Two things are deliberately **outside** the contract. The **arena** levels its
footprint as a material override in `genCellIn`, not as a change to `Col.h` —
folding it in would double-apply it and move cave depth and tree bases under its
footprint; `farSurfaceMat` is the one consumer that wants the levelled deck and
applies it locally. And the **fluid lab slab** is taken by both sides as an early
branch, so the lab surface does not move when worldgen knobs are tuned.

Enforcement is threefold and none of it is a comment: `scripts/check_invariants.py`
token-compares the `MIRROR-BEGIN noise` / `MIRROR-BEGIN height` blocks in
`worldgen.wgsl` and `world.cpp`, and compares the `landheight` blocks' authored
constants; the `terrain` gate's pass C1 compares CPU and GPU **per voxel** over
9,409 columns of pristine procgen. The cost is ~25 `hash3` per call, which is
fine at O(1) per frame (spawn placement, fixture anchoring, a mob ground probe)
and is forbidden in a per-voxel loop — the GPU has `genColumn` for that, hoisted
once per column.

There used to be a fourth height function, `surfHeightAt`, which hand-copied
this arithmetic for the far-field skin lookup and had already drifted (it never
took the lab branch). It is gone; `farSurfaceMat` takes the column.

`World::TerrainHeight` reads the same
`worldgen` values as the shader, so tuning terrain cannot desync collision from
the terrain you can see. `scripts/tuning_prelude.py` supplies the same constants
to `check_shaders.sh`, and is **generated** from `src/sim/tuning_params.def` —
the one table the emitter itself expands — so the offline validator and the
engine cannot disagree about a name, a type, or a default. They used to be two
hand-maintained lists, and only the *names* were ever compared.

#### Per-instance variance (2026-08-20)
A tuned constant makes every instance identical: every NPC bleeds exactly the
same amount, which is legible but lifeless. A `Variance` (`sim/tuning.h`) turns
one authored number into a distribution — the tuned value stays the **centre**,
and each instance draws an offset — so on a rare roll an NPC bleeds far more
than the mean and the moment is worth watching.

Stored as a sibling object next to its parameter, so the tuner's generic
`tune[group][key]` writer round-trips it with no save-path change:

```json
"bleedGain": 1.0,
"bleedGainVar": { "dist": "gaussian", "scope": "entity",
                  "amount": 0.35, "sigmaClamp": 3.0 }
```

- **`dist`** — `none` | `uniform` (flat ±amount) | `gaussian` (amount is one
  σ, clamped at `sigmaClamp` σ).
- **`scope`** — the reason "a rare NPC is a gusher" is expressible at all.
  `event` re-rolls per droplet (jitter *within* one wound); `entity` rolls
  **once per mob** and holds for its life (character). Event scope on a bleed
  rate averages out over a wound and reads as noise; entity scope reads as *this
  one is a heavy bleeder*. Entity draws resolve at spawn into
  `MobSystem::GoreProfile` and are re-drawn on F5 by `RefreshGoreProfiles()`.

**Determinism (rule #1).** Every draw is `Hash3(seed, tick, index)` — the same
stateless counter-based scheme the sim shaders use — so two machines at the same
tick draw the same offset and a replay reproduces it exactly. The gaussian is
closed-form Box-Muller, never a rejection loop, so it cannot vary in iteration
count across machines. Verified: the world hash is unchanged at `765da1f8` with
gore variance active, and 20k entity draws reproduce mean 1.00 / sd 0.35 with
~2.2% of mobs past 2σ.

**Where it may be applied.** Presentation and per-instance character only. It is
deliberately unavailable on the `sim.*` integers and on material interaction
rules: those feed voxel state through the CA, where the authored number *is* the
physics, and randomising them makes identical collisions resolve differently for
no legible reason. Bounded by construction (rule #2) — the gaussian tail is
clamped and count draws floor at 0 and cap, because an unbounded draw on a spawn
count is an unbounded particle budget.

### Two sizes of blood (2026-08-20)

Gore comes off a wound at two scales, and they are tuned separately because they
do different jobs. **Micro droplets** fly, stain what they hit, never re-enter
the grid, and are guaranteed to clear (`microLifeTicks`) — they sell the moment
of the hit. **Whole voxels** are conserved matter the CA carries, flows and
pools — they are what is still on the floor a minute later.

Most of the `gore` group governs the spray. The whole-voxel side is:

- `severVoxels` / `severVoxelSpeed` — the one-shot throw at a cut. These are
  particle spawns with `micro=false`, so they ride the 4096-particle budget and
  bypass the drip op budget entirely.
- `bleedVoxelGain` — multiplies what a wound OWES per point of damage, on top of
  the per-mob `bleed.perDamage` in the mob's own sidecar. The global "how wet is
  this game" dial.
- `bleedBudgetCap` — the ceiling on what ONE wound can still owe, and the real
  bound on how much matter a fight puts into the world. The rate below only
  decides how fast it arrives.
- `bleedDripTicks` / `bleedOpsPerTick` — the rate: at most one drip per wound per
  period, and at most N drips per tick across all mobs (and again for the
  avatar). Together these are the hard ceiling on blood entering the sim.

Note `bleedGain` (the per-mob gusher roll) scales spray and the sever throw, but
NOT the drip budget — the drip is the sustained bleed and is bounded by the cap.
All six former `120.0f` literals now route through one `AddBleedBudget` helper in
`sim/tuning.h`, because a per-wound ceiling duplicated six ways is how one of
them ends up stale.

These feed `BrushOp`s rather than shader constants, so like the rest of the gore
group they are a per-tick INPUT and not part of the hashed domain — but they DO
decide how much wet blood exists, and wet blood keeps its chunks awake while it
flows and soaks. The selftest's post-dismemberment settle window is sized against
them; raising the cap a long way is a settle-time change as much as a look one.

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
- **Region-exit test (2026-08-19):** a component reaching the scan region's
  boundary is only *anchored* when the structure genuinely continues outside —
  i.e. the cell just past the boundary face is itself solid/powder. Treating any
  boundary contact as anchored (the original rule) meant nothing taller than the
  scan box could ever fall: a tree at the 64³ support-margin region always grazed
  a face, so felling one produced no body at all, while its dithered crown rim
  became sub-8 "islands" that were deleted. Unknown/unfetched cells and the
  residency edge read as solid, so the conservative direction is unchanged.
- Known accepted flaw: large floating sections can survive. Ship it; revisit.

### Rigidbodies
- Detected islands are **removed from the grid** and become rigidbodies:
  marching cubes (Paul Bourke tables) over the island voxels → collision mesh →
  simplification pass to merge triangles → hand to **Jolt**.
- Mass = Σ per-voxel material density. Bodies keep their voxel payload so they
  remain destructible: damaging a body re-runs marching cubes and can split it.
- **Two-way handoff**: islands under ~8 voxels don't become bodies — they convert
  to their powder-equivalent material and drop back into the CA as rubble.
- **Body-worthiness budget (2026-08-19):** a rigidbody is expensive and
  permanent (compound-shape build up front, then broadphase + terrain-mesh
  upkeep every tick until it settles), while loose voxels in the CA are nearly
  free. So bodies are rationed, not granted to every loose component: at most
  `kMaxNewBodiesPerScan` per island scan and `kMaxNewBodiesPerTick` per tick
  across all shatter, with components taken **largest first** so the tree earns
  the body and the twigs fall back to rubble rather than scan order deciding.
  Anything over budget or under the size floor goes back to the grid as rubble
  or ballistic particles, which reads the same on screen and costs nothing.
  This is rule #2 (cost scales with activity) applied to the CPU side: without
  it, one burning forest converts an unbounded amount of the world into Jolt
  bodies and the frame time collapses.
- **Settle-back (2026-08-19, implemented):** the reverse handoff. A body asleep
  for 2 s whose rotation is within ~20° of a signed-permutation orientation
  snaps to the lattice and stamps its voxels back into the grid as exact-cell
  ops (fill-air-only flag: existing grid content wins, resolved on the GPU so
  it is deterministic and replayable). Odd-angle bodies stay bodies — snapping
  them would resample to mush. One body per tick, whole body or nothing (no
  partial settles losing matter). This closes the grid → body → grid loop and
  applies to blast debris and severed mob limbs alike.
- **Body burn (2026-08-19, implemented):** bodies are outside the CA, so
  without this a burning plank froze mid-flame the moment it detached.
  `DebrisSystem::BurnBodies` runs a CPU mirror of the reaction table (same
  per-material buckets, file order, per-mille chances, counter-based RNG over
  body serial/tick/voxel) over body voxel payloads each tick. Solid products
  swap in place (wood→ember, doused ember→wood); non-solid products (ash,
  smoke, fire) escape into the grid as fill-air-only exact-cell ops at the
  voxel's world cell, and the voxel leaves the body — burning debris visibly
  wastes away, its Jolt collider rebuilt batched (≥12 voxels shed, one body
  per tick), crumbling to grid rubble under 8 voxels. Pair rules match
  body-internal 6-neighbors AND world cells sampled from the chunk cache
  (already fetched for terrain meshing), so grid fire ignites a cold wooden
  body and the fire it emits lights anything nearby through the normal CA.
  Cost gates: bodies with no self-driven voxels skip entirely unless a chunk
  they overlap is dirty; scans/ops are budgeted per tick. Burn ops do NOT
  bump the island-scan freshness watermark — they are additive fill-air
  writes a stale scan can safely miss, and holding the watermark at the
  current tick while anything burns would starve island detection forever.
  Selftest gate: `body burn` (ember-topped wood body must shed voxels and
  emit fire ops).
- **Body shatter (2026-08-19, implemented):** when burn removals disconnect a
  body's voxels, `ShatterBody` splits it: the largest 6-connected component
  keeps the body, fragments ≥ `kMinBurnFragmentVoxels` (24) become bodies of
  their own at the same pose with inherited momentum while the per-tick body
  budget allows (parent collider rebuilt immediately so its ghost boxes don't
  fight the new body), and everything else re-enters the world as **ballistic
  particles** at their world positions with the body's rigid point velocity
  (`lin + ang × r`) — break a body enough and it just turns back into loose
  voxels. Burn fragments face a much higher bar than fresh islands (24 vs 8) for
  a specific reason: a body disintegrating in a fire re-fragments every few
  ticks and each fragment that earns a body re-fragments in turn, so the low
  threshold made one burning tree recurse into hundreds of tiny bodies that
  chugged the CPU. Charred bits falling off a burning object read as embers
  anyway, so below the bar they stay in the CA. The connectivity flood is O(n)
  per body, so it runs batched — every `min(6, n/16)` voxels shed, i.e.
  immediately for small bodies (a clump must detach before the remainder burns
  under the dissolve floor) and amortized for large ones. A body burned/broken below 8 voxels dissolves the
  same way (only when it actually lost voxels that pass — small split halves
  and mob hands are legitimate bodies and persist). CPU-authored spawns ride a
  new per-tick `spawnOps` stream consumed by `sim_particle.wgsl spawn`, which
  appends to the live page exactly like explosion ejecta — part of the tick
  input stream, so saves/replays capture shatter for free. Selftest gate:
  `body shatter` (ember-bridge dumbbell must drop its clump as particles).
  The fixture's plate is 5×5, not 3×3, for a reason worth keeping: the ember
  does not only burn the bridge, it ignites the wood it touches, so a 9-voxel
  plate erodes past the 8-voxel dissolve floor *before* the connectivity check
  ever separates the clump — the test then measured erosion rather than
  shattering and reported 0 bodies. Any fixture here needs the surviving
  component to outlast the burn with margin.
- **Direct body damage (2026-08-20, implemented):** bodies used to be immune to
  everything except fire — an explosion shoved them as a rigid whole and the
  laser could only bisect them along a camera-derived plane. Both now remove
  actual voxels through one shared core, `DebrisSystem::DamageBody`, which is
  the same "erase → re-skin → rebuild collider → maybe shatter → dissolve if
  under the floor" path burning already used. Consequences fall out of reusing
  it: a blast that severs a body yields real separate bodies, and one that
  takes it under 8 voxels turns the remainder into ballistic particles.
  - **Explosions** (`DamageBodiesRadial`, called from the `X`/grenade path
    before `ApplyRadialImpulse`) erase with a quadratic falloff so the crater
    rim is ragged rather than a billiard-ball scoop, eject the removed voxels
    as particles, and let the impulse act on what survives — including the new
    fragments, which is what makes a blown-apart object scatter. Reach is
    `physics.explosionBodyDamageScale` × the destruction radius, kept separate
    from the impulse reach so a blast pushes objects from further away than it
    dismembers them.
  - **The laser** (`MeltBodyAt`) bores a channel tick by tick and the body
    splits when that channel actually severs it. No cutting plane is chosen,
    so what comes apart is decided by the geometry the player carved rather
    than by where the camera happened to be. Melted voxels vaporize (no
    ejecta): a held beam damages every tick and spraying particles from each
    one would drain the spawn ring in a second.
  - Selftest gates: `body blast` (neck-jointed dumbbell must lose voxels AND
    become ≥2 bodies) and `laser kerf` (a held beam must bore through a rod
    and sever it).
- **Destructible micro bodies / copy-on-write bricks (2026-08-20):** the micro
  brick pool held only per-DEF art shared by every instance, so damaging one
  sphere would have cratered all of them — which is exactly why micro bodies
  were previously exempt from every destruction path. `MicroBodyOwn` now clones
  a shared model into a private block on first damage and returns a new model
  index exactly one body holds; `MicroBodyEdit` rewrites that block from the
  surviving voxels and re-derives the dims, so a body that lost half its mass
  draws a smaller OBB instead of marching through air. Freed blocks return to
  an exact-size free list (no coalescing: a compaction would have to rewrite
  every live `base` while the GPU may still be reading last frame's upload, and
  a body re-carving its own block reuses it in place anyway). `blockWords`
  tracks the reserved size separately from the dims precisely because shrinking
  reuses the block — freeing by dims would leak the surplus. Every body
  teardown routes through `DebrisSystem::ReleaseBody` so a culled, settled,
  dissolved or split body cannot strand pool words. Under pool exhaustion a
  fragment falls through to particles rather than to the cube path, which would
  draw it at scale-1 size — twice too big.
  - This also fixed two latent scale bugs that only bit once micro bodies could
    be damaged: `ShatterBody` built fragment colliders at pitch 1 (inflating a
    scale-2 fragment to 8× its mass) and `VoxelsToParticles` emitted one
    full-size particle per micro voxel (turning a scale-2 body into 8× the
    matter it had). Both now divide by the body's scale, and particle emission
    sub-samples on the micro lattice to conserve visible volume.
- **Terrain-mesh identity rule (2026-08-19):** collision patches rebuild from
  dirty chunks, but a chunk can be dirty for reasons that don't move the
  collision surface (liquid flowing or drying — liquids carry no weight in the
  mesh). Rebuilds hash the polygonized mesh and skip the rebuild *and the
  WakeNear* when identical, otherwise any drying pool re-wakes every settled
  body nearby forever and rule-#2 sleep never happens.
- Unlike Noita, body voxels do **not** live in the grid each frame (too expensive
  in 3D); bodies are meshes that carve/displace grid voxels on contact, ejecting
  displaced material into the particle system.
- **Terrain collision for bodies**: localized marching cubes over the contact
  region to get sloped normals (not box faces), generated on demand, cached per
  chunk until the chunk dirties.
- **Meshing cost (2026-08-19):** patch rebuilds are the steady-state CPU cost of
  having bodies at all, so the sampler is not a callback. Occupancy is gathered
  once into an 18³ bitmask (16 cells + the 1-cell border they read) by walking
  the ≤27 source chunks directly, turning 32768 `std::function` hops — each into
  a chunk-cache hash lookup — into 5832 direct reads, after which the polygonizer
  reads bits out of L1. Output vertices are welded: marching-cubes vertices sit
  at edge midpoints, i.e. on a half-integer lattice, so identical positions
  dedupe exactly by integer key with no epsilon compare, and ~4× fewer vertices
  reach Jolt's `MeshShape` build (the dominant cost of a rebuild).
- Sleeping: settled bodies deactivate entirely until another body or force
  intersects their AABB (Jolt does this natively).

### Carving living bodies (2026-08-20; `game/mob.cpp`, `MobSystem::CarveLimb*`)

Limb loss used to be a threshold: a limb had hp, hp hit zero, the whole limb
became debris. That makes dismemberment an *event the rig decides*, which caps
how precise a weapon can ever be — there is no way to express "took a bite out
of the shoulder", let alone "removed one specific voxel of brain".

Live limbs now carve per voxel, the same way rigidbodies already did. A limb
already owns exactly what a debris body owns — a `DebrisVoxel` payload plus a
copy-on-write micro brick — so the operation is the same one on a different
population: **erase the voxels a `keep()` predicate rejects → re-skin the brick
→ rebuild the collider → split off whatever the carve disconnected.**

- **Wounds are cosmetic until they are not.** A carved limb keeps its identity,
  its hp, its joints and its animation; it is the same `arm.L` driving the same
  loco state, just with holes in it. That is what makes damage *readable* — you
  can see how hurt something is — without every scratch being a gameplay event.
- **Dismemberment becomes geometry, not bookkeeping.** A limb severs when it
  can no longer hold together: under `kLimbCollapseFraction` of its authored
  volume, or when the carve disconnects it from its own joint anchor. No hp
  threshold decides it; the player decides it by what they cut away.
- **Disconnected chunks become ordinary debris** (`EmitCarvedFragment`), with
  their own COW brick, so a hand cut off mid-forearm is a real object that falls,
  collides, and can be carved again. Sub-`kMinFragmentVoxels` scraps spray as
  particles instead. The authored limb list never changes — only its geometry —
  so the rig, the gait and the dismemberment states are untouched by a carve
  that does not sever.
- **Precision scales with the def's skin scale, not with new code.** The carve
  is expressed once as a world-space volume and re-evaluated per lattice, so at
  `skinScale: 8` a radius of 0.125 world voxels is one skin voxel. Where the
  derived collider is too coarse to notice a fine carve, the skin still
  registers it — otherwise fine tools would be silent no-ops on exactly the
  detailed art the skin exists to serve. `tools.laserCarveRadius` is a float and
  sub-voxel by default: the beam that melts a 2-voxel hole in stone bores a
  roughly one-micro-voxel channel through flesh. Targeting a specific *region*
  (an organ, a part of a brain) is then an authoring problem — paint it as
  distinct materials in the limb's `.vox` — not an engine one.

Three things that are easy to get wrong here and are load-bearing:

- **A live limb is kinematic and re-posed every tick.** `CarveLimb` must NOT
  re-read the transform from Jolt: the `keep` predicate was built against the
  pose the caller measured, and refreshing it mid-carve tests the voxels against
  a pose the predicate never saw. That bug removes the wrong cells, then (as the
  animation drifts) none at all.
- **Re-skinning moves the limb origin, so the RIG must move with it.** A debris
  body only has to shift its transform; a limb also has to shift `restOffset`
  and `anchorLimb`, because the animation pipeline rebuilds its pose from those
  every frame and would otherwise undo the shift — the wound appears to crawl
  along the limb as it is carved.
- **A collider rebuild replaces the Jolt handle**, so the limb's joint, its
  children's joints and the intra-mob collision exclusion set are all rebuilt in
  the same breath (`RebuildLimbBody`). Miss one and the limb silently detaches
  or starts fighting its siblings.

Brick ownership follows the body: a carved limb owns its COW model, and
`DetachLimb`/`Die` hand that ownership to `DebrisSystem` (clearing `carved`) so
exactly one system will ever free it — which is also why a severed carved limb
keeps its wounds as debris with no special case.

---

## 8. Player & Projectiles

- **Player**: capsule controller (Jolt character), colliding against on-demand
  localized marching-cubes terrain patches. Voxel-type queries drive traversal:
  liquids slow movement and swap jump→swim; standing in gas/liquid can apply
  status effects; some materials are absorbed on contact (Noita stain system —
  and remember its lesson: players will invent rules for anything you surface
  in the UI, so communicate statuses deliberately).
  Traversal assists, all in `player.cpp`: step-up absorbs sub-step ledges, the
  water-edge mantle climbs out of pools, and ledge grabbing covers everything
  taller — airborne with space held, a lip between shoulders and fingertips
  latches into a dead hang, and W pulls up (a committed mantle when the lip is
  standable, an arm boost to the next grab when it is not, which chains up
  rough walls).
- **Projectiles** (spells, thrown things): no colliders — swept ray each frame
  (position + velocity look-ahead, anti-tunneling). Spell modifiers attach as
  **tags with per-frame logic** (material trail, AoE on hit, bounce, acceleration
  modifiers). On hit: run effect, or reflect off a local marching-cubes normal.
  This tag-composition structure is deliberately the seed of the Noita-style
  wand/spell system later.

### The spell system (2026-08-20; `game/spell`, `game/caster`, `assets/spells/glyphs.json`)

The Noita-style wand system this section always anticipated ("spell modifiers
attach as tags with per-frame logic ... the seed of the Noita-style wand/spell
system later"), crossed with the *ancient language* of Eragon: you speak words,
the words compose, and imprecision is punished rather than rejected. What is
implemented is an **exploratory slice** — one form (projectile), four elements,
two modifiers — deliberately optimized for being CHANGED rather than for being
complete. What follows is the part that is *not* meant to change.

**A spell is a program whose only output is op-stream emissions.** Every world
change a spell makes leaves as a `BrushOp`/`ExplosionOp`/`CellOp`/
`ParticleSpawn` on the MutationQueue (rule 3). `SpellEmission` is the only
channel out of the VM, and there is no path from spell code into a voxel
buffer. That is what gives spells save/replay/networking for free, and it is
why a spell blast joins the ordinary `exps` list rather than getting its own
detonation path — island checks, body damage, mob carving and impulse all apply
with no spell-specific code.

**The effect payload is position-parameterized, so backfire is free.**
`ApplySpellEffect(spell, at, dir, strength, out)` takes the position as an
argument, so "cast it at the muzzle" and "cast it at the caster's own chest"
are the *same call*. Backfire is therefore never per-spell special-case code: a
new element or form gets a thematic death the day it is added. This was built
this way from the first line, while only two effects existed, precisely because
it is the kind of structure that cannot be retrofitted once the glyph set grows.

**The VM is integer, in fixed point — and this is NOT rule 1.** Projectile
position/velocity are 24.8 fixed-point voxels (the exact convention
`ParticleSpawn` already uses), and mana/health/timers are integers. Spell state
is CPU-side gameplay state *outside* the hashed grid domain, exactly like mobs
and debris, so the world hash cannot see it either way — verified: the hash is
unmoved at `765da1f8` with the system live. It is fixed-point for **lockstep MP
(§10) and replay debugging**, where a projectile's path must reproduce
bit-exactly on every machine. The comments say so explicitly so nobody
"simplifies" it back to float. Floats appear only at the drawing boundary.

**The VM is not player-coupled.** Casting is a free function over (glyph list,
caster state, origin, direction) → emitted ops; a mob will drive the identical
`Cast()` call. Health is read through a `CasterHealth` callback rather than a
field, which is what lets the player's health stay where it actually lives —
`PlayerAvatar`'s per-part hp — instead of a parallel number that would drift
from the visible damage state within a session. `PlayerCaster` (inventory +
spoken stack) is a separate struct and `Player` is untouched.

**Rule 2 applies to magic, with no exception.** Every sustained effect declares
a finite budget up front: a trail carries a hard voxel VOLUME budget that only
decreases, and the projectile dies when it is spent; lifetimes and live
projectile counts are capped in `glyphs.json` and clamped against engine
ceilings at load. A generation counter is wired now (`Spell::gen`, capped)
even though nothing triggers anything yet — it is the subcriticality guarantee,
and it is annoying to add after triggers exist. This codebase has hit the
"permanent condition keeps chunks awake forever" trap three times already
(light-gated rules, staining, viscous liquids); a trail spell must not be the
fourth, and the selftest asserts the budget is respected *exactly*.

**Every sequence compiles into something that does something.** There is no
invalid spell — only one that does something other than you meant. That is the
design thesis: the ancient language punishes imprecision by granting the
literal request, not by refusing to parse. `CompileSpell` is total, and two
rules are what make it so:

- **Repetition amplifies.** The Nth utterance of a glyph contributes 2^(N-1)
  and costs 2^(N-1) times its mana, so `sand`=×1, `sand sand`=×2,
  `sand sand sand`=×4 — the running total for N words is (2^N − 1)× the base.
  Doubling rather than a linear ramp because the interesting decision ("is this
  worth an entire extra mana bar?") only exists if the curve is steep, and
  because it makes the price legible without arithmetic: each extra word costs
  as much as everything before it combined. Capped at ×64 (`kMaxAmplifyPow`);
  past the cap an extra word is free and does nothing, which is more honest
  than charging for an effect the budget will refuse to deliver. A *different*
  element replaces rather than stacks (`water lava` throws lava): an element is
  what the spell is made OF, and "made of two things" has no meaning here while
  "more of it" does. Repeating a form throws harder, repeating a modifier buys
  a proportionally bigger one — `trail trail` is one trail with twice the
  budget, not two trails fighting over one projectile.
- **An unspoken form is Spray.** `SpellForm::Spray` is the fallback, not "no
  form": a bare element flings a few loose voxels of itself out of the caster's
  hand as ballistic particles (the existing ejecta pipeline, §5 — they fly,
  collide and reinsert on landing). So the shortest legal spell is one word,
  and every longer sequence reads as an elaboration of it rather than as a
  correction. Spray is emitted from inside `ApplySpellEffect`, so a fatal spray
  backfires into the caster's own body for free like everything else.

Only silence is not a spell. A form with no element still charges and fizzles.

The HUD shows what the VM thinks the spell is — "spray: 6 voxels from your
hand", "bolt ×2, element ×4 + trail ×2" — which is worth far more than
validation, since the question is never "is this legal" but "what will this do".

One arithmetic trap this cost, recorded because the two counters look
interchangeable and are not: the amplification counters track repeats *beyond
the first* (one utterance ⇒ pow 0, which is what the `×` multipliers want),
while the mana charge needs the count of *prior* utterances. They differ by one
from the second utterance onward. Reading the counter directly for the charge
billed the second `sand` at ×1, so three sands cost 3/6/12 while the voxel
count correctly doubled at 3/6/12 — the output and the price silently disagreed.

**Casting into health makes a spell IMPRECISE, not merely expensive.** Cost ≤
mana casts normally; cost ≤ mana + health casts but spends the remainder as
health *and* wobbles the trajectory in proportion to how deep it went; cost >
mana + health runs the spell's own payload at the caster and kills them. That
middle case is the whole mechanic — it makes the mana bar a *precision meter*
rather than a second HP bar — so the HUD draws mana and health on one axis with
a hard break at the crossover, rather than as two numbers.

Two decisions worth recording because the obvious alternative is wrong:

- **`transmute_to` is an OVERWRITE brush op (mode 1), not the laser's melt mode
  (2).** Melt converts each cell to *its own* authored `molten` product
  (stone→lava, sand→molten glass), which is exactly right for a heat beam and
  exactly wrong for "transmute to acid", where the caster chooses the target
  material. Mode 1 is the existing primitive for that; no second conversion
  path was invented.
- **A projectile treats UNKNOWN cells as PASSABLE, the opposite of the player
  controller's choice.** The CPU mirror covers only the 3×3×3 chunks around the
  player (~48 voxels), which is ample for a capsule that never leaves its own
  neighbourhood and useless for a 48 vox/tick projectile that exits the mirror
  within one tick. Reading Unknown as solid — the conservative-looking option —
  detonates every bolt in the caster's face. Out-of-window space is still
  solid, per §3. The cost is that a bolt fired at a distant wall passes through
  it; the honest fix is a swept `RequestChunkFetch` along the flight path, not
  a bigger mirror.

Wards and glyph conjoining are landed as **shape only** (structs + `glyphs.json`
blocks, no behaviour). The recorded intent for wards is the load-bearing part:
a ward filters the incoming **op stream** (cheap, CPU-side, sim untouched), not
the CA — so it stops someone *casting* acid at your feet but not acid already
flowing toward you. That is deliberate: it keeps the falling-sand game
underneath and makes "cast next to them and let physics do it" the counterplay.
Ward drain is a **fraction** of max mana rather than a flat amount, because a
flat cost lets a big late-game pool buy invulnerability.

Op budget fairness is explicit (`SpellSystem::kSpellOpsPerTick = 24` of the 64
`BrushOp`s, alongside `gore.bleedOpsPerTick` — 6 by default — for mob and for
avatar bleeding each; magic's share is deliberately NOT tunable, so turning the
gore up cannot starve spells, and the bleed side is clamped to 64) and overflow is
**counted and shown in the HUD** rather than dropped silently — a spell that
sometimes doesn't fire is miserable to diagnose.

Selftest gate `spells`: the trail's voxel budget is respected exactly and the
projectile dies with it; an overcast resolves Fatal, emits its own payload, and
asks for the caster to be carved; and a bare element sprays, with N+1 words
producing *exactly* twice the matter of N at *exactly* the doubled price.
Invariants, not plausible numbers — the amplification is asserted as an exact
relation between three casts rather than as absolute counts, because a gate
that only checked "more words ⇒ more output" would pass a linear ramp too, and
because measuring the relation is what caught the off-by-one above. The first
version of the gate folded the impact op into the trail total and reported 343
voxels against a 64 budget, where the budget was fine and the measurement was
wrong.

### Items, and mouse-directed melee (2026-08-20; `game/item.h`, `game/melee.*`)

A hotbar and a sword, built as the melee counterpart to the spell system: the
same division of labour (main.cpp owns the player's inventory, the systems are
player-agnostic) and the same refusal to add a parallel damage path.

**The pose is the hitbox.** A swing does not switch on a hitbox during an
animation window and it does not test a cone in front of the crosshair. The
blade's authored `edge` segment is read through its **live** transform and
swept from where it was last tick to where it is now; whatever that quad passes
through is cut, at the point it was crossed. So the location struck is the
location that loses voxels — which is only worth saying because §7's live-limb
carving already made "lose voxels *there*" expressible. Melee adds no gore
code: it calls `CarveLimbRadial` for flesh and `MeltBodyAt` for debris, the
same two calls the laser splits between, and dismemberment stays geometric
(a limb comes off when the cuts disconnect it, not when a counter hits zero).

**The mouse is the swing.** Holding the attack button guards; the accumulated
mouse velocity steers the guard and, past a threshold, commits a cut along the
direction actually moved — a diagonal flick is a diagonal cut. Damage scales
with the blade's measured tip speed, so the player's own motion is the damage
roll: no cooldowns, no swing timer, no randomness, and being slow is punished
by being ineffective rather than by being locked out. The HUD prints the phase
and the measured mouse speed, because an input this analogue has to be
*falsifiable* — the player needs to tell "the game misread my flick" from "I
misjudged the distance".

**The arm swings; the blade does not steer.** The weapon keeps the grip angle
its rig gives it, orthogonal to the forearm, and only the arm's IK chain is
driven. An early version re-aimed the held part at the cut direction every
tick; it looked like the sword swivelling in the fist, and because the override
wrote into the flattened pose it also fought the animation pipeline and widened
the walk until the legs failed their own upright assertion — a "leg bug" whose
cause was the thing the character was holding.

**A held item is a rig part, not an object.** The sword is a part of the
avatar's own `.vox` (`scripts/gen_mina.py`, the staff precedent from
`gen_wizard.py`): severable, non-vital, cheap to knock loose. Equipping is
"show that part". So a dropped sword, a severed sword-arm, a burnt sword and a
carved sword are all things the existing systems already do, and `ItemDef`
stays a name plus a behaviour kind rather than a mesh.

Two traps worth recording, both found by the selftest:

- **A held prop must not inflate `MobDef::worldSize`.** That box is the
  *creature's*, and the avatar derives `origin_`, the gait pivot and its
  standing height from it. A blade reaching outside the body re-centred the rig
  on the weapon — visibly, it slid the whole avatar (and the camera riding its
  head) sideways. Anything tagged `prop` is now measured out of the box.
- **A weapon must not cut its wielder.** The blade starts inside its own hand
  and sweeps across the body's front, so without an explicit `OwnsBody` reject
  every guard saws through the arm holding it.

The gate asserts the property the feature exists for: the limb under the edge
loses voxels while a limb on the far side of the *same mob* does not. A
crosshair-cone hitbox would pass "did it damage something" and fail that.

### Voxel art pipeline, articulated mobs, and the laser (2026-08-19)

Implemented per `docs/PLAN_voxel_art_and_mobs.md`; the through-line is that
handmade art becomes matter the existing destruction pipeline already breaks.

- **Prefabs (`sim/voxload`, `game/prefab`):** MagicaVoxel `.vox` in
  `assets/prefabs/`, parsed with the scene graph (nTRN/nGRP/nSHP, frame 0).
  **Palette index == material ID** — modeling is painting with materials
  (`scripts/gen_palette.py` emits the palette PNG from materials.json); no RGB
  colour-matching, ever. The loader converts Z-up→Y-up once,
  chirality-preserving. `PrefabPlacer` stamps models as exact-cell ops, 16k per
  tick, pending cells kept in *world* coords because the residency window can
  shift mid-placement; the `kCellOpIfAir` spare-bit flag gives paint-into-air
  semantics with the occupancy test on the GPU (deterministic). Same-cell
  conflicts with island ops defer a tick rather than race GPU write order.
- **Mobs (`game/mob`):** one Jolt body per limb (scene-graph partitioned, each
  limb independently inside `DebrisVoxel`'s int8 range), joined by the new
  joint API in `Physics` (Fixed/Hinge/Ball, voxel-unit anchors, auto-derived
  from limb AABB adjacency or overridden in the JSON sidecar). **Ball is a
  LIMITED swing-twist cone** (Jolt's own ragdoll joint), not a free point
  constraint: since intra-mob collisions are disabled two sentences down, the
  cone is the only thing keeping a corpse's parts out of each other, and
  without it a ragdoll folded its thigh 180° up through its own pelvis. The
  cone's centre line is derived at load from the rig (anchor → limb centre)
  and its half-angles come from the limb's `tag` (waist 40°/25°, hip 80°/30°,
  shoulder 90°/70°, untagged 90°), overridable per limb. Joint DIRECTIONS are
  passed in the REST frame and rotated into each body's live pose inside
  `CreateJoint` — a world-space frame would re-centre every limit on whatever
  pose the bodies were in, which is wrong exactly where it matters, in
  `RebuildLimbBody`'s mid-ragdoll rebuild. Gate: `ragdoll-joints`. Bodies are
  CPU-float gameplay state outside the hashed grid domain; blood, severed
  limbs and corpses reach the grid exclusively through the op stream, so
  determinism rule #1 is untouched. Alive = kinematic keyframe walk (ground
  sampled from the chunk cache); death = flip dynamic and hand every limb to
  `DebrisSystem::AdoptBody`, where culling, terrain upkeep and settle-back
  apply with zero mob-specific code. Intra-mob collisions are disabled via a
  Jolt `GroupFilterTable` — adjacent limb boxes otherwise fight their joints
  and the ragdoll never sleeps. Mob limbs render as extra body slots appended
  after the debris bodies (shared 12-bit slot space, `kMaxBodySlots`).
### Mob steering: intent vs actuation (2026-08-21; `game/mob.cpp`)

Locomotion was one block that read the ground, snapped `heading += 90°` when
blocked, and translated. That is why mobs only ever moved on the four cardinal
axes: the *only* thing that ever wrote a heading wrote a right angle, instantly.
Turning and walking were the same statement, so there was nowhere to put an AI
that wanted to move at 23°, and nowhere to put a turn that took time.

It is now four stages with one direction of data flow, each with a single
responsibility:

| Stage | Function | May write | Why it is separate |
|---|---|---|---|
| sense | `SenseGround` | — | One terrain probe per tick, an 8-way fan in the *mob's own frame*. Intent and drive can no longer disagree about the ground (they each ran their own probe before), and a future sensor — vision cone, sound event, nav query — has one obvious place to join. |
| intent | `DecideIntent` | `desiredHeading`, `driveScale` | **The AI seam.** The only stage allowed an opinion. Today it is wander-and-avoid; a behaviour tree or utility scorer replaces this one function and inherits working steering, drive, gait and pose. |
| steer | `Steer` | `heading`, `turnVel` | The only writer of body facing, and it moves it at a bounded, ramped rate. |
| drive | `DriveLocomotion` | `origin`, `phase` | Translates along the **actual** facing, scaled by alignment with intent. |

The load-bearing split is `heading` (where the body points) versus
`desiredHeading` (where it wants to point). Because drive translates along
`heading` while `Steer` closes the gap over several ticks, a heading change
*automatically* traces an arc, and a mob that must turn around pivots roughly in
place — both fall out of one multiply rather than needing a turn-in-place case.

That split is also the guard rail. A behaviour is expressed purely as "set
`desiredHeading` and `driveScale` this tick" and structurally *cannot* teleport
the facing, so no future AI can reintroduce the snap by accident. The public
`SetDesiredHeading` is subject to the same clamp — "face the player" is a
request, never a rotation.

Free angles come from two choices, not from removing the 90° constant: the fan
is scored by **angular distance** from the current heading (so grazing a wall
deflects a few degrees along it instead of ricocheting orthogonally), and the
chosen probe is only aimed *toward*, at 0.6 of the offset, so the mob never
commits to a multiple of 45° — it re-senses as it turns and settles wherever the
terrain actually allows.

Two traps worth keeping written down, both found by measurement rather than
inspection:

- **Unknown footing must read as WALKABLE.** The probe reaches past the CPU
  mirror long before it reaches anything interesting, so treating unknown as
  blocked stops a mob dead at the edge of its own knowledge — an invisible wall.
  This is the mob-scale twin of the projectile rule in CLAUDE.md: *unknown* and
  *out-of-window* are different tests. The drive's own `haveGround` check is
  what prevents walking off into space.
- **Probe reach is per-axis, not an isotropic `max`.** A long creature that
  probes at `max(sizeX, sizeZ)` reaches well past where it can walk and refuses
  gaps it would fit through.

Limits live in `LocomotionDef` (`game/anim.h`), authored per creature in the
sidecar's `locomotion` block — deliberately *not* in `GaitDef`, which is a
property of the leg rig and is mirrored by the editor's preview. `turnRate`,
`turnAccel` (a critically-damped arrival, so a body decelerates *into* its
heading rather than ringing around it), `turnRateMoving` (turn tighter when
slow), and the `driveAlign*` band that scales forward speed by facing error.

The `mob steering` selftest gate asserts what "any angle" actually means, since
"the heading changed" and "it walked far enough" are both satisfied by the old
snap: the per-tick facing delta stays under the rate cap, the turn takes real
time (~0.67 s, not one tick), it arrives without overshoot, and the mob is seen
*travelling* along **21 distinct headings** in a single turn — impossible for a
90° snap (4 in the whole plane) and for turn-in-place-then-go (1).

- **Bleeding:** wound budgets, capped per tick, emitted as radius-1 brush ops;
  blood is a real material (organic tags → burns/reacts for free) with a
  subcritical dry-to-air decay so pools go back to sleep (rule #2).
- **Laser (hold F):** grid cuts are per-tick melt-mode brush ops at the picked
  surface — the recessing cut is just repeated ops, and cutting supports feeds
  the same island/support-loss pipeline as explosions. Body cuts ray-test Jolt
  first: crossing a joint anchor severs it; hits on plain debris **melt body
  voxels at the beam** (`MeltBodyAt`), boring a channel that splits the body
  once it actually severs it — see "Direct body damage" in §7. The original
  implementation bisected the body along a plane through the beam, which read
  as teleport-slicing and let camera orientation, rather than the carved
  geometry, decide what fell off. `SplitBody` survives for that plane-cut case
  but is no longer on the laser path.

### Animation pipeline (2026-08-20; `game/anim`, docs/PLAN_voxel_editor.md §B)

The single-sine limb swing became a layered pose pipeline. **All of it is
CPU-float presentation state and none of it is hashed** — the mob's only grid
contact remains BrushOp/CellOp (blood, severed limbs settling), so determinism
rule #1 is untouched. `game/anim.{h,cpp}` holds the schema-facing pose/blend/IK
machinery with no Jolt or World dependency (the editor shares it); `mob.cpp`
consumes it and owns the physics plumbing. Per mob per tick:

1. **Sample** active clips (fused quat+pos keyframes, integer ms, easing enum).
2. **Blend** override layers, weight-normalized, per-part masks, rest-pose
   fallback below Σw = 0.1. N-way blends are **nlerp with accumulator
   alignment** — each incoming quaternion is sign-fixed against the *running
   sum*, never against a fixed reference and never a chain of slerps, which
   would be order-dependent.
3. **Additive** layers applied *after* the normalize: `q_out = q_base *
   nlerp(identity, dq, w)`, with `dq = conj(q_ref) * q_src` measured against
   the clip's own frame 0. Applying them before the normalize would let the
   weight division scale the delta away.
4. **Flatten** parent→child in one linear pass; the loader topologically sorts
   limbs so a parent's index is always below its children's.
5. **IK** — two-bone analytic, in model space, strictly a **post-process**.
   IK is never a blended layer: blending two IK results yields a pose that
   satisfies neither end-effector constraint. Reach is clamped to
   `[|L1-L2|+ε, L1+L2-ε]`, every `acos` argument is clamped to `[-1,1]`, the
   root angle uses the `atan2` form, and the bend plane comes from an
   **explicit per-chain pole vector** with a fixed fallback axis when the
   cross product degenerates near full extension.
6. **Physics blend / submit** through the existing `MoveKinematicBody` path.

**Gait** is the base layer and needs no per-gait table. Each leg's ideal
contact is `hip + fwd·strideBias + vel·leadTime`, snapped down through
`GroundHeightAt`; a foot unplants when it has drifted past
`stepThreshold·legLength` **and no other leg in its group is swinging**. That
one constraint *is* the gait state machine: singleton groups give a walk,
diagonal pairs give a trot, and losing a leg just means the survivors take
their turns sooner. Stride and lift scale by speed so an idle mob's feet are
genuinely still. **Body height and tilt are derived from the foot average and
the foot-plane normal (Newell's method)** — which is why walking up voxel
stairs works with zero slope-handling code. Pelvis bob runs at 2× step
frequency (one rise per footfall), sway/roll at 1×, plus spine
counter-rotation and a progressive phase lag per hierarchy level.

**Springs** (Holden's closed form, unconditionally stable at any dt) drive
parts like tails. A part is *keyed or jiggled, never both*, so a spring never
fights a clip for the same channel. **Flipbooks** re-point a limb at another
`.vox` model by an integer-ms frame index, rebuilding instances only on an
actual frame change (the Jolt shape never changes — a frame swap must not
rebuild collision).

**Dismemberment** gained a second threshold, `severImpactSpeed`: a fast enough
hit takes the limb regardless of remaining hp (absent ⇒ infinite, so old rigs
are unchanged). A severed piece is handed to `DebrisSystem` immediately but
**holds its last animated pose kinematically for ~0.25 s** before going
dynamic — cutting straight to ragdoll on the hit frame reads as a teleport.
On release it gets `ClearCollisionGroup`: the mob's `GroupFilterTable` would
otherwise suppress contact between the severed arm and the torso it came off
*forever*. Constraints are removed via Jolt's `RemoveConstraint`, not left
disabled.

Rigs are data. Every new sidecar field (`gait`, `tag`, `chains`, `clips`,
`flipbooks`, `spring`, `severImpactSpeed`) is optional; `dummy.json` still
works untouched, its `swingAmp`/`swingPhase` now running as a procedural layer
*inside* the same pipeline rather than as a parallel code path.
`assets/mobs/critter.*` (`scripts/gen_critter_mob.py`) is the worked example:
a quadruped with two-segment legs (real two-bone IK), diagonal-pair gait
groups, a spring tail and a masked flinch clip.

### Player avatar and third-person camera (2026-08-20; `game/avatar`, `game/thirdperson`)

The player has a visible, dismemberable body: a small hooded, robed figure
("mina") authored at **4 microvoxels per world voxel** (`assets/mobs/mina.*`,
`scripts/gen_mina.py`), rigged into 15 independently severable parts — head
(the hood), torso, hips (the robe skirt), upper/fore arms, hands, thighs/shins
and feet.

**The rig is sized from the engine's own player constants, not by eye.** At
`kVoxelMeters = 0.10`, `Player::kHalfY` (0.85 m) makes the collision box
exactly **17 world voxels** tall and `Player::kEyeOffset` (0.65 m) puts the
first-person camera at **world voxel 15** — so the figure is authored 17 voxels
tall with the hood's face void centred on voxel 15, and `gen_mina.py` asserts
both rather than trusting a comment. The predecessor (`gen_wizard.py`, kept as
a second character) was authored 28 voxels tall against an assumed
`kVoxelMeters` of 0.0625; at the real 0.10 that is a 2.8 m giant standing in a
1.7 m box, with the eye camera inside its chest. Deriving the art's height from
the same constants the controller uses is what stops that class of mismatch.

Which def the player wears is one string — `avatarDefName` in `main.cpp`, which
the selftest's avatar block reads too, so swapping characters cannot leave the
test pinned to the old one.

**One schema, two drivers.** The avatar is an ordinary `MobDef`: same `.vox` +
sidecar format, same loader, same `AnimSkeleton` runtime. It therefore
inherits clips, masks, two-bone IK, the gait state machine, springs and — the
reason it is worth doing this way — the `states` dismemberment table, at zero
marginal cost. What it does *not* inherit is `MobSystem`'s driver: `PlayerAvatar`
takes its position and facing from `Player` instead of the wander drive.
Expressing this as "a mob the player possesses" would have meant threading
input through the wander drive, the despawn sweep and `kMaxMobs`; a separate
driver costs one file and leaves `MobSystem` untouched. Anything an animator
authors for a mob works on the player and vice versa.

**Damage is the same path as everything else.** A laser crossing a joint
anchor severs that part; the piece is handed to `DebrisSystem::AdoptBody` with
its `MicroBodyRef`, so it keeps its microvoxel detail and is then culled, burnt
and settled by the ordinary debris rules with no avatar-specific code. Bleeding
goes out as `BrushOp`s and `ParticleSpawn`s on the shared per-tick budget, i.e.
through the MutationQueue like every other world edit (rule 3). Every field in
`PlayerAvatar` is CPU-float presentation state and never touches the hashed
grid (rule 1) — `--selftest` determinism is unchanged with an avatar standing.

**Your own body must not push you** (`Layers::AVATAR`). The avatar's limbs are
drawn *around* the player's capsule proxy — `origin_` is derived from
`player.pos` — so on the ordinary `MOVING` layer every limb is permanently
interpenetrated with the proxy. That leaked into movement twice: the solver saw
a contact it could never resolve, and `PlayerPushOut` summed a large
depenetration vector whose *direction swung with the gait animation*. The
result was the player being steered backwards and diagonally while trying to
walk forward, sporadically — a movement bug whose cause was the renderer's
character model.

The fix is a fourth object layer, `AVATAR`, identical to `MOVING` except that
`ObjPairFilter` refuses the `AVATAR`↔`PLAYER` pair and `PlayerPushOut` (which
filters on `MOVING`) cannot see it. The split is about **contacts, not
visibility**: rays still hit these bodies, so laser damage and dismemberment
are untouched — `CastRayBody` uses a `DynamicLayerFilter` accepting both
layers rather than a single-layer filter. `Physics::SetBodyAvatarLayer` moves a
body on or off it; both layers map to the same broadphase layer, so this is
never a broadphase rebuild. A severed limb is switched *back* to `MOVING` when
its hold expires (and the whole corpse on death), because a detached arm has
stopped being "you" and should bump you like any other debris.

Selftest-gated: the avatar test walks a proxy alongside a spawned avatar for 30
ticks and asserts the peak `PlayerPushOut` magnitude is zero. Sampled over many
ticks rather than once because the failure was *animated* — a single sample can
land on a frame where the limb swing happens to cancel. With the layer
assignment removed that assertion reads ~19 voxels/tick.

**Dismemberment drives movement.** `AvatarLocomotion` is the single place the
damage state is turned into gameplay: `speedScale` comes from the matched
`AnimStateRule`, while `jumpScale`/`canJump` are derived from *leg liveness*
rather than authored per state, so a new rule cannot accidentally grant a
legless wizard a jump. `Player` multiplies its tuned speeds by these, which is
why losing a leg slows you, losing both drops you to a crawl, and fly mode
ignores all of it. The camera reads the same struct, so the pose and the
framing agree by construction instead of via two tables kept in sync.

One trap worth recording: `AnimSelectState`'s `minChainsLost` counts **every**
IK chain, and this rig has arm chains as well as leg chains. `minChainsLost: 2`
would therefore have fired "crawl" when both *arms* came off. The wizard's
rules name leg parts directly; the two formulations are only equivalent on an
all-legs rig like the critter.

**Camera.** `Camera` stays orientation-only and is shared by both modes, so
mouse look, the picking ray and the walk basis are identical in first and third
person. `ThirdPersonRig` adds only the position policy: an orbit boom, swept
against the voxel world and pulled in to the first hit (unloaded space counts
as solid — the residency-window rule — or the camera backs out of the world).
Pull-in is instant and push-out is eased; easing inward would leave the camera
inside the wall for the duration of the ease, which is the artifact players
actually notice. In first person the body is hidden but the arms, hands and
staff are kept. The render eye is the only consumer — brush, laser, grenade and
physics all keep using `Player::EyePos()`, so no camera setting can move the
world hash.

---

## 9. Rendering

- **Ray traversal, not meshing.** Terrain geometry changes every tick; re-meshing
  churn would dominate. Raymarch the voxel grid directly (DDA through chunks,
  `nonEmpty` flags for empty-space skipping — the same flags the physics uses).
  The sim already lives in GPU memory, so the renderer reads it for free.
- Pipeline: fullscreen ray pass → G-buffer (albedo/normal/depth/material) →
  deferred lighting. Voxel face normals from the hit axis; liquids take
  smoothed normals from the fullness-field gradient instead (see the water
  section below — implemented).
- Variant nibble → palette jitter in-shader (stable per-grain color, no reshuffling
  as grains move — exactly why the variant lives in the voxel).
- Rigidbodies/debris: two options. v1 = raster their marching-cubes meshes,
  composited by depth against the raymarched terrain. Better (proven by the BFS
  project): **GPU-driven render of each body's bounding box, then raymarch the
  body's own voxel payload inside the box to the exact voxel hit** — debris stays
  voxel-crisp instead of marching-cubes-smooth, and reuses the terrain shading
  path. Adopt once bodies carry their voxel payloads (M6).
- **Far-field cascades (implemented 2026-08-19; docs/PLAN_far_field_cascades.md):**
  view distance beyond the residency window comes from kFarLevels nested
  toroidal kFarN³ (256³) volumes centered on the player, one material byte per
  cell. The far grid is DECOUPLED from the window size (phase 5, when the
  window went 512³): level k cells span 2^(k + kFarShiftBase) fine voxels with
  the shift base chosen so level k's box edge is always 2^k WINDOW edges —
  cascade distances scale with the window at constant memory (128 MiB total at
  kFarLevels = 8; outermost half-extent = 256× the window radius ≈ 4 km at the
  512³ window and 6.25 cm voxels). Levels are filled on the GPU by sampling `genCell()` at stride
  (worldgen.wgsl `far` — the "sieve"), recentered with hysteresis like the
  streaming window, and refilled a plane at a time (≤ kFarListCap
  level-chunks/tick, managed by `sim/farfield`). **Edits reach the far field
  (phase 2):** each tick, `worldgen.wgsl fardown` runs one workgroup per entry
  of the compacted dirty list — the same `DispatchWorkgroupsIndirect` args the
  occupancy update uses, so a settled world dispatches nothing — and re-derives
  from the LIVE voxel grid every cascade cell whose sample point falls in a
  chunk that changed. It samples the identical center voxel the sieve does, so
  edited and pristine regions agree exactly where they meet, and a chunk edited
  while resident leaves its downsampled ghost behind automatically (eviction
  needs no special handling). Because a level-k word packs 4 cells = 4·2^k fine
  voxels — wider than a 16-voxel chunk for k ≥ 2 — neighboring chunks' byte
  writes collide, so `farVox`/`farOcc` are atomic in both far kernels
  (`atomicAnd`+`atomicOr` per byte, `atomicMax` on the occupancy flag, which
  keeps it conservative: never falsely zero). Atomics are legal here precisely
  because cascades carry no determinism requirement.
  **Edits SURVIVE a cascade refill (edit persistence, 2026-08-24;
  `src/sim/faredits.h`):** the downsample above is only half the story, because
  the sieve is the other producer of the same cells and it knows nothing but
  procgen. Every refill — an incoming plane after the player crossed a level's
  box edge and came back, a `ResetLevel` on teleport, the `FullRefill` at
  startup and after `LoadWorld` — used to overwrite the ghost with pristine
  terrain, so a crater you dug and walked away from healed itself and a
  reloaded world's horizon showed the hillside the save had faithfully
  destroyed. `FarEdits` is the memory that survives it: a CPU index keyed by
  (level, level-chunk) holding the RAW MATERIAL each cascade cell's sample
  voxel carried the last time the CPU saw the chunk that owns it, fed by the
  same eviction harvest that fills the `ChunkStore` (so it is populated exactly
  for chunks that diverged from procgen, `Stream::modified_`) and reconstructed
  from those region files by `FarEdits::RebuildFromStore` on load.
  `FarField::PrepareTick` attaches each fill entry's patch list to the dispatch
  (the `farPatch` buffer: an (offset, count) header per dispatch entry, then a
  payload of `(mat << 12) | cellIndex`), and the same `far` workgroup applies
  them right after its pristine sweep, behind one `storageBarrier()`. The patch
  carries the raw material and NOT a finished byte, so `farSurfaceMat` — the
  same function the sieve and the downsample call — still decides the color: a
  patched cell is byte-identical to what `fardown` would have written for it,
  which is what keeps their agreement invariant intact and keeps a region from
  changing appearance as it flips between resident-and-downsampled and
  refilled-and-patched. Budgeted like every other queue here: `kFarPatchCap`
  words per tick, charged before emission, and an entry that will not fit stays
  queued rather than being filled with half an edit. **Cascades stay DERIVED
  and DISPOSABLE — they are simply derived from (seed + persisted edits) now
  instead of from the seed alone.** Nothing about `farVox`/`farOcc` is saved,
  nothing is authoritative, nothing is hashed, and a world with no edits
  uploads a zero-count header and dispatches exactly the work it did before.
  **What is representable, honestly.** The sample rule is unchanged and is the
  resolution limit: a level-k cell is the ONE fine voxel at the center of the
  2^(k + kFarShiftBase)-wide region it covers, so an edit shows up at level k
  only if it moves a voxel that happens to be a cell center — in practice only
  if its extent reaches a whole cell. Majority/occupancy downsampling was
  rejected: the sieve would have to evaluate `genCell` 2^3k times per cell
  (4096× at level 4, unbounded by level 8), and any rule other than
  center-sampling breaks the sieve↔downsample agreement that keeps refilled
  planes seamless against live chunks. At `kFarShiftBase = 1` and
  `kVoxelMeters = 0.10` that gives (level: cell size, band it serves, smallest
  edit it can show):

  | level | cell | serves out to | smallest visible edit |
  |---|---|---|---|
  | 1 | 4 vox (0.4 m) | 51 m | ~0.4 m — a brush stroke |
  | 2 | 8 vox (0.8 m) | 102 m | ~0.8 m — a doorway |
  | 3 | 16 vox (1.6 m) | 205 m | ~1.6 m — a small crater |
  | 4 | 32 vox (3.2 m) | 410 m | ~3 m — a room, a big blast |
  | 5 | 64 vox (6.4 m) | 819 m | ~6 m — a tower, a quarry |
  | 6–8 | 128–512 vox | 1.6–6.6 km | 13–51 m — terrain-scale work only |

  So "dig a crater and see it from 60 m" (level 2, needs ~0.8 m) works and
  "see a single dug voxel from 3 km" does not, and the second one is correct
  LOD behaviour rather than a gap to close. The index grows only with edits
  (~73 cells per edited 16³ chunk, 4 bytes each — a chunk contributes 64 cells
  at level 1, 8 at level 2, 1 at level 3 and usually none above) and is
  RAM-only; a `RebuildFromStore` over a world that was fully flushed by a save
  over-indexes with pristine chunks, whose patches are exact no-ops on the GPU
  because the sieve produces the same byte.
  Rays that leave the fine
  march without a hit continue through the cascade boxes with the same
  occupancy-skipped DDA in level-cell units; t-ordering (each level starts at
  the previous box's exit) keeps coarse data from ever occluding fine data.
  **A ray leaves the fine march at whichever comes first: the window exit, or
  the in-window LOD handoff** (`TUNE_LOD_HANDOFF_DIST`, 24 m default, render
  group). The handoff is a `min()` clamp on `trace()`'s `tExit`, so it moves
  where the cascade takes over without touching the handoff machinery — the
  cascade start distance, the one-sided seam dither and the `tPrev` ordering
  all read `tExit` exactly as before. PRIMARY rays only (`wantMedia`): a shadow
  ray that gave up at 24 m would report "lit" for a receiver whose blocker is
  further, which unshadows terrain rather than coarsening it. Setting the knob
  ≥ the window half-extent (25.6 m) restores window-exit-only behaviour.
  Measured worth ~8-11% of the offscreen frame, saturating at 22-24 m; past
  the handoff, terrain quantises to level-1 cells (40 cm), which preserves
  silhouettes but coarsens mid-field grass detail
  (`docs/PLAN_surface_flight_perf.md` A1).
  **Distance look (phase 4, 2026-08-19):** kFarLevels is 8 (128 MiB farVox —
  exactly the WebGPU default storage-binding limit; the horizon sits 2 km out
  at 6.25 cm voxels). Cell COLOR is decoupled from cell SHAPE: shape still
  comes from the center sample, but a cell that is the topmost solid cell of
  its column (`farSurfaceMat` in worldgen.wgsl, shared verbatim by sieve and
  downsample so their outputs stay identical) takes the SURFACE skin material
  — `genCell` at y = `surfHeightAt(x,z)` — instead of the body material the
  center sample lands on. Without this every distant hillside read as stone
  with grass contour stripes, because the grass skin is 1 voxel thick and a
  coarse center sample almost never hits it. At levels ≥ 5 (cells 2 m+, wider
  than a tree) surface cells under a crown's XZ footprint take the leaf
  material instead (`treeCanopyAt`) — trees too thin to survive center
  sampling are flattened into the terrain, so the far forest keeps its canopy
  color. Far hits shade with the same palette/face/ambient constants as the
  near field plus: palette jitter keyed at a fixed ~0.5 m world frequency
  (not per level cell, which flattened coarse cells into single-color slabs),
  a one-sample AO from the cell above, sky reflection on distant water top
  faces, and a SOFT sun-shadow term (`farShadowed`) — one occupancy-skipped
  DDA toward the sun through the hit's own cascade level, attenuating lambert
  to 0.3 rather than zero because at cascade resolution most casters are
  single-cell terrace steps and a hard term reads as speckle noise. Fog is
  aerial perspective: `applyAerial` converges surfaces exactly to
  `skyColor(rd)` (the old ×0.9 target left everything hanging slightly darker
  than the sky it should dissolve into, which read as a gray veil).
  **Transition polish (phase 3):** each handoff — the window→level-1 one and
  every level→level one — is pulled NEARER by a per-pixel hash of the fragment
  coordinate, up to half a cell of the outer level at that seam
  (`farDither` in `raymarch.wgsl`), so the constant-distance ring where the
  representation changes dissolves into a static stipple. The offset is
  strictly one-sided because the levels tile `t`-space exactly: pushing a
  handoff *farther* opens a band that no level marches and rays fall straight
  through it (measured as thousands of speckled holes when it was first written
  two-sided). The hash takes no time input and is keyed on screen space — a
  temporal key crawls, a world-space key re-aligns into arcs as the boxes
  recenter. Fog density is a uniform tracking the cascade radius that is
  actually FILLED: `FarField` counts pending fills per level and reports the
  half-extent of the level below the innermost incomplete one, so a cold start
  or teleport fogs out the bands still in the queue instead of showing sky
  through them, clamped between the full-horizon pin (`kFarFogDensity`) and a
  ceiling at level 2's half-extent (`kFarFogDensityMax`, so the residency
  window is never fogged away) and eased over a few frames. Determinism is
  untouched by construction: cascades are derived render-only data — never
  read by the sim, never hashed, no MutationQueue involvement (the selftest's
  `far-downsample` gate proves the propagation works, `far-persist` proves it
  survives a refill, and the determinism gate proves the hash is unmoved).
  Remaining limits: center-sampling terraces the surface *within* a level (the
  dither only addresses the seams between levels; a real blend would cost a
  second march per pixel), edits smaller than a cascade cell are invisible at
  that level (the table above), and an edit landing on a hash tick (every 15th,
  which takes the whole-world occupancy path and never compacts the dirty list)
  propagates one tick late. Coarse-level cave
  suppression was assessed and rejected: `caveAt` carves enclosed column bands
  capped at `h - 10`, so caves never breach the surface and coarse center
  samples never land in a void — verified by rendering levels 4–6 with fog at
  3% of nominal, which shows solid terrain with no swiss-cheese.
- **Water as a surface, not fog (implemented 2026-08-19):** translucent liquids
  were originally shaded purely as participating media — a per-metre tint
  accumulated along the ray — and that is why a lake read as a flat blue disc
  painted onto the terrain. An absorbing volume with no interface has no
  reflection, no glint, no refraction and no depth cue, and no amount of tuning
  the tint fixes any of those. Liquids now carry BOTH halves: the volume terms
  (`mediaTau`/`mediaTint`) still accumulate, and `trace()` additionally records
  where the ray first crossed into liquid (`liqT`/`liqCell`/`liqAxis`) plus the
  total distance travelled inside it (`liqPath`). `shadeWater()` then builds a
  real air/water interface there:
  - **Normal from the fullness field, not the voxel face.** The liquid state
    nibble is fill level in eighths (§4), so the liquid column height varies
    cell to cell and its gradient is the true macro slope of the surface —
    the standard scalar-field-gradient normal, over data the sim already
    maintains for free. Four taps, and it is what stops a lake looking like
    tiled glass. Side/bottom faces keep their flat voxel normal (the gradient
    describes the top surface only).
  - **Ripples** as a sum of 5 directional wave bands (analytic slope, no
    texture), each faded out once the per-pixel footprint approaches its
    wavelength — per-band mip selection done analytically. The footprint must
    be the screen-space one (distance × pixel angle ÷ grazing cosine): a raw
    distance term is radial about the camera and visibly stamps concentric
    rings onto the water.
  - **Fresnel (Schlick, F0 = 0.0204)** blending reflection against refraction.
    This is the term that makes water read as wet — 2% head-on and ~100% at
    grazing across one continuous surface.
  - **Traced reflection.** The engine already has a DDA, so the reflection is a
    real secondary ray (step-budgeted, media-blind) rather than a screen-space
    trace — no missing-information artifacts at screen edges and correct
    reflections of geometry behind the camera, which is precisely what
    Teardown's SSR cannot do. Sky fallback, blended across the horizon rather
    than switched, or the ripples break the surface into per-pixel speckle.
  - **Per-channel Beer-Lambert absorption** over `liqPath`, in metres, plus an
    in-scatter floor. Red is absorbed ~9× faster than blue, so shallow reads
    cyan-green and deep reads blue — the strongest depth cue there is, and
    structurally impossible with a scalar tint.
  - **Caustics** from the curvature (divergence of slope) of the long swell
    bands, projected along the sun direction and applied MULTIPLICATIVELY to
    the bed. Additive caustics light up the volume and read as glowing blobs
    floating in the water; multiplicative ones scale the light already landing
    on the bed, which is what they physically are.
  - **Sun glint** as a tight specular lobe that gets *tighter* with distance to
    match the ripple damping, and shoreline foam masked by the ripple field.

  One consequence worth flagging: the media saturation early-out
  (`MEDIA_TAU_MAX`) is a SMOKE optimization and had to be scoped to gases.
  Water's authored opacity against the legacy absorption constant saturates
  after ~2.7 m of path, so any lake deeper than waist height — or any shallow
  one viewed at a grazing angle — used to terminate the primary ray in
  mid-water and report no hit, which is the mechanical reason lake beds were
  invisible. Liquids get their own far looser depth cap instead.

  All of this is render-only float math on render-only data. The sim never
  reads it and the world hash never covers it, so determinism rule #1 is
  untouched (it scopes to sim state).
- **Viscous liquids / blood (implemented 2026-08-20):** a third case, distinct
  from both water and lava. Blood is nearly opaque, strongly absorbing and
  viscous, and — because it comes out of NPCs — is usually NOT a still pool but
  droplets in flight, thin trails and disconnected spatter. `shadeViscous()`
  handles it; the material is classified by authored data (`moveEvery > 1` and
  high `opacity`, on a non-`MATF_OPAQUE` liquid), never by material ID.
  - **The smooth field normal is the load-bearing part.** Water gets away with
    a normal from the gradient of its COLUMN HEIGHT because a lake is a wide 2D
    height field. Blood is fully 3D and often one or two voxels thick, so that
    treatment leaves every side and bottom face with its raw voxel normal and
    the result reads as a heap of individually-shaded cubes. Instead the liquid
    is treated as a scalar DENSITY field (fullness, which the sim already
    maintains), sampled with trilinear interpolation and differentiated —
    continuous across cell boundaries, so neighbouring voxels agree on their
    normal and the cube structure dissolves. A per-cell dome term was tried
    first and made it worse: it renders each voxel as a ROUNDED cube, which is
    precisely the gelatin look. Costs 48 voxel reads, so it runs only on the
    primary blood hit — never in reflections or shadow rays.
  - The silhouette is softened separately, by fading toward the background
    where the field value is low. The smooth normal fixes the SHADING; without
    this the ray still stops on a voxel face and a droplet's outline stays a
    hard cube however well it is lit.
  - **No travelling ripples.** Water's five wave bands are wind-driven gravity
    waves on an open surface; a splash of blood is centimetres across and far
    too viscous to carry them, and running that field over blood makes a puddle
    look like it is boiling. Only a slow, tiny surface-tension wobble, faded out
    on pools.
  - A **wet sheen** (tight specular lobe, plus an ambient term so it still
    reads wet in shade) is what makes it read as fluid rather than red paint,
    and it is deliberately not gated to up-facing surfaces — a trail running
    down a wall is wet too. Droplets get a BROADER lobe than pools: a bead's
    curvature spreads its highlight, and using the pool exponent on a droplet
    makes it vanish at any distance.
  - `bloodPooling()` measures droplet-vs-pool the way `moltenPooling` does for
    lava, but samples all three axes rather than horizontally only: vertical
    extent is what separates a wall trail (tall, thin, moving) from a floor
    pool (wide, flat, still).
  - **Stains** (§3, §6) render as a change to the ALBEDO, composited before
    lighting, so they take the same sun, shadow and AO as the surface they
    soaked into — added afterwards they glow in shadow and read as decals
    floating above the geometry. They MULTIPLY toward the stain colour rather
    than replacing it, so the substrate's texture stays visible through them
    (soaking, not painting), and a value-noise threshold breaks the coverage up
    so a light stain is scattered flecks and a heavy one is near-solid.
- **Molten surfaces (implemented 2026-08-19):** lava is the OPPOSITE problem to
  water and reusing the water treatment would be wrong in every particular.
  Water's look comes from what it REFLECTS and TRANSMITS; lava is `MATF_OPAQUE`
  (it resolves as a surface hit and never enters the media path) and its look
  comes almost entirely from what it EMITS — there is no reflection worth
  tracing, nothing behind it to refract, and no depth to absorb through.
  Previously lava was flat palette albedo plus one per-cell random flicker,
  added at `emission/255 * 1.7` on top of an already sun-lit surface: every
  channel saturated and a pool rendered as a featureless WHITE slab, brighter
  than the sky and with less structure than the grass around it. Note that
  merely lowering the intensity would only have produced a flat ORANGE slab —
  the defect was absent spatial structure, not exposure. `shadeMolten()`
  replaces it (detected by class + `MATF_OPAQUE` + emission, never by material
  ID, so any modder's emissive opaque liquid gets it):
  - **A crust.** The whole look. Real flows are dark basaltic plates with
    glowing cracks between them, not uniform orange. Built from 4 octaves of
    ridged value noise (`1 - |2n-1|`, which turns smooth blobs into the thin
    branching filaments that read as fractures — smooth noise alone gives soft
    mottling that reads as rust), in world metres so plate size is independent
    of voxel scale, advected per-octave so the crust shears rather than
    sliding rigidly.
  - **A blackbody-ish ramp** anchored on the AUTHORED palette (so retinting
    lava in JSON still yields a coherent heat ramp), with band boundaries
    pushed late: most of a flow is crust and cooling red rock, and spreading
    the bands evenly puts the average pixel in the orange/yellow range, which
    renders the pool as glowing gold honeycomb.
  - **Emission scaling steeply with temperature** (~T³, tuned not physical) so
    cracks out-radiate plates by a large factor — without that the surface
    averages back into the flat slab this replaces. Peak intensity is bounded
    so crack cores stay inside the range where the tonemap still discriminates
    hue.
  - **Heat spill** onto neighbouring non-emissive surfaces, so a pool lights
    its own rim instead of sitting in its basin like a decal.
  - Top faces run cooler than sides (they radiate to the sky and skin over
    first), and a grazing-angle term draws a hot lip around the far edge.

  **The tonemap had to change with it, and this fixed a scene-wide bug.** The
  output stage was a bare `pow(color, 1/2.2)`, so anything over 1.0 clipped
  flat — a hot surface lost all colour AND all structure at exactly the moment
  it got interesting. It is now Reinhard-with-white-point applied to
  LUMINANCE, with the colour rescaled by the luminance ratio. Per-channel
  Reinhard (the obvious implementation, and the first one tried) desaturates
  catastrophically at ALL intensities, not just bright ones: the `1/(1+c)`
  denominator compresses a strong channel far harder than a weak one, so a
  saturated ember orange (`#ff5a1a`, sat 0.90) comes out tan (sat 0.59) even at
  *half* exposure — every warm emissive surface in the scene was being turned
  gold. A controlled amount of per-channel behaviour is blended back in
  weighted by `mapped³`, so hue survives the midtones and only genuinely hot
  cores bleach toward white, which is the real blackbody progression.

  **The crust only applies where there IS a crust (`moltenPooling`).** The
  whole model assumes a continuous surface — plates, cracks between them, a
  skin that cools and shears — and none of that means anything on a single
  voxel. Laser spatter, splash droplets and individual melted voxels have no
  room for a plate, so the crack field just paints an arbitrary slice of noise
  across them and they read as dirty smudges; those cases genuinely looked
  better under the old flat emissive shade. `moltenPooling()` samples the
  horizontal neighbourhood (horizontal only: what matters is whether there is
  EXTENT for a crust to form across, and a one-voxel-deep sheet spread over a
  floor is still a pool) and every crust-specific term — crack field, per-patch
  temperature jitter, face split, embers — is scaled by it. Isolated lava falls
  back to a uniform hot value, which is exactly the pre-crust look.

  The threshold is deliberately LOW (ramp over 0.10..0.42, not 0.35..0.85). The
  useful distinction is "isolated speck" vs "everything else", not "pool
  interior" vs "pool edge": a rim cell has neighbours on one side only, so a
  high threshold puts the entire boundary of every pool in the transition band
  and draws a bright orange ring around it — more objectionable than either
  look on its own.

  **Embers are render-only, and that is an architectural choice.** The sim has
  a particle system and a data-driven `emit` reaction — lava emitting fire is
  one line of `reactions.json`. But lava is PERMANENT: unlike ember, which
  decays away, a pool never stops existing, so an emit reaction on it fires
  every tick forever and its chunks never clear their dirty flag. That is the
  subcritical-growth rule in §11 / CLAUDE.md #2, and the selftest gate
  (`sleepActive < 32 && particlesLeft == 0`) fails on both counts. A cosmetic
  effect must not hold the simulation awake, so sparks are reconstructed
  analytically from position and time instead — no voxels, no particles, no sim
  state. The tradeoff is that they cannot collide or ignite anything, which is
  correct for blow-off sparks; gameplay-relevant ejecta should go through the
  particle system, spawned by a bounded event (a splash, an impact).

  Two non-obvious things about drawing them, both of which produced a distinct
  visible artifact before being fixed:
  - **Resolve each spark analytically, never by sampling the field.** Evaluating
    a spark density at each march step paints a flat patch of whatever cell the
    step landed in, and the sparks render as large translucent RECTANGLES — the
    marching grid becomes the thing you see. Computing the ray's closest
    approach to each spark in closed form is step-size independent, so sparks
    stay points however coarsely the segment is stepped.
  - **The spark lattice must be STATIC in world space.** Advecting the lookup
    (`q.y += time*rate`) and undoing the shift on the spark position makes a
    whole vertical column of cells map onto sparks at similar screen positions,
    so consecutive steps each resolve a different spark just above the last and
    they chain into ~100-pixel vertical streaks. Keying one spark per (x,z)
    COLUMN, rising and looping within its own height band, makes a spark a
    point by construction — a ray cannot stack several of them.
  Spark radius is clamped at both ends: a pixel-size floor so a spark never
  falls between samples and vanishes, and a ceiling so a distant spark's
  linearly-growing pixel footprint does not inflate it into a fuzzy square.

  Perf note: `heatSpill()` runs on every non-emissive surface pixel and cost
  ~13 ms of a 30 ms frame unguarded — paid overwhelmingly by terrain nowhere
  near lava. It is now gated on a one-read chunk-occupancy probe along the
  normal and capped at 4 taps, which brings it to ~1 ms. Any future per-pixel
  effect keyed on a *rare* material needs the same treatment.
- **Sky: a scattering model, not a gradient (2026-08-20).** The sky used to be
  a two-colour lerp with `pow(dot(rd, sun), 800)` added for the sun. That
  cannot produce a sunset, cannot light the world differently at different
  times of day, and reads as a painted backdrop. It is now a single-scattering
  model, and every part of the look falls out of it:
  - **Rayleigh** in-scatter with the real 1/λ⁴ coefficients (0.144/0.313/0.794)
    makes the sky blue and the setting sun red *from the same numbers*.
  - **Mie** in-scatter with a Henyey-Greenstein lobe puts the haze glow around
    the sun, wavelength-neutral, so it turns orange only because the light
    reaching it has.
  - A **Kasten-Young air-mass curve** (1.0 at the zenith, ~38 at the horizon)
    thickens both toward the horizon, which is what gives the pale horizon
    band, the deep zenith, and the reddening of a low sun.
  - The **sun disc** is drawn at its true 0.53° angular size (oversized 3× by
    default because physically-correct is a pinprick at game FOV),
    pixel-antialiased, with per-channel limb darkening so it reads as a sphere.

  Two coefficient traps, both of which produced a *khaki* sky and both of
  which are easy to re-introduce:
  1. **Do not apply the sun's reddened transmittance to the Rayleigh
     in-scatter.** The in-scatter integral already accounts for the
     wavelength-dependent loss; multiplying a blue in-scatter by a red
     transmittance cancels the blue. Measured: zenith 0.33/0.34/0.13.
  2. **Extinction strength must be a separate constant from in-scatter
     strength.** Sharing `skyRayleigh` between "how blue is the sky" and "how
     fast does the sun redden" couples two knobs that want opposite values — at
     12 it left a 15° sun keeping 72% of red and 20% of blue, so the whole dome
     went khaki (measured 102,98,75). They are now `skyRayleigh` and
     `sunReddening`.

  The horizon's warmth is driven by the **sun's** air mass, not the view ray's:
  the view mass is ~38 at the horizon whatever the sun is doing, so using it
  reddens the horizon at midday, which is wrong.

- **Night sky and the moon (2026-08-20).** Stars are point-sampled from a
  hashed direction grid (nearest cell centre, angular distance to it) rather
  than thresholded noise — a `step(0.99, hash(dir))` starfield samples a
  *volume* and sparkles violently under rotation. Twinkle scales with air mass,
  so stars scintillate near the horizon and sit steady overhead, as they do.
  Over that: a galactic band with fbm dust lanes, two nebula masses, and
  aurora curtains built from a product of two scrolling noise fields (a product
  is filament-like where a single field is blobby). The moon is a real disc
  with phase geometry, maria, craters and earthshine, and it is a genuine
  second key light — `keyLightColor()`/`keyLightDir()` route sun-vs-moon
  through one place, so shadows, water glints and caustics all follow whichever
  body is actually up.

### Static micro-detail (2026-08-20; docs/PLAN_voxel_editor.md §A)

Grass, foliage and flowers are ordinary world cells that the RENDERER draws at
2×/4×/8× finer resolution. A material may declare a `"micro"` block naming a
`.vox` whose every model is one flipbook frame; the loader (`src/sim/microvox.*`)
packs all frames of all such materials into one brick pool (`array<u32>`, four
8-bit palette indices per word) plus a per-material `MicroBrick` table sized
`kMaterialSlots`. Palette index == material ID, as everywhere else, so a micro
voxel shades through the existing material table with no new colour path.

**Where it hooks.** In `trace()` (`raymarch.wgsl`), when the world DDA lands on a
solid cell whose material carries `MATF_MICRO`, a nested Amanatides–Woo DDA runs
over the `subdiv³` brick in cell-local space. A hit reports the sub-voxel's
material and the face it was entered through; a **miss lets the ray continue past
the cell**, which is the load-bearing half — a grass cell is mostly air, and a
micro cell that blocked on a miss would draw every tuft as a solid 6 cm cube.
Per-cell variety is a quarter-turn yaw swizzle plus an optional whole-sub-voxel
XZ jitter, both keyed on `hash3(seed, 0, cellIndexW(cell))`.

**Why this is free.** The world cell stays one ordinary 16-bit voxel. The CA
never sees the brick, the world hash never covers it, chunk sleeping is
unaffected, and the `.svx`/`.svd` save format is unchanged — a meadow costs
exactly what the dirt it replaced cost (§11). The alternative, storing real
sub-voxels, would have multiplied resident memory by `subdiv³`.

**Determinism (rule 1).** Wholly render-side. `microBricks`/`microPool` are bound
to the raymarch pipeline and to nothing else, so no sim kernel can read them.
The two per-cell hashes and the flipbook frame index are integer functions of
`(seed, cell)` and of `tick` respectively — never of wall time — so the animation
reproduces in a replay without any of it becoming sim state. `--selftest`
confirms it: the world hash is byte-identical with the feature on and with
`microMaxPerRay: 0`.

**Bounds (rule 2).** Three, all necessary:
- the nested DDA is capped at `3·subdiv + 4` steps, the exact worst case for a
  diagonal crossing of an `S³` box;
- `TUNE_MICRO_MAX_PER_RAY` (~8) caps how many bricks ONE ray may enter, because
  a grazing ray over a meadow crosses dozens of cells and every *miss* keeps it
  alive. Past the cap the next micro cell is treated as solid — terminating is
  bounded, letting the ray fly is not;
- past `TUNE_MICRO_LOD_DIST` the cell shades as a plain voxel, since at ~1 px per
  cell the nested march is deciding the colour of a sub-pixel. This makes a
  micro material's own `colors` its LOD colours, so they must be authored as the
  model's AVERAGE (a poppy is muted green with a red cast, not red).

**Shadows and occupancy.** Shadow and reflection rays skip micro cells entirely
(v1, matching the plan), and `isRayBlocker` correspondingly excludes
`MATF_MICRO` from the per-chunk BLOCKER count. The two must agree: without the
occupancy half, a chunk full of grass would report itself solid and terminate
every chunk-skipping shadow ray at the meadow surface — a lawn casting the
shadow of a wall. This is safe because occupancy is derived render-only data,
written by `sim_occupancy`/worldgen and read only by the renderer and by CPU
streaming; no sim kernel reads it and the hash does not cover it. The `total`
count is deliberately untouched — a grass voxel IS present, and save/stream
worthiness keys off that.

The `occupancy` buffer additionally carries a **sub-chunk bitmask** in its tail,
past the per-chunk count words: two u32 per chunk per class (TOTAL, BLOCKERS),
one bit per 4-voxel block, written by the same three producers in the same
sweeps (`world.h` `kSubOccShift`, `common.wgsl` SUB-CHUNK OCCUPANCY). It is one
buffer rather than two so every `pass_table.def` `uses` row, barrier and
bind-group entry for `Occupancy` already covers it. **The raymarcher's consumer
is DEFAULT OFF** (`const SUBOCC_SKIP` in `raymarch.wgsl`): measured, it makes
the frame slower, because the mean chord of a 4-voxel box is 2.7 voxels and a
box-exit jump costs 3-4 DDA steps. Kept as a re-runnable refutation with the
content number it turns on reported by `--measure` (MEASUREMENT 1d); the full
argument is Correction 6 of `docs/PLAN_surface_flight_perf.md`.

Worldgen does not place these yet (Wave 1a deliberately does not touch it); the
`--shot` harness paints a demo meadow, and they are brush-selectable like any
other material.

### Dynamic microvoxel bodies (2026-08-20; docs/PLAN_voxel_editor.md §C)

Static micro-detail above substitutes a brick for a *grid cell*. Creatures and
rigidbodies are the other half of the problem: they have free float transforms,
so there is no cell to stand in and nothing for the world DDA to hand off to.

A mob sidecar may declare `"skinScale": 2`, `4` or `8`. Its limb `.vox`
coordinates are then **skin units** — that many per world voxel — so the same
silhouette gets 2×/4×/8× the resolution without getting bigger.

**Skin resolution and collider resolution are separate** (2026-08-21). They were
one number until it became the binding constraint: `DebrisVoxel` stores local
coordinates as `int8`, so a single lattice bounded a limb at 120/scale world
voxels — 30 at scale 4, 15 at scale 8, and the player avatar is 17 world voxels
tall. Widening the type would have been the wrong fix, because the two costs are
unrelated: the brick march is a fragment shader over one OBB, so **skin cost
tracks screen area**, while the collider is Jolt boxes and **tracks voxel
count** — an 8× collider is ~512× the boxes to greedy-merge and solve against.
Coupling them held the cheap axis hostage to the expensive one.

So `skinScale` is authored and `MobDef::physScale` is DERIVED at load: the
finest of {8,4,2,1} that fits both the int8 bound and a cost ceiling
(`kMaxPhysScale`). It is engine-picked because the bound it satisfies is a
property of how big the art is, not a choice an author can make usefully — and
because that makes collider resolution *emergent*, which silently moves mass,
contacts and ground probes, the loader logs the value it chose for every def.

The **skin is authoritative and the collider is derived from it** by majority
fill (`phys/lattice.h DownsampleSkin`, tested CPU-only in `tests/lattice_test.cpp`).
A carve edits the skin and re-derives the collider, so the two cannot drift —
disagreement is unrepresentable rather than merely discouraged. Data flows skin
→ collider and never back, which is what keeps the whole mechanism outside the
hashed domain: the moment skin state fed a collider decision it would be in the
hashed domain and rule 1 would apply. `src/sim/microbody.*` packs each
limb model once at def load into a second brick pool (`array<u32>`, four 8-bit
palette indices per word, palette index == material ID as everywhere else) and
records a `MicroBodyModel { base, dims, scale }` per limb. Models are shared by
every instance of the def while undamaged; the first time a particular body is
blasted, cut or shattered it clones its model into a private block and edits
that (copy-on-write — see "Destructible micro bodies" in §7), so the shared art
stays pristine and only bodies that actually took damage cost pool words.

**OBB raster + per-fragment march.** Each micro body draws as ONE box — 36
vertices — between `DrawBodies` and `DrawSprites`. The vertex shader positions
the corners from `bodyXforms[slot]` and the model's dims; the fragment shader
rebuilds the world ray, rotates it into object space by the body's conjugate
quaternion, slab-tests, and runs an Amanatides–Woo DDA over the brick, or
`discard`s. Cost therefore scales with the SCREEN AREA a limb covers rather than
with its voxel count, which is the whole point: the instanced-cube path
(`debris.wgsl vsBody`) is one 36-vertex instance per voxel, so a scale-4 limb
would cost 64× the instances for the same on-screen result. That inverts rule
§11's "cost tracks activity, not content".

**Backfaces only.** The pipeline culls FRONT faces, so only the far side of the
OBB rasterizes. Front faces would vanish the instant the camera entered the box
(near-plane clipping); the far side is covered from every camera position
including one inside, and the march simply starts at the ray's slab entry rather
than at the triangle it was generated from.

**Depth is the load-bearing detail.** The fragment writes `frag_depth` with
*exactly* the raymarcher's reversed-Z convention — `viewZ = t·dot(rd, camFwd)`
then `KNEAR / max(viewZ, KNEAR)` — with `rd` the UNNORMALIZED camera-to-fragment
vector on both sides. Nothing in the shader normalizes anything: the object-space
ray is the world ray under a rigid rotation, and the micro-unit ray is that
scaled uniformly, so all three parametrizations share one `t`. Get this wrong by
normalizing and micro bodies punch through terrain or sink into it. Hardware
`GreaterEqual` testing then composites them against the world, particles, cubes
and sprites with no sorting anywhere.

**Routing.** Body render slots are shared: a slot with a micro model draws here
and contributes NO cube instances (drawing both would double-draw at the wrong
size); a slot without one keeps the cube path unchanged. The routing key is the
**body**, not "is a mob limb", which is what makes a severed micro limb keep its
detail for free: `MobSystem` hands a `MicroBodyRef` to `DebrisSystem::AdoptBody`
alongside the voxels it already passes, and the debris body owns it from then on.
Deliberately a parameter rather than a side table keyed by physics handle — a
side table would need syncing at every spawn, sever, death, cull and reset, and a
recycled Jolt `BodyID` could then paint unrelated debris as somebody's leg.
Physics
builds micro limbs at voxel pitch `1/physScale` (collider extents, convex radius
and per-voxel volume all scale together), so mass and physical size match the
scale-1 art they replace whatever resolution the collider was derived at; the
`pitch == 1` path is arithmetically identical to before.

**What micro bodies deliberately do not do (v1).** They do not burn, split or
shatter. Both of those mutate `b.voxels`, and the brick they render from is
shared per-def — a per-body edit would be invisible on screen, and a
copy-on-write pool is out of scope. Body burn additionally maps body-local
coordinates straight onto world cells, which a micro voxel is 1/scale of. They
DO settle back into the grid: `SettleBodies` collapses each `scale³` micro block
to at most one world voxel by majority fill (blocks under half full become air),
which is bounded and paid at most once per body.

**Shadows: none, v1** — parity with the cube path, which also casts none. Shadow
rays must never iterate models; a coarse occupancy proxy stamped render-side is
the stretch goal.

**Bounds and cost.** The per-fragment DDA is hard-capped at `3·maxDim + 4` steps
(worst-case diagonal of the brick) with no data-dependent loop bound anywhere.
The draw list is CPU-compacted, so the instance count IS the number of micro
bodies and a world with none does no upload, no bind and no draw. Determinism is
untouched: the pool, the model table and the draw list are bound to the
microbody pipeline and to nothing else, and `--selftest` reports an unchanged
world hash with a scale-2 critter walking through the scene.

- Later: emissive materials feeding a cheap GI (light propagation volumes or
  per-chunk flood lighting), volumetrics for gases.

## 9b. Wind (added 2026-08-25)

Plan of record: **`docs/RESEARCH_wind.md`** — the decision record, the industry
survey behind it, and the five-phase schedule. This section is the binding
summary; that file is where the reasoning lives.

**Phases 1, 2, 3 and 4 have landed and the gate is ON.** `sim.windMode` ships
at **1**: the CA drift bias, the ballistic-particle drag and the MPM node force
are live, and the pinned hash moved `882a30f3` -> `47dd1520` in a dedicated
rebaseline commit (and again to `b717a33d` for the gas vertical model).

**Phase 2 (wind primitives) landed 2026-08-26, and with it entrainment.**
Settled powder can now be blown, and it is safe: a fan/gust/tornado is a bounded
parametric object that declares its own footprint through the mutation path,
which is what makes those chunks CPU-known before anything writes them. See
"Wind primitives" below. Zero primitives is an exact identity all the way down,
so the feature ships hash-neutral.

**The gas vertical model (2026-08-25).** A gas used to rise *unconditionally* —
step 1 of the movement tail was a bare `tryMove` straight up, returning on
success — so a plume with open sky above it never reached a line of wind code
and no drift bias could make smoke lean. Buoyancy is now a **probability that
wind redistributes**, in 1024ths of a move attempt:

    rise = 1024 - down          the move carries +1 Y
    sink = (down - 1024) / 2    the move carries -1 Y
    flat = the remainder        the move is horizontal only
    lean = fh - up              a rising move ALSO carries a downwind step

`down`/`up` are the vertical wind as a fraction of `sim.windDriftSpeed`, `fh`
the horizontal one, both scaled by the material's authored `windResponse` and
the dev multiplier. The horizontal share is spent on an **up-diagonal**, not a
flat step, so a plume leans without slowing its climb — paying for drift out of
the rise rate would make a 45-degree plume climb at half speed, which is not
what a gas in a crosswind does. An updraft **straightens** rather than
accelerates: `rise` is already at certainty in calm air, so the only way lift
can read as more vertical is by cancelling the lean.

Three consequences worth knowing:

* **This one adds candidates.** The `sink` tier is a genuinely new move that no
  gas could make before, so the "only reorders what it was already going to
  try" argument does not cover it. The bound is argued directly instead: every
  candidate is reach 1, every one goes through the ordinary `tryMove` (stamp
  discipline, density test and `markDirty` untouched), and `canDisplace` is a
  density comparison rather than a direction one, so downward motion needed no
  change there.
* **Calm air is bit-identical.** `gasIntent` returns `(0,1,0)` whenever wind is
  off, the material does not respond, or `rise == 1024 && lean == 0` — the same
  move step 1 would have made.
* **The model is asymmetric about vertical gusts, by design.** An updraft
  cannot push `rise` past certainty while a downdraft subtracts from it, so a
  gusty field lowers a plume slightly. At the shipping weather that is a
  fraction of a cell; the `wind-gas` gate quiets gusts precisely so its height
  assertion reads one decision rather than this.

Ambient weather is horizontal in the MEAN (`windAtQ` returns `y = 0` for it);
the vertical component comes from the two gust bands at `WINDQ_VERT` = 0.18 of
gust amplitude. So the `flat` and `sink` tiers are not reachable from ordinary
weather — they need the `wind x voxels` dev multiplier, a lowered
`sim.windDriftSpeed`, or a wind primitive. Mode 2 (GLOBAL settled-powder entrainment) is
implemented but is still not a default — see below; PER-PRIMITIVE
entrainment is, and it is the shipping path. Phase 5 (updrafts, violent-wind
excite) is not started.

### The decision, in one paragraph

Wind is a **pure function, not a stored field**: `windAt(worldPos, t)`,
evaluated on demand. It is composed of a deterministic CPU-computed weather
vector that evolves chaotically over minutes, two travelling gust bands whose
phase is a function of world position, an altitude ramp, and — in later phases
— a derived updraft term and a bounded list of parametric **wind primitives**
(fans, spell gusts) evaluated analytically like point lights. There is no
per-chunk vector storage, no neighbour-constraint relaxation pass, and no
resolution to choose: the function is continuous and costs only where it is
sampled. Rejected: per-chunk stored vectors smoothed by a ±θ neighbour
constraint — that is a per-tick pass over all 32,768 window chunk slots whether
anything moves or not (rule 2), it is new authoritative state that must be
saved, hashed or excluded, streamed and someday replicated, and what it
converges to is a smooth low-frequency field, which an analytic function
already *is*, for free, with no convergence latency. See RESEARCH_wind.md §3.

### The field

```
windAt(p, t) = (weather(t) + gustBands(p, t)) * altRamp(p.y)
             + updraft(heatBelow(p))     // phase 5
             + Σ primitives_i(p, t)      // phase 2
```

Units are world **cells per second** (`kVoxelMeters` = 0.10, so cells/s = m/s ×
10); the m/s knobs are converted once, on the CPU. The altitude ramp scales the
whole field including the mean, because wind aloft is faster wind rather than
the same wind with bigger gusts.

`windSampleAt` / `windAt` live in **`assets/shaders/common.wgsl`**, which is
prepended to every shader, so the field is in scope everywhere without being
copied anywhere. The evolving weather comes from **`WindWeather`
(`src/sim/wind.h`)** — a pure function of (tuning, seed, tick) that holds no
state and integrates nothing, so asking for tick 90,000 costs the same as tick
1 and gives the same answer on every machine. Its three outputs ride
`RenderParams` today and will also ride `TickParams` in phase 4 (the `dayPhase`
precedent: CPU-computed inputs that replay and the determinism gates must
capture belong on the tick input stream).

### Invariants

1. **Wind is a function. There is no stored wind field and no per-voxel wind
   state, ever** — voxel bits 19–23 stay free.
2. **One authoritative field implementation, in `common.wgsl`.** Every consumer
   samples it; none builds its own bands. This is why the debug overlay is
   evidence rather than decoration — it calls the same function the grass
   calls, so it cannot draw a wind the world is not in. A C++ mirror, if one is
   ever needed, gets a `check_invariants.py` entry.
3. **The ambient field never wakes a chunk.** Primitives (phase 2) dirty-mark
   only their own bounded, budget-charged footprint, through the mutation path.
   This is the "light-gated rules never sleep" lesson applied ahead of time: a
   condition that is always true must never call `keepAwake`.
4. Wind bias applies only to voxels **already executing the movement tail**;
   settled matter moves only via the entrainment threshold, inside awake
   footprints (phase 4).
5. Player and world wind exist **only as primitive ops on the input stream** —
   no side-channel writes (rule 3's philosophy).
6. Sim consumption is integer (`windAtQ`), gated by `sim.windMode`, and flipped
   only in a dedicated rebaseline commit (phase 4).
7. `windResponse` / `windFriction` are authored **material data** (JSON), never
   hardcoded per material in a shader (phase 3).

### What phase 1 shipped

- `windSampleAt` / `windAt` / `windBandWS` / `windMeanWS` / `windSway` in
  `common.wgsl`; `WindWeather` in `src/sim/wind.h`; a `windDir`/`windSpeed`/
  `windGust` row on `RenderParams`.
- **The two foliage sway sites rewired to sample it** — the brick sway and the
  strand blades in `raymarch.wgsl`. The two travelling gust bands, their four
  incommensurate rates and their world-position phase were *promoted out of*
  that code, so the character is preserved deliberately. One thing changed in
  the look: the elliptical anisotropy used to be hardcoded to "X leads, Z
  trails" and is now a projection onto the weather vector, which is why turning
  `wind.windDirDeg` now turns the grass. Per-column hash scatter and per-blade
  band weights are untouched — that decorrelation is what makes a field read as
  wind instead of as one rocking object.
- **The debug slope-field overlay** (`assets/shaders/debug_wind.wgsl`,
  `Simulation::DrawWindField`): an arrow per lattice point around the camera,
  oriented and coloured by magnitude. **F4** in-game, `wind.dbgWindField` in the
  tuner. Nothing is uploaded for it — the vertex shader derives each lattice
  point from its instance index and `R.camPos` — and it is skipped entirely when
  off rather than drawn transparent. It is a render draw, so it has no
  `pass_table.def` row: that table describes the sim's *compute* recording.
- The `wind.*` tuning group (Wind tab).

**Phase 1 is hash-neutral by construction** and was verified so: it touches no
sim kernel, and the pinned world hash is unchanged.

### What phases 3 and 4 shipped

**`windAtQ`, the integer field** (`common.wgsl`, next to the f32 one). Every
phase-3/4 consumer's output reaches the voxel grid — ballistic particles
reinsert themselves as voxels, MPM settles back through the excite seam, the CA
drift bias steers matter outright — so all of them read an integer
transcription of `windAt`, not `windAt` itself: f32 `sin()` is not bit-identical
between GPU vendors and the world hash is compared across machines. Q16.16
throughout, BAM angles (65536 = one turn) so the gust phase's modular reduction
is exact, and `windSinQ`, a parabola-plus-correction polynomial rather than a
lookup table because a WGSL `const` array cannot be indexed dynamically and a
`var<private>` one would be mutable state in a kernel that must not have any.
The two evaluations sit adjacent in one file deliberately: they cannot share
code across number systems, so proximity is the only thing keeping them from
drifting apart. They agree to well under a percent, which is what "the sand
blows the way the grass leans" needs; they are not, and need not be, the same
number — the render clock is wall time and the sim clock is the tick.

**Three consumers, one gate.** Ballistic debris and spray get a drag law at
`sim_particle.wgsl`'s single gravity site; MPM gets one at the grid-node update,
scaled by a low-mass exposure weight; the CA gets the drift bias and
entrainment. Each tests `T.windMode` at its own call site rather than relying on
`windAtQ` returning zero, because a drag term reading zero wind is not a no-op
— it would drag everything to a standstill and move the hash.

**§8's open question, answered: MPM wind acts on low-mass nodes only.** Wind on
every node of a pond is a *current* — the body translates, the surface stays
flat, and it reads as the lake being poured sideways. What wind does to water is
act on the interface. Gating on node mass gets that free, because "how much
fluid is around this node" is a number the solver has already computed.

**`windResponse` / `windFriction`**, 0–15 each, authored in `materials.json` as
`"wind": {...}` and packed into `MaterialGpu.flags` bits 8..15 — the struct is
64 bytes with no spare word, and every existing reader of `flags` on both sides
of the language boundary tests it with a mask, so a nibble in the high half is
invisible to all of them. Absent means derived from density (~4800/density and
1 + density/400). Derived defaults exist so a new material is windy the day it
is added; the *authored* value is the truth, because real susceptibility is
area-over-mass — SIZE — and a uniform grid has erased size.

**The `wind` gate** (`src/test/selftest_wind.cpp`) tests sign and invariance,
not magnitudes: reversing `windDirDeg` reverses the smoke's displacement (+28.96
vs −11.41 cells against the gate-off run), the settled sand bed is **bitwise
unchanged** under any drift-bias wind (invariant 4), the same script twice with
wind on gives the same hash, and grain count is conserved.

### Two dev force multipliers, one per tier

`sim.windGasScale` and `sim.windPartScale` (dev-panel sliders, 0–16x, default
1.0) multiply how hard the wind pushes each tier. They ride **TickParams as Q8
integers** rather than being const-folded like the rest of the wind coupling,
which is the point: a slider you have to press F5 to see is a slider nobody
drags. Deterministic all the same — integers on the tick input stream — and at
exactly 1.0 every consumer takes an exact-identity early-out, so "the sliders
are at 1x" and "the pinned hash holds" are one statement rather than two.

They scale **different quantities**, and that asymmetry is deliberate:

- **gas** scales the CA drift-bias *probability*, past its `windDriftMax` cap,
  up to certainty. Scaling the velocity there would move the slider for about
  the first 2x and then do nothing, because the bias ramp already saturates
  near the default weather — and a control that goes dead halfway is worse than
  no control. At the top of the range every moving gas voxel tries downwind
  first, and smoke reads as a conveyor belt.
- **particle** scales the wind *velocity* that debris, spray and MPM nodes are
  dragged toward. That is the only way past the drag law's own ceiling, since a
  particle cannot outrun the air however hard it is dragged.

Entrainment's threshold test deliberately reads the **raw** field through
neither multiplier: that test asks whether the wind beats a material's authored
friction, which is a property of the wind, and running a debug knob through a
physical threshold would silently retune every material's saltation point.
`sim.windEntrainSpeed` is the knob for that.

### Wind primitives (phase 2, landed 2026-08-26)

The player-facing half. A primitive is a **parametric object** — position, unit
axis, strength, radius, reach, lifetime, flags — summed analytically at every
wind sample exactly the way a point light is. There is no lattice, no resolution
to choose, and nothing stored per voxel or per chunk. Three kinds, chosen to
span the requirements rather than to enumerate shapes:

| kind | what it is | what it is for |
|---|---|---|
| `cone` | a jet along the axis, linear taper along it, quadratic across | fans, gust bolts, wind walls (JC4's wind tunnels) |
| `burst` | radial push from a point, or a **vacuum** at negative strength | blast fronts, implosions |
| `vortex` | tangential swirl + inflow + axial lift about the axis | tornadoes; whirlpools when the medium mask says water |

Anything else is these composed, which is the point of making them summable.
There is no square root anywhere in the evaluation — the radial profile is
quadratic in `r^2` and the burst takes its direction from the offset itself —
which is what keeps it affordable in the CA's inner loop.

**They ride the UNIFORMS, not a storage buffer.** At most 32 x 48 bytes of
per-tick CPU-authored configuration goes into `TickParams` and `RenderParams`,
so the whole feature costs **no new binding, no new barrier and no new
dispatch** except the wake below. A storage buffer would have meant a new
binding in both group-0 layouts (`common.wgsl` is prepended to every shader, so
one identifier cannot carry two binding numbers), a new pass-table row set and
the same again on the render side — for 1.5 KiB that changes once a tick.
Because the render copy is the same list, **the grass leans in a fan's blast
with nothing wiring foliage to fans**, and the F4 arrow overlay shows primitives
for free: all three sample one function (invariant 2).

**The price of riding the uniform: those two structs must be passed BY POINTER,
never by value.** This is not style, it is the condition under which the choice
above is affordable at all. Shipped by value, wind primitives cost the game
**220.1 ms/frame p50 against 20.3 ms by pointer — a 10.8x collapse, with zero
primitives alive** (measured 2026-08-26, RTX 3060 Ti, `--frames 400`). The cause
is not the struct's size and not the loop, which never runs at count 0: it is
that `windPrimAt` **dynamically indexes** `windPrims[b]`. A by-value uniform read
only at static offsets is scalarised away after inlining — which is why the old
~400-byte `RenderParams` was passed by value for a year at no cost. A dynamic
index has no static offset to fold, so the driver must materialise all 1936
bytes in scratch memory per call, and in the raymarcher's per-micro-detail-cell
sway path that is thousands of 1.9 KiB spills per pixel. Occupancy dies with it.
So every function that indexes the list takes `ptr<uniform, T>`, and so does
every function that calls one — a single `*R` anywhere in that chain reinstates
the copy. By pointer the cost is genuinely nil: 8 primitives force-evaluated at
every micro-detail cell in the world with the AABB reject disabled measures
20.5 ms against 20.3 ms idle. Note that **no headless gate can catch this** —
they measure the sim, and this is a render-occupancy cliff; the same family as
the far-shadow 45x and cascade-shadow 48x regressions.

**Movement is analytic in time, resolved on the CPU.** A travelling gust's
position is `origin + vel * (tick - spawnTick)`, evaluated once per tick for the
whole list rather than millions of times per sample; the GPU never mutates a
primitive. The lifetime envelope (attack/release, so a 40 m/s gust does not
switch on between two ticks) is applied to `strength` in the same pass.

**Producers, all through the op stream (invariant 5).** The `gust` glyph in
`glyphs.json` carries a `wind` block and emits through `SpellEmission` like
every other spell effect — so it is position-parameterized, and a *fatal* gust
goes off in the caster's own chest for free. The dev panel can place one where
the camera is looking, through the same `WindPrims().Spawn()`; there is no
dev-only path into the wind system. Both are refused rather than silently
displacing something when the world list is full, and the refusal is shown.

### Entrainment: the licence, and the landmine it defuses

Entrainment — a settled grain pulled loose by a wind that beats its authored
friction — is **the first rule in the engine that makes resting matter move**,
and two things depended on that never happening:

- **Rule 2.** An exposed dune, once woken, re-marks its own chunks for as long
  as the wind blows. The ambient field still cannot *wake* anything (invariant 3
  holds), but it can keep something awake.
- **The page table.** Entrainment is the first rule in the engine that makes
  SETTLED matter move, and the materialization set
  (`PLAN_page_table.md` §3.2) is derived from a mirror that is *tightened*
  against a lagging snapshot. That tightening is sound under every pre-wind
  rule, because a chunk of settled powder writes nothing — so dropping it, and
  letting its empty neighbour's page retire, costs nothing. Now a grain steps
  into a neighbour the CPU was told would never be written. Measured: 62 lost
  voxels over two 160-tick runs, at the same ticks in both, so deterministic
  rather than a race.

**A primitive holding `kWindPrimEntrain` is the licence, and it closes both.**
Every tick it lives, the CPU resolves its footprint, filters it against the
snapshot's occupancy (a cube of sky has nothing to entrain, so this is what
turns a fan's footprint from "a box" into "the surface it is aimed at"), charges
it against `sim.windWakeChunks`, and then does two things with the same list:
declares those chunks as **op targets** to the page table — so they are
materialized with their 26-ring before the command buffer exists — and hands
them to **`sim_mutate.wgsl`'s `windWake`**, the one kernel in the engine that
dirty-marks a chunk without writing a voxel. By the time a grain hops into a
neighbouring chunk, the CPU had already said that chunk could be written.

The licence is bounded at spawn, not trimmed at wake time: a primitive whose
footprint exceeds `kWindWakeMaxChunks` (512) is refused the flag outright and
still blows. Trimming would make entrainment work in an arbitrary corner of the
blast, which is worse than not working.

The **`wind-prim` gate** runs in the default suite and asserts, in a chamber
with no scaffolding at all: a licensed fan creeps a settled bed +12.65 cells
downwind and reversing it reverses the creep; a fan *without* the licence leaves
the bed **bitwise** unmoved while visibly blowing smoke; in a chamber with no
smoke — so genuinely asleep — the fan wakes 10 chunks and the bed still creeps,
which is the wake proving itself; grain count is conserved; twice-run equality
holds; and the suite's page-fault counter stays at **0**.

**One bug worth recording, because its shape recurs.** The wake did nothing at
first, silently: the per-tick counts cross THREE hand-written structs on their
way to the recorder (`Simulation::RecordCtx` → `rhi::TableCtx` → the recorder's
own `RecordCtx`), one copy was missing, the row's condition read a default zero,
and the row was simply never recorded. No error, no validation message, a green
build. `check_invariants.py`'s `counts` check now compares all three field-by-
field and asserts both copy sites, so the next one fails loudly.

### The GLOBAL entrainment mode is still not a default

`sim.windMode` is a ladder — 0 off, 1 drift + per-primitive entrainment, 2 also
**ambient** entrainment everywhere. Mode 2 is the same saltation rule with the
licence removed: no footprint, no budget, no CPU-visible cause, so both defects
above return in full. It stays a thing to look at (`SANDVOX_WIND_ENTRAIN=1` runs
its gate arms, which pass on their own terms) rather than a thing to ship, and
`LoadTuning` warns whenever the knob is set to 2. What phase 2 changed is that
you no longer need it: the shipping way to blow a dune flat is to point
something at it.

**The wider lesson, because it will be true again.** The page table's soundness
argument quietly rests on *"settled matter does not move"*. Any future rule that
makes resting voxels move without a CPU-visible cause lands in the same hole,
and the tell is a non-zero page-fault count with no obvious lost voxel near the
thing you were testing.

### Phases remaining

| # | Scope | Hash risk |
|---|---|---|
| 5 | Per-chunk hot-material counts → updraft term; violent wind promoting voxels to particles; capes when cloth exists | rebaseline |
| 6 | **Drafts through openings** — a small, local, coarse, sleeping relaxation volume, so a room with a door and a window carries a draft and smoke finds the exits. The first requirement the pure function cannot satisfy, because the answer depends on geometry. RESEARCH_wind.md §11 | rebaseline |

Phase 4b, the flip, is **done**: `882a30f3` -> `47dd1520`. What the rebaseline
established, beyond the sim being self-consistent: `sleep` still reports **0 of
32,768 chunks active** after settling, so rule 2 survives — which it must, since
the drift bias only reorders candidates a moving voxel was already going to try
and cannot make a settled one move. (**Superseded in part** by the gas vertical
model below: for a GAS, wind now adds candidates rather than only reordering
them. The rule-2 half of the argument is unaffected — a settled gas is a
contradiction in terms, and powders and liquids still only get the reordering.) `--residency dense` reproduces the same
hash, so the new number is the world and not a paging artifact. Both `--vk-smoke`
tables were re-recorded; `worldgen` is byte-identical in both, as it must be.
The QUIET smoke scenario moved this time (it did not for the previous two
liquid rebaselines), and that is correct rather than alarming: quiet is the
first 50 ticks after worldgen, when terrain powders are still coming to rest,
and matter in motion is exactly what the bias steers.

## 5b. Water bodies — a still lake as a NAME (added 2026-08-28)

`docs/PLAN_water_master.md` is the plan of record; `src/sim/waterbody.{h,cpp}`
plus `assets/shaders/sim_waterbody.wgsl` are the code; `--gate waterbody` is the
acceptance gate. **M1 (the registry) and M2 (the drain ledger and the surface
shave) are landed, and neither moves the pinned world hash.** What follows
describes what exists, and says plainly what does not.

### 5b.1 The problem, in one number

Draining a default pond through a 1-voxel hole takes ~39 minutes of game time,
and over that period the CA propagates pressure through ~87,000 cells for
~70,000 ticks to move water that a level and an area could account for with two
integers. §4's CA is the right model for water that is DOING something; it is a
very expensive way to hold water that is merely THERE.

So: give a still body of water a name and a small record of aggregates — its
free-surface level, that surface's cell count, its total volume in eighths, and
a ledger of what has been taken out but not yet taken off the top. Then draining
is arithmetic on the record, at O(1) per body per tick regardless of volume.

### 5b.2 What M1 actually is

A CPU-side registry, and at M1 nothing else: no GPU pass, no buffer, no
binding, no `pass_table.def` row, no `TickParams` field. (M2 adds all five —
see §5b.4. What survives unchanged is everything below.) `sim.waterBodyMode` is 0 by default
and mode 0 is an immediate early-out, so the subsystem costs one branch per tick
and **cannot move the pinned world hash at either value** — measured, not
argued: `--sweep sim.waterBodyMode=0,1` reports one hash, and the gate runs the
same 40-tick mutation script at both modes and compares.

Three pieces:

* **The basin registry.** A container is a pure function of (seed, tuning),
  because the terrain overhaul made a tarn's bowl REPLACE the ground rather than
  `min()` into it — so `pondAt` is an exact integer parabola and the three
  authored pools are exact flat-floored cylinders. `World::PondTile` and
  `World::AuthoredPoolList` publish what `world.cpp` already knows; nothing is
  re-derived, because a fourth copy of the terrain is how the deleted
  `surfHeightAt` went stale.
* **The container curve** (component 2, analytic half). Cell count per height,
  its prefix sum in eighths, and a binary search of that prefix sum.
  `crossSectionD2` algebraically INVERTS `pondAt` — `floor(a/b) >= m` is exactly
  `a >= m*b` for non-negative integers — so there is no resampling and no
  floating point. Cells, never columns: counting columns would silently
  reimplement the single-span-per-column assumption that got heightfields
  rejected.
* **The jurisdiction ladder** (component 5's structure). Below spill, over a
  volume threshold, quiescent for K ticks, no straddling chunks. Enter and exit
  thresholds are distinct and `LoadTuning` FORCES the gap, because a body parked
  on one shared threshold changes representation every tick and every change is
  a seam crossing where mass can be lost.

Chunk labelling is a sparse aux layer keyed by chunk (guideline #2), not four
bits stolen from the voxel word. A chunk holding two basins at two levels is a
STRADDLE and both bodies are refused — falling back to the CA is a safe
degradation and the detection is one branch.

### 5b.3 What it measured

The analytic curve reproduces the world **exactly**, on both container kinds:

| check | analytic | voxels / columns | error |
|---|---|---|---|
| authored lake volume | 2,782,656 eighths | 2,782,656 | +0.00% |
| authored lake surface | 14,493 cells | 14,493 | +0.00% |
| tarn r54 bowl (level walk vs column walk) | 128,214 cells | 128,214 | +0.00% |
| settled surface spread | 0 vox | — | — |

The bowl is checked against `World::TerrainHeight` per column rather than
against voxels, and that is a deliberate limitation with a reason:
`pondInfo`'s keep-out box is the 768-voxel square at the origin, which is
exactly the residency window the harness runs in, so **no tarn is ever resident
during the gate**. Sweeping one would mean moving the window, which is what
`voxregion` does and why that gate must run last. The column walk is
transitively grounded in real voxels by the `terrain` gate's pass C, which runs
immediately before.

### 5b.4 M2: the drain ledger, the surface shave, and the authority split

**M2 is landed and it is where the feature acquires behaviour.** Four GPU passes
(`assets/shaders/sim_waterbody.wgsl`), one GPU-owned state buffer
(`world.waterBodyState`, 4 KiB), one new binding and one new tuning knob.
`sim.waterBodyMode` is still 0 by default and mode 0 still records no pass at
all, so the pinned world hash is untouched.

**The arithmetic, in one line.** Fullness is eighths, so lowering a body's
surface by one eighth costs exactly `area` eighths, where `area` is its
free-surface cell count. A drain therefore adds to ONE integer, and when that
integer reaches `area` a single flat pass takes one eighth off every surface
cell. Integer against integer; no scaling and no rounding anywhere.

**The four passes, and the order is load-bearing:**

| pass | shape | what it does |
|---|---|---|
| `wbQuiet` | one thread per listed chunk | was this chunk disturbed this tick? |
| `wbLedger` | one thread per body | the whole state machine and all arithmetic |
| `wbReduce` | one workgroup per listed chunk | sums a candidate's voxel eighths |
| `wbShave` | one workgroup per listed chunk | takes eighths off the free surface, and REPORTS what it took |

The ledger runs BEFORE the shave, so it consumes *last* tick's shave report and
publishes *this* tick's instruction — the engine's mark/apply cadence, with the
recorder's generated barriers making the ordering a fact rather than a hope.

#### The authority split, and why the ledger is GPU-owned

This is the decision M2 turns on, and it is forced by plan §3.2's master rule:
**debit what was granted, never what was demanded.** The only honest source for
"what the shave actually removed" is an atomic the shave increments. A CPU-side
ledger would have to read that atomic back — so how far a lake had dropped would
depend on when a fence retired, and that decides a voxel write. That is rule 1
broken through the back door, and no determinism gate that runs twice in one
process with the same fence cadence would catch it.

So authority is SPLIT rather than moved:

* **The CPU decides what is a pure function of (seed, window, tuning)** — basin
  geometry, chunk labelling, straddles, residency, the spill elevation, the
  volume thresholds and their hysteresis. It PROPOSES bodies.
  `WaterBodyState::Proposed` does not mean "governed"; it means "no objection".
* **The GPU decides everything that depends on what the world is DOING.**
  Quiescence is measured from `dirtyIn` and the MPM block map — the hashed
  world's own state — and the Candidate → Measuring → Adopted ladder, the level,
  the area and the ledger all live in `waterBodyState`.

**That closes the M1 hazard** this section used to end with: the quiescence term
read `World::Snap()`. Nothing in `waterbody.cpp` reads a snapshot now, and
nothing in it may start to — the payload it builds gates a voxel write.

#### Three things that are cheap because of how they are shaped

* **The band is two Y values.** The shave only considers cells at `level` and
  `level-1`, so a listed chunk whose Y span misses the band returns after three
  scalar loads. A body's full footprint can be listed once and the dispatch
  still only does work where the water is. One thread owns one (x,z) COLUMN and
  writes at most one cell in it, so the pass is lattice-safe by construction —
  reach 0 for the write, reach 1 for the read directly above, no mark/apply.
* **`area` is MEASURED, not predicted.** The shave counts the surface cells it
  saw and the ledger uses that count next tick; the analytic curve seeds only
  the first one. So component 2's table is genuinely a schedule and not an
  authority, and the GPU needs no copy of it.
* **Idle cost is zero, not small.** No shave fires when nothing drains, and the
  CPU declares the footprint to the page table only when one can
  (`WaterBodyGpu::writesThisTick`). A named lake wakes no chunk and materializes
  no page. This is the wind-primitive wake's lesson repeated: `cpuDirty`'s
  tightening against a lagging snapshot is only sound because settled matter
  writes nothing, and the shave is the second rule in this engine to make
  resting voxels move.

#### The invariant, and the gate

```
voxelEighths(t) + drained(t) - debit(t)  ==  voxelEighths(0)
```

`drained` is what left the body forever; `debit` is what has been taken from the
ledger but is still sitting in the voxels because no shave has removed it yet.
That second term is plan §3.3's legitimate divergence and it is a STORED field,
never implied — a gate that forgot it would report a leak of up to one
eighth-step that does not exist. `--gate waterbody` pass A asserts it as integer
equality and names the body, the term and the delta when it fails.

Release is mass-exact in both directions for the same reason: a released body
keeps shaving, and stops accepting new debit, until the ledger is square.
Dropping the descriptor with a debit outstanding would hand the CA a lake
holding water nobody owns — it would invent water.

#### The measured numbers

`docs/PLAN_water_master.md` §1.2 carries them. The one worth repeating here is
plan §9's ranked-first risk: WP5 measured 169,616 excite candidates over 400
ticks on `worldlake` from a *draining CA* leaving transient gaps under cells,
enough to convert the whole 262,144-particle pool. The shave takes from the TOP
so the mechanism should not be there, but the CA re-levels on the chunks the
shave woke and "should" is not a measurement — so `--gate waterbody` runs a
quiet window and a draining window of equal length and reports both counts.

### 5b.5 What is NOT here

The discharge law and local excite at the hole (M3), the current field and
surface waves (M4), and dug-basin discovery (M5). The drain source at M2 is
`sim.waterBodyTestDrain`, a development tap of a known size in eighths per tick,
0 in every shipped world — it exists so the ledger and the shave could be proved
exact before there was a hole to be exact about.

Two gate passes are absent and deliberately so: **B** (split scheduling) needs
component 10's union-find sweep, and **F** (determinism mid-drain) needs a second
in-process world to compare against. Both would be assertions against zero today.

## 9c. The terrain viewer and the edit layer (added 2026-08-27)

Two things live here: a way to LOOK at generated terrain per voxel, and a way to
CHANGE it that composes with worldgen instead of replacing it.

### 9c.1 Why the column map was not enough

`--heightmap` renders a grid of `World::TerrainColumn` — ground height, slope,
sediment depth, water depth per (x, z) — and the tuner's Worldgen tab drew it.
Its 3D mode extruded that same grid into one heightfield mesh, which is why it
read as a single continuous sheet: it had exactly the information a column field
has. A cave, an overhang, a tree, the floor of a pond and a single voxel are all
things that are *not* functions of (x, z), so none of them could ever appear.

The world's real content is `genCell` in `worldgen.wgsl`, and genCell runs on the
GPU. So the viewer needs actual voxels off the actual device.

### 9c.2 The region server (`src/tools/voxregion.h`)

`--voxserve` boots the engine headless and answers region requests on stdin
until told to quit; `--voxdump` is one request through the same path. A request
is a chunk-aligned box plus a `lod`, and it returns an `SVVX` binary: RLE over
the engine's own 32-bit voxel words.

Three properties matter and each is a deliberate cost:

* **It generates only the chunks the box covers.** `EncodeGenList` already took
  a slot list (the streamer's primitive), so a 64³ region is 64 chunk dispatches
  rather than the window's 32,768. Cost tracks the box, which is rule 2 applied
  to a tool. A 64³ region is ~8 ms once the process is warm.
* **It is a server because boot is ~3 s and a region is ~8 ms.** A viewer that
  streams as the camera moves cannot pay device creation and SPIR-V compilation
  per box. The tuner keeps one process and takes the machine-global run mutex
  around each request rather than for the process lifetime — a persistent holder
  would block every build in every worktree for a whole session.
* **`lod > 1` is a MAJORITY over the block, per chunk.** 16 divides every
  supported lod so a chunk's cells never straddle a sample. Point sampling was
  the obvious alternative and it deletes exactly what you zoom out to see: a
  one-voxel cave roof, a trunk, a shoreline.

Regions are never held at full resolution CPU-side — a lod-16 box spans 512³
voxels (512 MiB of words) and yields 128 KiB of samples, downsampled per chunk
as each is read back.

### 9c.3 Geometry and material are separate (`assets/worldview.js`)

The browser's mesher merges on ONE bit — "is this face exposed" — and carries no
colour at all. Each region uploads its cells as an `R16UI` 3D texture and the
fragment shader reads the cell just inside the face and looks the colour up in a
palette texture.

This is the load-bearing decision. A conventional coloured-quad greedy mesher
merges almost nothing here, because worldgen's palette jitter gives every
adjacent stone cell a different variant: a 64³ region of plain rock becomes ~25k
unmergeable quads. Merging on exposure alone makes a flat plain a handful of
quads whatever it is made of — measured 2.6k quads per region against ~25k — and
the material stays exact per voxel, jitter included. The same texture pays for
per-fragment ambient occlusion (which therefore survives the merging, unlike
baked vertex AO) and for the voxel edge lines that fade in when cells are big
enough on screen to be worth outlining.

Four LOD shells of 64³-sample regions at lod 1/2/4/8; a coarse region is skipped
when it lies entirely inside a finer level's coverage, which is exact because
every extent is a power-of-two multiple of the last. Past the last shell the
column map draws the horizon — the one thing a heightfield is strictly better
at, since it covers kilometres for one fetch.

Region seams are meshed with the six neighbour face slabs when they exist, and a
region is re-meshed when a neighbour arrives. A missing neighbour reads as
solid, so the transient artifact is a hole that fills itself rather than a
z-fighting double face that does not.

### 9c.4 The edit layer (`src/sim/worldedit.h`)

Brush strokes and selection operations write a sparse, chunk-keyed patch of
world cell → voxel word, saved to `assets/worldedits/<name>.svedit` and named by
`worldgen.editLayer`.

It is deliberately none of the three things it could have been:

* not a **ChunkStore save** (`world.svd/`), which is a live world pinned to one
  seed and one history and cannot compose with a worldgen change — and the whole
  premise of the Worldgen tab is that you are still moving the sliders;
* not **FarEdits**, which is derived, disposable cascade state;
* not a **second writer into the voxel buffer**. It emits `CellOp`s and they go
  through the MutationQueue like every other mutation (rule 3), which is what
  keeps it inside the save/replay/network stream for free and is why application
  is deferred by a tick rather than folded into `genChunk`.

Chunks are queued at the point of generation — startup worldgen queues the
window, `Stream::FillSlots` queues each refilled slot — and paid out on the
ordinary per-tick op stream. The streaming re-queue is not optional and fails
the same way the far-field sieve does without `FarEdits`: `genChunk` overwrites
a refilled slot with pristine procgen, so an edit would heal itself the moment
you flew far enough for its chunk to scroll out and back.

A queued chunk that has scrolled out by the time it drains is DROPPED, not
clamped: a cell index is window-relative, so applying one for a non-resident
chunk punches a hole in whatever now owns that slot.

**Any layer moves the world hash**, because it puts voxels in the world. That is
why `worldgen.editLayer` ships empty and no gate sets it.

### 9c.5 Verification

`--selftest --gate voxregion` asserts the data: a lod-1 dump is bit-identical to
a direct `ReadVoxelsSync` of the same voxels (stamp excluded), lod 4 invents no
material the fine box lacks and keeps its fullness, malformed requests are
refused rather than clamped, and a `.svedit` written as bytes and read through
the real loader lands on the cell index it names — then goes through a real tick
and reads back as the material it asked for.

`bash scripts/check_worldview.sh` covers the half that C++ cannot see: real
Chrome, real WebGL2, the real worker, `/api/voxregion` over HTTP, greedy
meshing, LOD shells, a pixel readback, a raycast, an edit and an undo.

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
  - **"Costs nothing" has to include the RECORDING, not just the dispatch.** 54
    indirect dispatches whose args say `(0,1,1)` still cost ~2.25 µs each on the
    CPU, and the settled world was paying 137 µs/tick for provably zero
    invocations until `Cond::CaActive` stopped recording them (§3.4). The
    corollary bit twice: any CPU term that says "not settled" must be EVIDENCE,
    never a timer — a 400-tick post-explosion timer disabled that skip for 13.3
    seconds after every blast (ROADMAP_scale.md §3.2d).
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

**Update (2026-08-22): the browser requirement is dropped and a native Vulkan
port is adopted.** The plan of record — measurements, phases, and the
green-gate checkpoints — is `docs/PLAN_vulkan_port.md`; the measured pass/
dependency map that seeds the hand-written barrier graph is
`docs/vulkan_pass_map.md`. What the paragraph below called the "escape hatch"
is now the road, taken for the capabilities parity cannot reach: sparse
residency for the voxel buffer (measured 83% of pages empty on the default
seed), explicit memory and synchronization control, and async compute for
derived render-only passes. Three things survive the port unchanged: **WGSL
stays the authoring language** (compiled to SPIR-V via Tint at load, so the
generated-prelude machinery, F5 hot-reload and `check_shaders.sh` are
untouched); **the determinism rules survive verbatim** (integer sim kernels,
no subgroup ops in sim state — Vulkan makes those *available*, not
*permitted*); and **Dawn was retained during the port** as the reference
backend and cross-backend hash oracle (same seed + same tick ⇒ same world
hash) until Vulkan was validated — it was then removed, per the update two
paragraphs below. The rationale below is kept as the record of why WebGPU was
the right call while web was a requirement.

**Update (2026-08-22, phases 4–6): the port LANDED, and Vulkan is now the
DEFAULT backend.** It runs headless and windowed, all 23 selftest gates, the
`--shot` harnesses and `--measure`; barriers are *generated* from
`src/sim/pass_table.def` by a last-access tracker (`gpu/vk_record.cpp`) rather
than hand-placed, exactly as `docs/vulkan_barrier_graph.md` specifies. Parity
is measured, not assumed: both backends report the same pinned 200-tick world
hash (`7cfa2420`), the same pass/fail set, and character-identical per-gate
detail strings — the only difference across a gate-by-gate `--json` diff is
`perf`, which reports wall clock. Vulkan is also cheaper on the sim, by
`--measure`'s GPU timestamps: a settled tick is 229–236 µs vs Dawn's 306,
almost entirely in per-dispatch driver overhead on the 54 empty CA dispatches
(the §11 idle-cost debt phase 0 flagged), while the genuinely GPU-bound
full-world occupancy scan is unchanged at ~98 µs. **Render cost is a wash and
should not be quoted from a single run** — the selftest's 1080p sweep varies
8–19 ms/frame across runs depending on machine load and the world state
earlier gates leave behind, and the two backends trade places inside that
spread.

**Update (2026-08-22, user decision): DAWN IS REMOVED. The engine is
Vulkan-only, and the determinism guarantee is now scoped to the Vulkan
backend.** The previous paragraph planned to keep Dawn until the phase-7 page
table was validated, because its auto-generated barriers were the reference
implementation of the barrier graph. The user relaxed that requirement
explicitly: **cross-backend bit-equality is no longer a goal**, and what
matters is that rule 1 holds on Vulkan going forward, anchored by the pinned
golden hash `7cfa2420` in `tests/baseline.json`. Dawn was retired having
already done its job — it agreed with Vulkan on all 23 gates, character for
character, for a full phase.

What this costs and what replaces it, stated plainly because rule 1 is
involved:

- **Lost:** the ability to localise a generated-barrier mistake by
  disagreement. `--vk-smoke`/`--vk-smoke-loud` no longer diff two backends;
  they check the world hash at 5 and 19 probes against sequences **pinned as
  constants** (the values the cross-backend diff agreed on and every phase
  since has reproduced byte-for-byte). That is *more* coverage in one
  direction — it catches a change that would have moved both backends
  identically, which the diff was blind to — and less in the other.
- **Kept, and it was always the stronger detector:** `VK_LAYER_KHRONOS_-
  validation` with **synchronization validation**, which reports a hazard from
  the recorded commands *without needing a divergence to occur*. Every
  headless path fails the run on a single message. Two real barrier bugs were
  found this way during the port, neither by hash divergence.
- **§14 risk #3 stays open and its scope narrows honestly:** it was already
  "one vendor, one driver, one shader compiler". It is now also one host
  layer. Closing it still needs a second vendor's hash sequence (plan phase 5
  option (b)); a CPU Vulkan ICD (lavapipe/SwiftShader) remains the cheap
  partial step and is *unaffected* by this removal, since it plugs in below
  our host layer.

**The `rhi::` seam stays, including its polymorphic impl layer.** Confining the
GPU API behind ~10 concepts is what made the port testable one phase at a time;
an abstract impl with a single subclass costs one virtual hop on ~60 dispatches
a tick, and it is the slot phase 7's paged-residency buffer plugs into.
**Tint stays too, and with it the Dawn checkout** — WGSL→SPIR-V at load and at
every F5 reload is what keeps WGSL the single shader source of truth. What was
removed is the Dawn *engine* (dawn_native, webgpu_dawn, webgpu_cpp,
webgpu_glfw), which is excluded by turning off every `DAWN_ENABLE_<backend>`.

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

## 12b. Audio (added 2026-08-20)

**Spatialized, occluded, data-driven sound. Presentation only — the audio layer
may never feed anything back into the sim (rule 1), and it is the first real
concurrency in the codebase (see the threading contract below).**

### The spatializer is vendored, not written

`src/audio/xyzpan/` is a verbatim copy of the binaural engine from the
`audio_webgame` project (itself a snapshot of the `xyzpan` VST). It is pure
C++20 with **zero third-party includes**, which is the property that made it
droppable in. Each `XYZPanEngine` instance spatializes exactly ONE mono source
through: doppler delay → comb bank → pinna/ear-canal EQ → ITD/ILD split → chest
and floor bounce → distance gain + air absorption → early reflections → FDN
reverb. Full provenance, local modifications, and the two traps below are
recorded in `src/audio/xyzpan/VENDORED_FROM.md` — read it before touching
`MakeParams`.

Two conversions happen in exactly one function (`AudioWorld::MakeParams`):

- **Axes.** The engine is Z-up / Y-forward; sandvox is Y-up. `(x, y, z)` →
  `(x, z, y)`.
- **Units.** The engine needs METERS, not voxels — its binaural cues use virtual
  ears offset by 0.087 *units*, which is a head radius only if a unit is a
  meter. Feeding voxels would put the listener's ears 87 cm apart.

### Why every asset on disk is mono

The spatializer synthesizes the stereo image from the emitter's position. A
stereo asset arrives with its own baked image, which fights the panner and
smears the direction — so `Library` decodes everything to 1 channel at the
device's negotiated rate, and `scripts/split_footsteps.py` writes mono files.

### Occlusion is a voxel ray, not diffraction

`audio_webgame` solves Maekawa knife-edge diffraction analytically against a
list of cylinders and boxes. A voxel field has no such list, so the model here
is different in kind: trace the straight listener→source line, accumulate the
material crossed, and convert it into (broadband gain, low-pass cutoff) using
per-material acoustic properties derived from class/hardness/tags.

The **low-pass is the load-bearing half**: transmission loss rises with
frequency (mass law), so material between you and a source removes highs far
faster than lows — a wall makes a sound *dull*, not merely quiet. Getting that
tilt right matters more for readability than the absolute level does.

Cost obeys rule 2: one ray per *audible* voice per frame, capped in length and
step count, over the same chunk cache the avatar's foot probe uses — no GPU
work, no readback, no new synchronization. What this deliberately does NOT model
(diffraction imaging, portals, geometry-specific reflections) is listed at the
bottom of `src/audio/occlusion.h` so nobody assumes it does.

### Footsteps come from the gait, not from a distance accumulator

The avatar's gait state machine already has an exact touchdown moment
(`f.planted = f.swingTo`). Footsteps fire there, which means a step is heard on
the frame the art shows the foot land, at whatever cadence the gait chose, and a
leg lost to dismemberment stops producing steps for free because it never swings
again. The ground probe that picks the foot's target already reads the
supporting voxel's material and used to discard it; it now returns it.

Events are QUEUED (`PlayerAvatar::Footfall`) rather than fired directly, because
`PreTick` runs inside the fixed-tick loop up to 4× per frame — firing inline
would stack several steps on one instant. The consumer drains them once per
frame.

Material → sound is DATA: `materials.json` carries a `"footstep"` key naming a
folder under `assets/sounds/footsteps/`, resolved once at load into a flat table
indexed by material id. Materials with no key fall back **by tag** (foliage →
leaf, organic → branch, soil/mineral → path), so a new material is audible the
day it is added — the same guard against the N×M explosion that reactions use.

### Sound slots: one authoring surface for every noise a thing makes

A **slot** is one authored binding — an owner (a material, a mob) naming a sound
set for one event. Materials carry theirs in a `"sounds"` object
(`{"footstep": "leaf", "impact": "stone"}`); mobs carry theirs in their `.json`
sidecar, so a creature stays one `.vox` plus one `.json` with its voice
included. The older flat `"footstep": "leaf"` is still read and still written
back for materials that already use it — opening the tuner never silently
rewrites a file into a new shape.

The slot list itself lives in **`assets/sound_schema.js`**, and it is the only
place that knows a slot exists. Each entry names the namespace it binds into
(`footstep` → `footsteps/`, `hurt` → `mobs/`), which is what lets the tuner
offer a correct set list per slot instead of every set in the project. The
engine does the same concatenation through `Cues::kSlotPrefix`; the two tables
are mirrored and adding a slot means a row in each plus a call site that fires
it. Material slots fall back to the footstep set (a body hitting stone and a
boot hitting stone are the same surface, separated by pitch and gain);
`break` and every mob slot are **silent** when unbound, because a shatter is not
a step and one creature borrowing another's voice is always wrong.

Creature voices are rate-limited per source (`kMobVoiceMinGap`): a body taking
a burst of per-tick laser damage speaks once, rather than firing a machine-gun
of overlapping copies of one sample. Death and sever bypass the limiter — they
happen once and are the events the player most needs to hear.

### Authoring is drag-and-drop (the tuner's Audio tab)

Because a set is a folder and nothing else, importing a sound is a file copy —
so the tuner does it. Dropping a `.wav` onto a slot creates the set folder,
renames the file into that set's numbering (variant order is a filename sort,
and `take 3 FINAL.wav` landing mid-list would silently renumber every variant),
writes the binding into the owner's JSON, and rescans. The same set editor —
waveform audition, rename, move, delete — is embedded in the wiki page for
every material and every sound set, so it is the same edit wherever you make
it. Deletes move to `assets/sounds/.trash/` (skipped by the loader, like
`raw/`): a recorded take is not reproducible, so the destructive path is not
trusted to a click. All of it needs the tuner server or app; a `file://` page
cannot reach the folder.

### Threading contract (the first real concurrency here)

    GAME THREAD owns  World, Player, PlayerAvatar, Tuning, Library.
    AUDIO THREAD owns playback position, filter state, engine smoothers.
    Shared: ONLY lock-free atomics + sample buffers the library keeps immortal.

The audio thread must never touch a game object, and in particular must never
call `CurrentTuning()`: F5 replaces that global wholesale. Tuning is read on the
game thread and copied down. `Library` is append-only with buffers behind
`unique_ptr`, so a `const std::vector<float>*` handed to a playing voice stays
valid forever — do not add a remove/replace op without solving reclamation.

### Headless is silent

`--selftest`, `--shot` and `--shot-mob` return before audio init, so they open
no device (there is no sound hardware in CI, and a gate that needs one is not a
gate). `--noaudio` forces the same. A failed device init is never an error —
the game runs silent. The selftest still asserts the *events* (`avatar
footfalls`), which is the half that can break silently.

### Impacts come from a Jolt contact listener (2026-08-24)

`Cues::Impact()` used to be the standing example of a cue nothing fired. It is
now driven by a `JPH::ContactListener` on the physics system, and the shape of
that wiring is the interesting part, because a contact listener is the easiest
place in the engine to build a machine gun.

**`OnContactAdded`, never `OnContactPersisted`.** Jolt reports a manifold once
when it first appears and then again every step while it lasts. The first is a
LANDING; the second is a body resting. Listening only to the former is what
makes a settled pile of debris cost literally zero — there is no "is it asleep"
check anywhere, because a sleeping body generates no new manifolds.

**Three bounds, and each one is load-bearing** (rule 2):

| Bound | Where | What it stops |
|---|---|---|
| speed gate (`audio.impactMinSpeed`) | inside the listener, before the buffer | a rock rolling to rest touches down at cm/s; only a rock that FELL is a sound |
| per-body gap (`audio.impactMinGap`) | `DebrisSystem::CollectImpacts` | one bounce is one thud, not one per contact face |
| per-step cap, loudest kept | same | a wall blasted into thirty pieces lands them together; thirty simultaneous rock impacts is not a sound design |

**The listener runs on Jolt's job threads**, which makes it the second piece of
real concurrency here after the audio thread, and it inherits the same rule:
it must never call `CurrentTuning()`, because F5 replaces that global
wholesale. `Physics::Step` latches the gate into the listener from the game
thread before handing control to Jolt. The buffer is mutex-guarded, which is
cheap only because the speed gate runs *before* the lock.

**The material is the surface STRUCK, not the striker** — a log landing on
stone sounds like stone. `CollectImpacts` probes the voxel grid on both sides
of the contact plane through the same chunk cache the body burn uses, and falls
back to the other body's dominant material (cached per body, refreshed by
`RecountBurn`) for a body-vs-body hit.

**Your own body cannot fire one.** `Layers::AVATAR` and `Layers::PLAYER` are
rejected by name in the listener, and a LIVE mob limb is filtered for free by
ownership: limb bodies belong to `MobSystem`, so the handle never resolves in
`bodies_`. A SEVERED limb has been `AdoptBody`'d by then and does start
thudding, which is right.

Reported, never voiced, here: `DebrisSystem::ImpactEvents()` mirrors
`BreakEvents()` exactly, and `main.cpp` turns them into cues — so the physics
layer still knows nothing about audio, and the headless path drains the queue
without an audio device existing.

### Creature voices: hurt and death (2026-08-24)

`MobSystem::VoiceEvents()` is the same reporting shape as `SeverEvents()`, for
the same reasons (the def index rides on the event because a killing blow
despawns the mob before the frame drains it). `Hurt` is raised by
`MobSystem::Damage` — the laser and melee path — and by `CarveLimb` on its
SURVIVING return, which is what makes an explosion that only wounds a creature
audible. A blow that severs deliberately says nothing there: `Sever()` already
reports, and the sever cue falls back to the hurt set, so voicing both would
double one blow. `Death` is raised in `Die()`, the single choke point every
kill funnels through, positioned on the root limb's live transform rather than
`mob.origin` (the spawn corner, which is the trap `Sever()` already documents).

Hurt is de-duplicated per mob per drain window at the EVENT layer, on top of
the audio layer's per-source `kMobVoiceMinGap`. The two are not the same guard:
the wall-clock limiter cannot stop the queue itself from growing, and it is
bypassed entirely on a machine with audio off. Bounding the event is the game
layer's job; choosing not to play it is the audio layer's.

### The ambience bed drives itself off the CPU mirror (2026-08-24)

A material carrying an `"ambience"` slot (`water`, `lava`) gets a positioned
loop automatically. `Cues::ProbeAmbience` subsamples the 3×3×3 voxel mirror on
a stride-4 lattice — 1728 reads, at most twice a second, over memory that is
already resident — and `UpdateAmbience` keeps ONE loop on the strongest result.

- **Position is the CENTROID of the sampled cells**, not the nearest cell and
  not the listener. This is the whole design decision: an emitter parked on the
  player pans to nothing, while a centroid makes a shoreline swing left as you
  walk along it. Eased, so the emitter drifts instead of jumping each scan.
- **Gain is the sampled cell COUNT.** A puddle is under the floor and silent; a
  pond that fills the mirror is at full gain. Also eased.
- **Radius is one authored number** (`audio.ambienceRadius`): a bed is a bed,
  and a body of water has no authored size.

**Idle cost is a single bool.** If no material in the project binds an ambience
set, `anyAmbience_` is false and nothing is scanned, ever. That check, not the
scan's cheapness, is the rule-2 property.

**Exactly one bed plays at a time**, and that is a deliberate ceiling rather
than a limitation to fix later. Two lakes on opposite sides of the player is a
CLUSTERING question — "is that one body of water or two" — and answering it
would be a system, not a hook.

The probe is split out of the voice so the selftest can assert it with no audio
device (`--gate audio-ambience` builds a `Cues` and never calls `Init`).

### Still not triggered: `idle`, `alert`, `attack`

These three mob slots have **no AI event to hang off, and none can be invented
from the audio side.** `MobSystem::DecideIntent` has no awareness of the player
at all: its only sensor is a terrain probe (`GroundSense`), there is no target,
no state enum, and no previous-state field to difference — the sole discrete
transition in the whole behaviour layer is "just bumped a wall". And mobs never
attack: the one `PlayClip("attack")` in the engine is a FLINCH on being hit.

So `alert` needs the AI seam §"Mob steering: intent vs actuation" describes,
`attack` needs a mob attack action to exist, and `idle` needs a per-mob timer
and a notion of "unaware". Each says so in its own `fires:` field in
`assets/sound_schema.js`, and the tuner shows it on the slot — so binding a
sound to something nothing triggers tells you so at authoring time instead of
leaving you wondering why it is silent.

Each cue wired above landed with a gate (`src/test/selftest_audio.cpp`:
`audio-impact`, `audio-mob-voice`, `audio-ambience`), asserting the EVENT and
its idle counterpart — that a settled pile reports nothing, that a corpse says
nothing more, that a dry world finds no water. Headless is silent, so the event
layer is the only thing there is to test, and it is the half that breaks
quietly.

## 13. Roadmap

Each milestone is playable/demoable. Don't start a milestone's "later" items early.

> **Combat test arena (2026-08-21).** An authored walled POI near spawn
> (centre 180,110; 64 voxels square, 24-voxel wall, four 2 m doorways, ramp up
> the -z side), for trying melee, spells and mob fights on ground that is not a
> noisy hillside — on natural terrain a miss is ambiguous between bad reach and
> a foot half a voxel up a slope. Same shape as the wood-platform set piece:
> absolute coords in `genCell`, inert stone, so a settled world still reports 0
> active chunks and the determinism/streaming hashes are unaffected.
>
> Two things it shook out. (0) The deck was first levelled to `baseHeight` at
> its CENTRE, which buries the uphill half and digs a pit: terrain spans 20
> voxels across the 64-voxel footprint (51..71, centre 60 at the default seed),
> so the deck now sits at centre+16 — above the +11 worst case — and the arena
> is a low plinth that fills down to the ground rather than an excavation. The
> span was measured before choosing the number, not guessed. (1) A plinth that
> stands 1 m proud is far over the step-up reach, so it needed the approach ramp
> or the only way in was to jump the wall. Placed OFF the x==z diagonal
> deliberately: every selftest fixture column sits on it (60/80/90/100/108/120/
> 140/150) and each assumes `TerrainHeight()` is the top of the world there.
> `surfHeightAt` mirrors the flattening, as it must, or the far field keeps
> painting the original hillside and the deck pops on approach.
>
> **v0.5.6 (2026-08-20)** — rigidbody feel pass: mass-relative shoves + true
> spheres. The player proxy is now a DYNAMIC capsule (rotation-locked, zero
> gravity, tuned `playerMassKg`) teleported to the authoritative position each
> tick instead of a kinematic one: kinematic = infinite mass to the solver, so
> a strolling player launched a two-ton block exactly like a bucket. With real
> mass the solver splits contact impulses by mass ratio, and since body mass
> comes from per-voxel material density, shove strength falls out of the
> material data (selftest-gated: light sphere must move >3x a heavy one under
> the same walk). Teleport-implied velocity is capped at 30 m/s so a world
> load can't hand a resting body a huge impulse. `PlayerPushOut` and the
> player's own terrain sweeps are unchanged — the proxy's solver displacement
> is discarded every tick. New `Physics::CreateSphereBody`: an analytic Jolt
> sphere (greedy-boxed voxel balls can't roll), rendered as a center-origin
> voxel ball via `AdoptBody`; K spawns one at the crosshair, half the player's
> height in diameter, made of the current brush material. Rolling smoothness:
> `mEnhancedInternalEdgeRemoval` on all dynamic bodies + terrain-mesh active
> edge threshold 5°→25°, killing the ghost contacts with internal
> marching-cubes edges that made debris snag and hop on flat ground. CPU-float
> Jolt only — world hashes unaffected.
>
> **v0.5.5 (2026-08-19)** — generative branching birch. Every species was a
> solid crown volume centred over a straight bole: at distance that silhouette
> reads as a lollipop, and it was the birch — the slender species that most
> needs structure — that showed it worst. Birch is now an IMPLICIT BRANCH
> SKELETON rather than a crown. `treeCell` re-derives a fixed skeleton from the
> tree's hash for every cell it evaluates and tests point-to-segment distance:
> a leaning 3-part bole, `BIRCH_LIMBS` two-segment limbs (straight out, then
> bent over at an elbow — a single straight segment reads as scaffolding),
> `BIRCH_SUBS` twigs off BOTH the elbow and the tip, and leaves ONLY as small
> hash-eroded tufts at the twig tips. No leaf ball anywhere.
>
> This shape had to be built implicitly because worldgen is a pure per-cell
> function — there is no place to grow a tree with a turtle and write voxels as
> it walks, since a chunk may be generated in isolation and must agree with its
> neighbours. Everything derives from `t.rnd`, so the tree is identical from
> whichever chunk asks. New integer helpers: `segDist2` (point-segment, scaled
> so products stay in i32) and `isin` (256-step integer sine, Bhaskara-style).
> Both are integer-only — this feeds voxel state, so rule 1 forbids `f32` here
> even though it is "just" worldgen.
>
> Three gates a branching species needs that a crown species does not, each of
> which cost a wrong render before it was found: (a) the limb direction vector
> must be NORMALIZED, since callers scale it by `len/256` — the first cut's
> un-normalized `(cos*horiz, rise, sin*horiz)` gave ~11 voxels of horizontal
> reach and rendered as a bare pole with a fork; (b) `treeAt`'s horizontal
> reject is `radius + 2` for crowns but must be `radius*5/2 + 4` for birch,
> whose limbs reach well past the nominal radius; (c) its vertical reject needs
> `+ radius` of headroom for the leaf cluster carried on top of the highest
> twig tip, or every birch is sheared flat. `treeCanopyAt` (far-field XZ
> footprint) also special-cases birch: a wider disc with a hash punch-out, so
> distant stands stay airy instead of flattening into solid canopy.
>
> Cheaper than what it replaced — sparse tufts give shadow rays far less
> foliage to march (selftest render 1080p shadows-on 21.9 ms -> 11.4 ms).
> Selftest PASS end to end, including determinism and the far-downsample
> invariant. `--shot` gained a `screenshot_birch.bmp` camera: this species'
> silhouette can only be judged on a single specimen against the sky.
>
> **v0.5.4 (2026-08-19)** — forest overworld. The world was one desert; it is
> now forest-dominant, driven by a low-frequency biome field (forest / meadow /
> pine slopes, with the old sand desert kept as a rare ~12% destination biome
> and snow still overriding above height 80). Surface caps as a one-voxel grass
> skin directly over stone — deliberately NOT over a dirt layer, because `dirt`
> is a powder and a loose shell under a solid skin avalanches out from under the
> grass on every slope, so the whole surface creeps and never sleeps. Ponds fill
> noise basins to a local water table, kept independent of the cave bands (a
> cave breaching a pond drains it forever, the same failure the authored rims
> already avoid). Trees: 5 procedural species (oak, great oak, pine, birch,
> bush) placed one-per-16² XZ tile by tile hash and sampled from the 5×5 tile
> neighborhood so canopies overhang tile borders — pure `genCell()`, so a tree
> straddling a chunk boundary generates identically from either side and the
> far-field cascades get it for free. (Birch was rebuilt as a branching
> skeleton in v0.5.5; the other four remain crown volumes.)
>
> Six new INERT materials (grass, leaves, pine_needles, autumn_leaves,
> birch_wood, petal). Inert is the whole point: they carry organic/flammable
> tags so fire, acid, mites and the laser treat them like the reactive garden,
> but no reaction uses them as a growing `self`. Worldgen paints foliage by the
> million, and a generated forest of `stem` would keep every chunk in the world
> awake forever (rule 2). Relatedly, worldgen no longer scatters reactive
> `seed` at all — even at 1/4000 the stalks outgrew the oaks, and being the only
> moving thing in frame they were what the world looked like. The garden is
> unchanged and one brush stroke away; it just isn't the default overworld.
> Settled world now measures **0 active chunks** (was 4).
>
> **World scale (the second pass).** The first cut sized everything in bare
> voxel counts, which silently assumed a voxel was about a metre. It isn't:
> `kVoxelMeters` is 6.25 cm, so there are 16 voxels to the metre and the player
> capsule is 27 voxels tall. Every feature was therefore a tabletop model of
> itself — 4 m hills, a 2 m "lake", 0.9 m ruins with a mouse-hole door, and
> "oaks" whose 10-voxel trunks stood 60 cm high. Fixed by making scale explicit:
> a single `HSCALE` (currently 2) multiplies every horizontal noise cell — hills,
> biome field, lake basins, cave masks, flora clumps, the ruin tile — and tree
> and building dimensions are written in metres (`VOX_PER_M`) or tenths of a
> metre, never as raw voxel counts. Trees are now 5.5-12 m with trunk radius
> proportional to height and tapering; ruins are 7 m square with a 2 m doorway;
> the spawn platform is a 5 m deck on posts.
>
> Vertical is deliberately NOT on `HSCALE`. The residency window is 256 voxels
> = 16 m tall and does not stream in Y, so height is a hard budget: hills got
> ~2.5x (band y32..y140) and no more, because a 6 m tree crown on the highest
> ridge has to still fit under y256. The old bare `80` treeline became
> `TREELINE` sized to the new band. `World::TerrainHeight` mirrors the new
> `baseHeight` exactly, `HSCALE` included — they must stay bit-identical.
>
> Three things this shook out. (0) The mob walk test bounded vertical drift at
> 6 voxels; over a 90-voxel height range a mob walking 20 voxels honestly
> descends ~7, and it failed at exactly -6.0. The bound exists to catch "fell
> through the world", not honest downhill walking — raised to 16. Measured
> first: mean |dh/dx| only went 0.29 -> 0.34 and max step stayed 2, so the
> terrain is not meaningfully steeper, just taller. (1) The selftest's
> debris/burn/shatter fixtures
> drop bodies on fixed columns whose margins were tuned against loose sand, and
> a single solid grass tuft one voxel up is enough to rest a burning body higher
> and change which of its voxels the fire reaches — so those pads keep the
> original bare sand cap (`onFixturePad`), and a wider spawn clearing keeps
> ponds and trunks off the fixture sites. (2) `DebrisSystem::Reset()` did not
> reset `nextSerial_`, and body serials seed the burn RNG — so a body's fire
> outcome depended on how many bodies happened to exist earlier in the process.
> That silently coupled unrelated scenarios: a worldgen tweak that changed how
> many islands an earlier test produced re-rolled a later fire. Reset now clears
> it, making a burn a function of its scenario rather than of history.
>
> **Scale, third pass + burning forests (2026-08-19).** The HSCALE=2 world
> overshot: the verdict in play was "trees are right, everything around them
> is too big". So the world halved around the trees: `HSCALE` back to 1, hill
> wavelength AND amplitude halved (band y32..y86 — same slopes, half the
> size), `TREELINE` 116→72, authored pools 68/32/24-voxel radii (depths kept —
> a halved lake would be too shallow to swim), ruins 3.5 m huts (the 2 m
> doorway deliberately NOT halved — it clears the 1.7 m player), spawn deck
> footprint halved at unchanged 3 m height. Tree dimensions and the 384-voxel
> biome cell are pinned: biome regions must stay many 9 m tree-tiles wide or
> the field reads as per-tree noise. `World::TerrainHeight` mirrors as always.
>
> Ponds were not retuned but REDESIGNED, because the retune exposed a latent
> leak: contour-fill ponds (basin-noise mask filled to a noise water table)
> spill wherever the mask edge crosses ground below the local table — the
> sleep gate caught one pouring downhill forever (82 chunks awake after 600
> settle ticks; a CPU-mirror scan counted 175 spill edges around that one
> pond, and the committed tune had only passed by luck of where its mask
> edges fell). Ponds are now DISCS placed one-per-224-voxel-tile by tile hash,
> exactly like trees: water level = 2 below the lowest of 24 integer-circle
> terrain samples on the pond's own rim, bowl carved into the terrain beneath.
> Contained by construction — the shore is above the water everywhere. Keep-out
> discs cover the spawn clearing, the streaming-test ball column and the
> authored pools; `surfHeightAt` mirrors the carve for the far-field skin.
>
> Burning a tree used to rain "charcoal dust" through the canopy: foliage had
> NO combustion rules (only the trunk's `wood` burned), so the torched trunk
> vanished, the support scan found the crown, and the island detector crumbled
> the procedurally-dithered rim — thousands of sub-8-voxel leaf "islands" —
> into `ash`. Two-sided fix: the foliage set (leaves/pine_needles/
> autumn_leaves/grass/petal) now flashes to `fire` and is consumed (birch_wood
> smolders to `ember` like wood), and sub-8 floaters of `tag:foliage`
> materials vanish to air instead of crumbling (`DebrisSystem` rubble
> handoff) — leaf crumbs shed by ANY support scan near a tree, not just fire.
> Foliage combustion is still rule-2 safe: fire is consumptive, nothing grows.

> **v0.5.3 (2026-08-19)** — far-field cascades: view distance from ~1 window
> radius to ~64 window radii (§9, docs/PLAN_far_field_cascades.md phase 1).
> Six nested render-only 256³ LOD volumes (1 material byte/cell, 2^k-voxel
> cells, ~96 MB) fill from `genCell()` sampled at stride on the GPU
> (worldgen.wgsl `far`), recenter with the player (`sim/farfield`, plane
> refills ≤4096 level-chunks/tick through the tick submit), and extend the
> raymarch past the window exit with the same occupancy-skipped DDA per level.
> Fog density became a RenderParams uniform pinned to the outermost level.
> Sim untouched: cascades are derived data — not read by any sim kernel, not
> hashed, regenerated from seed (edits beyond the window invisible until
> phase 2's dirty-driven downsample). Selftest PASS end to end; render
> 1080p shadows-on 9.4 ms with the far march + fully filled cascades.
>
> **v0.5.2 (2026-08-19)** — fire pass: burning rigidbodies + fire look.
> Detached islands froze mid-flame forever (bodies are outside the CA);
> `DebrisSystem::BurnBodies` now runs the reaction table over body payloads —
> embers advance to ash, emit real grid fire (fill-air-only cell ops), grid
> fire ignites cold bodies via the chunk cache, burned voxels leave the body
> (batched collider rebuilds). Removals that disconnect a body shatter it:
> big fragments become bodies, small clumps and sub-8 remainders re-enter the
> world as ballistic particles with the body's point velocity, via a new
> CPU→GPU particle spawn stream (`spawnOps` + `sim_particle.wgsl spawn`).
> New selftest gates: `body burn`, `body shatter`.
> Rendering: the media march accumulates per-CELL optical depth + tau-weighted
> tint (fire→smoke paths shade each stretch with its own material; the
> saturation early-out is now exact instead of first-material-approximated),
> plus a separate emissive channel — per-voxel phase flicker, dimmed by the
> media in front of it, driving a temperature ramp (deep-orange wisps →
> white-hot plume cores). Ember voxels on debris bodies flicker like their
> grid counterparts. World hashes unaffected by the render work; burn ops ride
> the MutationQueue like settle-back (selftest PASS end to end).
>
> **v0.5.1 (2026-08-19)** — fire-scene perf pass. Root cause of the burn-time
> FPS collapse was the renderer, not the CA: gas plumes defeated chunk-level
> empty-space skipping (occupancy counted any non-air voxel) and media rays had
> no absorption early-out, so primary AND shadow rays walked entire smoke
> volumes voxel-by-voxel. Fixes: occupancy word now packs (rayBlockers << 16) |
> nonAir — shadow rays skip on the blocker count (writers: sim_occupancy,
> worldgen, stream FillSlots; CPU readers mask low 16) — and trace() stops once
> accumulated optical depth passes MEDIA_TAU_MAX (~exp(-6) transmittance),
> writing depth at the stop point so raster geometry can't draw through opaque
> smoke. Frame-loop honesty: FPS overlay now reports frames/wall-clock over a
> 0.5 s window + worst-frame ms (the old EMA of 1/dt read 100+ when GPU-bound
> at <10), and the fixed-tick accumulator clamps its backlog at 4 ticks so a
> slow stretch no longer leaves the loop in 4-ticks/frame catch-up forever.
> Render-only + counters: world hashes unaffected (selftest PASS).
>
> **v0.5 (2026-08-19)** — engine-debt pass: the three consciously deferred
> v0.4 simplifications paid down. All selftest-gated (new gates: player-body
> overlap push, region-store spill roundtrip).
> **Player↔body collision:** kinematic Jolt capsule proxy in a PLAYER layer
> (collides with MOVING only; terrain stays the AABB controller's) driven by
> MoveKinematic each tick, plus a narrow-phase depenetration query applied
> through the normal voxel sweeps — debris can't pass through the player,
> gets shoved by them, and can be stood on (upward push = ground support).
> **Async eviction:** shifts no longer block on the leaving-plane readback —
> pooled staging + mapAsync completes ticks later; a pending-eviction set
> force-completes chunks that stream back in or get saved (see §3). Filter
> semantics (and the accepted trailing-plane race) unchanged.
> **Region-file ChunkStore:** 16³-chunk regions, lazy disk load, LRU spill at
> 64 RAM regions, saves are now a `world.svd/` directory of region files +
> meta — RAM stays bounded on long journeys and saves stop being monolithic
> (see §3; SVX2 retired).
>
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
> pool forever and the world never sleeps). Surface biomes, ponds and the
> procedural forest layered on top of this in v0.5.4.
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
> Deferred consciously: destructible bodies (re-split on damage), body
> re-fusion into the grid, particle↔media interactions. Player↔body collision
> landed post-v0.4: a kinematic Jolt capsule proxy (PLAYER layer, collides
> with MOVING only — terrain stays the AABB controller's job) follows the
> player via MoveKinematic so debris can't pass through and gets shoved; a
> narrow-phase depenetration query (`Physics::PlayerPushOut`) pushes the
> player out of overlapping bodies through the normal voxel sweeps, and an
> upward push counts as ground support (standing/jumping on debris works).
> Selftest-gated (overlap ⇒ push, clear ⇒ none).
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

  > **The gap, named precisely (2026-08-24).** M8's parts all exist — mobs with
  > a steering layer (§"Mob steering"), avatar health + respawn, the spell VM,
  > melee, items — and yet there is no game, because **`MobSystem` has no player
  > awareness at all.** `DecideIntent`'s only sensor is a terrain probe: there
  > is no target, no aggro state enum, and no previous-state field to difference
  > against. **Mobs never attack.** The single `PlayClip("attack")` in the tree
  > is a flinch played on *being hit*, not an attack the mob decides to make.
  >
  > This was found from an unexpected direction: the audio pass tried to wire
  > the `idle` / `alert` / `attack` sound slots and discovered there is no event
  > to hang them on (§12b). Those slots' `fires:` fields now record exactly what
  > is missing, so the gap is visible at authoring time in the tuner instead of
  > reading as a silent bug.
  >
  > So M8's first work package is a **sensing + target layer** on the intent
  > side of the sense/intent/steer/drive split — not more content. `DecideIntent`
  > is the seam and it is already the only place that decides; adding a target
  > and a state enum there is where the `alert`/`attack` events, and the fight,
  > both come from.
- **M9 — It's online (prototype)**: choose lockstep vs. server-authoritative (§10)
  based on measured determinism (per-tick hashes across two GPU vendors) and
  bandwidth data; headless build, 2–4 player LAN test. *Everything before this was
  built against the MutationQueue with deterministic kernels, so this milestone is
  plumbing, not surgery.*
- **Beyond**: GI/lighting, wand/spell crafting depth, persistent meta-progression,
  Steam networking, temperature layer, structural stress.

## 14. Risks (ranked)

0. **Page-pool exhaustion is a FATAL ERROR, not a caveat on rule 1.** Under
   paged residency (§3) the CPU materializes every page a kernel might write
   before the command buffer is submitted; if the pool cannot satisfy that set,
   the engine aborts with a clear `page pool exhausted` message, in every mode.
   The reasoning: if the pool can exhaust in normal play then the pool is
   MIS-SIZED, and the right response to a bug is to fail loudly at the moment
   of detection rather than to invent a graceful behaviour that hides it and
   mutates the world while doing so. This is a condition the engine detects and
   refuses to continue past — the same register as an out-of-memory allocation
   failure — and it does NOT qualify the determinism guarantee: an aborted
   process produces no hash to diverge. Pool sizing (`kPoolPages`, world.h) is
   therefore load-bearing rather than advisory, and `--measure` reports the
   high-water mark so the margin stays a tracked number.

1. **Island detection** — flagged by the one team that's done it as "hardest, not
   completely solved." Mitigation: bounded fills, chunk-face metadata, accept
   imperfection (M6, not M1).
2. **GPU↔CPU readback latency** for player/physics collision — the architecture
   stands or falls on the async mirror pattern. Prototype it in M0/M1, not M4.
3. **Determinism erosion** — one scheduling-dependent op, float sneaking into sim
   state, or a driver-divergent intrinsic silently breaks cross-GPU reproducibility.
   Mitigation: per-tick world hash asserted in CI-style test runs from M1; validate
   on two GPU vendors early, not at M9.
   *Status 2026-08-22 (Vulkan port phase 5): still OPEN, but narrowed.* The
   200-tick hash is now **pinned** to a golden value in `tests/baseline.json`
   (`7cfa2420`), so a silent drift is a REGRESSION rather than a
   self-consistent green run — and the full 23-gate suite reproduces it, with
   character-identical per-gate detail, on **two independent host layers**
   (Dawn's auto-generated barriers and the Vulkan backend's table-generated
   ones). That varies the API, the barrier regime and the SPIR-V producer; it
   does **not** vary the thing this risk is about. This machine exposes exactly
   one physical device (RTX 3060 Ti — `vulkaninfo --summary` reports one GPU,
   `--adapter low` finds nothing else, and the i7-11700F has no iGPU), so one
   vendor, one driver and one shader-compiler back end remain untested against.
   What closes it is a second vendor's hash *sequence*; `docs/PLAN_vulkan_port.md`
   phase 5 records the options (a CPU Vulkan ICD such as lavapipe/SwiftShader as
   a shader-compiler cross-check, or a second physical machine, which is the
   only one that truly closes it) with their costs.
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
