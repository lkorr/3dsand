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

#### Ticket slots: the slot space is no longer the window

**The slot space and the window are two different things as of
`docs/PLAN_chunk_tickets.md` P0.** They used to be the same thing by
construction: a chunk's slot WAS its world coordinate modulo the window
(`chunkSlotIndex` = a bitmask), so there were exactly `kNumChunks` slots and
arithmetic assigned every one of them. That is why "simulate a chunk 40 chunks
away" could not be done by allocating a page — page space was never the
constraint, **slot identity** was, and a distant chunk wants the slot a near
chunk already holds.

So storage now runs `[0, kNumSlots)` where `kNumSlots = kNumChunks +
kTicketSlots`. Slots below `kNumChunks` are the window, addressed by the same
mask as ever. Slots above it are TICKETS — chunks far outside the window, held
resident so matter that leaves can finish falling, burning or settling instead
of freezing — and **no arithmetic reaches them**, only a CPU-built map. The
resulting rule, which every buffer and every kernel now follows:

> `kNumChunks` / `NUM_CHUNKS` is a statement about the WINDOW's geometry: what
> the toroidal mask wraps at, what `chunkInWindow` measures, what the raymarch
> clips to. `kNumSlots` / `NUM_SLOTS` is a statement about STORAGE: buffer
> extents, dispatch bounds over the slot space, and the plane strides inside
> multi-plane per-slot buffers. Conflating them is how a ticket slot ends up
> reading a window chunk's memory.

`kPoolPages` follows `kNumSlots` rather than `kNumChunks`, and the exhaustion
proof above is unchanged because its first line already counted slots.

Four functions in `common.wgsl` are the only sanctioned way to cross between
world coordinates and slots — `chunkSlotOf(wc, o)` (the slot, or `SLOT_NONE`),
`chunkResident` / `cellResident` (replacing `chunkInWindow` / `inWindow`
wherever the question was "may I touch this", never where it was "where is the
box"), and `slotWorldChunk(slot, o)` (the inverse, which replaced ten
copy-pasted decodes that could not express a ticket slot). `voxWordAt`,
`voxWordIndex` and `voxStore` resolve through one shared `voxSlotOfCell`.

**P0 ships `kTicketMax = 0`**, so `kNumSlots == kNumChunks`, every ticket branch
is a dead const-expression, and the world hash and both smoke probe tables are
bit-identical — which is the whole acceptance criterion for a commit that
touched 15 shaders and every per-slot buffer. The lifecycle (activation as a
MutationQueue op, the cap, dedupe, timeout, release to `ChunkStore`) is P1; the
per-site classification, and the three site classes the plan did not anticipate,
are recorded in `docs/tickets_p0_audit.md`.

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
(Since 2026-09-02 the human is meat only on the OUTSIDE — §7 "What is under
the skin" — but the split this section describes is unchanged: material is
what a voxel *is*, paint is what its surface *shows*.)
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
- **Deferred shift wake (2026-09-03, R1 of `docs/RESEARCH_streaming_hitch.md`):**
  a window shift no longer FENCES. Until this landed, `Stream::FillSlots`
  submitted the plane's `worldgenList` and then blocked on a readback of
  `occupancy`, because the CPU page-table mirror had to learn the ACT SET in
  the same tick — `PageTable::Materialize` must give a woken chunk's
  26-neighbourhood pages before the CA runs on it, and waking a chunk the
  mirror has not heard of is a `voxStore` into a sentinel, silently dropped.
  `MapReadDeferred` borrows the fence of the last submit and a fence on one
  queue waits for everything queued before it, so that wait sat behind the
  previous frame's render and GI passes and the previous ticks' CA: **33.06 ms
  of a 34.80 ms shift**, 618 shifts in 540 frames of `--autofly-surface`.

  The shift is now a two-phase pipeline with a FIXED tick latency
  `Stream::kWakeLatency` (K = 4). At tick T the plane is evicted, paged,
  generated, rendered — and left INERT: `genChunk` clears `dirtyIn`/`dirtyOut`
  for the slots it wrote and publishes its per-chunk "can any cell act" verdict
  to the new `genAct` side buffer (`TickParams::genDeferWake` selects this; the
  whole-window worldgen and `--voxserve` paths still wake in-kernel). The
  occupancy + `genAct` readback is queued with **no `Wait()`** and a
  `PendingShift` records what is owed. At tick T+K `Stream::Update` polls the
  ticket, runs the same CPU logic as before (act set → `RefilledSlot`, pure sky
  → `PT_EMPTY`, full → demote copies) and only then writes the act set into
  both dirty pages. Between T and T+K the plane is resident and drawn but
  nothing dispatches it; a neighbour that writes into it lands on a real page
  (every gen slot has one) and marks it dirty through the ordinary path.

  **The world hash moves once**, because a streamed-in plane first acts at T+K
  instead of T. K is a constant, so "when does a plane first act" stays a pure
  function of (inputs, tick); the deferral applies in BOTH residency modes, so
  it cannot itself be a source of paged/dense difference.

  > **CORRECTION 2026-09-03 (P4-G).** "paged and `--residency dense` still hash
  > identically" was an argument, not a measurement, and nothing in the engine
  > was checking it: the `streaming` gate's `sdet` is a TWICE-RUN comparison
  > inside one mode. It now also prints a fold of its whole 300-tick hash
  > sequence and the hash of its FIRST tick, and **the two modes do not agree**
  > — `f23ebbe9` paged against `397cc3e2` dense on `85133c1`, with `t1`
  > matching. A matching first tick puts the divergence in the SHIFT path (tick
  > 1 is one CA tick after a fresh whole-window worldgen, before any shift), not
  > in worldgen or materialization. Each mode is reproducible against itself, so
  > this is not a determinism failure; it is the paged/dense oracle being broken
  > for streaming, and it is not attributed further yet.
  > docs/RESEARCH_streaming_hitch.md §R2/P4-G.

  Three things make it correct rather than merely faster. (1) The sky demote at
  T+K is refused when the slot is in `cpuDirty` or the latest snapshot's dirty
  flags — a neighbour may have dropped a grain in during those K ticks, and
  `SetSentinel(PT_EMPTY)` over it would be voxel loss. A held page is not a
  leak: the slot is resident and reported, so §3.6's hysteresis frees it.
  (2) A later shift on another axis regenerates the 32 slots where the two
  planes intersect, so `InvalidatePendingSlots` blanks those entries in every
  older pending verdict — a stale "pure sky" applied to fresh stone is the same
  voxel loss. (3) `cpuDirty` must be a SUPERSET of `dirtyIn`, so every slot the
  wake writes is `RefilledSlot`-declared, `genAct` included.
- **Spreading the plane's worldgen over those K ticks: MEASURED AND REJECTED
  (R2, 2026-09-03).** `worldgenList` is 98% of the streaming GPU bill, so the
  obvious next move was to issue the plane 256 chunks a tick instead of 1,024 at
  once. Built in full and it is a **28% regression** on that very pass. The
  reason is arithmetic and no implementation fixes it: at the measured **0.99
  window shifts per tick** four planes each contribute a quarter every tick, so
  the per-tick total is unchanged and only the dispatch count goes up (~350 us
  each in submit plus the head-of-command-buffer barrier). The control that
  proves it — the same machinery with the batch set to the whole plane —
  reproduces the baseline exactly. Numbers, the placeholder design and what to
  do instead (do not generate the ~586 pure-sky chunks of a plane at all, which
  needs a CPU mirror of worldgen's `colTop`) are in
  docs/RESEARCH_streaming_hitch.md §R2/P4-G.
- **One shift per frame (R4, same doc):** `Stream::Update` is a per-TICK call
  and the frame loop runs up to four ticks, so a slow frame used to shift two
  or three times and compound its own slowness. `Stream::BeginFrame()` (called
  once from the frame loop) caps it at one. Callers with no frame loop — the
  selftest gates, `vk_smoke` — never call it and are ungated, which is what
  they already did. Pending-shift COMPLETION is per tick, not per frame.
- **Measured** (`--frames 600 --autofly-surface`, 2026-09-03, before → after):
  shift 34.80 → 2.74 ms each (its `demote` term 33.06 → 0.23); whole frame p50
  32.8 → 16.0, p95 95.8 → 56.4, p99 129.4 → 87.1, max 233.9 → 119.8; frames
  over 33 ms 270 (50%) → 96 (18%), over 100 ms 19 → 2. The cost is residency:
  the plane holds its pages K ticks longer, and the page-pool high water goes
  22,652 (65%) → 25,831 (74%) of 34,816. K is one `constexpr` and the whole
  trade-off curve was measured — K=2 gives back the residency but leaves the
  fence in place on 76% of shifts.
- **Unloaded space is treated as solid, inert, and a SINK FOR GAS** so liquids
  can't drain off the edge of the loaded world (Burkelbear's solution; adopted
  verbatim) — with one deliberate exception since 2026-09-09. Solid and inert is
  the right answer for anything that would FALL out of the world and for
  everything that would react out there. It is the wrong answer for a gas, whose
  whole behaviour is to leave: a smoke voxel refused at the window's top face
  could not tell that refusal from a stone wall, fell through to the lateral
  ring, and sheeted across the entire top chunk plane until it decayed. So the
  edge is one-way for `CLASS_GAS`: a gas voxel whose intent points out of the
  window deletes itself and becomes a gas PARTICLE (§5), which keeps moving
  outside under the same model and reconverts to a voxel if it drifts back in.
  Nothing else crosses, and nothing reacts out there.
- Overworld draw distance beyond the window is handled by the render-only
  far-field cascades (§9) — the streaming horizon is no longer visible from
  the surface. Underground, darkness still hides it.

---

## 3b. Voxel scale — authored size is METRES (added 2026-08-30)

`kVoxelMeters` is meant to be a knob. Halving it should make the world finer,
not smaller: a 1.7 m player stays 1.7 m, a 7.5 m oak stays 7.5 m, and only the
number of cells they are made of changes.

That did not hold. `kVoxelMeters` had not moved since v0.2, and when it went
0.10 -> 0.05 the split showed up immediately:

* **The player controller was fine.** `player.h` has said
  `kHalfY = 0.85f / kVoxelMeters` since the beginning, so the capsule stayed
  1.7 m and every movement row in `Tuning::Player` came with it.
* **Everything AUTHORED halved.** The avatar art, mobs, items, armour and every
  tree are baked cell counts, and a cell count only means a size next to the
  scale it was authored at — which nothing recorded. The human became 0.85 m
  *inside a 1.7 m capsule*, and the whole selftest suite stayed green, because
  every existing assertion is expressed in the asset's own lattice and those
  ratios all held.

### The rule

> **No authored quantity representing a physical length may be expressed in
> cells without also recording the cells-per-metre it was authored at.**

Non-worldgen code authors in **metres** and converts at use, through
`src/sim/scale.h` (`MetresToCells`, `MetresPerSecToCells`, `MetresToCellsI`).
The map's terrain block (`assets/worldmap/<name>/map.json` `terrain`, since
environment truth P-G) keeps its own older mechanism — numbers authored in
voxels at `terrain.refVoxelsPerMetre`, rescaled by `LoadWorldMap`, with the
prelude's `REF_VOXELS_PER_METRE` for the shader's own literals — because a
terrain octave amplitude has no natural metre value. Two mechanisms, one line
between them; do not add a third.

### Three asset classes, three mechanisms

| Class | Examples | Mechanism | At 2x resolution |
|---|---|---|---|
| Procedural, authored in metres | `assets/trees/*.json` -> `.svtree` | Re-baked at the engine's scale; the header records it and the loader refuses a mismatch | Same height, **finer detail** |
| Hand-authored voxel art | `assets/mobs/*.vox`, `assets/items/*.vox` | Sidecar declares `artVoxelsPerMetre`; `skinScale` is derived | Same silhouette, finer world-cell model |
| Scalar constants | capsule, gait, reaches, melee | Metres in source, converted at use | Same physical value |

**Trees.** `assets/editor/treegen.js` takes the bake scale as a parameter;
`scripts/bake_trees.mjs` reads `kVoxelMeters` out of `world.h`. `.svtree` is
version 2 and stores `bakeVoxelsPerMetre` in header word 29, and
`treeatlas.cpp` **refuses** an atlas whose scale is not the engine's, naming
both numbers — the same policy as `worldio.cpp`'s bit-exact save check. The run
word was repacked `material(12) | state(4) | y0(11) | len(5)`; the old 9-bit y0
capped a variant at 512 cells and a 22 m redwood needs ~520 at 5 cm.

**Voxel art.** `artVoxelsPerMetre` is the authored fact and `skinScale` is
`artVoxelsPerMetre / kVoxelsPerMetre`. The human is 80 art vox/m, so skinScale
is 8 at 10 cm and 4 at 5 cm — **the .vox is untouched** and the collider gets
finer for free. Art coarser than the world is block-replicated
(`UpsamplePrefab`); the reverse is deliberately absent, since downsampling is
lossy and cannot arise while every asset is drawn at or above the coarsest
voxel size in use.

Authored offsets (anchors, sockets, cutting edges, cover boxes) stay in the
art's own lattice and convert through `MobDef::ArtToWorld()`, which divides out
any upsample. **Two lattices meet at the rig**: `ld.anchor` is authored while
`AutoAnchor` is measured off the (possibly replicated) prefab, and they need
different divisors — using one for both collapses an upsampled rig toward its
own origin while the art still looks correct.

World-space sidecar rows (`speed`, `severImpactSpeed`, `gait.rideHeight`,
`states[].bodyYOffset` — now only meaningful on states that are *not*
`groundAlign`ed, §8 — clip position keys) are neither art units nor metres:
they are world voxels, declared by `sidecarVoxelsPerMetre` (default 10) and
rescaled at load. `bleed.perDamage` is deliberately NOT rescaled — it is a
volume budget, which goes as the cube, and it is gore rate rather than size.

### The gate

`--gate scale` (`src/test/selftest_scale.cpp`) is pure CPU and runs near the
front of `kOrder`. Its load-bearing assertion is the only non-circular one
available: the avatar's height comes from the art, `Player::kHalfY` comes from
`0.85f / kVoxelMeters`, and nothing but intent ever linked them. Most other
size checks reduce algebraically to `box / artVoxelsPerMetre` and would pass at
any scale, so the rest of the gate leans on metre pins in `tests/baseline.json`
— recorded at one voxel size, re-checked at another.

`scripts/set_voxel_scale.sh <metres>` edits `world.h`, re-bakes the atlas and
prints the build + rebaseline commands. It refuses to go below 10 vox/m,
because that is the resolution the art is drawn at.

### Known-unscaled, on purpose or not yet

Not fixed here, and none of it affects tree/player/mob size: worldgen's POI
literals (the arena, wood platform and ruins in `worldgen.wgsl` never took
`vlen()`), explosion and spell radii, `gore.*`, `debris.min*Voxels`, the `sim.*`
integer rows (determinism-critical, so their own change), and `.svedit` layers.

## 4. The Simulation (GPU cellular automaton)

Fixed 30 Hz tick. Each tick, dispatch compute over dirty chunks only, batched via
SSBO lists of chunk indices.

### Movement rules (Noita rules lifted to 3D)
- **Powder**: try the cell directly below. If blocked, try the four horizontally
  adjacent cells *of the below cell* in random order (RNG replaces 2D's left/right
  alternation and breaks the symmetry that would create perfect square pyramids).
  Else stay and clear the moving bit. This alone produces angle-of-repose piles.
- **PER-MATERIAL ANGLE OF REPOSE (2026-09-13).** One down, one across is 45°,
  and for years that was the angle of *every* powder in the engine — dry sand,
  angular gravel, snow, ash and dust all built the same cone. One optional
  authored integer changes it:

  ```json
  "repose": 34          // degrees, POWDERS ONLY, 18..72
  ```

  Absent (or 45) compiles to the word **zero**, which the kernel reads as the
  bare down-diagonal, so a material that says nothing is bit-identical to the
  old sim rather than merely close to it. Loader:
  `PackRepose`/`MaterialGpu.repose` (`src/sim/materials.h`), kernel: the ANGLE
  OF REPOSE block in `sim_step.wgsl`, gate: `repose`.

  - **The angle is a RUN:RISE RATIO, because a lattice CA cannot hold anything
    else.** Movement is whole cells, so the expressible angles are those whose
    tangent is a ratio of small integers. Five tiers, in two families either
    side of the old rule: `3:1` (18.43°) and `2:1` (26.57°) flatten a pile;
    `1:1` (45°, the word 0) is unchanged; `1:2` (63.43°) and `1:3` (71.57°)
    steepen one.
  - **FLOWY: a 2:1 face was ALWAYS stable under the old rules; nothing ever
    built one.** On a 2:1 staircase the cell one down and one across from a
    surface grain is the top of the next run, i.e. filled, so stage 2 refuses
    and the grain rests. Flatter faces do not collapse — grains simply *stop as
    soon as they cannot descend*, which is one cell too early. So the flowy half
    is one extra stage rather than a new force: a grain that has already failed
    the straight fall AND all four down-diagonals takes **one lateral step**
    toward a drop it can see within its tier's run (`(2d,-1)` for 2:1,
    `(3d,-1)` for 3:1). It cannot fire for a grain that could have descended,
    because descent is tried first and returns.
  - **STEEP: the mirror image.** A 1:2 grain takes the down-diagonal only if
    `(d,-2)` is open too — i.e. only if it would fall at least 2 — so a
    one-voxel step never spreads and the face grows one run per two drops. The
    cohesive look that falls out of it (a small bump on a drift simply stays) is
    the intent, not a defect.
  - **Between tiers is a per-grain MIXTURE, not a rounding.** The word carries
    two tier codes and a blend; `blend/256` of the grains use the second. Sand
    at 34° would be ~60% of grains on 2:1 and ~40% on 1:1, and the pile rests
    between the two. The choice is a hash keyed on **WORLD POSITION with NO TICK
    in it**, and both halves are load-bearing. No tick: a grain that has not
    moved decides the same way on every tick of its life, so it fails the same
    stage, writes nothing, marks nothing dirty, and the pile SLEEPS (rule 2);
    keyed on the tick, a settled dune would re-roll a share of its grains every
    tick, a few would find a move, and those chunks would never sleep again.
    World position rather than the slot index (the first cut used the slot, and
    it was wrong): a slot index is WINDOW-RELATIVE and renames on a streaming
    shift, so a settled pile would silently re-roll its whole mixture the moment
    the player walked far enough. Same mixing as `synthJitterState`.
    **The blend must be gated on the GRAIN'S CODE, not on the material.** Gating
    the slide on `repose != 0` made every angle from 27 to 44 behave as pure
    2:1 and the blend inert — a bug no pure-tier test arm can see, which is why
    the gate has two arms driven by real `PackRepose` output.
  - **A per-tick move CHANCE is not an angle**, which is why `moveEvery` was not
    reused for this. A chance changes how *fast* a slump runs and leaves the
    resting state exactly where it was; repose is a property of the *fixpoint*.
    Both exist, and `moveEvery` is now documented for powders as the speed knob.
  - **WRITE reach is still 1** — the lateral step is an ordinary `tryMove`, the
    same single-cell write the mite wander stage already makes, so
    `flagSupportLoss` still runs for the cell the grain vacates.
  - **READ REACH: THE OCCUPANCY SNAPSHOT IS THE ONLY LICENCE IN THE CA FOR A
    READ AT DISTANCE ≥ 2, and the sharp form of the lattice argument was not
    previously written down anywhere.** The 3×3×3 write boxes of same-colour
    acting cells, spaced 3 apart, **tile space**: every cell in the world lies
    inside exactly one acting cell's box. So a read at distance ≤1 is inside
    *my* box and only I can write it — exactly the licence `soloSolid` spends on
    its six face probes — and a read at distance ≥2 is inside *somebody else's*,
    where pre-write versus post-write is decided by GPU scheduling. There is no
    "probably fine" band in between. And repose cannot be done at reach 1 at
    all: a 2:1 face and a 1:1 face are *locally identical* at a surface grain
    (uphill lateral filled, downhill lateral air, downhill diagonal filled, cell
    below filled) — the difference IS at distance 2. That is a fact about the
    lattice, not about this rule.

    So every probe reads `reposeSnap` (`world.h`'s `kReposeSnap*` block): **one
    bit per voxel, "a powder could drop into this cell", taken at TICK START**
    by the `reposesnap` prepass and read by the CA behind a barrier the pass
    table generates. A snapshot bit is the same in every run of the same tick by
    construction. It is a side table per design guideline 2 — derived, not
    hashed, not saved — and it is deliberately **stale**: a drop that fills in mid-tick still
    reads open, the grain slides toward it, the ordinary `tryMove` refuses
    (the destination is a powder), and next tick's snapshot says so. The error
    is bounded by one tick and identical in every run, which is the only
    property rule 1 asks for.

    **Validity is per SLOT, and required.** The window is toroidal, so a slot is
    reused by a new world chunk as the window walks; each slot carries `tick+1`
    and a probe into a slot whose stamp is not this tick's REFUSES. Zero is
    therefore never a valid stamp, which makes the zeroed allocation the correct
    cold start with no reset path. Nothing finer is needed: the window origin is
    constant for the whole of one command buffer, so a slot cannot change
    occupant between the prepass and the CA rows of the same tick. In practice
    the refusal path is unreachable for an acting cell — a cell only acts if its
    own chunk is on the dirty list, and every probe target is within that
    chunk's probe ring, which the prepass covers by construction.

    **The ALLOCATION is dense; only the WRITING is sparse.** One bit per
    cell in the window is 16.125 MiB resident from `World::Init`, roughly +36%
    on the page pool's own resident bytes, and that is a flat cost paid by any
    world whose materials.json carries a `repose` line. What scales with
    activity is which chunks get refilled. A genuinely sparse form would trade
    the flat cost for an indirection in the CA's hottest read; at one bit per
    voxel that trade is not worth making, but it is a trade.

    **MEASURED, per row, with `--measure`'s row-granular timestamps** (RTX
    3060 Ti, `SANDVOX_RUN_EXCLUSIVE=1`). The row has its own pass group, so
    this is the prepass's own GPU time and not a difference of two totals:

    | scenario | active chunks/tick | `reposeSnap` | % of sim | `ca(54)` |
    |---|---|---|---|---|
    | (c) settled | 0.0 | **not recorded** | — | — |
    | (e) minimal | 0.7 | 0.089 ms | 3.5% | 0.93 ms |
    | (b) active | 4.5 | 0.123 ms | 3.5% | 1.33 ms |
    | (a) settling | 37.1 | 0.151 ms | 2.7% | 2.73 ms |
    | (d) heavy | 152.2 | 0.556 ms | 5.9% | 4.65 ms |

    So it costs 9–12% of the CA loop it feeds, and 0.56 ms/tick at the heaviest
    activity the suite produces. The settled row is the important one: it is
    absent from the table entirely, not zero-valued, because `Cond::ReposeActive`
    did not record it.

    **A whole-sim A/B is NOT the instrument for this, and trying it first was a
    wasted pair of runs.** `--gate far-fog`'s "active scene" ms/tick read 10.07,
    15.28, 10.43 and 37.88 across four runs of near-identical trees — it swings
    by 3.6x on its own, so the arm WITHOUT the prepass came out slower than the
    arm with it. A row with its own pass-group label can be timed directly;
    differencing two noisy totals to find a 3% term cannot.

    **The prepass costs nothing at rest**: it is one indirect dispatch over the
    same compacted dirty list the CA is about to use, so a settled world
    dispatches zero workgroups. Each workgroup fills its dirty chunk *and the
    nine neighbours a probe can reach*, because a resting grain's probes reach
    3 cells and cross into a neighbour chunk that may be asleep, and a boundary
    artifact every 16 cells is not acceptable. Nine rather than the 3x3x3
    block's 26, on two one-line facts about the probe offsets: **every probe has
    `dy <= -1`** (nothing here looks up), and **no probe offsets both lateral
    axes** (so each layer's corners are unreachable). That is 2.7x off the
    prepass, and it is measured INERT — the same world hash and the same six
    pile hashes as the 27-chunk version. The price is a table coupled to those
    offsets: a probe that looked up, or stepped diagonally, must grow it in the
    same commit or it silently reads an unsnapshotted slot and REFUSES, which
    corrupts nothing (a refusal leaves a grain where it is) but would quietly
    steepen piles near one chunk boundary in sixteen. Ring members are re-filled by every dirty chunk that
    touches them — redundant, never divergent, since the bits are a pure
    function of voxel state nothing in the pass writes — and a per-slot stamp
    skip removes most of that. A sentinel chunk (`EMPTY`/`UNIFORM`/`JITTER`)
    costs 128 stores and **no voxel reads at all**: its material is uniform, so
    the whole bitfield is all zeros or all ones and `synthWordAt`'s two PCG
    rounds per cell are never paid.

    **A REJECTED DESIGN, recorded because the failure is invisible.** The first
    cut read the TICK STAMP of an air cell to prove no acting cell owned the
    probe's write box. The ownership argument was sound; the *bit* was not. The
    stamp is explicitly not representation-invariant (`sim_occupancy.wgsl`: it
    "legitimately differs between two runs that reached the same world"),
    `world.h` strips it on save, and `pagetable.cpp`'s `kAirDemoteMask` demotes
    an all-air chunk to `PT_EMPTY` ignoring stamps — after which every cell
    reads `STAMP_NEVER`. With a 7-long cycle a stale stamp on vacated air
    aliases the current substep one time in seven, so the guard would allow in
    one run and refuse in another; a refused slide marks nothing dirty, so if it
    landed on a grain's last attempt the chunk slept and the pile stayed one
    cell steeper — invisible to both the world hash and the twice-run
    comparison. **No CA path reads a tick stamp on an air cell**; `main`'s own
    gate reads it only after returning on `MAT_AIR`.
  - **Termination is the nearest-drop-first ordering, not an assumption.** A
    slide is the one move in the powder chain that does not lower a grain, so an
    unbounded run of them would keep a chunk awake forever. It cannot happen: a
    slide toward a drop *k* cells out leaves the grain with that same drop *k-1*
    out; at *k-1 == 1* stage 2 takes it and the grain falls, and at *k-1 == 2*
    the near pass fires again and cannot instead go BACK, because the cell two
    out backwards is this grain's old down-diagonal, which was filled. Searching
    3-out before 2-out would break exactly that, so the stage is two passes.
  - **Storage: `MaterialGpu` grew 64 → 80 bytes** (`repose` plus three reserved
    words) rather than squatting in `hardness`'s spare 24 bits. Every blast and
    dig path compares `hardness` as a whole word, and a masked reader in nine
    places to save 16 bytes in a 4096-entry table — 256 KiB → 320 KiB, once, in
    one buffer — is the worse trade. Nothing else had to change: every reader
    addresses the table by index through `sizeof(MaterialGpu)`, so the stride is
    written down nowhere else. The five tier codes and the `(codeA, codeB,
    blend)` shifts/masks are emitted by `ShaderConstantPrelude()` from the C++
    definitions, so neither `common.wgsl` nor `sim_step.wgsl` restates them, and
    `scripts/check_shaders.sh` scrapes `materials.h` so a shader edit still
    validates with no build.
  - **THE FLATTER TIERS CANNOT BE GIVEN TO THE BULK TERRAIN POWDERS YET, and
    that is measured rather than cautious.** `Land.slope` is documented as
    "256 == 1 voxel/voxel == repose" (`src/sim/worldmap.h`), and every place
    worldgen decides whether ground is "too steep for a powder bed" — the biome
    skin in `genCellIn`, the sediment wedge's `sedSlope`, the pond bed's
    steepness test and `bed.substrate` — is sized against that one number. Give
    sand 34° and the desert's own sand cap is suddenly over its angle of repose
    on every slope steeper than 2:1, so the whole window starts creeping
    downhill the moment it generates. Measured as a full-`--selftest` A/B whose
    only difference was those authored lines: sand 34 / gravel 40 / dirt 40 /
    snow 38 turned **`sleep`** red (the world never reached the <32-active-chunk
    bar inside the gate's 3000-tick cap) and **`worldmap`** red (`voxel air but
    twin says biome desert (skin sand)` at (144,426) — the cap had slid off
    before the comparison), and took `corpse-burn` with them. The control arm
    reproduced the base `determinismHash` exactly, so the attribution is not a
    judgement call.

    **A STEEPER value is safe on worldgen ground, and the argument is one line:
    a steep tier only ever REFUSES a move the old rule allowed**, so it cannot
    wake anything 45° did not. That is why `snow` ships at 63° (a packed drift
    holds a steep face) while `sand`, `gravel` and `dirt` keep 45.
    **Follow-up:** make those worldgen slope gates read the placed material's
    own repose instead of the constant 45, then author sand 34 in the same
    commit. That is a worldgen change with its own hash move and its own gates
    (`terrain`, `waterbody`, `worldmap`, `settle-back`) and it is deliberately
    not bundled here.
  - **Authored:** `snow` 63 (steep), `ash` 40, `dust` 30, `seed` 30 (flowy);
    `sand`, `gravel`, `dirt` keep the 45° default for the reason above. `mite`
    keeps it too: it wanders, and its movement is not a pile.
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
- **A NEUTRAL RULE MAY NOT LICENSE ITSELF — `FILM_LICENCE`** (2026-09-08). The
  argument above quantifies over an OPPORTUNITY (*"which CAN therefore split"*),
  and on an open water surface the opportunity is taken by somebody else: films
  surround every hole, one of them advances into it before the pressing cell
  splits, nothing splits, `SUM(f*f)` does not move and the configuration has
  merely rotated. Measured at the authored `home_lake`: 14 chunks awake forever,
  reasons `MOVE 14 film-press 14`, 37 of 47 changed words back where they
  started over 20 ticks, not one split — a shuffle at a third of a voxel per
  tick, invisible in the game and a permanent breach of rule 2. The riser branch
  (`filmStepAllowed`) has the same shape of hole, admitted in its own block:
  two one-voxel risers facing each other 3 apart is a 2-cycle, and no reach-1
  predicate can break it because a terrace tread's inner cell and a 2-wide
  rimmed gutter's cell have byte-identical 3×3×3 neighbourhoods (the one read
  that separates them, `c + 2d`, is racy — acting cells are ≥3 apart and each
  writes within 1, so a cell exactly 2 away is the one another thread may be
  writing). **Both film branches now require a licence**: they may only fire in
  a chunk where something that DID strictly decrease a Lyapunov function — a
  descent, an equalize, a split, a bridge, a powder or gas move — or an external
  input (mutation, seam, particle, reaction) marked the chunk last tick. The CA
  reads its own previous verdict, `dirtyIn[chunk]`, one scalar per workgroup
  (`R(DirtyIn)` on the `ca` row, the same read `waterQuiet` already makes). Cost,
  measured: `ca-slope` unchanged to the digit (96.9% into the basin, 0 eighths
  left on the ramp), `ca-level-one` and `ca-level` unchanged; `sleep` goes from
  14 chunks awake forever to **0, fully quiet**. Pinned by `ca-gutter`.
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
    the same blob on standing water), `ca-gutter` (the negative of `ca-slope`:
    a 2-wide slot cut into a plateau, the geometry the riser step cannot
    resolve, asserted to SLEEP rather than to drain).
- **…AND THE SET HAD A NEUTRAL RULE IN IT — `DIRTY_M_DISPLACE`** (2026-09-16).
  The doctrine above is right and was applied to one rule short. **The lateral
  lighter-fluid displace** (`stepLiquid` stage 3's last branch) swaps two whole
  fluid cells at the SAME level: both keep their `y`, so `SUM(f*y)` is
  unchanged, and both keep their fullness, so `SUM(f*f)` is unchanged. It is
  neutral in both — and it was ungated, *and* its mark was a member of
  `FILM_LICENCE`, so it licensed itself and the film branches as well.
  Measured at the owner's desert tarn (`--gate pond-shore`, pass C): 5 of 507
  shore chunks awake forever, reasons `MOVE 2 displace 2`, 1 of 2 changed words
  back where it started, `water<->steam` at (-2344,112,1670) — a steam cell
  batted back and forth between water cells on a flat pond surface.
  **What starts it is DAYLIGHT**, which is why nothing caught it: evaporation is
  authored `when: "day"`, and `ca-gutter`/`ca-slope` both pin the phase to a dim
  dawn *on purpose* ("freezing and evaporation are authored mass sinks and would
  make the audit inexact"). Correct for an audit, and it meant every liquid gate
  in the engine ran with the rule that seeds the churn switched off. `ca-gutter`
  additionally cuts its slot from stone and sets `fluidExciteMode 0`, so nothing
  in its fixture can grant a licence at all — a correct test of the rule and of
  nothing else, while the licence's entire soundness argument is about *what
  else is in the chunk*.
  **Fix**: the bit is out of `FILM_LICENCE` and the branch takes the same
  licence the film steps do (and so does its `canFlowAnywhere` mirror — the two
  must agree or a chunk pins awake or sleeps with work left). A **downward**
  displace is untouched: stages 1–2 go through `tryDescend` and strictly
  decrease `SUM(f*y)`, so water still falls through smoke and oil still
  stratifies. What is lost is only the same-level sideways swap between two
  settled fluids, which had no driving force behind it in the first place.
  Gate: **`pond-shore`** — the owner's own shore at (-2317,112,1674), three
  passes (pristine / disturbed by a splash / disturbed at NOON), reporting awake
  chunks, the reason histogram, a 2-cycle count and the water ledger. It is the
  first gate in the engine to tick a PROCEDURAL pond at all: `terrain` pass D
  reports 0 awake because its window is the harness pad, which the map's own
  site table declares as "no tarns".
  - **Also measured, and it is worth knowing**: a sand bank drinks **132,033
    eighths** out of that pond in its first 130 ticks, through sand's authored
    `absorb: {capacity: 6}`, and then saturates and stops. That is the designed
    behaviour of absorption (§6) at a shoreline's scale, not a leak — the ledger
    in `pond-shore` is what says it *stopped*.
- **Gas**: inverse powder (up, then up-diagonals, then lateral), plus decay chance.
- **Solid**: doesn't move; participates in reactions and structural checks only.
- **Density displacement**: a mover entering a cell occupied by a less-dense
  fluid swaps with it (oil floats on water; sand sinks through both). DOWNWARD
  and diagonal displacements are free — they strictly decrease `SUM(f*y)`. A
  LATERAL one between two liquids/gases at the same level changes neither
  Lyapunov function, so for a liquid it needs the film licence; see the
  `DIRTY_M_DISPLACE` entry above.

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
- **A LIQUID IS NOT A WALL** (2026-09-13, `docs/PLAN_debris_buoyancy.md`). It was
  one until then — "splash = plop onto the surface" — and that is what built
  rafts of exploded tree hanging over a pond: the first chip stopped on the
  film, the next was blocked by the first, and no CA rule moves a solid touching
  another solid. A particle whose material authors `fluid.lift > 0` now flies
  THROUGH a liquid under buoyancy (`g · ρfluid/ρself`, capped) and viscous
  damping, so which way it goes is `density` and not a knob, and the damping is
  what makes the rise/overshoot/fall converge to a bob rather than oscillate.
  Three consequences for reinsertion, all of them load-bearing: a particle may
  take a cell holding a liquid it is **denser than** (a sinking rock's resting
  cell is water, and the displaced eighths are dropped exactly as
  `sim_mutate.wgsl` drops them — displacement raises the level of a whole body
  of water, which is not a write this pass can reach); it may only settle where
  something **supports** it, or neutrally buoyant matter converts to a voxel
  hanging mid-water; and a floater that finds its berth taken **drifts** instead
  of reinserting on top of it. `sim.partFloatPatience` wet ticks bound the hunt,
  so a pond too small for the debris thrown into it jams into a pile rather than
  keeping particles alive forever (rule 2). `lift == 0` is the opt-out and keeps
  every older behaviour: micro spray, and liquid/gas ejecta, whose landing in
  its own liquid is a MERGE and a different rule than this one.
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
  mechanisms will reason a world settled while matter is still moving. Gas
  parcels (below) are the second source and are inside it: `NoteSnapshot`
  disqualifies on `gasLive_` beside `particleCount`. That obligation was
  discharged only after the `gas-leave` gate went looking for it, which is the
  argument for the gate measuring it rather than the design doc asserting it.

**Gameplay projectiles are a separate CPU system** (§8) — they carry game logic.

### Gas particles: the window edge is a sink (2026-09-09; `sim_gas.wgsl`, docs/PLAN_gas_particles.md stage 1)

A SECOND particle population, with its own buffers, for the one kind of matter
whose whole behaviour is to leave the world: gas.

**The defect.** `tryMove` returns false out of the residency window, and a gas
voxel could not tell that refusal from a stone wall. It fell through to the gas
ladder's lateral ring and SHEETED across the entire top chunk plane — up to
1,024 chunks plus the dilated layer under them — until its decay rule fired
(smoke is 9/1000, so ~111 ticks of it). That sheet is the chunk outline visible
in the sky over a big fire, and at 27 colour phases × 2 substeps × 4,096 cells
per awake chunk it was most of the fire's CA cost. §3's "unloaded space is solid
and inert" is right for everything that would fall out of the world and wrong
for a gas.

- **Representation.** The same 32-byte `Particle`: `payload` bits 0..11 material
  and 12..15 state, plus `PFLAG_GAS`. Position is 24.8 fixed with a **zero
  fraction** — a parcel lives ON a cell and moves in whole cells — and velocity
  is always zero, which keeps `particlePriority` a pure function of the visible
  state. No age field: death is rolled from the material's own bucket.
- **Edge conversion.** In `sim_step.wgsl`'s gas tail, a `tryMove` that fails
  because the target is out of the window (as opposed to failing on
  `canDisplace`) calls `gasLeave`: the cell becomes air, the chunk is marked,
  and a record is appended to `gasSpawn`. Reach 0 — it writes only its own cell,
  and the target was never writable. Any face, not only +Y.
- **Motion outside is the grid model verbatim.** The same `gasIntent` roll,
  `windLateralStart` order and fourteen-candidate fallback ladder the CA uses,
  refactored to take the identity key and the substep as arguments so both
  kernels call one definition. Not a reduced-fidelity puff.
- **Blocking outside is `farVox`**, the far cascade's level-1 byte — the same
  test the far march uses. Dense toroidal array, direct index, **not the page
  table**: the page table does not extend past the window and has no business
  being asked about smoke 60 m away. Outside the cascade reads as OPEN, not as
  blocked, or every plume would pin against an invisible wall.
- **Bounds (rule 2), all three stateless tests on the parcel's own position:**
  the outer box, `origin.y + WORLD_N + kGasCeilingVox`, and the authored decay.
  `gasDecayProduct` walks the material's `RK_DECAY` entries exactly as
  `doReactions` does, so the chance an author tunes for the voxel is the one the
  parcel obeys; a gas product morphs, `air` dies, a grid product proposes a
  landing. Neighbour-COUNT scaled rules are skipped — out there a parcel has no
  neighbours and `scaledChance`'s answer would be a fabrication.
- **Re-entry = RECONVERT.** A parcel whose next cell is inside the window
  proposes itself as a voxel through the ordinary `atomicMax` claim path, one
  claim per parcel per tick, deterministic winner, losers retry. That is the ONE
  place a gas parcel meets the page system, and it is why gas that leaves is not
  gas that is lost: it rejoins the reaction system on landing.
- **Rendering** is `gasOuter`, a coarse density box: one 16-bit COUNT per cell,
  two to a `u32`, 128³ cells over exactly two window edges, centred on the
  window, 4 MiB, at `renderBGL_` binding 21 / GAS group 6 / `simBGL_` 34.
  Cleared and re-splatted every tick (`atomicAdd` of a half-word lane, so the
  result is scheduling-independent), sampled once per pixel by `gasOuterFill`.
  Render-only derived data: the sim never reads it, the world hash never covers
  it, it is never stale. Saturating at 60,000/cell rather than carrying into the
  neighbouring cell's half of the word.
  **Deviation from the plan:** the cell is 0.8 m, not 0.4 m. The plan asked for
  0.4 m cells AND a 2x-window span AND 2 MiB; 128³ bytes IS 2 MiB and 128 ×
  0.4 m is half the stated span, so the three never agreed. Span is kept.
  **Second deviation:** the parcel splat is folded into `gasResolve` rather than
  given its own dispatch, because resolve already walks every live parcel once.

#### Stage 1b: the two representations CROSSFADE, they do not abut

Stage 1 left an explicit seam. Inside the window gas is a 0.1 m voxel; outside
it is a parcel drawn from 0.8 m cells; the two met at the window face with no
overlap, so there was a visible line 25.6 m from the window centre where crisp
voxel smoke became 64x-larger soft cells. The fix is entirely on the render
side — **the sim is untouched and the determinism hash does not move (9bfed213
before and after)**, because sim behaviour that depended on camera distance
would be a rule-1 violation and in-window gas must stay a voxel to keep the
reaction system.

- **The CA splats too.** `sim_step.wgsl`'s `gasOuterSplat` adds every in-window
  gas voxel to the same box, ONCE per tick (substep 0, after the stamp gate,
  before the move), gated on `sim.gasMode` so a world with the gas rows
  unrecorded cannot accumulate into an uncleared box. The box therefore holds
  BOTH populations, which is what makes a crossfade possible at all: a voxel
  can only fade out into a coarse cell that has something in it. `gasOuter`
  joins the CA at `simBGL_` binding 34 and the `ca` row's R/W set as
  `A(GasOuter)`.
- **The byte became a u16** for this: 192/512 = 37.5% full was a ceiling nobody
  saw while the box only held distant parcels, and a crossfade INTO it would
  have thinned every dense plume exactly at the seam. Measured by `gas-leave`: the in-window splat reaches **204 in a single 0.8 m cell** twenty ticks after a 4,096-voxel puff, so the byte's 192 guard was not a theoretical ceiling -- it clips this fixture.
- **The weight is the max-norm distance from the WINDOW CENTRE**, smoothstepped
  from `render.gasBlendStart` × 25.6 m to 25.6 m. The centre and not the camera:
  the max-norm distance is exactly the half-extent at every point of all six
  faces, so the weight is 1 at every face regardless of where the camera stands.
  A camera-relative ramp would have to be re-tuned per view and would still
  leave a seam on the faces it was not tuned for.
- **Voxels fade out** by `1 - b` in `trace()`'s media branch (gases only; a
  liquid has no coarse representation), and **only into a non-empty coarse
  cell** — the CA visits awake chunks only, so a settled plume in a sleeping
  chunk is in no coarse cell and keeps its full opacity instead of fading into
  nothing. That one extra fetch is what makes the sleeping-chunk hole a
  non-event rather than a disappearing plume.
- **The coarse fill fades in** over the same shell: `gasOuterFill` grew a second
  segment, from the inner box out to the window face, weighted by the same
  ramp and with its own step budget so widening the band cannot thin the
  distant plume. It now runs for rays that HIT inside the window too, bounded by
  the near hit rather than by "did the ray leave".
- **RenderParams bit 3 (`RFLAG_GAS`)** is what keeps all of it free in a world
  with no smoke. `Simulation::EncodeTick` publishes it through
  `SetGasRenderActive` (renderspec.h) from a latch armed by live parcels OR the
  gas dirty-reason bits in the snapshot, held one second past the last sighting.
  It is a correctness gate as well as a budget: `gasOuter` is only cleared on
  ticks the sim records the gas rows, so with the flag off the box is stale and
  must not be sampled. NOT a `SPEC_` constant — it flips whenever a fire starts
  or goes out.
- **Measured cost:** raymarch fragment register count 168 → 168, no spills,
  binary +0.18%; the CA's `step` kernel 56 → 56 registers, binary +0.11%.
- **Both live wires are ASSERTED, not argued.** `gas-leave` now fails if the
  render flag never arms over a 400-tick run with a live plume (it is on for
  320 of them) and if `gasOuter` is empty INSIDE the window at t20 with the
  whole plume still in it (sum 3,437, max 204). Either zero leaves every other
  number in that gate untouched and simply turns the crossfade off, which is
  the definition of a thing that needs its own assertion. The in-window probe
  is deliberately EARLY: at the main t200 probe this fixture's plume has
  entirely left through the ceiling, and the first version read 0 there and
  reported a dead splat on a run where it had worked for a hundred ticks.
- **Known and not repaired:** the coarse contribution still does not feed
  `gasHalfT` or `fireGlow`. Stage 1's argument for that was geometric (every
  coarse voxel-length was beyond the window exit); stage 1b's band is inside the
  window, so a raster body can stand behind coarse gas the depth raster does not
  know about. Bounded by the crossfade weight — zero at the inner edge, and only
  fully coarse at the face, 25 m away.
- **Own buffers, not a share of `kParticleCap`.** `gasParticles[2]` at
  `kGasParticleCap` = 262,144, plus `gasCounts` / `gasClaim` / `gasSpawn` (the
  CA's outbox, whose 8-word header is this tick's counters) / `gasSpawnOps` (the
  CPU op stream, rule 3) / `gasArgs` / `gasOuter`. The two populations must not
  be able to starve each other: a fight full of ballistic debris must not thin a
  plume, and a forest fire must not stop a sword from shattering.
- **Determinism, and the hole it had to close.** A parcel outside the window
  touches no voxel, so the world hash **cannot see it** — a scheduling-dependent
  step out there would reproduce a matching hash sequence for a whole run and
  only surface a hundred ticks later when the parcel landed. The population
  therefore carries its own digest (`kGasSpDigest`: the SUM of every surviving
  parcel's `particlePriority`, a sum because the pool's append order is
  scheduling-dependent by construction), which the `determinism` gate compares
  across its two runs alongside the hash series.

**The open problem, recorded because sizing a cap is not the same as fixing
it.** `gasLeave` charges a shared `atomicAdd` cursor, so WHICH voxels are
refused when the per-tick list fills is decided by which workgroup arrived
first — and a refused voxel STAYS IN THE GRID, where the world hash can see it.
That is scheduling-dependent output, i.e. a rule-1 hazard. It is held off by
sizing `kGasSpawnPerTick` (65,536, a quarter of the window's top face in ONE
tick) out of reach, which makes the POOL the binding constraint instead — and a
dropped parcel is already outside the window and cannot move a voxel. The
`gas-leave` gate asserts refusals == 0, so the day that is not enough it is a
printed number rather than a silent divergence. **The real fix is mark+apply**:
the CA flags cells that want to leave and a second pass converts them in a
deterministic order, which is the pattern `sim_explode` already uses for exactly
this reason.

**The settled-tick skip vs parcels in flight — FOUND BY THE GATE, FIXED IN
`simulation.cpp`.** This section's own paragraph above states the obligation:
"a new GPU-side particle source must land inside that count, or both mechanisms
will reason a world settled while matter is still moving". Gas parcels did not.
`Simulation::NoteSnapshot` licensed the settled-tick skip on
`activeChunks == 0 && particleCount == 0`, `particleCount` is the BALLISTIC
population only, and `inputsThisTick` in `EncodeTick` had no gas term. The
C_GAS latch was never the answer to this and was briefly reported as though it
were: it decides whether the gas ROWS are recorded, so parcels kept flying — but
a re-entry landing is a voxel write at a cell the CPU did not choose, and if it
happened on a tick the CA rows were skipped for, that chunk would be simulated
one or two ticks late, "how late" depending on when the readback ring got round
to reporting it. Late is not lost; scheduling-dependent is a rule-1 break.

Both halves are closed now. `NoteSnapshot` disqualifies on `gasLive_ != 0`
alongside `particleCount`, `NoteGasLive` additionally clears `settledProven_`
directly so the fix does not depend on call order, and `gasSpawnsThisTick_` is
in `inputsThisTick` for the reason `windWakeCount` is — a CPU-queued parcel can
re-enter and land, so it is a chunk-dirtying input. `gasLeave` conversions are
deliberately not in that disjunction and do not need to be: a voxel can only
reach the residency edge on a tick the CA ran, and the CA running already means
the world was not proved idle.

The `gas-leave` gate keeps counting ticks that fall in the window and printing
the number on its own line. It measured **0 of 400 before the fix** — that
fixture's own plume keeps the window busy for the whole run, so it never
entered the hazard — and it should now be provably zero rather than
incidentally zero. It stays because the assertion is cheap and because the next
GPU-side source to arrive will need exactly this measurement made for it.

**Gates:** `gas-leave` (4,096 smoke six chunks under the window's top face,
open shaft above, 400 ticks twice) and `gas-reenter` (256 CPU-queued parcels two
cells outside an X face into an inward wind). The first never asserts the quiet
top plane alone — the same run must show that smoke reached the face and left
through it, or a broken fixture passes too.

### Far fire plumes: the smoke of fires the window has left behind (2026-09-15; `sim_gas.wgsl` `gasFarPlume`, `src/sim/farplumes.h`)

**The gap.** A chunk evicted mid-burn is FROZEN. Its `ember` / `lava` /
burning-foliage voxels were downsampled into the far cascade by the last
`fardown`, so the fire stays visibly orange at cascade distance for the rest of
the session — that is the cascade working. Its SMOKE is not, and that is the
hole: a smoke parcel is only ever born at the window face by the running CA
(`gasLeave`), and the parcels already in flight die on smoke's authored
9/1000-per-tick decay, ~3.7 s mean. So within about four seconds of the eviction
a distant forest fire is a silent orange smear with nothing rising off it.
Nothing was wrong; the producer had simply left.

**The shape.** The CPU keeps an index of WHERE the frozen fires are and the GPU
synthesizes their plumes. Both halves are deliberately small:

* `FarPlumes` (`src/sim/farplumes.h`) is fed from the SAME eviction harvest that
  feeds `FarEdits`, at the same four call sites in `stream.cpp` — the one place
  the CPU ever sees an evicted chunk's voxels. It aggregates per gasOuter COLUMN
  FOOTPRINT: a cell is 8 voxels and a chunk is 16, so a burning chunk
  contributes at most FOUR emitters however much of it is alight, each carrying
  the column's hot-voxel count and its topmost hot voxel. What counts as fire is
  data (`hot` tag + emission > 0 + not a gas), latched from the material table
  by `Simulation::UploadTables`; no material id appears anywhere in the feature.
  `fire` itself is CLASS_GAS and `farCellIsSolid` never writes a gas into the
  cascade, so what is actually visible out there is the ember under the flame,
  which is exactly the set the rule selects.
* `Build()` drops every emitter that is back INSIDE the residency window and
  every one outside the density box, then keeps the 256 nearest the window
  centre. The in-window drop is what stops a plume being drawn twice: a resident
  fire is a running fire, the CA makes real smoke voxels for it, and `sim_step`
  already splats those into the same box. No retirement event is needed for
  either direction — the test simply stops passing when the window arrives, and
  starts again when it leaves.
* `gasFarPlume` is one workgroup per emitter, one thread per CELL of height. Each
  thread scatters three puffs into a radius that grows with height, tilted
  downwind by (wind speed / rise speed) and modulated by a hash of (emitter,
  height packet) where the packet index slides down with the tick — so the
  billowing RISES through the column at the speed a parcel would, with no state
  anywhere.

**It is render-only, structurally.** The only buffer it writes is `gasOuter`,
which the sim never reads, the world hash never covers, and which is rebuilt
from scratch every tick. It does NOT queue parcels: the parcel pool is
deterministic sim state pinned by `kGasSpDigest`, and a parcel that drifts back
in writes a hashed voxel — so a frozen fire able to spawn parcels would be a
frozen fire able to move the world. There is no path from the index to
`QueueGasSpawns`. The `gas-farplume` gate asserts this as a DIFFERENTIAL rather
than against a pinned number: the same fixture with and without the emitter list
must produce an identical per-tick world-hash series.

**Cost (rule 2).** A frozen fire deliberately does NOT arm `C_GAS`. Arming it
would record all five parcel rows, and `gasSpawnStep` alone is a fixed
1,042-workgroup dispatch over both spawn lists whether anything is in them or
not — a distant fire burning for an hour would pay that every tick for a
population of zero. So the splat has its own condition (`C_GASFAR`,
`gasFarEmitCount > 0`) and the density box's per-tick CLEAR moved to their union
(`C_GASOUT`). That union is load-bearing, not tidy: the clear used to be on
`C_GAS`, and with parcels off and emitters live the box would never be cleared
while the splat kept adding to it — a plume that accumulates forever. The
emitter list is uploaded only on the ticks it CHANGES (an eviction, a store hit,
a window move), so a world nobody has set alight writes no bytes and records no
rows, exactly as before.

**The anti-carry bound is a proof, not a margin.** `gasOuter` packs two 16-bit
cells per word, so an add that overflows its half carries into the neighbour's
— a bright cell one over, on some runs only. The parcel splat gets away with a
load-then-add because it adds exactly 1; this one adds a weight, so the guard is
sized: within one emitter no two threads can collide (a thread owns one cell of
height and the puffs only move in x/z), so the worst reachable value after a
race is `FAR_PLUME_CEIL + (kGasFarEmitMax * PUFFS - 1) * ADD_MAX`, and a
`const_assert` in `sim_gas.wgsl` is that it still fits 16 bits.

**Knobs:** `render.farPlumeStrength` (0 = exact off: no emitters, no row, no
write) and `render.farPlumeHeight` (metres, clamped to the box).

**Gate:** `gas-farplume`. One chunk of the first hot+emissive+non-gas material in
the table, harvested off the GPU and handed to the index at a coordinate half a
window away — the same words eviction would have handed it, without forcing a
window shift that would move the origin out from under every gate after it
(`far-persist` feeds `FarEdits` the same way and for the same reason). Four
claims, none of which passes alone: density over the fire's own column AND zero
over a control column the same size; an identical world-hash series with the
emitters removed; a downwind lean in a 20 m/s wind; and, with every gas
condition false, a density box that is byte-identical four ticks later — a box
nobody cleared is the direct positive observation that no gas row was recorded,
where asserting zero density would have been satisfied by a row that ran and
added nothing.

**THE LONG-RANGE HALF (the same day; `gasFarPlumeWide`, `gasFarOuter`).** ±51.2 m
is a sliver of what the cascade draws, so a fire 200 m out was still a silent
orange smear. There is a second density box, identical in every respect except
its cell size:

* **128 cells of 64 voxels — 6.4 m cells over ±409.6 m.** The number this is
  chosen FOR is the box edge: `128 << 6 = 8,192` voxels is **exactly far cascade
  level 4's box edge** (`kFarN << (4 + kFarShiftBase)`, which reduces to
  `16 * kWorldN` because the cascade's alignment constant makes
  `kFarN << kFarShiftBase == kWorldN`). Matching a cascade box edge means the
  plume LOD boundary and the terrain LOD boundary are the same distance instead
  of two visible rings, and levels 5..8 sit behind the pinned fog at ~90% and up,
  so there is nothing past it worth a third box. At the far edge a 6.4 m cell
  still subtends ~16 px at 1080p. Same 4 MiB as its sibling.
* **Its origin is FLOORED to the cell** and gasOuter's is not. The window origin
  is a multiple of 16 voxels and this cell is 64, so without the floor the whole
  lattice would slide 1.6 m sideways on every window shift and a settled plume
  would visibly re-quantise as the player walked. It costs up to 48 voxels of
  off-centreness out of ±409.6 m.
* **The two emitter lists are DISJOINT**, split in the max norm at the fine box's
  own half-extent, and they live in two sections of one buffer (the header words
  reserved when the fine half landed now carry the wide count and the wide box's
  shift). A fire is in one list or the other, never both — so "the two boxes
  cannot double-brighten" is a property of the DATA rather than of a blend weight
  the renderer has to get right, and the raymarch needs no crossfade at all: the
  coarse segment simply starts where the fine box's ends.
* **The wide list is aggregated AGAIN, per coarse column.** A 6.4 m cell holds up
  to 8x8 fine columns in x/z and four chunks in y, so without it one burning
  hillside would spend the whole 256-emitter budget on a patch 64 voxels across.
  Aggregating also buys information the fine list does not have: the wide
  emitter's strength is the SUM of its fine columns', so the kernel reads TWO
  things out of it — density saturating at one full column (past that a fire is
  not more opaque, it is bigger) and a height multiplier of `sqrt(columns)`
  capped at 4. A burning tree is a wisp; a burning hillside is a column four
  times as tall and proportionally wide.
* **The shape is shared, and carried in METRES.** `plumeSplat` is one function
  both kernels call; the radius and the height are metres divided by the box's
  cell size at the point of use, because a radius in CELLS would make the same
  fire eight times wider in the coarse box than in the fine one.
* **The raymarch cost is a third segment of `gasOuterFill`**, 16 steps, gated on
  its own `RenderParams` bit 5 rather than sharing bit 3 — a campfire ten metres
  away arms bit 3 every frame and must not also put a second 4 MiB volume walk on
  every terrain pixel. The samples fold into the SAME accumulator with **no scale
  factor**: a cell's count over 512 is a volume FRACTION, a dimensionless number,
  so `count * dt` is voxel-lengths of gas at either scale. Measured with
  `--shader-stats`, before and after: the raymarch fragment stays at **128
  registers, 160 bytes of local memory, pressure 160** — byte-identical spill —
  and the binary grows 1,631,232 -> 1,632,512 (+0.08%). It was the only one of 97
  executables to move at all.
* **Cost:** one condition, `C_GASWIDE`, gates the wide splat AND the wide box's
  clear — this box has exactly one writer, so "whoever writes it" and "the clear"
  are the same predicate and there is nothing to union. `render.farPlumeRange` 0
  empties the wide list CPU-side, and then no row is recorded, the 4 MiB box is
  never cleared, the flag stays down and the coarse segment is not walked.

**Gate:** `gas-farplume2`, the same fixture 204.8 m out. Its four claims are the
near gate's, plus the one that is specific to having two boxes: the NEAR box must
read exactly zero over the same column and the fine emitter count must be zero
while the wide one is not — the disjointness asserted rather than assumed,
without which a bug that put every emitter in both lists would pass the density
claim. Its first run failed on an assertion the GATE had wrong, and the failure
is worth recording: the no-emitter arm's box is NOT empty, because with no wide
emitter there is no row and therefore no clear, so it still holds the previous
arm's plume. Requiring `== 0` there would have required the clear to run in a
world where the whole feature is off, which is exactly the per-tick cost rule 2
forbids. What it asserts instead is that nothing arms, nothing is recorded and
the box is untouched.

**What it does not do.** Past ±409.6 m a fire is still silent, and deliberately:
that band is behind ~90% fog. The handover between the two boxes is a STRICT
switch rather than a crossfade, so a fire crossing 51.2 m as the window walks
changes plume representation in one frame — bounded (both plumes are the same
physical column, the cells differ) and the same class of transition the cascade
itself makes at a level boundary. If it proves visible, the fix is a per-emitter
weight so a shell of distance feeds both lists with complementary strengths; the
record is full at four words, so that costs a fifth.

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

- **`burnTint` (2026-09-03):** `"burnTint": true` on an emissive material says
  its `colors` are what the voxel looked like *before* it caught, and the
  renderer supplies the fire: **every** site that turns a material's emission
  into light goes through `burnTint()` in `common.wgsl`, which breathes each
  cell between that palette and `render.burnTintColor` on a slow per-cell phase
  (`burnTintRate/Min/Max`) and scales the emission by the same weight, so the
  leaf end of the breath is lit like a leaf rather than glowing green. Sets
  `kMatFlagBurnTint`; render-only.

  **"Every" is eight sites in five shaders, not the four grid ones.** Besides
  the primary hit, the far cascade and secondary rays in `raymarch.wgsl`, a
  voxel's emission also becomes light in `debris.wgsl` (`vsBody` and
  `vsParticle` — the crown that falls off a burning tree is a rigid body, not a
  grid cell), in `microbody.wgsl`, and in the two writers of the irradiance
  grid: `sim_openness.wgsl`'s walk and `shadow_resolve.wgsl`'s deposit. The
  last pair is the sharpest case — they write the *same word*, so they must
  agree, and both deposit `burnTintMean()` because that grid is an EMA over
  frames and would otherwise beat with the pulse. The first version covered
  four of the eight and every one of the other four showed as green glow the
  same day (owner report 2026-09-03: burning leaves *glowing and pulsing
  green*). The raster paths key the breath on their per-voxel flicker hash
  (`burnTintWeightH`) rather than a world cell, because a tumbling piece's
  phase would otherwise slide as it fell. `check_burn_tint_sites` in
  `scripts/check_invariants.py` refuses a raw emission-to-light conversion
  outside a `burnTint()` call, with a named allowlist for the sites that
  legitimately read emission for something else (lava's molten crust, the gas
  media march, and the helpers that take an already-tinted value).
  This is why there are three burning-leaf materials (`leaf_burning`,
  `pine_burning`, `autumn_burning`, one per foliage family's palette): the
  voxel word has no bits to remember which leaf a burning one was, so the
  material id is the memory. Same rules and numbers in all three.

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

- **Per-member exception on a tag rule (2026-09-03):** a pair rule whose
  neighbour is a `tag:` may carry `"neighborChance": { "<material>": <mul> }`,
  a different chance for a named member of the tag:
  ```json
  { "self": "wood", "neighbor": "tag:hot", "chance": 12, "selfBecomes": "ember",
    "neighborChance": { "fire": 0.125 } }
  ```
  Any hot neighbour ignites wood at 12‰, except that `fire` — the free-floating
  flame gas, the thing that rises off every burning voxel — does it at an
  eighth of that. The GPU matches a tag rule with one mask test and a mask
  cannot say "hot but not fire", so the loader (`ExpandNeighborChance`,
  `sim/materials.cpp`) compiles the field away into two ordinary rules: the
  base rule re-pointed at a **synthetic tag** (`hot-fire`, set on every hot
  material except the named ones — in `MaterialGpu.tagMask` only, never in the
  authored tag strings, so `tagBit("hot")` in `mob.cpp` still resolves to the
  authored bit) and one exact-neighbour rule per name at the scaled chance,
  appended at the END of the bucket — the kernel rolls each rule as
  `hash3(rnd, ruleIndexInBucket, slot)`, so an insertion mid-bucket would
  re-roll every authored rule after it (measured: fluid-react's ledger gap
  1.37% → 4.47% from the shift alone). Both evaluators (`sim_step.wgsl`,
  `sim/reactcpu.h`) get the split for free. Only ignition rules carry it: the
  steam / melt / mite rules and the skin sear on `tag:hot` are untouched, and
  flesh's minCount-3 ignition is a decay COUNT with no per-material weight.
  Gate `weak-flame` reads the compiled table and asserts the split.

  The cost, accepted by the owner: the flame a burning voxel emits downward is
  fire too and was fire's only path across an air gap, so `fire-down`'s
  air-gap arm fell from saturating to ~9% and the gate is carried as
  known-failing. A canopy with air between its layers burns its top.

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

Every octave is a **centred deviation** (`n - 8192`), so `terrain.baseHeight`
(the map's; P-G) is the world's *mean* height rather than its floor and there is as much room
below the datum for sea basins as above it for mountains. Measured over a 3,072
voxel transect from the origin: **y-351 .. y+607**, i.e. ~96 m of relief where
the pre-overhaul world had 5.4 m.

**Derivative attenuation is a rule-2 mechanism, not a look knob.** Each octave
is divided by `1 + fbmAtten·|g|²` against the gradient accumulated from the
octaves coarser than it (iq's trick). Without it the ladder sums five 0.5 slopes
into 2.5 and puts *the entire world* above the angle of repose, where nothing
loose can ever come to rest. With it the field saturates near slope 1.2 on
ridges and goes genuinely flat in valleys.

**The calm home area** fades only the two coarse octaves toward the **spawn
site** (`map.json sites[]`, kind `spawn`; `spawnCentre()` in both mirrors reads
it from the map header's `WM_H_SPAWN_X/Z`), over the map's
`terrain.homeArea.fade` past a Chebyshev radius of `terrain.homeArea.radius`
(P-G; they were `worldgen.spawnPlainFade/R`). Fading the whole deviation would pin spawn
to a mathematically exact plane 64 m across — which is not "calm", it is a
dinner plate, and it would make the `terrain` gate's per-voxel pass a test of a
constant. The fade *width* is load-bearing: a ramp of magnitude A over width W
adds slope up to 1.5·A/W, so squeezing 640 voxels of coarse relief into a
300-voxel fade builds a cliff at exactly the boundary. Pass A4 measures it;
read that number rather than guessing it. **Two centres** (environment truth
P-C, 2026-09-04): the harness pad box gets the same fade measured from its
*edge* (`harnessOutside()`, 0 anywhere inside the box) and the calmer of the
two wins — kind `pad` is not in the site table, so `sitePadAt` never levels
it, and its flatness only ever came from this fade. The fixtures were written
against that ground; moving the spawn out of the pad must not move the pad's
ground with it.

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

`terrain.sedSlope = 0` (map.json, P-G) turns the wedge off. It is a map word
rather than a tuning row now, so `--sweep` cannot reach it; the `terrain`
gate's C1 (the CPU twin against the GPU per voxel) is the proof the words the
map carries are the words the kernel reads.

##### A loose cover is born at rest too (2026-09-09)

The sediment wedge's slope gate was the only one. The desert / ocean **sand
cap** was four voxels of POWDER laid on ground of any steepness, and those two
biomes also author their *skin* as `sand`, where the skin branch had always
assumed a solid ("a powder shell on a slope avalanches out from under itself").

Measured on raw worldgen output (`--voxdump`, no CA), in a 128x96x128 box at the
authored lake: **1,249 of 10,539 sand cells had a legal CA down-move on tick 0,
and 418 of them went straight into the water.** The water itself is born
perfectly at rest -- every cell full, flat top, solid floor, zero violations --
so the reported bug (freshly streamed lake terrain stays awake for seconds) was
never the water. It was the bank falling into it.

`looseCoverDepth` splits a cover the way the wedge is split: the LOOSE part
keeps its authored depth on ground the wedge already calls flat
(`terrain.sedSlope`), tapers to zero at the CA's own angle of repose
(`CAP_REPOSE_Q8 = 256`, one voxel per column -- not a knob, it is the constant
`sim_step`'s diagonal slide defines), and whatever the taper takes away becomes
the biome's `cover.firmSkin` (`WM_B_FIRM_COVER`; desert and ocean say
`sandstone`, a material that far-aliases `sand` so it costs no palette slot).
On flat ground the loose depth is the full authored 4 and nothing changes.

Two authored discontinuities force it to zero outright, because **neither is in
the noise field and the analytic gradient reads both as level plateau**: a water
body's bermed / excavated bank (`Col.nearWater`) and an authored pool's rim
annulus (`inRim`). `nearWater` is deliberately *not* `shore.onShore` -- that
flag carries the shore feature's bluff cut, which turns itself off on exactly
the tallest cut walls in the world.

The skin half is gated on the authored class AND on the biome having authored a
`firmSkin`. The opt-in is the author's veto: tundra's skin is snow, also a
powder, and firming a tenth of the tundra broke its own authored claim ("99% of
columns wear snow at `y == h`", the `env-truth` gate) for a settle transient
tundra has always accepted.

**Known gap, not fixed here.** This gate reads `Land.slope`, the *landform*
gradient, which by design excludes the detail and grain octaves (see the note
above -- gating on the full gradient turns the wedge into a cliff). So the
+-2-voxel steps fine noise puts on an otherwise gentle dune face are invisible
to it: the open dune field still measured **645 movers before and after**. The
lake case is fixed; the dune case needs a different instrument (a local step
test, not a gradient), and the tuning that did catch it by brute force also
stripped 87% of the desert's loose sand.

`bowlSteep` carries a second known gap of the same shape, documented at its
definition: it compares against the next integer radius *ring* rather than the
four axis neighbour columns, and under-reads the step where the profile crosses
two voxels between rings (168 bed grains on one marsh bowl face). The exact fix
is written and works, but it moves the harness pool's bed enough to need the
`waterbody` gate's conservation ledger reconciled with it.

##### Per-biome height curves (2026-09-01, Lin 13.3.3)

"This biome is flat plains, that one is jagged mountains", authored as nine
numbers per biome instead of a hand-tuned noise ladder. `biomeCurve` reshapes
**the coarse sum only** — `o0.dev + o1.dev`, the continental and range rungs
that decide where the mountains and the basins are — and leaves hill, detail and
grain alone. A biome changes the *landform*; it never changes the texture on it.
That is the same split `Land.slope` already makes for the sediment wedge.

* **Nine knots, not eight.** Eight knots is *seven* intervals, so the identity
  curve's values are `-16384 + i·32768/7` — not integers, so an identity curve
  could not be authored at all and "the default moves nothing" would be
  unprovable. Nine knots is eight intervals and the identity is
  `-16384 + i·4096` exactly, which is what a biome that authors nothing
  carries (`assets/biomes/<name>.json` `terrain.curve`, P-G).
* **The domain is half what it looks like.** `octave` returns
  `((n − 8192)·amp) >> 14` and `n − 8192` is ±8192, so one rung spans ±amp/2 and
  the coarse pair spans ±`(landformRangeVox + rangeAmplitude)/2`. Authoring
  against the full sum would leave the outer knots unreachable at every seed.
* **The identity is bit-exact, by three separate pieces of arithmetic.** (1) The
  Hermite basis is summed *before* the shift: `h00+h01` is exactly 4096 and
  `h10+h11+h01` is exactly `t` in integers, whatever the rounding of t² and t³,
  so a straight line evaluates to `k0 + t` with no residue. (2) The result is
  applied as a **delta against the identity**, whose value at the same parameter
  is exactly `p − 16384`, so the Q14→voxel round trip (a floor, which would bias
  every column down by one) never happens for an identity curve. (3) `dv/dp` is
  exactly 4096 for the identity, so the Q8 gradient scale is exactly 256 and
  `(g·256) >> 8 == g`.
* **The gradient is scaled, not just the value.** iq's attenuation divides each
  finer rung by `1 + fbmAtten·|g|²`; if `g` still described the pre-curve ladder
  then a biome that flattened its landform would keep attenuating its hills as
  though the mountains were still there. `cv.y` is the curve's own slope in Q8
  and it multiplies the accumulated gradient.
* **Interpolation is Fritsch–Carlson** (the harmonic mean of the two secants,
  zero at a local extremum), so an authored plateau is flat and an authored ramp
  has no crease. Catmull-Rom would overshoot both, and an overshoot here is a
  hill nobody put there.
* **`CURVE_IDENT_ALL` is a module const**, so with the default knots Tint folds
  the whole feature — including its two extra `vnoise2d` samples — out of the
  shader. A world that does not use a curve pays nothing for it.

###### Where the curve lives now (environment truth P-G, 2026-09-07)

The curve is **the biome file's** (`assets/biomes/<name>.json` `terrain.curve`,
nine Q14 knots) beside three Q8 multipliers on the map's hill / detail / grain
octaves (`terrain.hill/detail/grain`, 256 = the map's amplitude), packed into
the biome record (`worldmap.h kB_CurveKnot0..kB_GrainMul`) and read by the
height mirror on both sides. The 36 `worldgen.curve*` knobs, `biomeBlend`,
`biomeLog2` and the three thresholds are gone.

**It blends on the map's own lattice.** A column reads the FOUR map cells
around it — the same cell-centred bilinear `mapLandformQ8` uses — and mixes
the knots (`biomeMixAt` / `curveKnotAt`) before the Hermite, so two biomes'
curves crossfade over a whole 102 m cell and never meet on a cliff. That
retires the old "±18 band units of crossfade" ceiling: the ramp is a cell
wide by construction, and a 35-voxel knot delta is 0.03 voxels per column.
Four equal values mix to that value exactly (`a + ((b − a)·f) >> l` adds
nothing when `b == a`), which is the fourth piece of the identity's
bit-exactness.

**Nothing folds at compile time any more.** `CURVE_IDENT_ALL` and the
select chain over 36 constants went with the knobs; the knots are table
reads, so an authored curve is a file edit and an F7, never a shader
recompile — and never the fifteen-minute driver compile the constant knots
produced the first time a non-identity set was tried. The identity ships
in every biome because the fixture gates were written against the map's
relief; a biome's curve is now the author's to change without a global
default moving under `armor-react`.

##### The terrain is the map's (environment truth P-G, 2026-09-07)

Every number that shaped the ground used to be a `worldgen.*` row in
`tuning.json`. It is **`map.json terrain`** now, per map, packed into the
worldMap buffer header (`worldmap.h kHTerrain*`, words 32..53) and read by
name inside the height mirror on both sides (`wmTerrain(WM_H_TERRAIN_*)`,
token-identical in `worldgen.wgsl` and `world.cpp`): the datum, what a
painted landform 0..255 spans (`landformRangeVox`, the old `contAmplitude`
— a painted ridge can be a 150 m mountain by raising it), the four seeded
octaves (range / hill / detail / grain, amplitude + log2 cell), `fbmAtten`,
the calm home area (`homeArea.y/radius/fade`), the sediment wedge, the
treeline and the reference scale. `TerrainWords` is the one packing; the
loader keeps the same words on `WorldMapData` for the CPU twin, and the
`terrain` gate's C1 is the per-voxel proof. Lengths are rescaled from
`refVoxelsPerMetre` at load exactly as `LoadTuning` used to rescale the rows.
There is no continental noise octave: the painted landform plane IS the
continental rung.

**A landform can be declared, not only painted.** `map.json sites[]` takes
`{kind: "landform", shape: peak | ridge | basin | plateau, at, radius,
heightVox, rotation?}` — "there is always a mountain to the east". The loader
overlays it onto the packed landform plane (`OverlayLandformSites`: a cone,
an elongated cone at a heading, a sunk cone, a flat top ramped out, in
landform units of `landformRangeVox / 256` voxels each), so the shader reads
the plane exactly as before and nothing new enters the mirror. Tier A: no
seed anywhere near it. The shipped map declares `east_range`.

**The ground flora is rows.** The shader's hard-coded undergrowth / flower
chain (mushrooms under crowns, brambles, moss, saplings, litter; flowers, tall
grass and edge litter in the gaps) and the alpine-cushion block are cover
rows in the biome files with a **canopy condition** (`canopyMin/Max`, 0 open
sky .. 255 deep shade, `worldmap.h kC_CanopyMin/Max`) and a `minY` at the
treeline for the cushion. `undergrowthSite`'s 25-tile scan runs once per
column, only for a biome whose rows bound the canopy (`kBF_CanopyRows`),
handed into `genCellIn` as the memo the flower stalk used to be. The cover
stack has no treeline gate any more: a row's `maxY` is its own snowline (the
seeded rows stop at 227). The `env-truth` gate therefore asserts every
biome's rows; the tile plants (`plantColumnAt`'s ferns and big toadstools)
and the proc cactus are the two second sources left, reported not asserted,
and a canopy-conditioned row is an interval (hi only) like a nearWater row.
The stale species height caps (`placement.maxY` 20–23 m, authored for a
y32..y86 world and sitting on the 20 m home area) are lifted and the atlas
re-baked.

**What is left in tuning.json:** `world.mapLayer` / `world.editLayer` (which
files the game loads; the map page's selectors) and the two plant dev
switches, `debug.vegetation` and `debug.groundCover` (Tuning → Dev switches).
They are one axis, not two features: `groundCover` off removes every plant a
body walks through (biome cover rows, tile plants, shore rows, pond life, wet
moss, cave flora) and leaves the trees and cacti standing; `vegetation` off is
the extreme end and takes those too. The shader ANDs them (`GROUND_COVER` in
worldgen.wgsl), so there is no fourth state. The Worldgen tab is gone (P-I):
Environment → World map is the one front door — map + edit layer, the
Terrain section, the Sites panel (pad, spawn, water, landform, stamp:
select / rename / delete), the heightmap backdrop, and the heightmap + voxel
views re-homed as a preview pane.

**Tree sizes are metre-true again.** `VOX_PER_M` in `worldgen.wgsl` was a
hardcoded 16 — correct when a voxel was 6.25 cm, and left behind when `world.h`
moved to `kVoxelMeters = 0.10`. For that whole interval every tree in the game
was 1.6× the metre size its own table documents (the "11.9 m" great oak was 19 m
of trunk). It reads `VOXELS_PER_M` from the prelude now, so the table's metres
are true at any voxel size. (Trees themselves no longer read it at all — their
sizes are metres in `assets/trees/*.json` and voxels in the baked atlas. The
cacti still do.)

##### Trees: a BAKED ATLAS, and one voxelizer (2026-08-29)

> **The editor is the only voxelizer. `assets/editor/treegen.js` produces every
> tree voxel in the game; the engine samples what it baked and knows nothing
> about how a tree is built.**

Worldgen is a pure per-cell function: `genChunk` answers for one voxel with no
memory of its neighbours and no way to walk a turtle. So every tree the engine
grew was an IMPLICIT SHAPE re-derived per cell — a hash-eroded ellipsoid for
oak, a diamond cone for pine, a hand-unrolled five-limb skeleton for birch.
That is the ceiling of the technique and it is why the forest read as lollipops.

The pipeline now is:

| stage | artefact | owner |
|---|---|---|
| author | `assets/trees/<species>.json` | the tuner's **Trees** tab |
| voxelize | in memory | `assets/editor/treegen.js` |
| bake | `assets/trees/<species>.svtree` | `scripts/bake_trees.mjs`, or the tab's Bake button |
| load | one GPU storage buffer | `src/sim/treeatlas.{h,cpp}` |
| place | voxels | `worldgen.wgsl` `treeCandsInto` / `treeCellFrom` |

The generator is Weber & Penn 1995 reduced to the parameters that earn a
slider: a recursive stem skeleton under a crown-envelope curve (`shape`),
stamped as round-cone SDFs, with smooth-min'd ellipsoid leaf CLUMPS at the
outer stems. The clumps are the visual thesis — foliage lives in lobes around
branch tips, never in one canopy-sized ball — and a **shading bake** (a mix of
the clump-sphere normal and the whole-canopy normal, plus depth into the clump)
gives those lobes readable form at zero engine cost, because it resolves to a
choice among a three-material shade ramp (`leaves_dark` / `leaves` /
`leaves_lit`, and the bark and needle equivalents).

Four consequences worth stating, because each one is a rule:

- **No second implementation.** There is deliberately no C++ or WGSL copy of
  the SDF/clump/shading logic. What the Trees tab shows is byte-for-byte what
  worldgen places, because it is the same function.
- **Nothing is added to a tree after the bake.** The atlas is the WHOLE tree.
  The cutover initially kept worldgen's implicit decoration — `treeVineFrom`,
  a closed-form predicate that draped vine curtains and Spanish-moss beards
  from the canopy underside and spiralled ivy ropes up the bole — and that
  quietly broke the bullet above: the tab showed a tree, the world showed a
  tree wearing something the author never saw and could not preview. It is
  gone (2026-08-30), along with its nine `worldgen.*` tuning rows. A
  decoration that belongs on a tree is authored in `treegen.js`, where it is
  visible while it is being made. The `vine_hang` / `creeper_flower` /
  `moss_hang` MATERIALS still exist for the brush; nothing generates them.
  Wall ivy on the arena and ruins is unrelated and unchanged.
- **Editing a species moves the world hash.** The atlas is engine input exactly
  like `tuning.json`. Re-bake, then one `--selftest --rebaseline`. The
  `tree-atlas` gate pins the atlas bytes separately, so the hash diff has a
  cause attached.
- **Authored by name, resolved at load.** A `.svtree` carries a material NAME
  table and its runs hold local palette indices; the loader maps them to engine
  ids. Renumbering `materials.json` cannot silently recolour a forest.
- **Placement rides in the species file.** Biome weights, a per-species
  altitude band and treeline, a slope gate and the canopy shade the forest
  floor reads all live in `<species>.json` and are lifted into the atlas
  directory. Adding a tree touches one file.

The per-cell cost went DOWN: a candidate is now dropped at hoist time unless the
tree's baked grid has something in this exact column, so `treeFromCands`
typically runs zero or one iterations, and each one is a short scan of a
column-RLE run list rather than fifteen segment-distance tests.

**The clump primitive is a knob, not a constant** (2026-08-30). A crown built
out of one shape can only ever be a pile of that shape, which is what made every
early crown here read as a bunch of grapes. `foliage.clumpShape` picks among
four — blob, plate (squashed along its axis: the tiered spray of a cedar),
spray (stretched along it and thin across: a leafy shoot, not a ball), and cone
(a spruce sprig) — and `foliage.clumpAxis` decides whether that axis is world-up
or the twig the lobe grows on. All four are the same spheroid field measured in
the clump's own frame, so they cost the same, they smooth-min to each other, and
a crown may mix them. `foliage.hollow` eats the core out of every lobe: a real
crown is a surface, and on an oak 0.7 is visually identical from outside for
**half** the leaf voxels (54,771 → 26,354).

Defaults are `clumpShape 0 / clumpAxis 0 / hollow 0`, and the field pass keeps a
separate loop for exactly that case. Not laziness — the general loop computes
the same spheroid but *reassociates* the arithmetic, and a float that
reassociates moves a voxel, which moves the atlas, which moves the world hash.
An opt-in knob must not re-bake ten species.

The tab grew a **quad view** to go with it: four seeds at once, one per
quadrant, under one orbit camera, because a shape knob is judged on a stand and
not on a specimen. It is four WorldView regions at four origins rather than one
composited grid — a 2x2 of great oaks in a single array is 32M cells of which
the trees are an eighth, and both the array and its 3D texture would pay for the
empty seven. Save, Bake and Export still act on the first tree.

##### The height contract (2026-08-26)

> `World::TerrainHeight(x, z, seed)` ≡ `genColumn(x, z, seed).h`, **exactly**, for
> all inputs.

The CPU mirror is not "roughly the terrain" and it is not "the topmost solid
voxel". It is the **ground**: the terrain octaves, the authored pool floors and
rims, the pond bowl carve, the tarn berm, and the **ruin pad** — everything
`landColumn()` in `worldgen.wgsl` applies to `h`, in that order. Literal
topmost-solid would
include canopy, ruin walls, grass tufts and the arena deck, and it cannot be
mirrored cheaply (it needs `treeAt`'s tile scan in a tick path); every one of
TerrainHeight's ~30 callers is asking where the ground is so it can stand
something on it.

**The pond half of the contract reads a table since P-F** (environment truth,
2026-09-06). `pondInfo` / `bowlDepth` / `bermLift` / `pondNear` stay inside
`MIRROR-BEGIN height` and token-identical on both sides, but every number
they use is a water PRESET's (`assets/water/<name>.json`, packed by
`worldmap::WaterGeomOf` into the worldMap buffer's `kW_*` record and kept on
`WorldMapData::water` for the twin) read through accessors spelled the same
in both files — `wmWaterI`, `waterKnot`, `wmWaterRow`, `pondTile`,
`pondBand`. WHICH preset a column's pond wears comes from two sources through
one `Pond`: the biome's `water.features[]` rows rolled on the one pond
lattice (`kHPondTile`, the finest water tile of any biome, thinned per row
like the trees), or a `kind: "water"` site on the map (`waterSiteAt`,
found per column through the site index plane; its keep-out is its disc plus
its band, never its cells, and `sitePadAt` skips it). The bowl is the
preset's sampled profile, linear in d² between knots so the shader needs no
sqrt; a face steeper than a voxel per column wears the preset's substrate
instead of its powder bed (`bowlSteep`), which is what let the depth stop
being bounded by the radius. The `terrain` gate's C1 is still the per-voxel
proof; the harness tarn at (420,420) is still the authored pool below, not
a site.

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
9,409 columns of pristine procgen. The cost is ~25 `hash3` per call — and five
times that on the ~1.5% of columns inside a ruin's pad margin, which sample four
corner columns as well — which is fine at O(1) per frame (spawn placement,
fixture anchoring, a mob ground probe) and is forbidden in a per-voxel loop: the
GPU has `genColumn` for that, hoisted once per column.

###### Ruin pads: the ground yields to the building (2026-09-01, Lin 13.3.2)

A ruin used to be stamped in the **cell** half at `baseHeight(centre)` — the raw
octave ladder, missing the pond bowl, the pool floors, the berm and the wedge —
with no gate on how steep its ground was. On a hillside that floated one wall a
metre in the air and buried the opposite one; beside a tarn it put the floor
under the water table. The site decision moves into `landColumn`, next to the
ponds, and splits in three:

* `ruinTileAt` — **one hash3**, the tile's jittered footprint, no height at all.
  Cheap enough for the tree and ground-cover rules to ask "is this column a ruin
  floor?" per candidate.
* `ruinPad` — four `landColumnBare` heights at the footprint corners, **after**
  pond and pool composition. Pad height is their **median** (mean of the two
  middle values, floored by an arithmetic shift so both sides agree on
  negatives). Refused if the spread exceeds `worldgen.ruinMaxSlope`, or if any
  corner stands in water. Four corners are a *complete* pond test, not a sample:
  the footprint's half-diagonal is 39 voxels and `pondRadiusMin` is 48, so a
  disc overlapping the footprint must contain a corner.
* `landColumn` — flattens the footprint to the pad, zeroes the sediment wedge
  there, and ramps back to the terrain over `worldgen.ruinPadMargin` columns.

The two knobs are a **pair**: the apron's own column-to-column step is about
`ruinMaxSlope / (2·ruinPadMargin)` and the angle of repose is 1 voxel per
column, so `ruinMaxSlope` must stay under twice the margin. `landColumnBare` is
the split that makes the corner sampling possible — a `landColumn` that called
itself would not be a function.

A **refused** site still reads as a clearing, because `ruinFloorAt` is the cheap
predicate and does not know about the refusal. That is the choice: an old
foundation with nothing standing on it, rather than a hillside carrying a bald
5.6 m square for no reason.

###### Openness placement (2026-09-01, Lin 13.3.4)

"Place plants by how open the sky is" — but worldgen has **no sun direction**,
and above ground the only overhangs are trees, whose canopy cover already drives
`undergrowthSite`. So the feature is what was actually missing, in three parts,
and each one is a **closed form** — no column march, no neighbour voxel read:

* **A ruin floor is swept.** `Col.ruinFloor` (one `hash3` per column) takes the
  footprint out of the tall light-loving layer entirely and gives it the shade
  set's two lowest members instead — moss and leaf litter. It also rejects tree
  trunks in `treeInfoAt`. Without it the stalk pass grows flower stems through
  the walls, and a hut standing in hip-high meadow reads as a decal on a field.
* **Cave flora, from `caveBands`.** The bands give a cavern's floor and ceiling
  per column in closed form, so "standing on the floor" and "hanging from the
  ceiling" are comparisons. Mushrooms go on the **shallow** band's floor (the
  caverns you walk into from a hillside); a new emissive `crystal` material goes
  on the **deep** band's floor and ceiling, in seams shaped by a patch mask,
  never within `CAVE_LAVA_MARGIN` of the magma table. Both are inert, so a
  cavern full of them still generates at rest.
  <br>**The band is carved from `f1` UPWARD**, so `f1` is the lowest *air* cell
  and the stone under it is `f1-1`. The plan's `f1+1` would have floated every
  mushroom one voxel above its own floor — the same bug as the ruin wall.
* **Moss on a shaded face** of a ruin wall, chosen by `worldgen.mossFace`
  (0 = −Z, default). Worldgen has no compass, so this is a **convention**, not a
  measurement, and it says so rather than pretending to derive one. It is a skin
  swap on a wall cell that already exists — `ruinShellAt` re-evaluated at the
  neighbour, the same predicate trick the ivy pass uses — and the material is
  `wet_moss`, **not** the ground `moss_patch`: `moss_patch` is `passable`, and
  swapping a wall cell for a passable material punches a walkable hole through
  the building.

**Trunks are out of scope, and the reason is architectural rather than
budgetary.** A trunk's −Z neighbour lives in a *different column's*
`TreeCands`, which costs the 25-tile scan; and decorating a baked tree from
worldgen is the exact divergence the `.svtree` bake exists to end (the tree vine
and hanging-moss knobs were deleted for it). A trunk that wants moss grows it in
`treegen.js`.

There used to be a fourth height function, `surfHeightAt`, which hand-copied
this arithmetic for the far-field skin lookup and had already drifted (it never
took the lab branch). It is gone; `farSurfaceMat` takes the column.

`World::TerrainHeight` reads the same map words as the shader
(`WorldMapData::terrainWords`, the very `TerrainWords()` the packer wrote),
so editing the terrain cannot desync collision from the terrain you can see. `scripts/tuning_prelude.py` supplies the same constants
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
  `flagSupportLoss` sets a per-chunk *support-loss flag* (side-channel buffer,
  never fed back into voxel state) whenever a supporting voxel (solid/powder)
  vacates or transforms next to a solid; the flags ride the async readback, are
  cleared on consume, and become island-check events with a per-chunk cooldown.
  A solid component with powder directly below counts as *resting* (anchored) —
  without that rule every slab on a sand pile would convert to a rigidbody the
  moment a grain shifted.
- **Every removal path raises the flag, not just the CA (2026-09-03):**
  `flagSupportLoss` used to live in `sim_step.wgsl` and be called from it alone,
  so only CA-driven removals were reported. `sim_mutate.wgsl` (brush erase,
  laser melt, and the exact-cell ops that carry island removal, settle-back and
  body-burn escapes) and `sim_explode.wgsl` raised nothing at all — the CPU
  compensated with four hand-rolled `AddDestructionEvent` calls at the brush,
  laser and grenade sites, and anything with no such call (a spell carving a
  pillar) stranded whatever it had been holding up. Since solids never fall in
  the CA, island detection is the ONLY thing that can drop them and this flag is
  the only thing that summons island detection, so a missing call is a permanent
  floater. The function now lives in `common.wgsl` behind a
  `>>>SUPPORT_LOSS_BEGIN<<<` strip block — the same filter the page-table
  accessors use, keyed on whether the shader declares `supportOut`, so it is one
  authoritative copy rather than three (Lin's rule 3). The CPU calls stay: they
  are immediate, while the GPU flag rides a readback with a 45-tick per-chunk
  cooldown and a 2/tick drain, so the flag is a BACKSTOP and not a replacement.
  It also no longer stops at the first solid neighbour it finds — the old early
  `return` flagged one chunk, so a cell vacating between two solids in different
  chunks left the second one's matter hanging. Gate: `support-flag` (an
  all-stone post astride a chunk boundary, one exact-cell erase between two
  solids; the CA cannot move stone, so the two flagged chunks it measures can
  only have come from the mutation path).
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
- **A solid with nothing touching it falls in the CA (2026-09-04).** The one
  exception to the rule below, and the reason it is an exception is that it
  needs no connectivity analysis: a `CLASS_SOLID` voxel with no solid and no
  powder on any of its six faces **is** a component of one, decided from
  information the cell already holds. `sim_step.wgsl` drops it like a powder —
  no support flag, no cooldown, no readback, no queue, no region scan. Island
  detection keeps the job it is actually good at (is this *ledge* still attached
  to that cliff) and stops being asked to arrive somewhere for a single voxel,
  which measurably it did not: burning one tree in the `tree-fell` fixture left
  38 lone voxels hanging with every leak counter at zero, the sub-8 rubble
  handoff reached 401 times against 11105 small components anchored at a
  scan-box boundary, and `eventQueueFullSpilled` at 1113. Afterwards, 5.
  Three constraints make it legal rather than merely appealing. **The lattice
  bounds reads, not just writes**: acting cells are ≥3 apart and may write ≤1
  cell away, so a cell one step from me can only be written by an acting cell
  within two steps of me, and I am the only one there — this probe reads exactly
  distance 1, where the `seesSky` walk that broke determinism at tick 1 read 48.
  **Rule 2 survives**: the six faces are probed downward-first, so a terrain
  cell with ground beneath it exits after one extra load, and a failed move
  never calls `markDirty`, so nothing can hold a chunk awake (`sleep` still
  reports 0 / 32768 active). **Things that float on liquid still float**, with
  no new rule: `canDisplace` already refuses a denser target and every
  water-surface plant is far lighter than water. An unseen neighbour counts as
  attached, the same conservative direction `solidOutside` takes at the
  residency edge. Gate: `tree-fell`.
- **Nothing is left hanging by the handoff (2026-09-03).** Otherwise a
  `CLASS_SOLID` voxel does not move in the CA (`sim_step` returns early for it),
  so island detection is the *only* mechanism that makes solid matter fall for
  anything bigger than the single-voxel case above, and there is
  no second line of defence behind it: every component this path declines to
  convert stays exactly where it is, permanently. Four paths used to decline a
  component by leaving it in the grid, and each was a permanent floater:
  - the **grid-op budget** (`cellOps` over `kMaxCellOpsPerTick`) broke out of
    the component loop, but `PreTick` had already popped the event — so the
    comment said "next tick" while the code abandoned the whole tail of the
    scan;
  - the **particle ring** being full made the solid-rubble branch fall through
    to a grid write, stamping a scrap back exactly where its support used to
    be, which the comment immediately above it correctly said not to do;
  - the **oversize-bbox guard** (`>120`, the `int8` `DebrisVoxel` limit leaking
    into world logic) `continue`d, abandoning the component;
  - a **full event queue** made `AddDestructionEvent` return false, and the
    brush / grenade / laser call sites all ignored it.

  All four now DEFER instead of abandoning. The region is re-queued with a
  retry counter (`kMaxEventRetries`) and re-derived from the grid on a later
  tick — simpler and more correct than a resume cursor, because converted
  components have already left the grid, and safe because `lastCellWriteTick_`
  holds the re-queued event until the chunk cache reflects this tick's writes.
  A full event queue spills the region's chunks onto `pendingSupport_`, the
  queue that never drops, so a lost scan became a late one and callers no
  longer need to check the return.
- **Nothing settles into thin air (2026-09-03).** `SettleBodies` gated on
  asleep-long-enough, not-bleeding, near-axis-aligned, fits-window-and-budget —
  and never asked whether anything was underneath. That matters because
  **bodies are not in the grid**: a body resting on another body is resting on
  nothing the world knows about, so it settled, and when the lower body later
  despawned (`PostStep` retires the oldest past `kMaxBodies`, or it streams
  out, or it burns away) the stamped voxels hung there — with no support-loss
  flag ever raised, because no CA cell vacated. `SettleFootprintSupported` now
  walks the snapped footprint and requires grid solid/powder directly beneath
  some voxel that is not the body's own. Unknown chunks are requested and read
  as *no* support — the opposite of the convention in `RunIslandDetection`,
  and deliberately: there, assuming solid means "do not make a body"; here,
  assuming empty means "do not stamp into the world". Both refuse to convert
  matter on a guess. **A refused body is also WOKEN**, and that half is what
  makes the refusal honest: refusing alone only decides the body must not
  become grid, and leaving it asleep over a void is the same floater in a
  different representation — one that also holds a slot in a list capped at
  `kMaxBodies`, so unsettleable bodies accumulate and `PostStep`'s oldest-first
  retirement starts evicting live ones (measured as `wound-accumulate` seeing
  +0 debris bodies from a limb it had just severed). Waking closes the loop
  instead: the body falls, lands, and the next scan finds it supported. A pile
  therefore settles bottom-up. This also fixes the load path for free, since
  `worldio` recreates bodies asleep by design and a world saved mid-fall used
  to stamp itself into the sky.
  `SANDVOX_NO_SETTLE_SUPPORT=1` restores the old behaviour for one run, so
  "did the support test cause this" is an A/B rather than a revert.
- **Attribution, not a bare count.** `DebrisSystem::FloaterProbe` counts every
  decline at the site that makes it, split into leaks (`deferGaveUp`,
  `solidRubbleInPlace`, `stuckEventDropped`), deferrals (the repairs working),
  and why components were judged anchored (a *known* solid outside the scan box
  versus an unfetched chunk assumed solid). Gate: `floaters` — a body asleep
  over a void must not settle, the same body on the ground must still settle
  (the control, without which the first assertion is satisfied by a build that
  settles nothing), and a bounded sweep of the fixture box must find no
  unsupported solid component.
- **The queue was head-of-line blocked (2026-09-04).** `PreTick` examined
  `events_.front()` and nothing else, so a head whose chunks had not arrived did
  nothing for 120 ticks while everything behind it aged toward its own timeout:
  one region that streamed out took four seconds of island detection down with
  it, measured as **312 events dropped over one burning tree**. A bounded prefix
  of the queue is now probed and the ready ones run, with only the first couple
  of probes allowed to request fetches (a deep probe must not flood the fetch
  queue that the events in front of it are waiting on). A stuck event whose
  region is still resident spills to `pendingSupport_` rather than being
  dropped; only one that has genuinely left the window is dropped, and that is
  not a leak because there is nothing there to float. Also `EventReady` now
  requests the one-chunk **ring** around the region — the cells `solidOutside`
  reads and that nothing had ever fetched, so a component touching a scan-box
  face was anchored on a guess — but does not *wait* for it, since a stale ring
  answers "is there rock out there" perfectly well.
- **A solid that turns to powder in place stops holding things up
  (2026-09-04).** `flagSupportLoss` returned early for a powder *product* as if
  it still supported everything, and the ash then flowed away flagging only the
  cell above it — so a clump held sideways or from above by a leaf that burned
  to ash was never flagged and never scanned. Powder carries the cell above it
  and nothing else, and the flag now says so (all solid neighbours except the
  one above). `soloSolid` has the same correction: powder counts as attachment
  only when it is *below*. Both came out of the `tree-fell` natural burn-out
  series, where residue sat flat at ~30 while the hot count fell 3× — stranded,
  not smouldering. And `SettleFootprintSupported` no longer abstains-as-yes on
  an unfetched chunk below (the 12-voxel leaf body at the edge of the fetched
  region was the biggest floater left), and every settle now queues a support
  scan over the stamp, since a stamp vacates nothing and so raises no flag.
- **The flood seeds from what changed and stops at the first anchor
  (2026-09-04).** `RunIslandDetection` used to seed from every solid cell in
  the region, which for a 64³ box on a hillside meant labelling the whole
  terrain slab on every scan to learn that the ground is anchored. A component
  that does not touch the changed box (the erased cells or the flagged chunk,
  plus one cell of slack) did not lose its support *here*, so only solids in
  `Event::seedLo..seedHi` start a flood; and once a component is anchored — by
  the boundary, by powder beneath, by size, or by touching a component already
  judged anchored (the verdict is transitive, which is what makes stopping
  sound) — the rest of its walk is bookkeeping for nothing and is skipped.
  Unanchored components are still flooded to completion, because their cells
  are what gets converted. Measured on one burning tree: **40.2 M → 0.59 M
  cells visited (68×)** with identical verdicts, A/B in one binary via
  `SANDVOX_ISLAND_FULL_FLOOD=1`. The scan budget is now in **cells**
  (`kIslandScanCellsPerTick`), not scans, so the saving turns into scans. A
  narrow-margin first tier that escalated on clipping was tried the same day
  and reverted: near a tree 627 of 714 narrow scans clipped, so it doubled the
  work and the residue rose.
- **What the burn half of `tree-fell` measures now:** after an oak-sized tree
  burns and is quenched, **0 floating components** at +400 ticks and the world
  clean within 100 ticks (the sample stride) — down from 38 lone voxels and 29
  clumps. The residue the owner reported hanging "for a minute or two" was two
  things: matter still smouldering (a burnt crown keeps making floaters for
  thousands of ticks), and the queue above dropping or starving the scans that
  would have cleared them.
- **The scan is sparse and the region is 256 cells (2026-09-12,
  `docs/PLAN_rigidbody_islands.md` §4-§6, built).** `RunIslandDetection` no
  longer builds a dense mask over its region; it floods a visited map keyed by
  world cell, so a scan costs what it walks and the region can be 256 a side
  (`kMaxRegionCells`), enough to judge a redwood whole. Chunks are read from
  the mirror as the flood reaches them; one that is not cached is requested
  and the event re-queues itself (`Event::fetchRetries`, its own ceiling)
  **only if an unanchored component touched it** — anchoring is monotone, so a
  component that is already anchored needs nothing behind an unfetched chunk.
  Readiness (`EventReady`) is held to the event's tick for the SEED box only;
  any cached copy serves elsewhere. Terrain is bounded by two anchors the old
  box supplied by accident: a component reaching `kAnchorDropBelowSeed` (48)
  under the changed box, or `kAnchorReachBesideSeed` (96) beside it while no
  higher than its top, is the ground. The flood descends FIRST (the -y
  neighbour is what the stack pops first), so it reaches that ground in about
  as many steps as the structure is tall instead of wandering a crown. Measured
  on the `tree-fell` fixture: the cut oak is now **one 28,478-voxel body**;
  the burn pass went from 0.59 M cells visited (with the crown wrongly pinned
  at the 80-cell wall) through 41 M (whole-tree re-walks) to 3.4 M with the
  descent-first order and the terrain anchors.
- **Oversize islands are SHARDED, not rejected (§5).** A component is diced on
  a lattice of at most `kShardCells` (96) per axis; each lattice cell's
  6-connected pieces become one rigid body each (an oak is one shard, a
  redwood three), welded along a spanning tree rooted at the heaviest shard
  with `JointType::Fixed` joints, collisions among them disabled, sharing a
  `Body::assembly` id. Slivers under `kMinBodyVoxels` crumble as rubble.
  Shards that fit the tick's op budget are made largest-first; the rest stay
  in the grid and are re-derived by the re-queued event. `SettleBodies`
  settles an assembly **as a unit or not at all**: every shard asleep,
  aligned and unwounded, ground under ANY shard counts for all, one tick's
  ops for the whole set.
- **The debris system reads its own writes through an OVERLAY (2026-09-12,
  gate `cactus-fell`).** A severed saguaro became a body within three ticks
  and then stood in the air for as long as it liked: `ManageTerrain` had
  meshed the collider around it from a mirror copy that still held the
  severed top, and a cached chunk was only re-fetched while the snapshot
  showed it dirty and never inside 8 ticks of the last request — the vacated
  chunk was asleep again before that window opened. The mirror
  (`World::cache_`) is fed only by explicit fetches, not by the player's 3x3x3
  copy, so this held next to the player too. Now every cell this system
  writes (island removal, rubble, settle-back) is recorded per chunk with the
  word written (`NoteGridWrite`, `pendingVacate_`) until the mirror's copy is
  at or past that tick; the flood and the collider occupancy both read the
  mirror through it, and `ManageTerrain` re-fetches such a chunk whatever the
  dirty flags say (`chunkWriteTick_`). A body is therefore never born inside a
  mesh of its own former cells, a re-scan never converts cells a body already
  took, and no event has to wait for a fresh copy of a chunk a fire rewrites
  every tick. Empty (sky) collider builds no longer charge
  `kTerrainBuildsPerTick`. Gates: `cactus-fell` (three arms: the game's own
  fetch path, the brush's CPU door, a stone control), `tree-fell`.

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
- **A corpse keeps burning (2026-09-02):** the scan budget above
  (`kBurnScanPerTick`, 4,096 voxels) was spent in list order to the last
  voxel, so whichever bodies came first took it all and every body after them
  got `scanBudget == 0` and skipped. A corpse is fifteen bodies adopted in limb
  order, ~14k skin voxels on the human: three or four pieces burned and the
  other eleven — torso included — kept the exact ember count they died with,
  for good (`corpse-burn` measured 622→622 alight on the torso over 400 ticks,
  245→245, 333→333, 160→160, 396→396 behind it). On screen: a corpse whose
  embers pulse at the colour it died in and never char, smoke or ash. The live
  creature had the same bug on its limbs (`Mob::BurnTick`, "rotate the start
  limb by tick"); `BurnBodies` now does the same — the start body rotates by
  tick and each scanning body takes at most its SHARE (budget left over
  scanners left, floor `kBurnScanMinShare` = 256), handing the remainder on,
  with the per-body cursor carrying a body larger than its share across
  ticks. Deterministic: order is a function of tick and the body list, rolls
  of the (serial, voxel, tick, rule) key. Bodies burned below body-worthiness
  are erased after the loop, since the rotated order cannot survive a
  mid-loop swap-remove. **Gated self rules do not keep a body awake:** a
  decay/emit rule behind `scaleByNeighbors` cannot fire until something hot
  sits beside the voxel, and `flesh_charred`/`flesh_cooked` own only such
  rules — counted as "self-driven" they made every charred corpse a body the
  pass scanned to its budget every tick for the rest of the session (the
  light-gated-rules-never-sleep trap in another condition). They are tallied
  in `Body::scaledCount` and wake a body only with a dirty chunk nearby,
  exactly as pair rules do; an all-char corpse in a settled world costs two
  field reads. Gate `corpse-burn` (selftest_wound.cpp): the burn-cap bonfire
  until the human dies of it, then 400 ticks alone — every piece that died
  with ≥8 embers must have moved (fewer alight or more spent), the corpse as a
  whole must be retiring fire not growing it, the debris pass must still emit
  real fire, and every piece's BRICK must census the same alight/spent counts
  as its lattice (the lattice burns, the brick is what is drawn). Measured
  after the fix: 6,153→1,870 alight, 3,090→7,866 spent, 6,067 fire ops, 15
  bricks all agreeing. Fixture note: a burning NPC RUNS (32 voxels in 89
  ticks), and from an inset near the window edge it crossed it — no fetch, no
  terrain mesh, the corpse fell 225 voxels into nothing — so this fixture sits
  at the window centre; `DebrisSystem::TerrainCensus` (built / unfetched /
  empty meshes on the last sweep) is the instrument that said so.
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
  - **The crater on a BODY has a shape** (`Mob::CarveLimbRadial`, so the player
    avatar and every NPC share it; the debris path above is unchanged). The
    original rule — an independent coin flip per voxel against `1 - t²` — is
    the wrong shape twice over. `1 - t²` is still 0.36 at 80% of the radius, so
    a large blast really does take a bit of everything it touches; and white
    noise has **no feature size**, so what comes off is a fine speckle no
    falloff curve can turn into a piece. Three mechanisms fix it, all lerped
    from the old behaviour by `gore.carveChunkiness` (0 reproduces it exactly,
    which the `mob` gate asserts with a three-arm 0/1/0 control):
    - **correlated noise** — value noise at `gore.carveBlobSize`, sampled on
      the **skin** lattice so the crater is a property of the art and not of
      whatever collider resolution the engine derived;
    - **a tighter falloff** — `(1 - t²)^gore.carveFalloff`, same endpoints, so
      the crater concentrates rather than shrinking;
    - **spall** (`Mob::CarveSpall`) — `gore.carveSpallRounds` passes that
      remove survivors inside the blast with 3+ missing face-neighbours,
      re-deriving occupancy between rounds. This is the one thing a predicate
      *cannot* express, because it is a question about occupancy rather than
      about a voxel, and `CarveLimb` is the only place occupancy is known. It
      is what makes damage accumulate in one place: a second hit widens the
      first wound instead of stippling fresh flesh beside it.

    All three scale with **severity** — the blast radius against the limb's own
    extent — so a close charge chunks and a distant graze still stipples. The
    spall parameter is optional and only the radial path fills it, which keeps
    the burn flush and the laser's clean kerf out of it by construction.
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
  - **ONE RECORD, ONE HOLDER — and `MicroBodyOwn` is not how a NEW holder gets
    one (2026-09-14).** Own answers "may THIS body edit its brick?", so it is a
    no-op on an already-owned model; that is right for the second hit on a body
    and wrong for a body that INHERITED a model index from another one.
    `ShatterBody` did exactly that (`nb.micro = b.micro`) and reached its "own
    brick" through `ReskinMicro` → `MicroBodyOwn`, so whenever the parent was
    already owned — which is the normal case, since `DamageBody` re-skins after
    its carve and `BurnBodies` takes ownership on its first per-voxel poke —
    every fragment kept the PARENT's record. One `--selftest` pass took that
    branch 29 times. The consequences compound: the fragment's re-skin rewrote
    the parent's payload to the fragment's shape; both teardowns called
    `MicroBodyFree` on one block, and the first zeroed the dims of a brick the
    other was still drawn from (an invisible body); and the freed record went
    back on `freeModels` while live bodies still pointed at it, so the next
    `MicroBodyOwn` ANYWHERE — a different creature's first carve — was handed a
    record with stale holders whose eventual free then took the newcomer's
    brick. That is how a shattering corpse made a living mob's torso vanish
    (owner report 2026-09-14). `MicroBodyClone` is the always-clone sibling and
    is what a new holder calls; `MicroBodyOwn` is now `if (owned) return model;
    else Clone`. The `corpse-burn` gate counts bodies sharing a record and
    fails on any.
  - **`MicroBodyPack` recycles too (2026-09-14).** Pack is not only the loader —
    every carved gobbet and every re-fitted garment shell packs one — and it
    read neither `freeModels` nor `freeList`, so both the record table and the
    pool's high-water mark were one-way ratchets. It now takes a retired
    record and allocates through `PoolAlloc` like everything else. Relatedly,
    `Mob::AddWornShell` set `carved = true` on a resampled shell's brick without
    setting `owned`, and `MicroBodyFree` refuses to reclaim a shared block — so
    every equip/unwear cycle leaked its words, despite the comment there
    claiming otherwise since the day it landed.
  - **THE MODEL TABLE IS SHARED RECORDS PLUS EVERY OWNED CLONE, and it was
    sized against the first half only (2026-09-15).** Owner report: "after
    killing a bunch of zombies, new zombies spawn with 0 gore and won't get any
    when I hit them; it fixes itself when I leave the area with the corpses" —
    which is a precise description of an allocator, not of a rendering bug.
    `kMaxMicroBodyModels` was 256 and its comment named only the SHARED
    population, "one record per (def, limb) pair". That population had grown to
    **148 records at load**: 87 mob limb models against **61 item models**,
    because every garment COVER PANEL is a record and nine peasant garments had
    just landed. 108 records were left for the whole world's damage, and a
    zombie's `rot` block carves ~11 of its 15 limbs AT SPAWN — clones a corpse
    then holds until it is culled. So the seventh zombie exhausted the table,
    `MicroBodyOwn` began refusing for every body in the world at once, and every
    carve, char and blood mark stopped appearing. Walking away culled the
    corpses and handed the records back, which is the whole of "it fixes
    itself". The ceiling is now 1024 — shared plus `kMaxBodySlots`, since an
    owned brick belongs to a drawn body part — and the pool doubled to 2 MiW
    behind it, because the two ceilings have to be sized against the same scene
    or the cheaper one merely moves the wall (148 records already cost 313k of
    the old 1 MiW, and ~350 live clones at ~1.5x their model's payload would
    have hit it next).
    - **The defect was the SILENCE, not the number.** Refusing is the designed
      degradation and it is correct — losing detail under memory pressure beats
      refusing to be destructible — but nothing anywhere said it had fired, so a
      hard ceiling presented as "gore quietly stopped working". Every refusal
      now counts into `MicroBodySet::refusals` and prints at each power of two
      (rate-limited because a full table refuses once per burning voxel per
      tick), naming WHICH ceiling refused and the owned/retired split, which is
      what separates "too much is live at once" from "something is leaking
      records". The load line reports the shared population too, so how close to
      the wall a fresh world starts is a number and not an archaeology exercise.
    - **`BurnBodies` owns its brick LAZILY now**, at the first poke. It used to
      clone up front for any body `willScan` admitted — and `willScan` admits
      any body with a pair rule and a dirty chunk near it, which during a fight
      is every corpse on the ground. A pile of corpses therefore spent a record
      and ~1.5x a model's words apiece on smoke drifting past. The up-front
      clone bought branch-free poke sites; the branch it saved is one
      predictable test per poked voxel.
    - Render-only throughout, so `determinismHash` does not move.
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
- **Rebuilds are BUDGETED (2026-09-09):** `ManageTerrain` used to rebuild every
  stale patch it wanted in the tick it wanted it, and a chunk-boundary crossing
  stales a whole face of anchor chunks at once — a mob that has just acquired a
  target widens its anchor to `navRadius + 4` (~216 chunks), arriving at
  `World::kFetchPerTick` = 64 per snapshot and all polygonized, tree-built and
  broadphase-added on the tick they landed. That was the 50-100 ms "Game
  Systems" spike while walking. Now `kTerrainBuildsPerTick` (6) real rebuilds
  per tick, nearest anchor first (the ground under a body is always the first
  patch made; the far edge of a planner's horizon, which only the fetch
  serves, is the last), and the surface's identity is the hash of the 18³
  occupancy box taken BEFORE marching cubes — the mesh is a pure function of
  it, so a re-fetched chunk whose solids did not move (the 8-tick dirty-flag
  refresh, liquids flowing, blood drying) costs one sample and no mesh, no
  Jolt body and no wake. A COUNT budget, not a time budget: the debris gates
  run this under the selftest, and a body landing on a patch one tick later
  on a slower machine would settle somewhere else. `terrainMesh` is its own
  row on the Performance tab, debited from `gameLogic`;
  `SANDVOX_TERRAIN_BUILDS_PER_TICK` is the one-binary A/B arm.
- **...and what one body may ASK for is budgeted too (2026-09-12):** the report
  was "2 fps for five seconds when a tree enters the rigidbody system", and the
  `tree-fell` gate's cut pass now prints where the time went in one run
  (`DebrisSystem::PhaseProfile`, thirteen phases, off unless a gate or
  `SANDVOX_DEBRIS_PROFILE=1` asks). For ONE 28,478-voxel oak over the 300 ticks
  of its fall: **220,896 chunk visits (734 a tick, 900 in the worst one),
  26,232 occupancy gathers, and up to 454 chunk fetch requests in a SINGLE tick
  against `World::kFetchPerTick` = 64** — to build six patches. Nothing else in
  the profile was close: Jolt's own step was 0.56 ms a tick, the compound-shape
  build 8.7 ms ONCE, the island scan 43.8 ms on the birth tick and nothing
  after. STILL OPEN, and the profile says so rather than implying otherwise:
  the render-side rebuild of 28k box instances is timed (`Phase::Instances`)
  but a headless gate never calls `BuildInstances`, so that hypothesis is
  instrumented and unmeasured until someone runs a live session under
  `SANDVOX_DEBRIS_PROFILE=1`.
  The cause was a **bounding sphere**. `needAround` reached `radiusVoxels + 6`,
  and `radiusVoxels` for a 59 × 81 × 59 crown is 60 — a 133-voxel box, ~900
  chunks, of which the body can touch about 150. So a body now asks around its
  **rotated lattice AABB** (`Body::lmin/lmax`, cached on the voxel count, which
  every geometry edit here changes and burning does not) plus a 4-voxel skirt,
  and each chunk carries its distance to that BOX rather than to `xf.pos` —
  which is the lattice's min corner, so the old metric ordered a crown's chunks
  by distance from one bottom corner. Three nearest-first budgets then bound the
  sweep whatever the scene asks: `kTerrainNeedCeiling` (512) on the list itself,
  `kTerrainFetchPerTick` (24) on fetch REQUESTS so this system can never flood a
  readback queue that drains 64 a tick, and `kTerrainGatherPerTick` (24) on the
  18³ occupancy reads. None of the three DROPS anything — a chunk that misses
  its slot is at the head of the next tick's sweep, the same contract
  `kTerrainBuildsPerTick` has always had, so a patch is late and never absent
  and nothing falls through ground it was about to be given. Measured on the
  same gate: **814 ms → 172 ms** of debris CPU over the fall (2.70 → 0.57 ms a
  tick), ticks over 8 ms 22 → 1, gathers 26,232 → 7,176, worst-tick fetch
  requests 454 → 24. The remaining 30 ms spike is the birth tick's flood plus
  28k exact-cell ops, which is one frame and is what that work costs.
- **THE GATE SAID TREES FALL; THE GAME SAID THEY DID NOT. BOTH WERE RIGHT
  (2026-09-12).** The debris gates run `runTick(ops, fetch=true)`, which
  requests every chunk of the fixture box every tick, so their floods never
  wait on a readback. The live flood does: an event deferred because the flood
  reached an unfetched chunk kept its SEED box cached (only the crown was
  missing), so PreTick's drain loop found it ready again in the SAME tick and
  ran it again — all 32 `fetchRetries` in two ticks, long before the
  one-tick-latent readback could land. Every scan that left the 3×3×3 player
  mirror gave up on the spot: 99 scans, 3 give-ups, 0 bodies, 165k cells read
  from unfetched chunks, for an oak cut 48 voxels from the player. Now
  `Event::waiting`/`waitChunk` record the first chunk the flood stopped at and
  `EventReady` holds the event until that chunk's copy has landed (a chunk that
  streamed out is not waited for; `SANDVOX_NO_FETCH_WAIT=1` is the old arm).
  The same tree is a body 15-22 ticks after the cut, in rounds of
  `kIslandFetchPerScan` chunks. What made it findable in one run instead of a
  bisect: `SANDVOX_ISLAND_TRACE=1` prints one line per scan — seed, retries,
  components, the largest one's box and the cell that ANCHORED it, the chunk
  it waits for and whether that chunk is cached — and the first live attempt
  needed exactly that: planted at the SPAWN site the flood walked 54 voxels
  west and 40 down through a neighbour's crown to the ground and correctly
  refused, which `anchorAt` named and a counter never could. Fixtures for the
  live path go on the map's `harness` pad (no trees by construction).
- **The render half of the handoff has a number now (2026-09-12).** The gate is
  headless, so `--fell-tree <tick> [x,z]` (main.cpp) plants
  `selftest::BuildTree`'s oak ahead of the player under `--frames`, cuts it 120
  ticks later exactly as the gate does, and prints the gate's COST line plus
  whole-frame percentiles over the 300 ticks of the fall; the `--frames`
  summary's `drawBodies` / `terrainMesh` / `physics` rows are the rest.
  Measured on one 28.4k-voxel oak: `buildInstances` ran ONCE (0.4 ms — the
  instance list is body-local, so "28k cubes rebuilt while it moves" is
  retired); `drawBodies` is a STANDING 1.0 ms mean / 3.9 p99 / 5.1 ms max GPU
  per frame for the resting body (`fsBody`'s per-fragment shadow ray and/or
  the interior cubes nobody can see); and the live flood cost 4-6× the gate's
  because THREE events (the cut's CPU event and the two GPU support flags it
  raised) re-flood the same tree on every fetch round, ~10 rounds of 32
  chunks, at 1-2.5 µs a cell through a hash-keyed visited map: islandScan
  428-702 ms over ~32 scans, worst tick 77-204 ms. Those are the next
  packages' numbers, and the harness is how they are read back.
- **The compound build was the merge, not Jolt (2026-09-12).** With
  `SANDVOX_PHYS_PROFILE=1` splitting `CreateDebrisBodyXf` per body, the oak's
  8.6 ms was 8.3 ms of greedy box merge through two `unordered_map`s (the
  probe lambda inserted a default `used` entry per lookup) and 0.3 ms of
  `StaticCompoundShape::Create`. The merge now walks a dense byte lattice over
  the body's bounding box — a body is an int8 lattice, the oak is 51×90×51 =
  232 KB — in the same order, so the box set, collider and resting pose are
  unchanged (fuzzed against a port of the old walk, and the debris /
  settle-back / audio-impact / body-fastfall detail lines are byte-identical):
  **10.4 ms → 0.5 ms** on the birth tick. Still open: the 1,024-box cap covers
  only 5,474 of the oak's 28,478 voxels (19%) in flood order, so a canopy's
  collider is whichever fifth the island scan reached first; raising the cap
  moves resting positions and is its own gate-measured change.
- **The body draw was overdraw × a shadow ray (2026-09-12).** Under
  `--fell-tree`, `BuildInstances` ran twice for the whole fall (0.3 ms), and
  the draw cost 1.14 ms a frame while the oak was a body: every voxel was an
  instance, every fragment cast `fsBody`'s sun ray. Now `BuildInstances`
  emits only voxels with an exposed face (28,400 → 24,027 for the oak;
  `SANDVOX_BODY_DRAW_ALL=1` restores the full list), `vsBody` collapses the
  faces the camera cannot see before they are rasterised, `fsBody` casts no
  ray where the lambert it would multiply is zero, and `Simulation::DrawBodies`
  draws a depth pre-pass (`vsBodyDepth`/`fsBodyDepth`) first so the ray runs
  once per pixel: **1.14 → 0.52 ms** a frame, pixel-identical (the debris
  gate frame differs by ≤5/255, TAA noise; `body-shade` still 0.434 in
  shadow). Removing 15% of instances cut 48% of the time, which is what
  "per overdrawn fragment" means. `BODY_SHADOW_RAY` / `BODY_VIEW_CULL` /
  `BODY_SUN_SKIP` at the top of `debris.wgsl` are the one-binary arms. Bodies
  still cannot use the shadow cache: its patches are grid cells, and a moving
  body would re-register every frame. The floor without a look change is one
  ray per covered light-facing pixel (0.52 mean / 1.6 p99).
- **...and what it asks for is its MATTER (2026-09-12).** The rotated AABB was
  the fix for a bounding sphere, but the AABB of a tumbling 81-voxel trunk
  with a crown at one end is mostly empty: in the live harness one oak sat AT
  `kTerrainNeedCeiling` (505 of 512 chunks a tick) for as long as it turned.
  `Body::needBlocks` dices the lattice box into 8³ blocks with one bit each
  (cached with `lmin/lmax`), and `ManageTerrain` transforms only the set
  blocks — centre through the body basis, extent the rotated cube's per-axis
  half-width, plus the skirt — into a dense per-body chunk grid of least
  distance to any block box, so a chunk holding matter is still at 0 and the
  nearest-first budgets are unchanged (`SANDVOX_TERRAIN_NEED_AABB=1` is the
  old arm). Live: 96,056 → 47,439 chunk visits over the fall (max 512 → 185 a
  tick), Jolt meshes 251 → 68; the wall-clock win is modest (~110 ms either
  way) because the AABB list was already cheap per entry, and `terrainNeed`
  itself rose 6 → 17 ms for the 192 block transforms a tick. A creature's
  planning horizon (`navRadius + 4`) is handed over apart from its body
  radius (`AddTerrainAnchor`'s `horizonVoxels`) and listed on one tick in
  `kTerrainHorizonStride` (4), hash-phased, the body itself every tick
  (`SANDVOX_TERRAIN_HORIZON_STRIDE=1` is the old arm; unmeasured live, no
  targeting mob in the harness — the COST line's `anchor chunks` is its
  denominator). And the shared chunk-fetch FIFO is finally visible:
  `World::FetchSource` tags every request (IslandScan / Terrain / Mob / Settle
  / Other — the player's 3×3×3 mirror rides every slot unconditionally and
  never queues), `World::FetchProbe` counts per source (accepted / coalesced /
  refused non-resident / carried / dropped streamed-out, age in ticks to the
  carrying slot, queue depth, full drains, backlog), and `FetchReport()` is
  one line in `--frames` and on the FALL line. Measured: the collider sweep
  starves nobody (age 1.00, max 1, 0 full drains during a fall); standing
  still, the island scans are the queue's only real customer (~19 a tick, 35
  full drains, backlog to 34). Any tuning of `kFetchPerTick` or a consumer cap
  is justified by that line or not at all.
- **One flood per tree per round, and two or three rounds (2026-09-12).** The
  island flood's visited map is a dense int32 page per chunk it touches
  (`labelPages_`, allocated on first touch, recycled, capped at
  `kMaxLabelPages` past which a component is anchored as oversize), shared by
  every scan of one tick and cleared once in PreTick (`BeginTickLabels`): the
  three events a cut raises (the brush's event plus a GPU support flag per
  chunk it touched) cost ONE flood, because a seed an earlier scan labelled
  inherits that component's verdict (`tickComps_`: anchored / waiting on chunk
  W / held / made) and a flood that runs into such a component adopts it.
  Nothing is requested during the flood any more; the chunks a component could
  not read are requested after the verdicts, only for components a fetch could
  still change, with the face ring, the column to the drop floor and the
  26-ring speculated under one cap (`kIslandFetchPerScan` =
  `World::kFetchPerTick`, counted per chunk — the old cap charged every
  re-entry, so a scan's real reach was a handful of chunks), so a crown lands
  in two or three rounds instead of ten. The one exception is the dive
  frontier under the seed, requested whatever the verdict: the verdict is
  monotone but the cost is not, and a terrain flood that cannot dive 48 cells
  spreads 200k sideways (11.7 M cells over one burn without it). The shard
  dice and the weld pass read neighbours through the same pages (`index`)
  instead of two more hash maps, so a 28k-voxel oak is one 18 ms scan at
  0.65 µs a cell. Live, the same cut: islandScan 428-702 ms over ~32 scans →
  **61 ms over 5**, worst tick 73-204 → 32 ms, debris CPU over the fall
  611-1097 → 166 ms, the tree a body at +6 ticks (was +15..22); gate cut
  pass 259 → 173 ms with the worst tick 48 → 19.5 ms; the burn pass (the
  forest-fire proxy) visits 1.28 M cells instead of 2.43 M. Still open: the
  first support flag under a cut floods the terrain slab once (60k cells,
  the drop anchor needs the column below to land), and the burn pass's one
  stray leaf voxel is INTERMITTENT and not the wait's doing — the burn pass
  is not run-to-run reproducible on one binary (5.0 M vs 11.7 M cells
  visited; the traces diverge the tick the first burning bodies appear,
  because the terrain-collider budget is wall-clock and feeds Jolt), the
  survivors sit at ground+2 outside the gate's own forced-rescan tiling
  (which starts at `treeA.lo` while the sweep box reaches 2 cells beyond),
  and `SANDVOX_ISLAND_WATCH=x,y,z` / `ovHid`/`ovShow` are in place for the
  next failing run.
- Sleeping: settled bodies deactivate entirely until another body or force
  intersects their AABB (Jolt does this natively).
- **NOTHING PASSES THROUGH THE GROUND (2026-09-12):** a collision patch is a
  sheet of triangles with no thickness, so the only margin a body has is the
  thinnest box in its own collider — 2-3 voxels for a ragdoll limb, against
  17.8 voxels of travel in one 30 Hz tick at `player.maxFall`. Debris and limp
  rigs were going through the floor, and the guarantee needs THREE things,
  because each one covers a case the others cannot:
  1. **Jolt CCD.** Every dynamic world body is created `LinearCast`, not the
     default `Discrete` (which advances by v·dt and only then asks what it
     overlaps). Jolt pays for the shape cast only when a step exceeds 0.75 ×
     the collider's inner radius, so a settled or walking body costs what it
     always did. `Physics::CreateDebrisBody`'s note has the arithmetic; the
     `body-fastfall` gate pins it against a real `PolygonizeChunk` patch.
  2. **Patches requested ALONG the velocity.** A cast can only hit a triangle
     that exists, and the anchor box reaches about one chunk past a
     human-sized body while a patch costs a chunk fetch plus a slot in
     `kTerrainBuildsPerTick`. `ManageTerrain` now also asks for the chunks the
     swept segment passes through, 0.35 s ahead and capped at four chunks,
     each carrying its distance from the SAMPLE so the build order puts the
     ground a body is falling onto ahead of the ground it has left.
  3. **A body may not enter space no collider describes.** The Jolt twin of
     `Player::KnownDrop`: `DebrisSystem::UntunnelBody` puts a body that ended
     its step in a chunk with no built patch back at the last vouched point on
     the segment, keeping both velocities so it resumes the moment the patch
     lands. A CLAMP, NOT A VETO — a body already out there is let through, and
     one held for `kUntunnelHoldTicks` is released with a report, because the
     deadlock at exactly the speed the fix exists for is what the player's
     first blind-fall attempt hit. This is the "park a body whose chunks are
     not cached yet" the `debris` gate's buried-ejecta finding asked for.
  4. **...and a jointed rig is ONE body for that purpose**
     (`DebrisSystem::UntunnelRig`, 2026-09-13). Clamping each body of a ragdoll
     separately tore it apart: a human rig spans two or three chunks, so falling
     fast its LEADING bodies — the feet — enter the unvouched chunk several ticks
     before the head, and each was teleported back to its own last vouched
     sample *with its velocity intact* while its neighbours kept travelling. That
     is a fresh multi-voxel violation at every joint, every tick of the
     starvation window, which the solver then closes by force in a different
     direction per limb. `Mob::PostStep` now hands the whole limp rig over at
     once and one common vouched fraction of the step is applied to every member
     along its own displacement: `f = 1` moves nothing, `f = 0` puts the rig back
     exactly as it was, still rigid and still at speed. Both escape hatches keep
     their meaning at rig scope (any member that STARTED outside lets the whole
     rig through; the hold counter is keyed on the rig's first live body).

### A corpse in armour is not allowed to be a motor (2026-09-13; `Physics::SweepRunawayRigs`, gate `corpse-armor`)

Owner report: an armoured NPC, killed after its head had been cut off, spasmed
and flew apart **while still jointed**; the framerate collapsed and the game
froze for minutes. No `crash.log`, so this is not the FP-overflow kill 46e3848
closed — that one dies in `JobIntegrateVelocity` with `0xC0000091`.

**Every net that existed before this one is looking for garbage, and this is not
garbage.** `corpse-armor` reproduces it and measures the peak spin at
**47.12 rad/s, which is Jolt's own `mMaxAngularVelocity` to five figures**: the
solver asked for more and the clamp handed back exactly the ceiling. Every net
above it (`kInsaneSpin` = 1e4, `GuardBodyInertia`, `VelocityIsSane`) is three
orders away and correctly so. Lowering those thresholds would not describe this
failure; it would only move an arbitrary line.

**The bug is the persistence, not the value.** A limb thrown by a sword blow may
touch the clamp for a tick. A limb that *sits* on it is being driven — and a
dressed corpse has an obvious motor. A worn shell on a CORPSE is its own Jolt
body, `Fixed` to the limb it wraps and geometrically *inside* it, so armour
doubles the bodies, doubles the constraints, and puts an iron-against-flesh mass
ratio across every new one. A stiff constraint between deeply overlapping bodies
is the textbook way to make a sequential-impulse solver gain energy, and the
joints `Mob::Die` deliberately leaves on the corpse (see `corpse-intact`) spread
it from one limb to all of them. That is precisely what "flying everywhere while
still technically being attached" describes.

> **That motor no longer exists, in any phase.** It was cut later the same day:
> `Mob::Die` hands the follower strap to `DebrisSystem::StrapBody` instead of
> trading it for a `Fixed` joint, so a worn shell is a kinematic follower on a
> corpse exactly as it is on a living creature (next section). A dressed corpse
> now has the same bodies in the solver as a naked one, the same constraint
> graph and the same mass, and `corpse-armor` asserts that directly — no shell
> that is still a body may carry a joint — instead of only bounding what the
> corpse did afterwards.
>
> **The nets below stay, and are still the reason the report was survivable.**
> They are not made redundant by removing one motor: a rig can be fed by any
> constraint, and "a body that SITS at its own clamp is being driven" is a true
> statement about all of them. What changed is that the commonest source of one
> is gone.

**Why it costs minutes rather than merely looking silly.** Two multipliers that
this document already records elsewhere. Every dynamic body is
`EMotionQuality::LinearCast`, so once the linear velocity is large each step is
a swept compound-shape cast against marching-cubes terrain, per body, per step.
And the contact solve sees `v + ω × r` at every contact point — the same term
that turned a 5-second gate into an eight-minute hang when 1e30 rad/s was
injected past the sweep.

**Three nets, in `phys/physics.cpp`:**

1. **The linear ceiling comes down from Jolt's 500 m/s to 80 m/s.** 500 m/s is
   8.3 m — 83 voxels — of `LinearCast` sweep in one 60 Hz step. Nothing in this
   world legitimately exceeds 32 m/s (terminal velocity for a fall the height of
   the whole residency window); `physics.explosionMaxSpeed` is 30 and
   `ragdoll.maxLaunchSpeed` is 14. **The angular ceiling deliberately stays at
   Jolt's value**, because ω = v/r makes a one-voxel ball rolling at 5 m/s
   legitimately 100 rad/s, and a lower cap would make every pebble skid.
2. **`SweepRunawayRigs`**, before every `Update`, straight after
   `SweepInsaneVelocities` (which has already removed anything that cannot
   safely be squared). A per-body counter rises one per step spent at 90% of
   that body's own ceiling and falls one per quiet step, so a piece merely
   thrown hard decays out and is never touched. Past 12 the body is damped to
   20%; past 45 the **joints are cut** and the body is zeroed and deactivated —
   the motor is the constraint graph, and damping a body a constraint is feeding
   just means it is fed again next step. A corpse that comes apart is a far
   better outcome than a game that stops. **Jointed bodies only:** a body with
   nothing attached has nobody to feed it, and damping every rolling pebble
   would be a more visible bug than the one being fixed.
3. **An unusable transform is repaired, not narrated.** It used to be a
   `fprintf` on the argument that the position is already gone and the real fix
   is upstream. Both halves are true and it is still the wrong call, because the
   failures are not the same size: a wrong body is one dropped limb, a NaN AABB
   is the whole process wedged with a window that will not close. The body is
   put back inside a finite world, zeroed and slept, and the ordinary debris
   cull collects it. A NaN *quaternion* does the same damage by the same route
   and is now checked too.

A `Physics::Step` over 100 ms reports itself on stderr with the active-body
count and the fastest body attached — report-only, because wall clock may not
decide what the simulation does, but it may say what it saw.

**Neither stage ships unexercised.** `SANDVOX_PHYS_FAULT=pos:<n>` writes a NaN
position straight into a live body past every clamp (the two existing arms
cannot produce one — the velocity checks catch their garbage before it
integrates, which is what they are for), and `corpse-armor` drives two jointed
spheres at the ceiling on purpose and asserts damped > 0, cut > 0, joints 1 → 0
and a final spin of zero. `SANDVOX_NO_RUNAWAY_NET=1` is the A/B arm in one
binary, for the same reason `SANDVOX_NO_ANTITUNNEL` exists.

**What this does not do.** It bounds the symptom; on a corpse it does not remove
the energy source. The next section removes it everywhere a creature is still
alive, which is where the reported *visual* failure was; a corpse still carries
the constraint chain and still relies on these nets.

### A garment is not a separate object (2026-09-13; `MobLimb::wornHost`, `Mob::DriveWornShells`, gate `ragdoll-dress`)

Owner report: "when the human mob is falling, and is wearing clothes, when they go
into the ragdoll state the clothes will decouple from the body when moving at
high speeds... it becomes a crazy tangled mess ball of limbs and clothes that
aren't actually connected properly."

**Measured, on a dressed human made limp 200 voxels up: a greave 3.3 voxels off
the leg wearing it and rotated 151 degrees away from it.** That is what a `Fixed`
constraint between deeply interpenetrating bodies of very different mass does
under acceleration — it lags, and it pumps (previous section). Even solved
perfectly it would be the wrong model, because a garment on a limb has no pose of
its own to solve for.

So a worn shell is now a **follower**: `MobLimb::wornHost` names the limb it is
strapped to, it stays KINEMATIC in every phase a live creature has, it has **no
constraint at all**, and `Mob::DriveWornShells` teleports it onto the host's exact
rigid offset — position, rotation and the rigid-body velocity `v + ω × r` — once
per tick from `Mob::PostStep`, after the read-back. The offset is therefore
*derived* rather than solved, which is a constancy claim rather than a smallness
one: the same fixture now measures 0.000 voxels and 0.06°.

- **One place poses a shell.** `SubmitPose` skips them. It used to pose them too
  (an `AnimPart` whose rest transform is the identity gets the host's pose for
  free), and the two agreed only while both bodies were kinematic — a limp host
  is placed by Jolt and a get-up host by a per-limb blend, and either is a
  garment floating off a shoulder.
- **Death hands the strap over; it does not trade it for a constraint.**
  `Mob::Die` records the shell/host pairs, and once both are `DebrisSystem`'s it
  re-ties them there (`DebrisSystem::StrapBody`): the shell stays kinematic and
  its pose is derived from its host's every `PostStep`, by the same rigid
  offset, on the far side of the same read-back. **A garment is a follower in
  every phase there is**, which is what makes "a corpse in armour behaves like a
  corpse" a structural fact rather than a tuning result — the armour is not in
  the solver, so it cannot change what the corpse does.
  - A follower is not anti-tunnel-clamped and is not read back from Jolt (it
    has no trajectory of its own), and it never settles back into the grid on
    its own — a breastplate stamped into the world while the body inside it
    went on tumbling is the failure that rule prevents.
  - **When the host stops existing, the strap is cut** (`UnstrapBody`) and the
    garment becomes ordinary dynamic debris carrying the velocity it was being
    driven at — culled, looted, burned away, settled back, or dragged out of the
    loot panel (`ShedCorpseLoot`, which cuts it explicitly: off the corpse has
    to mean off it).
  - A collider rebuild mints a new handle and the strap is keyed on handles, so
    `CarryStrap` sits beside every `ReplaceBody` the way the joint re-target
    does. Without it a plate came off the first time the limb under it lost
    voxels to a sword or to fire.
  - The strap is **saved** (`DBRS` version 3: host index + rigid offset). A
    section that did not carry it reloaded an armoured corpse as a pile of free
    bodies sharing the same space, which is the same motor by the other route.
- **A shell that leaves ON ITS OWN stops being a follower** — knocked off, or
  its piece shed — and is ordinary debris from the end of its sever hold. A
  shell whose HOST leaves is a different case and follows it (above).
- **What it costs:** a kinematic body has no mass in the solver, so an armoured
  ragdoll tumbles with flesh inertia rather than with 489 kg of iron. Weight
  still tells where it is authored to (`BodyMassKg` sums the shells, so a blast
  launches a plated body far slower). Folding each shell's mass into its host's
  mass properties is a separate change with its own gate.
- **`SANDVOX_NO_RIGWELD=1`** is the A/B arm in one binary — jointed dynamic
  shells and the per-body anti-tunnel clamp — for the same reason
  `SANDVOX_NO_ANTITUNNEL` and `SANDVOX_NO_RUNAWAY_NET` exist. `ragdoll-dress`
  reports both regimes' numbers side by side in `tests/baseline.json`.

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

### The wound model: a blade cuts (2026-08-31; `Mob::CutLimb`, `sim/tuning.h` §E)

The section above says dismemberment is geometry. For blades it was not: three
thresholds in `Mob::Damage` — a hit within 1.75 voxels of a joint anchor, a hit
past the limb's authored `severImpactSpeed`, and hp reaching zero — each fired a
`Sever()` outright, and a swung sword tripped all three on first contact. The
per-voxel carve ran alongside them as decoration on a decision already made:
touch a creature with a sword, lose a limb, anywhere, every time.

**A cut is a slot, and dismemberment is what is left of the lattice.**

- **The kerf.** `BladeCut` (game/mob.h) is one hit as pure geometry: the contact
  point, the blade's own edge axis, the direction the edge is travelling, its
  half-thickness, and how deep it bit. Two of those are MEASURED — the item's
  authored `edge` segment through its live pose, and how far the tip really
  moved this tick — so a wound's orientation cannot drift from the art or from
  the swing. `Mob::CutLimb` builds an orthonormal frame from them and carves a
  tapering wedge-shaped slot through `CarveLimb`, so everything that already
  followed a carve (re-skin, collider rebuild, connectivity split, hp charge,
  bleed budget, hurt cry) follows a cut with no new plumbing.
- **The slot starts at the SURFACE, not at the hit point.** A fixed-depth kerf
  at a fixed point saturates: the first blow empties it and every blow after
  finds that space already gone. Measured before this existed — 46 identical
  cuts to one thigh took 2.6% each and never severed. `CutLimb` finds the
  nearest flesh along the travel axis over the slot's own core footprint and
  puts the entry plane there, so the second blow starts at the bottom of the
  first one's gash. It also takes the collider's convex-radius inflation out of
  the arithmetic, since `cut.at` is a ray hit on a padded box merge rather than
  a point on the art.
- **Blood is a MATERIAL, not a colour.** `StainWound` rewrites a hash-selected
  fraction of the flesh around the cut to the creature's wound material
  (`bleed.woundMaterial`, defaulting to its own blood; since 2026-09-15 that
  material is a PARAMETER — `Mob::StainWoundAs` — because a bruise and a bite
  leave the striker's material, not the victim's, see *Damage kinds* below) —
  the same mechanism charring uses, and for the same reason: a nonzero art slot overrides the
  material colour in `microbody.wgsl`, so a stain carried as paint is invisible
  on exactly the painted surfaces it matters most on. It renders, it travels
  with a severed limb, and it ejects as blood when cut again.
- **...and on a living body that material DRIES BACK TO FLESH, it does not
  evaporate** (`gore.woundHeals`, 2026-09-14). The body burn pass runs the
  ordinary authored reaction table over a limb's lattice, and blood's rule in
  `reactions.json` is `decay → air` at 8 per-mille a tick — written so a pool
  on the ground dries up and its chunk sleeps (rule 2), and applied unchanged
  to blood that is *inside a limb*. So every sword cut opened a hole that then
  ate itself outward at a ~3 s half-life until the sever rules below took the
  part off: the soak vanished, the anatomy under it showed through, and the
  limb came apart with no further blows. A pool drying and a wound drying are
  not the same event — blood soaked into meat leaves the meat, not a void. So a
  live limb remembers the word each soaked voxel covered (`MobLimb::woundWas`,
  a sparse aux layer keyed by limb: the word is full, and losing the table only
  costs the revert), rolls that decay `gore.woundHealSlow`× slower, and puts
  the flesh and its art slot BACK instead of removing the voxel.
  **`woundHeals: false` is byte-for-byte the old behaviour and is the UNDEAD
  setting** — global in `tuning.json` or per creature as the mob sidecar's
  `bleed.woundHeals`, so a zombie's cuts go on rotting outward and shedding its
  limbs while the living keep theirs. A severed limb is debris and has never
  had the table, so a part already on the ground rots either way.
- **Two structural sever rules, both blade-only.** *Cut through*: a component of
  the limb at least `gore.woundSeverFraction` of what it had is no longer joined
  to the anchor — the edge came out the other side. *Hanging by a thread*: the
  limb is still one piece but the flesh inside `gore.woundNeckRadius` of its
  JOINT has fallen below `gore.woundNeckFraction` of what was there intact,
  which is the thing connectivity cannot state (reach-1 lattices leave diagonal
  slivers that 6-connectivity calls "one piece" forever). Both route through the
  ordinary `Sever()`, so this changes WHEN a limb comes off and nothing about
  what coming off means. Neither applies to a burn (which has its own tested
  account of charring through) or to a blast (which has no "other side").
- **Heft is derived from the art.** `ItemDef::heftVolume` is the item's own
  voxel count in world voxels; the factor the wound model consumes is that
  against `gore.woundHeftRef` (the stock arming sword, 5.3). A greatsword cuts
  deeper because it IS bigger, and re-authoring the blade re-weighs it in the
  same edit. The library spans the factor rather than sitting on one side of
  it: `dagger` 0.22x, `shortsword` 0.62x, `sword` 1.0x, `cleaver` 1.85x, against
  a `HeftFactor` floor of 0.2 and a `gore.woundHeftMax` ceiling of 4.0. The
  small pair is what makes the floor reachable — "a knife needs sustained work"
  was an unexercised branch until a knife existed.
- **What survives of the three instant severs.** The joint-proximity rule is
  deleted outright. `severImpactSpeed` remains as an extreme-speed exception
  scaled by `gore.woundImpactSeverScale`, because the authored 9–20 voxels/sec
  were written for a world where contact severed anyway. `hp <= 0` no longer
  removes a limb: on a root or vital limb it still routes to `Die()`, so damage
  kills a creature — it simply does not take it apart (`Mob::HpZeroSevers`).

Gated by `wound-chip` / `wound-accumulate` / `wound-heft` / `wound-bleed`, with
the hit-count BAND (not an exact count) in `tests/baseline.json`.

### Damage kinds: cut, blunt, bite (2026-09-15; `game/impact.h`, `Mob::BluntHit` / `Mob::BiteHit`, `sim/tuning.h` §E6, `docs/PLAN_impact_unarmed.md`)

Everything above is a KERF, and until this landed a kerf was all there was:
`EdgeSweep` carried one `damage` float and the only things `MeleeSweepDamage`
could do with a body it met were `Damage` it and `CutLimb` it. A sword, a mace
and a fist arrived as the same thing, which is why there was no mace and no
fist — and why armour was a wall rather than a mechanic. `gear.cutHardnessRef`
correctly scales a blade's slot to a twentieth of itself on iron (hardness 160
against skin's 8); with an edge as the only thing a blow could be, "correctly"
meant "proof against everything".

**A strike is THREE NUMBERS, not one kind.** `StrikeProfile` (`game/impact.h`)
is `cut` / `blunt` / `bite`, each hp at full swing speed, plus two 0..1
fractions (`bluntCarve`, `armorBreak`) that scale a tuning radius rather than
naming a size in voxels, plus `infectMat` / `infectStain`. An `enum DamageKind`
was rejected for the reason design rule 4 rejects every closed-ended system
here: it would make a sword pure cut and a mace pure blunt and neither is true
— a sword's flat and pommel bruise, a flanged mace tears skin. Three numbers
let `items.json` say `sword: damage 14, blunt 2, armorBreak 0.05` and
`mace: damage 2, blunt 16, bluntCarve 0.6, armorBreak 0.8` and the SAME
resolution code do the right thing for both; `damage` is deliberately not
renamed, because it IS the cut part. `Total()` is the one number the pre-impact
callers wanted — debris melting and the parry spend a blow whole, since what a
blade catching a mace loses is its own hp and trauma is as bad for it as an
edge.

**One sweep resolves all three against the same struck thing**, in this order:
the PARRY, geometrically and first, and the one place the parts are not
distinguished; then CLASSIFICATION, asked once (`StruckKind`: a slot below
`AppendedBase()` is FLESH, one at or above it tagged `worn` is a SHELL, the one
at `HeldSlot()` is a WEAPON, an unowned body is DEBRIS — three resolvers and two
gates read it, and an enum is cheaper to keep agreeing than three copies of the
same two `if`s); then the CUT if `cut > 0`, the whole wound model above
unchanged, inside a `BladeCutScope` that now ENDS with the kerf, because a mace
caving a skull in is not a dismemberment and must not arm the wet dismember cue;
then `Mob::BluntHit` if `blunt > 0`; then `Mob::BiteHit` if `bite > 0`, with the
infection terms copied onto it ONLY when the classification said FLESH — armour
in the way means no infection, and that decision belongs to the sweep because "a
bite that touched flesh" is a fact about what this sweep met. Debris melts on
`Total()` and skips the rest. Every part draws off one per-probe key, so a
replay of the same tick cuts, bruises and tears identically.

**Blunt is trauma, and it never takes a limb off.** On FLESH `Mob::BluntHit`
charges hp through the ordinary `Damage` (flinch, hurt cry, and death on a vital
limb at zero, all unchanged), tops the drip budget up at only
`gore.bluntBleedScale` of a cut's rate — a punch does not open you — lays a
BRUISE in `gore.bruiseMat` over `gore.bruiseRadius · (0.5 + 0.5·power)` (a
COAT, not a rewrite — see "A bruise is an alpha that deepens" below), and
removes a voxel only if the weapon authored `bluntCarve`: a shallow radial DENT
of `gore.bluntCarveRadius · bluntCarve · power`, through the same
`CarveLimbRadial` an explosion calls, soaked in the victim's own `woundMat` on
the way out. The whole blow runs inside `Mob::BluntCarveScope`, and that scope
is the entire implementation of the owner's one-line spec: the COLLAPSE sever is
skipped, so a face may be caved in well past the point at which a blast would
have shed the head, however many blows land. hp reaching zero on a vital limb
still kills, because `HpZeroSevers` is about DEATH rather than about amputation.
A bare fist authors `bluntCarve` 0 and removes literally nothing.

**...and blunt is how armour is answered.** On a SHELL the same call does three
things and not one of them is a resist number. The piece takes
`gear.bluntShellHp` of the blow as its own hp (under 1, or a plate would be
destroyed by the same number of hits that kill its wearer and armour would have
no history). It is BEATEN IN over
`gear.bluntDentRadius · armorBreak · power · k` — real voxels, through the
ordinary radial carve, so the flesh under it is exposed to the next blow and to
fire and acid. And `gear.bluntThrough` of the blow TRANSMITS to the limb the
shell is strapped to (`MobLimb::wornHost`) as trauma with a bruise, no dent and
no bleed, whether or not the shell broke — the plate deforming IS how the energy
arrives, which is the whole difference from a blade. `k` is the shell's own
hardness against `gear.bluntHardnessRef` floored at `gear.bluntHardnessMin`,
exactly as the kerf is scaled but referenced at 120 rather than 8. That is
"plate stops swords almost entirely; maces go through", as geometry.

**A dent in a plate is still a dent** (2026-09-16). The shell branch returns
before the flesh branch below it constructs `Mob::BluntCarveScope`, so for a
while the armour carve ran UNSCOPED and the collapse sever was live against it:
a worn slot is `severable` (a cut strap drops the pauldron), so once
`CarveLimbRadial` had taken a cuirass below `kLimbCollapseFraction` — 25% of its
spawn voxels — `CarveLimb` called `Sever()` and the piece fell off the body.
Raising `gear.bluntDentRadius` 1.2 → 3.0 is what brought that inside a couple of
blows, and from outside it reads as a mace dismembering people. The scope now
opens at the top of the shell branch as well, so the rule
`Mob::BluntCarveScope` states — A BLUNT HIT NEVER TAKES A LIMB OFF — holds for
every slot rather than only for flesh. A mace beats plate IN and beats the
wearer through it; shearing it off the straps is an edge's job.

**A bite is a tear, and it severs only by collapse.** `Mob::BiteHit` on FLESH
charges hp, bleeds like a cut (refusing the drip would make a bite read as a
bruise), and carves a correlated-noise BLOB of
`gore.biteRadius · (0.4 + 0.6·power)` at feature size `gore.biteBlob` — the same
predicate `Mob::RotAtSpawn` draws the undead's holes with, now one
implementation shared through `Mob::CarveBlob`. `Mob::BiteScope` marks the carve
as a tear so it cannot inherit a blade scope somebody left standing; the blade
rules stay off (a mouth has no direction to cut through in) and the collapse
sever is deliberately left ON, because enough bites DO take a hand off and that
is the single rule separating a bite from a punch. On a SHELL a bite is
`gear.biteOnShell` of itself as blunt trauma, with no break and no infection.

**The material a wound rewrites flesh to is a property of the SOURCE.**
`Mob::StainWound` had the victim's `woundMat` baked into it, which is right for
a cut and wrong for everything else; it is now a one-line wrapper over
`Mob::StainWoundAs(limb, centre, radius, seed, rewriteMat, smearMat, ...)`. A
cut leaves the creature's own blood, a punch leaves `gore.bruiseMat` as a coat
(below) rather than as a rewrite, and a zombie's bite leaves the BITER's `rotflesh` smeared with the
biter's `ichor` — the victim's blood does not come into it. Because
`StainWoundAs` only ever rewrites flesh-class cells (`MobDef::tissue`), a
nonzero return IS "the tear exposed flesh", which is how an infection knows it
landed; the material is latched on the limb (`MobLimb::infectMat`) so the heal
path can tell a wound settling from a substance decaying. The rewrites dry
BACK through `ReviveWoundVoxel` where `WoundsHeal()` says so, on their own
clocks — blood at `gore.woundHealSlow`, rot at
`gore.infectHealSlow` (6.0 against 2.0), because rot living in you is not a
wound settling. In an undead it never goes away, which is the point.

**A bruise is an alpha that deepens, not a repaint** (2026-09-16;
`Mob::BruiseLimb`, `AddBodyStain`). It was a rewrite like the other two, and it
was the wrong shape for what a bruise IS. A rewrite is all-or-nothing per voxel,
so the only place the falloff could live was in the FRACTION of cells rewritten:
one blow left a hash-picked scatter of flat `skin_bruised` voxels and the next
left a different scatter beside it, which reads as pixel damage rather than as a
mark on a body. It is a BODY COAT now (`phys/bodystain.h`) — the voxel keeps its
material AND its art colour, and carries a 0..15 amount `microbody.wgsl`
multiplies and lerps over the albedo, the same blend blood and water already
use. Three consequences, and they are the whole point: the falloff lives in the
AMOUNT, so a bruise is dark at the contact and fades at the rim ON THE SAME
VOXELS; every tissue cell in range is touched rather than a subset, with the
mottle moved into a per-voxel jitter that varies the SHADE instead of punching
holes in it; and repeat blows ADD `gore.bruiseStep` (6 of 15 = 40% a hit) to a
`gore.bruiseMax` ceiling (12 = 80%, short of opaque so the anatomy stays
readable underneath). So a contact goes 40% on the first punch, 80% on the
second, and AT the ceiling a further blow rolls `gore.bruiseBleedChance` per
voxel to lay the creature's own blood there instead — bruising deepens until it
turns to blood.

**A coat below about a quarter of full draws NOTHING**, which is why the first
shipped step of 15% a blow was reported as "I don't see any bruising at all".
`bodyStainTint` does not draw `amt` directly: it thresholds against a
value-noise mottle first, `cover = (amt/15 · (1 + stainMottle) − mottle ·
stainMottle) · stainCoverage`, and at `stainMottle` 0.85 an amount of 2 is
negative for any voxel whose mottle exceeds 0.29 — seven in ten drew clean. The
applicator compounded it by scaling the authored step by the taper AND a
0.55..1.0 jitter AND `(0.5 + 0.5·power)`, three factors below one that between
them delivered under half the authored value at dead centre. Power is now spent
on the RADIUS only (it was being charged twice), the jitter is narrowed to
0.85..1.0, and what remains of the taper is the shape the owner asked for by
name: a mace leaves "a spectrum of bruise/wound applied in a circle radiating
outwards in how weakly applied each is". Because there is no rewrite there is no `woundWas` entry either, so a
bruise fades rather than reverting: `skin_bruised` authors `coat.decay` 45 s per
level, through the same ledger-driven drying sweep that evaporates water.
`skin_bruised` is also the first SOLID with a stain block, which
`materials.cpp` used to refuse outright — `"bodyOnly": true` is a material
saying "I am a coat, never a spill", and it forces the liquid path's `chance`
and `consume` to zero rather than trusting them, because those only mean
anything on a soak a solid can never reach.

**Three materials, appended after `blood` so the stain slots keep their
numbers.** `rotflesh` (solid, greenish, `emission` 70, organic + dissolvable,
crumbling to `ichor`, because what runs out of a rotten wound is not blood any
more); `ichor` (liquid, `stain.type: "rot"` — palette slot 3, after wet and
blood — and `washes: false`, since rinsing an infection off in a stream is not a
thing that should work); and `skin_bruised`, skin's own hardness and density
with a `tints` ladder that is an AGE rather than a colour, picked by the
art-colour quantization so a green-fleshed creature bruises greenish. All three
far-alias onto existing entries, because the far palette was exactly full.

**Fourteen tuning rows, all CPU-only** (`Tuning::Gore` §E6, `Tuning::Gear`):
`gore.bruiseRadius` 0.9, `gore.bruiseMat` `"skin_bruised"`,
`gore.bluntBleedScale` 0.1, `gore.bluntCarveRadius` 0.7, `gore.biteRadius` 0.45,
`gore.biteBlob` 2.5, `gore.biteStainScale` 1.5, `gore.infectHealSlow` 6.0,
`gear.bluntDentRadius` 1.2, `gear.bluntHardnessRef` 60,
`gear.bluntHardnessMin` 0.15, `gear.bluntThrough` 0.55, `gear.bluntShellHp` 0.6,
`gear.biteOnShell` 0.3. `bruiseMat` is the file's FIRST NAME-TYPED ROW and could
not be anything else: `tuning.json` reloads on F5 and `materials.json` on R,
independently, so an id here would silently start meaning a different substance
the first time anybody inserted a material. It resolves at use through
`MobSystem::MaterialIdNamed`, and an unknown name resolves to 0 = no bruise,
which is the right failure for a cosmetic row. (`biteRadius` is the one number
the plan got wrong at 1.1: that is most of the cross-section of a 1.2-voxel
thigh, and `bite-rot` measured one bite taking 936 of 1344 voxels and the second
collapsing the limb — an amputation with teeth rather than a wound.)

Four gates in `selftest_impact.cpp`, fabricated sweeps against a standing
fixture rather than strokes through the AI, at the end of the wound group:
`impact-blunt` (a mace past hp zero with the limb still attached, bruise cells
> 0, the dent inside its band, less bleeding than the same hp of cuts),
`impact-armor` (a sword on a cuirass takes at most a chip and leaves the host's
hp alone; a mace beats the same plate in and the man inside it takes hp,
asserted RELATIVE to the blade arm because the property is the difference),
`impact-fist` (a bare fist removes exactly zero voxels; a gauntleted one removes
some, per hit under the wound gates' own figure for one sword cut, which is what
makes "caving a face in SLOWLY" mean something) and `bite-rot` (rotflesh > 0
through skin, exactly 0 through iron). Every numeric row above is in the
`combat-tuning` round-trip table, with a string probe for `bruiseMat`.

### Blood is health, and burns cap it (2026-09-02; `Mob::DrainBlood`, `Mob::RecountBurn`, `sim/tuning.h` §F/§G)

The wound model above made a cut a *shape*. Two things were still pictures:
the blood that followed it, and the char a fire left behind.

**Every drop of blood is hp.** A wound carried a voxel budget and dripped it
out, and hp moved only at the moment of the blow — a creature could lose an arm,
stand in a puddle of its own blood for a minute, and be exactly as alive as the
tick after the cut. Now there is ONE door out of a body for blood,
`Mob::DrainBlood`, and every emitter goes through it: the drip (charged by the
sphere volume of the clump it painted, the same figure the budget is debited
by), the drip's spray, the arterial gout, and the whole voxels a sever throws.
A whole voxel costs `gore.bleedHpPerVoxel`; a micro droplet costs
`1/microScale³` of that, because that is the fraction of a voxel it is. The cost
is spread across the live *authored* limbs in proportion to what each still has
(the same spread `PlayerAvatar::SpendHealth` uses for an overcast, and for the
same reason: draining the first limb to zero picks an arbitrary limb to ruin),
so every limb reaches zero on the same tick and the creature dies through the
ordinary `Die()` — systemic, the corpse keeps its limbs. Held items and worn
shells have hp of their own and are not life, so `Mob::TotalHp` excludes them
and "total reaches zero" cannot mean "the sword broke". The charge lands where
the op is EMITTED, not where the CA lands it: a refused op cost nothing because
no blood left the body, and the CA never has to see the blood for hp to move.

**An amputation does not close.** `severStumpBudget` was the whole of what a
lost limb bled, and it ran dry in seconds. With `gore.stumpBleedsOpen` the
parent limb's wound (`MobLimb::stumpOpen`, set by `Sever()`) is topped back up
to ONE clump every tick for as long as the creature lives, so a lost limb is a
clock: the drip's own cadence and op budget bound the rate, `DrainBlood` turns
it into hp, and death ends it. Bounded under rule 2 by the creature's own hp —
at the defaults (30 Hz, a drip every 4 ticks, clump radius 0, 0.6 hp/voxel) a
body bleeds out at 4.5 hp/s from one stump. Topped up to a clump rather than to
the wound cap so a stump that cannot drip this tick (out of ops) does not bank
blood for later; cleared by `DetachLimb` so a stump that is itself cut off does
not drip forever at its rest-pose anchor from a body it is no longer on. Fire
still cauterises: a limb that burns through arms no stump (`inBurnFlush_`).

**Burns cap the health a body can hold.** Burning already charged hp for the
voxels it removed, but a body that is COOKED rather than consumed lost nothing
by that account — `flesh_cooked` and `flesh_charred` are still voxels, so a
creature 60% charred and 100% present read as healthy. `Mob::RecountBurn` takes
the burnt fraction of the body on the authoritative lattice as BURNT SURFACE
OVER SURFACE — the body-surface-area grading burns get in the clinic: every
burnt voxel at any depth (cooked/alight = ½, charred/ash/cinder = 1, a voxel
fire removed = 1 via `BodyBurnState::burntAway`, which unlike `removed` is
never reset) against the body's burnable voxels that had an open face when it
was whole (`MobLimb::surfaceAtSpawn`, taken lazily on the first recount;
burnable = `tag:flammable` or a burn stage, `MobSystem::BurnableOf`, so bone
never counts; garments and items are not the body). A surface and not a
volume for a mechanical reason: char is inert and shields what is under it.
Measured, a human stood in a fire column for 1,200 ticks converged at 30% of
its burnable VOLUME — 5,900 raw skin and 7,000 raw flesh voxels under a black
shell, 29.3% at tick 400 and 30.1% at 1,200 — and would have stood there
forever under any volume knot. Against its surface the same body reads burnt
through, which is what it looks like. Then
`Mob::ApplyBurnCap` clamps every live authored limb to `authored hp × cap`,
where the cap is one piecewise-linear curve through three authored knots:
intact → full, `burnCapMidFraction` → `burnCapMidHealth` (40% burnt → a third
of full), `burnDeathFraction` → zero (70% burnt is death, whatever the hp was).
hp is only ever clamped DOWN, nothing heals past the cap, and the HUD draws
`[HealthCap, HealthMax]` as charred off (`UIState::healthCap`) so a burnt body
reads as a permanently short bar rather than one that quietly rescaled. The
recount is dirtied by the burn pass and by every carve and taken at most every
`kBurnRecountTicks` (8) — a creature that is not changing costs nothing, one
that is burning pays one pass over its body per cadence, never one per burn
step (rule 2). The material list is `Mob::BurnStageOfMaterialName`, and
main.cpp's per-limb HUD readout resolves through it, so the two cannot disagree
about ash. Derived, not saved: a loaded avatar keeps its low hp but starts at
cap 1 until it burns again.

**Heat crosses a joint, and until it did a burning torso never lit the legs.**
Owner report, 2026-09-03. The cause was structural rather than a rate: a
creature's limbs are separate sparse lattices that cannot see each other, and a
mob is not in the grid, so the only channel from a burning hip to the thigh
under it was the `fire` gas the hip emits — and `fire` is a gas that rises with
probability 1 in calm air, so a flame emitted downward floats straight back up
through the limb that made it. Within one limb fire had always spread in all six
directions; across limbs it could not spread at all.

`Mob::BuildCrossLimbHeat` runs once per creature per tick and collects the WORLD
CELLS its limbs are alight in, from each limb's existing burn front (never a
walk over the volume — a torso is 31,456 skin voxels), keyed by cell and carrying
a BITMASK of the limbs that lit it. The mask is the load-bearing part: a world
cell is 8×8×8 skin voxels, so the cell just outside a surface voxel usually holds
more of that same limb, and attributing a cell to one limb would let a burning
limb read its own voxels back as an external hot neighbour — a self-sustaining
ignition loop dressed as a feature. A limb reads a cell only if some OTHER limb's
bit is set. Each cell is widened by its six face neighbours, because two limbs
meeting at a joint need not share a world cell. `MobSystem::BurnOneLimb` then
reads the list on faces where the grid holds air and nothing is worn in the way
(a shell still wins — that is the armour mechanic), with the same world-pitch
tangential widening the grid already gets, so a face pointing into a sibling's
fire counts for as much as that fire is WIDE and flesh's `minCount 3` is
reachable across a joint. The igniting is done by the ordinary authored table:
no limb-to-limb rule, no status effect. `combustion.crossLimbPct` (25) is what a
rule pays when a sibling is the ONLY thing arming it — decided once over all six
faces, because a pair rule fires on the first matching face and a cross face that
happened to sort first would otherwise have hidden a real fire and bought a scale
it should not get. Two guards: an inverted ramp ("how exposed am I") reads a
cross face as the air that is really there, and the inbound pass skips cross
faces entirely, so heat crosses a joint and damage does not.

**The cheap gate has to know about it too**, and that was the whole bug the
first time. `BurnOneLimb` opens by walking the WORLD around the limb and exiting
when it finds nothing and the limb is not itself alight; a sibling's lattice is
in neither place, so a thigh beside a burning hip returned before it built an
index. Measured: 35,080 cross cells offered and 71,173 faces taking one, with not
a single thigh voxel changed — the faces were limbs awake for other reasons, and
the one limb the feature was written for was asleep. The cross cells inside the
limb's AABB are now appended to the same list the world scan fills, so the gate
wakes the limb and the existing face-seeding loop queues the voxels behind each
cell unchanged. Gate: `mob-burn`'s two-arm differential — 279 of 558 thigh voxels
burnt at `crossLimbPct` 25 against **0** with it at 0.

**Fire spreads eight times slower than reactions.json authors, by one knob.**
Owner report the same day, once limbs began conducting: the spread was far too
fast. `combustion.spreadPct` (12) multiplies every ignition chance at
reaction-COMPILE time (`sim/materials.cpp`), the twin of `burnDurationPct` beside
it — that one is how long a lit voxel stays lit, this one is how readily the
voxel beside it catches. A knob rather than an edit to the authored numbers
because those numbers encode ratios tuned three separate times (dry needles
against green leaves, cloth against flesh, leather against cloth); one multiplier
moves the clock and leaves every ratio intact. A rule scales if its product
carries `tag:hot`, or if it is a pair rule on `tag:flammable` matter driven by a
hot neighbour — the second clause is the SEAR, and it has to be there: leaving
`skin → flesh_cooked` at the authored rate while everything around it slowed by
eight made flesh react to a single hot face four times more eagerly than the
cloth over it, inverting the comparison the whole body-burn section is built on
(`mob-burn` caught it as skin halving at t+31 against cloth's t+124). Not scaled:
anything marked `burnDuration` (each rule has exactly one owner among the two
knobs), EMIT rules (a slower fire must not also be a dimmer one), and heat's
other jobs — water steaming, ice melting — which the flammable test excludes.
The floating flame's own weakness compounds with it: `neighborChance` for `fire`
went 0.125 → 0.0625 in the same change, so a drifting flame now ignites at a
sixteenth of a stationary source's rate. **Fixtures sized in ticks are deadlines
that tighten as this comes down**: the player's 90-tick immersion scored 6.7%
past the sear against a 15% floor until it was scaled by the knob (49.2% after),
and `mob-burn`'s termination subtest now asserts CONVERGENCE — keep giving the
fire quiet ticks while the count is strictly falling, fail only on a plateau —
rather than a fixed window, which is the honest form of the rule-2 claim and
does not need retuning every time a combustion knob moves.

**The burn pass starved the legs.** Found by the gate's census, not by
looking: `MobSystem::BurnLimbs` spends one shared front budget
(`kBurnFrontPerTick`, 6,000 cells) per candidate cell, limb by limb in def
order, and a torso standing in a fire offers thousands of candidates every
tick. Measured on a human engulfed for 1,200 ticks: hips and torso burnt to
nothing, the lower legs and feet kept 636 of 668 and 368 of 382 surface voxels
RAW, and the right arm (later in the def) sat at 552 of 596 raw against the
left arm's 241. No body could reach the death knot that way, and in play it
read as "fire does not burn legs". `Mob::BurnTick` now starts from
`tick mod limbs` and `BurnLimbs` from `tick mod mobs`, so each limb and each
creature takes the head of the budget in turn — deterministic, since the tick
is the key. The budget itself is unchanged.

**A corpse says what killed it.** Four mechanisms now end in the same ragdoll
(a vital limb destroyed, a vital limb burnt or dissolved away, blood loss, the
burn cap), and a gate that found one could only guess. `Mob::DeathCause()` is
a static string set at every `Die()` call site that knows, mirrored into
`MobSystem` so it survives the husk sweep; `armor-react`'s acid bath prints it
beside the tick, and it is what showed that bath's death was the hips' hp
running out under the ordinary carve charge with the burn fraction at 0.0%.

**A corpse comes apart only where it is cut.** The one-blow dismemberment the
owner reported ("every single one of his limbs pops off all together") was not
in the blow: `one-hit` reproduces the blow and found nothing. It was in the
rest of the stroke. `MeleeSweepDamage` keeps probing after the killing tick,
the creature's limbs are debris from the moment `Die()` hands them over (with
their joints on, so the corpse hangs together), and a probe that lands on one
of them goes `MeltBodyAt` → `DamageBody` → `RebuildCollider`, which built a new
Jolt body and **removed** the old one — and `Physics::RemoveBody` destroys every
joint on the body it removes, because Jolt asserts on a constraint that
outlives a body. One nick to the torso took the neck, both shoulders and both
hips off in the same call; the burn shrink and the fragment parent rebuilt the
same way. Every collider rebuild in `DebrisSystem` now goes through
`Physics::ReplaceBody(old, new)`: each joint on the old body is rebuilt against
the new one under the same joint handle from the stored `JointDesc` (rest-frame
limits, so nothing re-centres), anchored where the **other** body still says
the anchor is (a rebuild may rebase the body's origin, so the replaced side's
own local anchor means nothing), and the collision group and object layer are
copied across, so a rebuilt torso neither collides with its own limbs nor
starts pushing the player. A body separates only when the carve actually
disconnects it (`ShatterBody`'s fragments have no joints, which is right: they
are the piece that came off). Gate `corpse-intact`: kill the fixture through
the root limb, melt the torso for three ticks at the sword's kerf width, settle;
the torso must have been rebuilt, the joint count must not have moved by one,
and every body of the corpse must still carry a joint.

**Amputations, retuned.** The impact-sever exception was reachable by an
ordinary swing: `severImpactSpeed` 9 on a lower leg × the scale of 4 was 36
voxels/s, and `melee.fullSpeedMps` 3.4 is 34 — a committed cut at a shin severed
on contact through the rule the structural model was supposed to have replaced.
`woundImpactSeverScale` 4 → 8 puts the bar at 7.2 m/s and above; `woundSeverFraction`
0.28 → 0.40 (the parted piece must be nearer half the limb) and
`woundNeckFraction` 0.28 → 0.20 (the joint must be more thoroughly gone). All
three are `tuning.json` edits and the band in `wound-accumulate` is where they
are felt.

Gated by `bleed-out` (the identity hp lost = blood emitted × rate over a whole
bleed, the amputation killing within the time the rate predicts, and the SAME
fixture surviving with the stump allowed to close — the differential that says
the top-up is the mechanism) and `burn-cap` (the curve, the cap biting on a lit
limb with every hp under it, and a body stood in a real fire dying with its
fraction at the death knot rather than of a vital limb burning through).
`bleed-out` is CPU-only (no tick submitted: the drain is charged where the op
is emitted, so the CA never has to see the blood). `burn-cap`'s last phase has
to be a world fire — direct ignition only lights surface voxels, and once the
surface has charred the layer under it never sees three hot faces; measured,
1500 ticks of forced ignition on every limb plateaued at 14% burnt.

### A wound is seen, and a corpse bleeds from where it is cut (2026-09-02; `Mob::StainWound`, `DebrisSystem::BodyWound`)

Two more pictures that were not yet mechanisms, both found the moment the
human had an inside.

**The soak follows what is exposed.** `StainWound` swapped a hash fraction of
EVERY voxel in a sphere for blood, buried or not, bone included. With a real
interior that read as a red sphere with the anatomy under it erased, and the
walls of the hole — the one place a wound is actually looked at — were only as
red as the rim. Now the pass builds an occupancy bitmap over the limb's box on
the hit tick (the same O(voxels) bound it already paid) and asks each voxel
whether it has an empty 6-neighbour in its own lattice. Exposed voxels (the
hole's walls and the skin round its mouth) take `gore.woundStainSurface`
(0.9), buried ones `gore.woundStainDensity` (0.3), both falling off as
`1 - t²` from a centre half a cut-depth in. And **bone is never soaked**:
`MobDef::tissue` marks, per material id, what crumbles to this creature's
blood (`rubble` in `materials.json` — skin, flesh and muscle do; bone crumbles
to dust), and `StainWound` leaves the rest alone, so a hole that reaches bone
shows bone through a splash of blood instead of one more red voxel. A mob
whose blood no material crumbles to gets the pre-anatomy soak-everything.
Radius up 0.6 → 0.9 world voxels so the splash reaches the skin around the
mouth of the cut.

**Every dismembered body bleeds from its own end of the cut.** Bleeding lived
on `MobLimb` only. A corpse carve emitted a one-shot puff of ≤60 droplets and
≤8 voxels and remembered nothing; a decapitation went straight to `Die()`,
which handed every limb to the debris system as it stood — the torso's neck
was never armed, the head stayed jointed to the corpse, and the sum was "an
amputated head leads to no bleeding". Now a debris body carries a
`DebrisSystem::BodyWound` — a body-local point and direction (world-voxel
units in the body frame, the same convention as `MobLimb::woundLocal`), a drip
budget under `gore.bleedBudgetCap`, and a gout countdown — and
`DebrisSystem::BleedBodies` (in `PreTick`, after `BurnBodies`) drains it every
tick from wherever that body has rolled to, on the live wound's own tuning:
the front-loaded gout over `severDecayTicks`, one whole voxel per
`bleedDripTicks` under `bleedOpsPerTick` (a ballistic voxel released just
outside the wound, since a body is not in the grid), and `bleedSprayPerDrip`
droplets with it. Three doors arm one:

- **`DamageBody`** (a sword, a laser or a blast on a corpse): the cut's
  centroid is captured in world space BEFORE the rebase, and after every frame
  is final the wound is opened on the surviving body AND on each fragment
  `ShatterBody` split off, each at its own voxel nearest the cut
  (`ArmWound`) — the owner's spec in one line, *blood should come from each
  of the rigidbodies that are dismembered and their locations*. Budget is
  `gore.corpseBleedPerVoxel` per world voxel carved; a cut that made a
  fragment is an amputation and adds what `Sever` gives one: the gout,
  `severStumpBudget`, and a throw of `severVoxels` whole voxels from the cut.
- **`Sever` on a vital, severable limb.** It no longer short-circuits to
  `Die()`: the head leaves like any severed limb (its own body, `DetachLimb`),
  the piece's wound is armed at its own neck (`anchorLimb`, blood leaving from
  the piece's centre out through the joint), the torso's stump is armed
  exactly as for an arm, and only THEN does `Die()` run. `DetachLimb` and
  `Die` both hand the limb's wound over through `AdoptBody(…, WoundOf(limb))`,
  and `DetachLimb` zeroes it on the husk entry — left there it dripped from
  `BleedTick`'s bodyless fallback at the rest-pose anchor of a limb no longer
  on the creature. The thrown voxels of a fatal sever are not charged to
  `DrainBlood` (the creature is dying of the limb, and the charge would
  rename the cause).
- **`EmitCarvedFragment`** (a gobbet cut off a live limb): `WoundBody` opens a
  small drip on the lump's face nearest the limb it came off. No gout.

A corpse does not pump: nothing tops a `BodyWound` up (`stumpBleedsOpen` is a
live creature's clock, bounded by its hp), and a wound that has paid out
closes. The wound is transient — not in `SaveState`, and a body that settles
back into the grid takes it with it. Rule 2: `bleedOpsPerTick` bounds the
corpses' drips as it does the creatures', the gout shares
`kMaxParticleSpawnsPerTick`, and an idle body costs two field reads.

### Blood on a body (2026-09-13; `phys/bodystain.h`, `MobSystem::StainLimbs`, `Mob::ApplySplatter`, `MicroBodyPokeStain`)

The soak above REWRITES flesh to blood, skips bone on purpose, and ran only on
the blade's kerf, so a cut through bone showed clean bone, and a blast
crater, a corpse cut or an outright sever showed clean everything. And a body
could not take blood from anywhere: a voxel had a material byte and an art
byte and nothing else. The owner's report: cuts "cleanly show the underneath
voxels without any blood", and killing something should get blood ON YOU.

**The stain byte.** `PrefabVoxel::stain` / `DebrisVoxel::stain` (voxload.h
`BodyStain*`): amount 0..15 in the low nibble, stain TYPE above it -- the same
palette slot the voxel word's bits 28..30 carry, so a stain moves between the
ground and a creature without translation and shades through the same
`STAIN_PALETTE_BASE` entry. Rendering is a side lattice in the micro brick
(`MicroBodyModelGpu::dims` bit 30, `kMicroBodyDimsStainBit`): one byte per
micro voxel after the payload, allocated only for OWNED blocks (a def's shared
model is clean by definition; a body becomes owned the first time anything
marks it), written by `WriteBrick` on every re-skin and poked per voxel by
`MicroBodyPokeStain`. The shader (`microbody.wgsl bodyStainTint`) reads one
byte at the hit and applies exactly `applyStain`'s multiply-then-lerp with the
same `TUNE_STAIN_*` knobs, mottled at world pitch, so blood that ran off an
arm onto the floor is the same colour on both. Never hashed (the body is not
in the grid); SAVED, because it rides the limb's own voxel list and the MOBS /
DBRS sections write those lists raw (this sentence said "never saved" until
2026-09-13 and was wrong); travels with a severed limb (the voxel lists move)
and into every fragment
(`DownsampleSkin` carries the heaviest stain of a block; `MicroBodyPack`
grows the lattice for a stained gobbet). Rule 2: the pool pays +50% only for
bodies that have actually been bloodied.

**Four sources, one rule each:**

- **The cut** (`SoakCut`, bodystain.h; called from `Mob::StainWound`, so the
  kerf, the crater in `CarveLimbRadial`, and BOTH faces of a `Sever` get it,
  and from `DebrisSystem::DamageBody` for a corpse). Every voxel within
  `gore.stainCutRadius` takes a stain: EXPOSED ones (an empty 6-neighbour in
  the limb's own lattice -- the hole's walls, the skin round its mouth)
  `stainCutAmount` tapering with the square of the distance and jittered per
  voxel; buried ones `stainCutBuried` at `stainCutBuriedChance`. Bone (not
  `MobDef::tissue`) is never REWRITTEN but always STAINED: `stainBoneMin`
  floors any exposed bone in range, so bone reads as blood-smeared bone.
- **...and A CRATER IS NOT A KERF** (2026-09-13, owner report: "explosions
  that cause minor damage cause way too much random noisily spread blood
  spatter"; gate `blast-stain`). The blast path first asked for the BLAST
  SPHERE -- `StainWound(cBody, radiusVoxels + 0.5)` -- and that is two wrong
  volumes at once. `CarveRadialAll` calls the carve for every limb within
  `radius + r + 2`, so a limb the blast took nothing from was soaked anyway;
  and the sphere is what the crater predicate SEARCHED, not the hole it made,
  while the outer skin is "exposed" by definition, so at `woundStainSurface`
  0.9 a graze repainted most of the limb's visible surface. Measured on the
  human's upper leg: **8 of 1344 voxels lost, 305 rewritten and 907 stained**
  (23% and 67% of the limb).
  Three rules replace it, and the middle one is the whole idea.
  (a) `Mob::CarveLimb` reports what it actually removed (`CarveReport`:
  count, centroid, RMS spread, and the CELLS), and a limb with `count == 0`
  is not soaked at all. (b) The taper is a distance to **the removed cells**,
  not to their centroid -- `BuildCellDist` / `CellDist` in bodystain.h, a
  3-4-5 chamfer over a box round them, shared by the rewrite and the tint.
  A centroid ball is still the wrong shape, because the crater predicate's
  falloff means a graze is a SCATTER across the whole sphere whose centroid
  is inside the limb. (c) `gore.craterStainRim` LATTICE CELLS past those
  cells is the whole reach (the tint rides at the authored
  `stainCutRadius : woundStainRadius` ratio of it), so the blood is the size
  of the hole whether that is a scratch or a bite. Same fixture after:
  **8 lost, 7 rewritten, 111 stained**. The kerf path is untouched -- every
  blade caller still passes a centre and `woundStainRadius`, and with no
  crater the ball and the old `stainCutRadius * scale` tint are what run.
- **...and white noise cannot make a smear.** The soak's per-voxel chances
  were independent draws, which have no feature size, so what they painted
  was an even red sprinkle -- the same mistake `carveChunkiness` exists to
  fix for the crater itself. `woundStainCoherence` blends the draw toward a
  `ValueNoise3` field of `woundStainBlob` world voxels (0 = the old draw
  voxel for voxel). The blob must be SMALLER than the wound it mottles: at
  the first default of 1.6 world voxels the field was constant across a
  3-cell nick and the coherence went all-or-nothing (7 rewritten or 0,
  depending on the seed), hence 0.5.
- **Splatter** (`SplatterEvent`; `Mob::BleedTick` queues one per gout tick
  and per drip spray, `MobSystem::StainLimbs` replays it against every
  creature's limbs, `PlayerAvatar::PreTick` against the player's). The GPU
  droplets cannot see a limb, so this is the CPU's record of the burst --
  origin, axis, cone, launch SPEED and count -- and the replay FLIES THE
  KERNEL'S OWN ARC (speed, then `sim.partGravity` per tick), so a body is
  marked where the droplets are seen to land and nowhere else: a drip's spray
  at 3.5 vox/s rises a tenth of a voxel and marks nothing it is not dribbling
  onto, and a gout at 17 vox/s drops 1.3 m over the metre to an attacker, so
  it reaches their shins, not their face (raise `severSpraySpeed` for that).
  SAMPLED IN PROPORTION: for each limb, the arc families (low / high) that
  pass its bounding sphere inside the burst's cone are solved in closed form,
  the sphere's solid angle over the cone's times the droplets thrown this
  tick is how many arcs are aimed across it, and each arc is marched through
  the limb's burn index (the dense box `BuildBurnIndex` already keeps) tick
  by tick, one slab test per tick until it enters the box. The first solid
  voxel it meets takes a SPLAT of `splatterSplatRadius` world voxels at
  `splatterAmount` (rim thinning and breaking up), not a single lattice
  voxel. That is how killing something covers you in it, and how a neck stump
  paints its own torso. `splatterPerLimb` caps the arcs per limb per event
  (past it one arc stands for several droplets and paints wider); `reach` is
  capped by `splatterReach` and by speed x life. Before 2026-09-13 the replay
  was a straight march of a fixed 24 droplets per limb regardless of the
  burst: a face got two or three 1.25 cm dots from a 1,190-droplet gout, and
  a standing body next to a bleeding one was painted 80 cm up the leg by
  sprays nothing visible had thrown there.
- **Contact** (`MobSystem::StainOneLimb`, from `Mob::StainTick`). The same
  world-AABB walk `BurnOneLimb` makes -- and the same exit at the cost of the
  walk when nothing is there -- for a STAINING LIQUID (blood: its authored
  `stain` type/amount/chance), a DRY STAIN on a solid (the voxel word's stain
  bits: half the amount, `stainFloorTransfer` of a nominal rate), and a
  WASHING LIQUID (water: `washes`). The walk stops at the first such cell;
  then the limb's SURFACE (`BodyBurnState::surface`: the exposed voxels of
  the burn index, listed once per index build) is swept, each surface voxel
  reading the world cell it sits in -- the body is not in the grid, so a
  pool occupies the very cells the feet do -- or else the cell one step out
  along its open face, and rolling the liquid's own per-mille chance on what
  it finds. So a creature standing in a pool bloodies its feet at the rate
  the pool stains the ground, washes them in the river at the rate the river
  rinses stone (`stainWashPerContact` off per successful roll), and a limb
  wholly under blood is bloodied all over. The first version kept the first
  24 contact cells of the walk and mapped each, dilated by one voxel, into
  the lattice: for a submerged hips those were one edge of the bottom row
  every tick, so its underside never saw a contact (2026-09-13, pinned
  fixture: 16 -> 16 through a 30-tick flood).
- **The corpse** carries whatever it died with; `DamageBody` soaks a fresh
  cut on the corpse the same way. (A lying corpse does not yet take contact
  stain from the pool under it -- the debris side has no contact pass.)

Budgets in `mob.h` (`kStain*`): 2,048 world cells per limb walk, 6,144
surface voxels per limb and 32,768 per tick for all creatures, start rotated
by tick; 64 queued bursts; `splatterPerLimb` arcs per limb per burst. Gate
`body-stain` (the human pinned with the `dummy` profile, standing in a stone
basin written round it so the sand cannot flow): an ankle-deep pool over 30
ticks stains the feet and nothing higher than `bodyStainShallowMaxRise` above
its top; a drip-speed burst from a metre away marks nothing; a deep cut
stains the limb and at least half of the bone it exposed; a 6 m/s burst aimed
at the hips lands; the wound's own spray queues bursts; a box of blood round
the hips over 30 ticks stains them and the same box of water over 60 ticks
takes at least half of it off again.

### A coat is a substance, not a look (2026-09-13; `docs/PLAN_body_coat.md`, `Mob::RecountCoat`, `Mob::ShedCoat`, `Mob::Footfall`)

The byte above named a palette SLOT, which is the right identity on the ground
(the substance is the liquid cell; the stain is what it left) and the wrong one
on a body, where there is no cell and the stain IS the substance. Seven looks
are not seven substances: a future nullifier and blood may share a ground
colour and must still be told apart on a hand. So the body stain is now a
`uint16_t` -- 12-bit MATERIAL id, 4-bit amount (`voxload.h` `BodyStain*`) --
and the ground's slot is DERIVED from it where a look is needed: the micro
brick's render lattice stays one byte and `microbody.cpp` converts through
`MicroBodySet::stainSlotOfMat`, refilled at every materials load, so
`microbody.wgsl` did not change. A dry floor stain rubbing onto a foot goes the
other way through `matOfStainType_` (the first material registered with that
slot). The width change bumped the MOBS and DBRS save versions once.

**`coat` block** on a material (`ParseCoat`, requires a `stain` block so the
substance has a look): `decay` seconds per amount level lost while on a body
(0 = washing only; blood 20, water 4 so wet dries), `shed` per-mille chance
per footfall that a coated foot deposits (blood 400), `effects` raw string tags
that nothing consumes yet. Behaviour is data (guideline 4): the first effect is
a tag in JSON plus one read of the ledger.

**The ledger** (`Mob::RecountCoat`): per limb, amount-weighted sums by material
over the live voxels, body totals over the base rig only; fraction
`sumAmt / (15 * voxels)`, so one splash cannot flip a threshold. Recounted
every `coat.recountTicks` and ONLY when a stain byte changed (`coatDirty_`),
so a clean crowd pays nothing (rule 2). `MobSystem::LimbCoatOf / BodyCoat /
CoatTagFraction(mob, tag, limbTag)` resolve the avatar by id like the stain
counts do.

**Decay** lives in `Mob::StainTick` AFTER the contact pass, because
`StainOneLimb` returns at the first empty AABB walk and a coat must fade in
clean air; per-voxel roll on `Hash3(limbKey ^ cell, tick, salt)` so it thins
unevenly, start rotated by tick under the same lattice budget, brick poked per
change. Ground stains do NOT decay: the world rule is monotone so a stained
chunk can sleep, and a drying rule would keep every stained chunk awake.

**Shedding.** NPCs never had a footfall; `Footfall` moved from `PlayerAvatar`
down to `Mob` and the NPC plant emits it (the avatar's per-frame audio drain is
untouched). `Mob::ShedCoat` at the plant, tick-side: a foot whose ledger names a
`shed` material rolls once, then emits ONE micro droplet per distinct ground
cell of the sole -- the particle kernel's `atomicMax` claim takes one deposit
per cell per tick, so N droplets on one cell waste N-1 -- born INSIDE the solid
cell under the foot with life 1, where `sim_particle.wgsl` claims and resolves
it that tick with the material's own `stain` block. Zero shader changes; the
foot loses what it shed; liquid ground is skipped (the droplet would park on
the water and vanish); charged against `coat.shedPerTick` and the spawn ring
before emission, one tick latent through `pendingSpawns_`.

**UI.** `UIState::BodyPartUI::stainFrac / stainMat / stainColor`: the HUD's
"stained NN%" line beside the health bar (hidden under `coat.hudMinFrac`), the
stick figure's limbs blended toward the substance's stain colour, and on the
character screen a `BLOOD 42%` chip beside CHARRED and a stain-coloured callout
for a stained but otherwise healthy limb.

Gate `body-coat` (one monotonic tick counter on purpose: the drying rule fires
on `tick % period`, so stepping the clock back between phases would mis-time
the control arm): after a deep cut the ledger's heaviest material on the limb
is blood (684 of 1330 voxels, fraction 0.24); at the authored 20 s a level the
count holds through 150 ticks (684 -> 684) and at `coat.decayScale` 300 --
two ticks a level, a value an author can set -- it falls to 62 in 47 ticks; a
direct deposit on a stone cell of the room reads blood's slot at amount 5 and
the cell is still stone.

### A creature is a variant of another creature (2026-09-15; sidecar `extends`/`model`/`palette`, `MobDef::undead`, `MobRotDef`, `Mob::RotAtSpawn`, gate `undead`)

A zombie is a human who walks slower, is paler, does not heal, and arrives
already bitten. Four differences, none of them geometry — and the first design
question is therefore not "how do we carve a zombie" but "how does a creature
exist that is MOSTLY another creature".

The answer the mob loader used to give was: it cannot. Discovery was "every
`.vox` in `assets/mobs/`, paired with the `.json` beside it", which makes the
ART the identity of a creature. Adding a zombie that way means copying
`human.vox` — a 150 KB binary that stops tracking the original the first time
anybody edits the human, which is the unowned-diverging-representation failure
design guideline 3 exists to prevent, and worse here than usual because the
**anatomy is baked into the `.vox`** (`scripts/anatomize_mob.mjs`): the copy
would quietly diverge in what is under its skin and no test anywhere would say
so. The 64 KB sidecar would have to be duplicated too, rig and clips and all.

So a def is now named for its SIDECAR, and the sidecar says what it wears:

| key | meaning |
|---|---|
| `"model": "<stem>"` | use `<stem>.vox` instead of my own |
| `"extends": "<stem>"` | ...and start from `<stem>.json`'s contents |

`extends` implies `model`, and is resolved by RFC 7396 merge-patch: objects
merge key by key, an array or scalar REPLACES wholesale, and an explicit `null`
deletes. That is what a rig override actually wants — `"speed": 22` replaces a
number, `"limbs": [...]` replaces the whole list rather than merging fifteen
entries positionally, and `"clips": {"walk": {"durationMs": 900}}` reaches one
field of one clip without restating its tracks. Depth-bounded at 8 (the bound is
the diagnostic; a cycle and an eight-deep chain are the same content mistake).
A `.json` with neither key and no `.vox` of its own is **not a mob** and is
passed over in silence — that is what `attack_styles.json` and `behaviors.json`
are, and keying on the field rather than on a filename blocklist means the next
shared table in that directory needs no code change.

`assets/mobs/zombie.json` is 30 lines and there is no `zombie.vox`.

**The recolour is a FILTER, not a colour table.** `palette` takes
`saturation`, `brightness`, `tint` + `tintAmount` and is applied to the def's
copy of the prefab's art palette *before* `MicroBodyMergeArt` folds it into the
shared one — which is also why it must happen there, since the merge dedupes by
RGB and recolouring afterwards would repaint the human too. Desaturation is
about Rec. 709 luma rather than the channel mean, because a mean desaturate
darkens reds and lightens greens, which on skin reads as the wrong ethnicity
rather than as the wrong health. A per-slot table was rejected for the reason
the `.vox` copy was: restating dozens of authored colours is a copy of the art
by another route, and a filter keeps tracking the original.

**`undead` is a content word, not a subsystem.** It does exactly two things:
it defaults `bleed.woundHeals` to false, and it makes `rot` apply at spawn.
Nothing else in the engine branches on it, and that is deliberate — the slower
walk, the paler skin and the shorter stride are ordinary sidecar numbers and
stay ordinary sidecar numbers, so that "undead" never becomes the name of a
second creature pipeline. A ghoul that sprints is one file.

The `woundHeals` half is the owner's ask restated: the 2026-09-14 wound-heal
work (see *A wound is seen*) stopped blood evaporation hollowing limbs out, and
in doing so stopped limbs falling off a few seconds after a hit. That looked
*right* on the walking dead and wrong on everything else, so `woundHeals: false`
is byte-for-byte the old behaviour, per creature.

**Rot is the ordinary carve, not a second notion of damage.** `Mob::RotAtSpawn`
hands a few blobs per limb to `CarveLimb` — the same function a sword, a blast
and a fire reach, through the same `Mob::BlobCarveFactory` a BITE tears with
(2026-09-15), so a zombie's own holes and the ones it leaves in you are one
implementation of one shape. The holes are real geometry, the collider and the micro brick
are rebuilt around them, the connectivity split runs, and hp is charged for the
volume exactly as any other damage would be. It runs after the mob is pushed
into `mobs_` (a carve clones the brick copy-on-write, rebuilds the Jolt body and
can re-enter the system by id to sever) and before `Spawn` returns, so
`voxelsAtSpawn` is the PRISTINE count — a rotted zombie reads as three-quarters
of a body rather than as a whole small one. `MobSystem::loading_` keeps it off
the save path, where the holes are already in the saved lattice.

Four properties worth stating because each cost something to get right:

* **Every length is a FRACTION of the limb's own extent.** A hand and a torso
  differ by two orders of magnitude in volume; a radius in voxels would erase
  one and graze the other.
* **The bites are drawn generously and then FITTED to `maxLoss`**, by shrinking
  every radius by a common cube root rather than by dropping any. A bite
  abandoned part-way leaves a lopsided hole and makes the total depend on the
  order the draws came out. This is also what makes `maxLoss` the knob an author
  turns: before it bound, it was inert and the result was whatever the radii
  happened to be — measured, 2.2% loss against an authored 20%.
* **The loss comes off in GROUPS.** Pure correlated value noise, no white-noise
  term, at feature size `blob` — the same argument the blast crater's
  `carveBlobSize` makes at length: an independent draw per voxel has no feature
  size, so no falloff shape can turn speckle into a chunk. Two level
  corrections make it work: the noise is stretched about its mean (trilinear
  value noise piles up around 0.5 at σ≈0.146 against a uniform 0.289), and
  **each bite is recentred on the noise at its own centre**. Without the second,
  `blob` and the bite radius being the same order — which is the whole point —
  meant a bite sampled roughly ONE value and then removed everything or nothing,
  and most bites lost that coin flip and vanished.
* **The holes are OLD, which is not the same as dry.** Two different things,
  and the first version of this conflated them and shipped bloodless bites —
  the owner's report was a zombie missing half its face with "just pale
  underneath". A rot hole now goes through `StainWound` over the cells the carve
  actually removed, exactly as a blast crater does, which buys both halves of
  what a wound looks like: the **rewrite** turns a mottled fraction of the
  exposed tissue into the creature's wound MATERIAL (the red *in* the hole), and
  the **smear** lays a stain *over* everything the hole exposed, bone included —
  which the rewrite refuses on purpose, and which is what stops a bite through
  the skull reading as clean bone. `rot.stainScale` multiplies
  `gore.craterStainRim`, above 1 because a wound that has been open a while has
  bled around itself where a fresh kerf has not; 0 is a bloodless rot, for a
  husk that never had blood in it.

* **...and every hole is a different AGE** (2026-09-15, `MobRotDef::dryFraction`
  / `wet`). The version above handed `StainWound` the whole limb's removed cells
  in ONE call, so every hole on a body came out equally bloody — and a body whose
  wounds are all the same age reads as uniform however right any single one of
  them looks. The owner's word for what it should be is a MISHMASH: half the
  holes old and dry, showing the flesh and bone the anatomy baked under the skin,
  the other half still wet, and a SPECTRUM in between rather than two settings.

  So wetness is a property of one BITE, and three things have to be true:

  - **The roll is a ramp with an atom at zero.** `dryFraction` of the bites get
    no soak at all — not a small one, a dry hole is a different look from a
    faint one — and the rest ramp from `wet[0]` (a trace) to `wet[1]`.
  - **The cells are ATTRIBUTED.** `CarveReport::cells` is one flat list of
    everything the carve took off the limb, so a per-bite soak has to decide
    which cells belong to which hole. Nearest in NORMALISED distance (d²/r²),
    not absolute: the bites differ in radius by up to 2x and the removal falloff
    is a function of t, so a cell two cells outside a small bite is further
    *into* it than one four cells inside a large one. Every removed cell is
    claimed by some bite, so the connectivity split's strays land on the nearest
    hole rather than being dropped. A dry bite is attributed and then simply not
    soaked, which is what lets a wet neighbour bleed into it — adjacent holes of
    different ages, not a body with one blood level.
  - **The ramp is uniform in HOW BLOODY IT LOOKS, not in `wet`.** `wet` scales
    the amount linearly and the reach as its square root (a smear that fades to
    nothing within one cell is invisible at this resolution, so a trace has to
    stay narrow rather than stay faint everywhere), which puts the stained count
    at roughly `wet^2.5` — and a linear ramp through a quantity that enters at
    the 2.5th power spends most of its length in the faint end. Measured: half
    dry plus a linear ramp took the rewrite from 0.095 of the body to 0.022, a
    4.3x cut for a change meant to remove half. `v^(1/2.5)` inverts it, and the
    wettest holes are as bloody as every hole used to be (0.029 / 0.137 against
    0.095 / 0.290, with 38% of hole-wall voxels completely clean).

  The bite count and `maxLoss` went up 25% / 30% in the same commit, and they
  had to move TOGETHER: the bites are drawn generously and then fitted to the
  budget, so raising the count alone would have bought more, smaller holes and
  a body no more chewed than before. What came off was the BLOOD, not the
  damage, so the damage is what compensates.

  What stays refused is **bleeding**: `Mob::inSpawnRot_` joins the burn and the
  garment on the line deciding whether a carve tops up a drip budget, and
  `StainWound` has no drip in it. So the holes look wet and the creature is not
  haemorrhaging — which is exactly why the soak and the drip are separate
  functions. The hp charge stays outside both, for the reason the burn exclusion
  gives: the damage is real, only the bleeding is refused.

Gate `undead` asserts all of it against a **living control arm** — the same
measurement on a human must come back zero, or "voxels are missing" is not a
result — plus two spawns of the same def differing per-limb (a per-def seed
would pass every other claim and produce an army of identical corpses), and a
spur count, since a voxel COUNT cannot tell a torn chunk from a fine sprinkle.

One place that control arm is deliberately **not** zero, and it caught the gate
out on first run: a living human already contains blood voxels, because its
anatomy speckles the muscle layer with `blood` at fraction 0.06 (182 of 26,494
measured). "The zombie has wound material and the human has none" is simply a
false claim. So the rewrite is tested as a RATIO against that baseline, while
the smear — which only damage ever applies — is tested against a true zero.

The gate's other end is **claim F, that not every hole bled**, and it needs no
recipe lookup to say what "in a hole" means: a hole's wall is an EXPOSED voxel
(an empty 6-neighbour on the limb's own lattice) whose material is one the
INTACT body never shows — and the set of materials a whole body does show is
measured off the living control arm rather than hardcoded, so the claim survives
an art edit, an anatomy change or a material rename. Voxels already rewritten to
the wound material are excluded, being blood by construction. Both ends of the
band are asserted, because either alone is satisfied by a uniform: all-dry is
the bloodless rot this replaced, all-wet is what replaced it.

The creature the **NPC AI panel** spawns is a dropdown over every def
publishing a `held_right` socket (the eligibility test the spawn already applied
silently — the panel arms what it spawns). The undead appear there with no UI
edit, because a zombie extends the human sidecar and inherits the socket; the
gate asserts that socket survives the merge, since losing it would drop the
variant out of that list with nothing else going wrong.

One bug found on the way out, and it was not in this feature: `--shot-mob`
renders its own frames and so never ran the frame loop's `if (mbSet.dirty)
UploadMicroBodies`, meaning **no brick edited after boot ever reached the GPU
there** — every carved, burnt or rotted limb silently did not draw. The harness
exists to answer "what does a damaged body look like" without a live session,
and damage was the one thing it could not photograph.

### What is under the skin (2026-09-02; `assets/editor/anatomy.js`, sidecar `anatomy`, `scripts/anatomize_mob.mjs`)

Every limb was skin all the way through. A cut face showed skin, a burn-through
showed skin, and the "paint an organ as a distinct material" answer the carving
section above gives had no organs to point at. The human now has an interior:
skin one voxel deep, `flesh` under it, `muscle` under that (speckled with
`blood`), and `bone` at the core; the head is a two-voxel bone skull around a
flesh brain.

**This is authoring, not engine.** Every runtime consumer was already per
voxel: `voxload` keeps enclosed cells, the micro brick is dense and its march
stops at the first solid cell (an interior costs memory and nothing else until
a carve exposes it), `CarveLimb` / `CutLimb` / the burn front / gib particles /
settle-back all read the voxel's own material. So the interior is baked into
`human.vox` as ordinary per-voxel materials and nothing in C++ learned a new
concept. What was added:

- **A depth field.** `unionDepth` measures depth-from-surface over the UNION
  of every limb at its prefab offset, EXCEPT that a limb's own faces are
  always depth 0. The first version measured the union alone, so the joint
  faces where a thigh meets the hips read as interior — which put vertebrae
  on the top of the neck and a shoulder socket on the arm, on show the moment
  the rig turned a head or raised an arm (2026-09-02, the owner's first
  complaint). Now a voxel with an empty 6-neighbour in its OWN model is
  surface (every limb is skin all the way round); everything under it keeps
  the union's depth, applied after the BFS rather than seeded into it, so the
  bone core still runs through the joint (a neck five voxels tall measured
  per limb would be skin / flesh / muscle / flesh / skin). A severed joint
  shows its skin cap and the blood the wound soaks into it, not an anatomy
  plate; a cut through the middle of a limb shows the schedule. `keep` on the
  skin layer repairs a kept voxel made of an interior material back to skin
  (that is what the old bake left on every joint face), so re-applying the
  recipe migrated `human.vox` in place.
- **A recipe, in the sidecar, by name.** `human.json` → `"anatomy"`: an
  outermost-first list of `{material, depth}` layers with an open-ended core,
  optional per-limb overrides (`limbs.head`), `garments` (surface voxels that
  are clothes — the voxel under the linen shorts is skin, not flesh; the
  deeper schedule is unchanged so the bone core does not move), and a
  `speckle` on any layer (a hash fraction of it swapped for another material;
  the hash is on prefab coordinates so applying the recipe twice is a no-op).
  Layer 0 is `keep: true`: the recipe never touches the painted surface, and
  every voxel it does write has its art slot CLEARED, because a nonzero art
  slot overrides the material colour in `microbody.wgsl` and painted flesh
  would show the skin's paint. A cyborg is a different recipe over the same
  tool (`{steel}` under a `{skin}` shell, or `{chrome}` at depth 0 without
  `keep`), never a different tool — design guideline 4.
- **Two materials and two rules.** `flesh` (117) and `muscle` (118) enter the
  skin's burn chain by the same `tag:hot → flesh_cooked` entry rule at the
  same chance, so a limb burns through its layers on one clock. `bone` has
  no fire rules and survives by construction, as the flesh chain's note
  always said it would. Mob voxel material ids must stay ≤ 127 — the art
  palette owns 128..255 — which leaves nine slots after these two.
- **The tuner's peel.** The Models tab has an *anatomy* row: **Peel** hides
  the outermost depth layer of every limb (PageDown / PageUp), peeled voxels
  are neither drawn nor picked so every brush lands on what the peel
  exposes, **Fill layer** paints the whole exposed layer with the active
  material, and **Apply recipe** runs `planAnatomy` through the undo log (a
  sidecar without a recipe gets the stock human one written into it). The
  depth field is a snapshot taken when peeling starts, not re-derived per
  stroke: depth is measured from the surface, so re-measuring after every
  erase would make the hole you just cut re-skin its own walls and vanish
  under the peel.

What the interior changes at runtime, all of it a consequence of materials
already being per voxel: the derived collider's plurality blocks take flesh
/ muscle / bone materials inside a limb, so limb MASS follows the recipe
(`densityOfMat` in `CreateDebrisBodyXf`); a gibbed or settled limb puts bone
and muscle in the world grid; the burn gate's body census counts flesh and
muscle as body and as charrable flesh (bone in neither). What it does NOT do
yet, stated so nobody infers it from a screenshot: hp per carved voxel is
still pure volume (`kCarveDamagePerVolume`) — bone costs what skin costs;
severing (`Sever`) still does nothing to the cross-section, which is now
exactly why it needs nothing. (`StainWound` stopped soaking bone the same
day — see "A wound is seen" above.)

Verified by `node scripts/test_anatomy.mjs` (the depth field on a two-model
block whose seam faces must read surface and the row under them union
depth, a kept bone face repaired to skin, the schedule, garments, speckle,
idempotence, an unknown material refused; then the committed `human.vox`
must match its own recipe, every limb must have a bone core, and no paint
may sit below depth 0) and by the existing `mob-burn` / wound / armour gates
running over the baked model.

---

## 8. Player & Projectiles

- **Player**: capsule controller (Jolt character), colliding against on-demand
  localized marching-cubes terrain patches. Voxel-type queries drive traversal:
  liquids slow movement and, **once the body is three-quarters under**
  (`kSwimSubmersion`), swap jump→swim. Shallower than that is WADING, which is
  a walking state: footing, jump, step-up and the ground snap all survive, and
  only the submersion-scaled drag/buoyancy/wade-speed apply. `Player::swimming`
  is that distinction; `inLiquid` means only "some part of the body is wet" and
  is the wrong flag to gate a movement rule on. Standing in gas/liquid can apply
  status effects; some materials are absorbed on contact (Noita stain system —
  and remember its lesson: players will invent rules for anything you surface
  in the UI, so communicate statuses deliberately).
  Traversal assists, all in `player.cpp`: step-up absorbs sub-step ledges, the
  water-edge mantle climbs out of pools, and ledge grabbing covers everything
  taller — airborne with space held, a lip between shoulders and fingertips
  latches into a dead hang, and W pulls up (a committed mantle when the lip is
  standable, an arm boost to the next grab when it is not, which chains up
  rough walls).
- **The collision box is not the figure** (2026-09-02, `Player::Box`).
  `Player::pos` stays the centre of the NOMINAL 1.7 m figure box
  (`kHalfXZ`/`kHalfY`: the art contract, the Jolt proxy, the mob sense actor,
  every `feet = pos.y - kHalfY`), but the box the sweeps use is a smaller one
  standing on that same sole — `player.collisionWidth` (0.4 m) wide and
  `player.collisionHeight` (1.5 m) tall, both live tuning — and movement is
  decided by it alone. Shoulders, arms and the top of the head overhang it
  and clip terrain by exactly that overhang, on purpose: a corridor a
  head-clip lower than the figure still admits the figure, and a doorway the
  elbows brush does not catch. Every sweep/probe in `player.cpp` takes the
  box as a parameter (`Player::BoxFor` is the one place its shape is
  decided). The first-person eye is the LOWER of the figure's face row and
  just under the box top, because only the box is guaranteed clear.
  **Crouch (Ctrl):** the box shrinks to `player.crouchHeight` (1.15 m), speed
  scales by `player.crouchSpeedScale` and sprint is off; releasing Ctrl only
  stands up once the standing box is clear where the body is, so a
  crawl-space cannot wedge you. The eye change is banked into `viewYOffset`
  like a step-up. The avatar mirrors `Player::crouching` into a held pelvis
  drop (`player.crouchKneeDrop`, `PlayerAvatar::crouchHold_`, capped in leg
  lengths) kept separate from the gait's per-step `stanceCrouch_`; the leg IK
  turns the drop into a knee bend with the feet where the gait put them.
- **Projectiles** (spells, thrown things): no colliders — swept ray each frame
  (position + velocity look-ahead, anti-tunneling). Spell modifiers attach as
  **tags with per-frame logic** (material trail, AoE on hit, bounce, acceleration
  modifiers). On hit: run effect, or reflect off a local marching-cubes normal.
  This tag-composition structure is deliberately the seed of the Noita-style
  wand/spell system later.

### The spell system (2026-08-20; grammar 2026-09-04; `game/spell`, `game/caster`, `assets/spells/glyphs.json`)

The Noita-style wand system this section always anticipated, crossed with the
*ancient language* of Eragon: you speak words, the words compose, and
imprecision is punished rather than rejected. The first slice (2026-08-20) had
three glyph types and a "last element wins" fold; `docs/PLAN_magic_grammar.md`
replaced the *language* on 2026-09-04 while keeping the four structural
commitments below, which are the part of this section that is *not* meant to
change.

**A spell is a program whose only output is op-stream emissions.** Every world
change a spell makes leaves as a `BrushOp`/`ExplosionOp`/`CellOp`/
`ParticleSpawn`/`WindPrim` on the MutationQueue (rule 3). `SpellEmission` is
the only channel out of the VM, and there is no path from spell code into a
voxel buffer. That is what gives spells save/replay/networking for free, and it
is why a spell blast joins the ordinary `exps` list rather than getting its own
detonation path — island checks, body damage, mob carving and impulse all apply
with no spell-specific code.

**The effect payload is position-parameterized, so backfire is free.**
`ApplySpellEffect(payload, at, dir, strength, out)` takes the position as an
argument, so "cast it at the muzzle", "cast it where the bolt landed", "cast
it at the clicked body part" and "cast it into the caster's own chest" are the
*same call*. Backfire is therefore never per-spell special-case code: a new
verb gets a thematic death the day it is added.

**The VM is integer, in fixed point — and this is NOT rule 1.** Projectile
position/velocity are 24.8 fixed-point voxels (the exact convention
`ParticleSpawn` already uses), and mana/health/timers are integers. Spell state
is CPU-side gameplay state *outside* the hashed grid domain, exactly like mobs
and debris, so the world hash cannot see it either way. It is fixed-point for
**lockstep MP (§10) and replay debugging**, where a projectile's path must
reproduce bit-exactly on every machine. Floats appear only at the drawing
boundary.

**The VM is not player-coupled.** Casting is a free function over (cast list,
caster state, origin, direction) → emitted ops; a mob drives the identical
`Cast()` call. Health is read through a `CasterHealth` callback rather than a
field, which is what lets the player's health stay where it actually lives —
`PlayerAvatar`'s per-part hp — instead of a parallel number that would drift.
`PlayerCaster` (inventory + spoken stack) is a separate struct and `Player` is
untouched.

**Rule 2 applies to magic, with no exception.** Every lowered cast carries four
finite numbers — `ticks`, `voxels`, `instances`, `generation` — and law L8
asserts them. A trail carries a hard voxel VOLUME budget that only decreases,
and the projectile dies when it is spent; lifetimes, live projectile counts,
multiplicity, fan count and generation are capped in `glyphs.json` and clamped
against engine ceilings at load.

#### The grammar (three rules; `ParseSpell`, `LowerBox`, `LowerSpell`)

Hundreds of glyphs, uncountably many sequences, every one does the predictable
literal thing, and nobody ever writes a rule for a specific combination. The
way to get that is the way programming languages get it: a handful of
**sorts**, a fixed **valence** per word, a tiny set of **primitives** every
effect lowers to, and a **tariff** that prices what the spell does to the
world rather than the words it used.

Six sorts, declared per glyph in `glyphs.json` (`sort`): **Matter** (a
material by name; `air` is the void, `anything` the wildcard), **Effect**
(something that happens at a point: a `verb` plus parameters), **Delivery**
(how an Effect reaches a point: `mech` instant | flight | continuous plus the
record fields), **Mod** (a field edit on the delivery record: `field`, `op`,
`amount`), **Operator** (a verb with argument slots — `left`/`right` list the
sorts each accepts, `result` the sort produced; every unary operator takes the
one item BEFORE it, only `transmute` is infix), and
**Separator** (`also`, which ends one sentence and starts the next). Nothing
in C++ knows which words exist. The C++ vocabulary is the sort names, the verb
names (`spray`, `place`, `convert`, `explode`, `wind`, `mend`, `trail`,
`sustain`, `filter`, `repeat`, `launch`) and the record field names — each verb
maps to ONE op type or ONE engine seam, and `ApplySpellEffect` switches on the
verb and nothing else.

**Three rules, and they are the whole grammar** (rewritten 2026-09-10; the
2026-09-04 six-rule version is superseded, and `docs/PLAN_magic_grammar.md` §2
R4/R6 and §11.4 are marked as such):

- **1. A NOUN GOES INTO THE PILE.** A Matter word, an Effect word, or an
  operator group whose result sort is Effect is pushed onto the PILE. Order
  inside the pile does not matter: runs merge (`shotgun shotgun` is
  `shotgun×2`, capped by `budgets.maxMultiplicity`), identical non-adjacent
  items merge too, and the lowering rebuilds the pile in a CANONICAL order (by
  key) because mods compose through integer arithmetic that does not commute
  under clamping — 49 halved then doubled is 48. Matter and Effects ADD (×N of
  the verb's declared axis); Mods COMPOSE.
- **2. A DELIVERY BOXES THE PILE, AND SPEAKING CONTINUES.** The Delivery word
  takes the WHOLE pile and wraps it into ONE Effect value of verb `launch`:
  that delivery's `DeliveryRec`, the pile's Effects as its payload, the pile's
  pending Mods stuck to its record. The pile becomes exactly that one value —
  so a box is a noun again and can be boxed once more, taken by an operator, or
  spoken beside other nouns. **Deliveries NEST, and word order is what decides
  what is inside what.** `explosive projectile projectile` is a bolt that, when
  it hits, fires a bolt that explodes. Deliveries are the one sort that does
  NOT merge on repeat, precisely because each one boxes what is in front of it.
- **3. A MOD STICKS TO THE BOX THAT CLOSES THE PILE.** A Mod word — or an
  operator group of result sort Mod, which is `trail` — goes into the pile as
  PENDING and is applied to the record of the NEXT delivery spoken. So
  `explosive shotgun projectile` and `shotgun explosive projectile` are the
  same three fanned exploding bolts, while `explosive projectile shotgun` is
  one bolt fired from three fanned points and `explosive projectile shotgun
  projectile` is three bolts that each fire one. With no delivery to close the
  pile a mod sticks to `hand`, the implicit outermost delivery, where `shotgun`
  is three fanned resolve points and `float` is the hop — and **every other mod
  on the hand is a charged no-op**, named in the describe line ("`swift` is
  wasted: it landed on your hand, which has no speed") rather than quietly
  editing a field nobody reads.

The outermost box is ALWAYS `hand`, so the whole utterance lowers to ONE cast —
unless **`also`** is spoken. `also` is a new sort (`separator`, word cost 0):
it closes the current pile as a finished cast, hand-delivered if no delivery
closed it, and starts a new one. **Law L4 (cost additivity, union of emissions)
attaches to `also`**, not to a delivery, because a delivery no longer ends
anything.

Unary operators (`trail`, `aura`, `echo`, `null`, `mend`) take the ONE item
immediately before them, which may now be a launch box: `explosive projectile
echo` is a turret and `explosive projectile aura self` a sustained status that
fires a bolt every tick. `transmute` is still the one infix word. An operator
with an empty required slot is still INCOMPLETE — charged, does nothing, drawn
as `_`. `trail` still yields a Mod, so it is pending and sticks to the closing
delivery, and its inner effect may itself be a launch; on any record that does
not travel it is charged and does nothing.

An **empty box** — a delivery spoken with nothing in front of it — is a carrier
with an empty payload, which is today's kinetic hit (`impactRadius`). It is
legal, total and priced.

**`split` is gone.** `explosive projectile shotgun projectile` subsumes it and
says what it does; the `children` field, `ModField::Children` and the resolve
lambda's children branch went with it. `SpellFan` stayed.

**A nested launch, at runtime.** `SpellVerb::Launch` is an `EffectInst` that
carries a `DeliveryRec` and its payload in `inner`. `ApplySpellEffect` stays
op-stream-only (thesis 1): a Launch appends a `SpellLaunchReq` to
`SpellEmission::launches`, and `SpellSystem` adopts those at the end of `Cast()`
and inside `Tick()` exactly the way it adopts statuses, echoes and filters
(`AdoptLaunches`, then `Adopt`). Flights go through `Launch()`, bombs through
`RequestBody()`, a nested `beam` runs ANCHORED — from the point along its
direction for its tick cap, unheld, because that is the only bounded reading —
and a nested `self` resolves its payload at the caster's body when the owner's
`SpellBodyProbe::bodyPos` can say where that is, otherwise where the parent
resolved. The drain loops (bounded by `maxGeneration + 2` passes) because an
instant nested box resolves in place and may ask for carriers of its own.

**Where a child launches from, and which way.** From the parent's LAST FREE
position (the `from` in the resolve lambda — a child born inside the wall its
parent hit would impact on its own first sub-step and chain), with direction =
the parent's incoming direction REFLECTED off the surface it hit, per axis,
using the same probe `bounce` uses. With no surface — a bomb at rest, a fuse
resolve, an orb expiring, a `self` — it goes straight up. `shotgun` fans around
that direction with the existing `SpellFan`.

**Two bounds on a nest (rule 2).** Each nested launch is generation + 1 and
nothing past `budgets.maxGeneration` launches — the existing check in
`Launch()`, plus the same check in `AdoptLaunches`. And the LEAF instance count
(the product of the fans down each path, summed over paths) is capped at
`budgets.maxInstances`: a box whose fan would overrun it is clamped at lowering
and the describe line says so. `SpellCast` carries `depth`, `leaves` and
`instancesClamped` beside the four old numbers.

**Price is recursive, and carry composes multiplicatively.** A launch effect's
tariff is (payload tariff + trail tariff) × instances × the carry of its
delivery, so a bolt that fires a bolt pays 3.0 × 3.0 on whatever the inner one
finally does — which is exactly the "you are paying to move it twice" reading
carry exists to give. Word costs sum over the whole tree. `priceUnknown`
propagates. `CastList` still exposes wordCost / tariff / carryCost / manaCost;
the tariff is the tree's base and the carry line is every premium in it.

**Fatal casts FLATTEN.** `ApplySpellEffect` takes a `flatten` flag, and a Fatal
outcome runs the payload at the caster with it set: every Launch resolves its
own payload IN PLACE, recursively, and no carrier is created. So an overcast
`explosive projectile projectile bomb` goes off in the chest with everything it
was ever going to do. Law L11 asserts a Fatal cast emits no launches at all. A
misfire (live-projectile cap, generation cap) flattens for the same reason.

`ParseSpell` (the three rules, in one left-to-right pass — operator binding has
to happen there, because the item to an operator's left may be a box a delivery
just made) produces a `SpellTree` of `SpellNode`s that are now raw words,
operator groups, or BOXES; `LowerSpell` calls `LowerBox` on each clause's root
and recurses, turning each box into a `SpellCast` — a `DeliveryRec` (the record
Mods edit), a payload of `EffectInst`s whose Launch entries are the nested
boxes, an instance count, the rule-2 budgets and the price split into word /
tariff / carry. Every sequence lowers to a definite cast list; there is no
misfire state, and the imprecision penalty lives entirely in the mana/health
crossover. The HUD's brackets are derived from the tree, so a box draws as
`[ … DELIVERY]` and what you see is what nested.

**The reference interpreter is the oracle.** `scripts/magic_grammar.py`
implements the same three rules over the same glyph table and generates
`docs/MAGIC_PERMUTATIONS.md` (the worked sets, every word, every brief
sentence, every pair over a 19-word alphabet, every triple over a 10-word
core), with a header metric per alphabet: orderings vs distinct spells vs
distinct spells that do anything at all. `--oracle` writes
`assets/spells/grammar_oracle.json`, and the `spells-oracle` gate parses every
entry with the C++ and compares the bracket string and the clause structure.
A row of the permutation table that reads wrong is a rule that is wrong; fix
the rule in both places and regenerate. Never hand-edit the generated files.

**The laws are the gate, not a pinned list.** The `spells` gate asserts
algebraic properties over every sequence of length ≤ 3 drawn from that
alphabet, generated in the test: L1 totality (non-empty cast list, finite
cost, non-empty description), L2 pile commutativity (permuting nouns and
pending mods within one pile lowers to an IDENTICAL cast list, compared field
by field), L3 multiplicity (`g g` ≡ `g×2` exactly until the cap — except a
delivery, which nests N deep, and `also`, which is N+1 sentences), L4 `also`
additivity (`cost(A also B) = cost(A) + cost(B)` and the casts are the union),
L5 delivery invariance (the payload INSIDE the box of `E… D` is identical for
every flight D; only the record differs), L6 locality (a unary operator's
operand is exactly the item to its left, and inserting a word anywhere outside
that item's span does not change it), L7 tariff monotonicity, L8 budgets (finite
ticks, voxels, instances, generation, plus leaves ≤ `maxInstances` and a finite
depth — what stops a deep nest from FIRING is the generation cap, asserted at
runtime, since a sentence may legally SAY more than the engine will do),
**L9 nesting** (for any E and flight deliveries D1, D2, the payload of the
`E D1 D2` box is exactly one Launch whose payload is bit-for-bit the lowering
of `E D1`), **L10 mod placement** (`E μ D` ≡ `μ E D`, and in `E D μ` the mod is
on the hand record and never reaches inside the box), **L11 flattening** (a
Fatal cast of any sequence emits no launches). A change that breaks a law
breaks a *class* of spells, which is what the line says; a change that moves one
spell's numbers is a rebaseline.

Gate `spells` check (8), the nest at runtime: `explosive projectile projectile`
launches exactly one child and it is the child that explodes; `explosive
projectile fuse projectile` rests on impact and launches the child exactly
`fuse` ticks later; `explosive projectile shotgun projectile` launches three
children over three blasts; and five nested deliveries never reach past
`budgets.maxGeneration`.

**Sustained things are the `aura` operator, one word for wards and curses
alike (plan §7; `SpellStatus`, `SpellSystem::Adopt`).** `X aura` produces an
Effect that, where it resolves, attaches X — an Effect, a Matter (sprayed), or
a Mod — to the body at the point (the owner's `SpellBodyProbe::bodyIdAt`, an
opaque id: a mob's, or the player's caster id) or to the place if no body is
there. `float aura self` is floaty; `float float aura projectile` lifts
whoever the bolt hits; `fire aura self` sprays fire from your body every tick.
The status runs every tick where the body is now and is BILLED every tick to
the caster (`SpellEmission::bills`) at the inner effect's tariff — the same
tariff as it emits, no second mechanism — and the caster's max mana shows the
per-tick sum × a 30-tick horizon as RESERVED (`CasterState::reserved`, drawn
on the bar the way the burn cap is). It ends when the caster drops it (Delete
drops the newest), when the body is gone, when they run dry (mana, then
health; when neither pays, `DropAll`), or at the hard tick cap
(`budgets.maxStatusTicks`); a caster may hold `maxStatusPerCaster` at once
and the aura beyond it is charged and attaches nothing (rule 2). A sustained
Mod acts on the body as if the body were the delivery: gravity is a per-tick
impulse the owner applies (`bodyImpulses`; the player's controller today,
mobs have no impulse seam yet), the rest have no meaning on a body and were
charged for the word.

**`null` is the op-stream filter, at the MutationQueue splice.** `W null`
yields a filter entry (one tick, unless an aura re-issues it every tick) that
refuses incoming ops of W's kind within its radius — by W's SORT and VERB,
never by name: a Matter word refuses ops and spawns of that material,
`transmute` refuses overwrite/melt ops, `explosive` explosions, `gust` winds,
a Delivery word absorbs carriers of that mech (bolts die inside a
`projectile null aura self`). The owner calls `FilterStreams` on the tick's
`ops`/`exps`/`spawns` right before `SubmitTick`, whoever produced them (the
brush, a mob, a spell — including the ward-caster's own), and the count is
shown in the HUD. The op stream, never the CA: acid already flowing still
flows, and that is the counterplay, on purpose.

**`beam` is continuous delivery; `echo` a bounded repeat.** A held beam
(`SpellBeam`) follows the caster's aim (`HoldBeam` every tick with the cast
key's state), marches the ray to the first solid or its reach, resolves the
payload there every tick and bills the tariff of one resolve per tick; it
ends on release, on running dry, or at its tick cap. `E echo` runs E now and
schedules it again every `everyTicks` for `repeats` in all (`SpellEcho`),
priced up front as repeats × E.

Gate `spells` check (6): an aura attaches, bills every tick, reserves, caps
per caster, drops on request and runs out at the tick cap; a beam resolves
every held tick and is gone after release; an echo fires exactly `repeats`
times; a ward refuses a convert op inside its radius at the splice, passes a
paint op, and is gone the next tick.

**Mend is the graft loop, and the anatomy `.vox` is the recipe (plan §7;
`Mob::RestoreVoxels`).** `M mend` is an Effect-operator with a left Matter
slot: where it resolves it takes up to `perTick × n` voxels of M from within
its radius — each leaves the world as a `convert(cell→air)` op filtered to M,
so nothing else goes — and posts a `SpellRestore` (caster, M, count) that the
owner applies to the caster's body: `Mob::RestoreBody` walks the rig
root-first and `RestoreVoxels` fills the next missing cells of each LIVE limb
with M, nearest the joint anchor first so a stump regrows outward, re-derives
the collider and the brick the way a carve does, and credits hp for the
volume put back. "Missing" is well-defined because the def's prefab model is
what should be there (rebased by the drift `ReskinLimbMicro` left on
`restOffset`). The restored cell IS material M: wood burns, steel does not,
acid eats flesh and not glass. The tariff makes the anatomy's own materials
(the glyph's `native` list) cheap and everything else dear
(`foreignPenaltyMille` × `arcane(M)` per voxel). A severed limb is not
regrown: it has no lattice to fill; `mend self` finds no matter at the
caster's own body (the body is not in the grid) and mends nothing. The
starter page `heal` is `blood mend`.

**The cauterise rule is a body rule, not a spell rule.** In the bleed tick,
a wound whose EXPOSED flesh has charred is CLOSED: the budget is dropped, the
gout stops, the stump no longer tops itself up (`Mob::WoundCharred`). The
measure is the surface voxels (an open face) within 1.5 world voxels of the
wound that can char at all — bone and steel neither bleed nor burn — and the
wound is closed when a third of them are at burn stage 2. Surface, not
volume, and a third, not half, for the reason the burn cap grades by
body-surface area (`Mob::RecountBurn`): the char is inert and shields what
is under it, and charred voxels burn down to ash and leave the lattice, so a
stump in a fire has its whole outside black while its charred share of
volume converges near a third and its charred share of surface plateaus well
under one. So `fire self` on a bleeding stump chars the exposed flesh and
stops the bleeding — and so does any fire, from any delivery, and it costs
the burn. Gate `spells` check (7): the VM half over a fake mirror, the graft
on a carved creature (missing count falls by exactly what landed), and a
severed forearm's stump bleeding, then standing in world fire (the path a
sprayed `fire` takes: bare skin catches from hot cells beside it, direct
ignition is the cloth entry point), then closed while the creature lives —
at tick 811 of 1500 on the human.

**Deliveries are three mechanisms, and Mods are field edits on their record
(plan §5; `DeliveryRec`, `ApplyMod`).** `hand`/`self` are *instant* at a
point; `projectile`/`bolt`/`lob`/`orb`/`bomb` are *flight* (speed, gravity,
lifetime, bounces, pierce, seek, fuse, count, resolve radius, a trail with
its budget); `beam` is *continuous*. A Mod names ONE field and how to edit it
(`field`/`op`/`amount` in `glyphs.json`), sticks to the box that closes the
pile (rule 3), and repeating it applies the edit again — `shotgun` ×count,
`float` −1 g, `swift` ×speed, `long` ×lifetime (and ×fuse, and ×reach when
anchored), `wide` ×radius, `bounce`/`pierce`/`seek` +1, `fuse` +30 ticks.
Adding a Mod is one JSON entry naming a field. A record only HAS the fields
its mechanism means (`ModMeansAnything`): a mod that lands on one without the
field is charged and named in the readout rather than editing something nobody
reads, which is what makes rule 3's "every other mod on the hand is a no-op"
honest. The runtime reads the record and nothing else: a bounce reflects the
axis that entered the solid (each axis probed alone) and loses a fifth of the
speed; a pierce passes through one wall and resolves on the next; a fused bolt
rests where it landed and counts down; a seeking bolt turns toward the nearest
target the owner names (`SpellBodyProbe::nearestTarget`, integer steering
after one float→fixed conversion at the query boundary). A gravity Mod on an
anchored delivery is reported as `casterImpulseVps` for the owner to apply to
the body — `float self` hops. `split` is GONE: `explosive projectile shotgun
projectile` says what it did and says it better.

**A bomb is a rigid body through the existing debris path.** The VM cannot
create a body (that is physics, the owner's business), so `bomb` reports a
`SpellBodyRequest` (centre, velocity, radius, the delivery glyph's `material`)
and the owner makes a Jolt sphere with a voxel ball, adopts it as ordinary
debris — it falls, rolls, settles, burns, can be blown apart — and hands the
handle back through `SpellSystem::AdoptBody(token, handle)`. Each tick the VM
asks `SpellBodyProbe::bodyAt` where it is, lays its trail as it rolls (the
trail mod runs on the record regardless of speed, so a `fire trail bomb` lays
fire down the slope), and when the fuse runs out resolves the payload WHERE
THE BODY IS and reports the handle in `bodyDone` for the owner to remove. A
body that is gone before its fuse (something blew it up) resolves where it
was last seen; a request the owner never adopted resolves at the hard tick
bound. Bombs share `maxLiveProjectiles`.

**`self` from the character screen resolves at the clicked part.** The health
inspector's limb rectangles become targets while a sentence is on the stack
(`InspectCastPicks`); the click latches only a body slot (`castAtPart`), and
`main.cpp` turns the slot into the limb's world transform and calls the same
`Cast()` with `selfAt` — the effect radii clamped to the delivery's impact
radius so `fire self` on a stump chars the stump and not the torso beside it.
Nothing in the VM knows what a part is.

**Cost: you pay for voxels, not for words (plan §4; `EffectTariff`,
`PriceCast`).** Every glyph has a small fixed `word` cost. The real price is
the TARIFF on the ops the cast emits, and the lowering knows those before
anything is cast, so the HUD shows the split — word + tariff + carry — live,
and "why is this 900 mana" is answered before the click. Material value is one
integer per material (`arcane` in `materials.json`, derived from density when
absent; `MaterialDef::arcane`), not a from×to table, so hundreds of materials
stay O(N) and a modder prices a new one with one key. Spray/place cost voxels
× `arcane(M)` × `rates.place`; convert costs voxels × (`rates.convert` +
max(0, `arcane(B) − arcane(A)`)) — down in value is the base only, up is the
gap, and water→gold over a pool of thousands of voxels is the story the brief
wants told; explode costs power × r³ × `rates.explode`/1000, the one
superlinear curve per word because the WORLD effect is; wind costs footprint
× ticks; mend costs voxels × `arcane(M)` × `rates.graft`. Each Delivery
declares `carry`, a per-mille premium on the payload tariff (`hand`/`self`/
`bomb` 1.0, `projectile` 3.0, `bolt` 4.0), and instances multiply everything,
so `shotgun³` pays 27 bolts' worth and reads as a lethal number before you
commit. Sustained effects (`aura`, a held `beam`) price at zero up front and
pay the same tariff per tick as they emit.

**`anything` is priced when it lands.** The wildcard's tariff is unknowable
before the cast (the HUD shows `+ ?`): on resolve, the material actually at
the point (the centre cell stands for the volume) is read through the
`SpellProbe` over the CPU mirror, and the cast bills the conversion from it
plus that matter's value × `budgets.anythingSurchargeMille`, reported as
`SpellEmission::billOnResolve` and paid by the owner mana-first, then from the
body. An `anything transmute gold` into a gold vein is cheap; the same bolt
into a lake bills the water→gold gap for the whole resolve volume after the
fact, which is the danger the word is for.

**Imprecision degrades the product, not just the aim.** An Unstable cast's
convert ops land in melt mode (`BrushOp` mode 2, each cell to its own authored
heat product) with probability equal to the instability, per op, by
counter-based hash — so the caster who tries `water transmute gold` on a tarn
spends the pool, runs into health, and the last ops boil the water instead of
gilding it. The instability rides on the projectile to its impact. One `if` on
an existing op mode, not a new system.

Two decisions worth recording because the obvious alternative is wrong:

- **`transmute` is an OVERWRITE brush op (mode 1) with a FROM filter, not the
  laser's melt mode (2).** Melt converts each cell to *its own* authored
  `molten` product, which is exactly right for a heat beam and exactly wrong
  for "turn dirt into water", where the caster chose both ends. The from
  filter rides in the op's spare words (`_p0` = the only material it may
  replace, 0 = any; `_p1` bit 0 = the wildcard matches matter, not the void),
  zero for every other producer, so the brush and the laser are unchanged.
  `air transmute B` is a paint-into-air op; `A transmute air` is an erase
  filtered to A. Melt mode is used for exactly one thing: the share of an
  UNSTABLE convert that goes wrong (plan §4).
- **Matter under `trail` lowers to `place` (a mark), not `spray`.** A spray
  at every marked voxel would multiply the particle spawn count by the spray
  size along the whole path, and the trail budget is a voxel count: one mark
  per voxel is what it measures. Everywhere else a free Matter is `spray(M)`,
  which is what keeps the one-word spell alive.

**Casting into health makes a spell IMPRECISE, not merely expensive.** Cost ≤
mana casts normally; cost ≤ mana + health casts but spends the remainder as
health *and* wobbles the trajectory in proportion to how deep it went; cost >
mana + health runs every cast's payload at the caster and kills them. That
middle case is the whole mechanic — it makes the mana bar a *precision meter*
rather than a second HP bar — so the HUD draws mana and health on one axis with
a hard break at the crossover.

**A flight collides in three tiers of knowledge, and with bodies by ray.** The
CPU mirror covers only the 3×3×3 chunks around the player (~48 voxels), and
everything a bolt does happens past it. Reading Unknown as solid detonated
every bolt in the caster's face; reading it as passable (the 2026-09-04 answer)
meant nothing past the mirror was ever hit — a bolt fizzled at the end of its
life, and `explosive projectile` did nothing. Now `SpellSystem::Tick` asks, in
order: the mirror (the truth, one tick latent); the on-demand chunk cache
(`World::Cached`), which the flight keeps warm by requesting the chunks along
its velocity every tick (`RequestChunkFetch`, coalesced, bounded by
`kFetchPerTick`); and the ground contract `World::TerrainHeight(x, z,
world.WorldSeed())` for a cell nobody has fetched yet — so at worst a bolt
lands ON the ground, never under it and never through it. Bodies are not in the
grid at all, so one `Physics::CastRayBody` over each tick's segment
(`SpellBodyProbe::bodyHit`) resolves the cast on the first mob limb, debris
chunk or bomb it meets; the caster's own parts are rejected by ownership, the
laser's rule. Out-of-window space is still solid, per §3. The `spells` gate
fires `explosive projectile` straight down from 60 voxels up, far from any
mirror, and requires the explosion at or above that column's contract height.

**Speed is 24.8 fixed voxels per tick and is authored fractional.** The
deliveries shipped at 48–96 whole voxels a tick (144–288 m/s at 30 Hz, faster
than any arrow) and were cut tenfold on 2026-09-05: `projectile` 4.8, `bolt`
9.6, `lob` 2.4, `orb` 1, `bomb` 1.6 vox/tick (14.4, 28.8, 7.2, 3, 4.8 m/s).
`GlyphDef::speedFx` / `DeliveryRec::speedFx`; `swift`/`slow` multiply the
fixed-point value; the describe line prints m/s. Spray (`budgets.spraySpeed`)
is loose matter, not a carrier, and was not touched.

**How a carrier looks is content: the delivery's `look` block.** `shape` (bolt
| ball | orb | spark), `size`, `tail`, `tailStep`, `glow`, `color` — read into
`GlyphLook`, never by the VM, and drawn by main.cpp's sprite pass at the
float boundary: a bolt is a core with a streak back along the velocity, a ball
a core with six lobes and a short tail, an orb a breathing core with motes
orbiting the flight axis, a spark a flicker (a bomb's fuse, a resting fused
bolt). Tinted by `CastTintMaterial` when the cast carries matter, else by the
look's colour. Every resolve also reports a `SpellImpactFx` and main.cpp draws
a nine-tick shell of motes there. `kMaxSprites` is 512 for this.

Op budget fairness is explicit (`SpellSystem::kSpellOpsPerTick = 24` of the 64
`BrushOp`s, alongside `gore.bleedOpsPerTick` for mob and avatar bleeding;
magic's share is deliberately NOT tunable) and overflow is **counted and shown
in the HUD** rather than dropped silently.

**The tongue: two banks on the number row (plan §12a; `game/caster.h`).**
`1`–`0` speak bank A (slots 0..9), `Shift+1`–`0` bank B (10..19): twenty live
words, one hand, no menu. Sprint is on Shift outside magic mode and magic mode
captures the number row, so nothing collides. A slot holds a glyph OR a
grimoire page (`SlotKind`), both by NAME; the HUD strip draws two rows and
lights the bank Shift is holding. The character screen's ARSENAL is a full
column of its own (`ui/inventory_ui.cpp`): the twenty bound keys on top, then
EVERY WORD as a table with one band of rows per sort — matter, effect,
operator, delivery, mod — the sort's name in a gutter on the left, each glyph
with its sort's colour and its valence mark (`<` takes the word before it,
`><` is infix), unowned glyphs greyed with their name hidden (the shape of a
word you have not learned is visible and the word is not), and hover opens
the §9 info box, every field of which is read from the glyph's JSON entry
(`UIState::GlyphUI`), so the box is never wrong about the glyph and a modder's
glyph gets one free. The table is the drag SOURCE for the keys and for the
grimoire, so it is never hidden behind a mode: the first version put the
grimoire behind a toggle on the arsenal, and the glyphs it needed vanished the
moment the toggle was pressed. Every tooltip on the screen is set in the 13 px
small font, wrapped at 320 px; the screen's own 26 px face under the cursor
covered a third of the panel it was describing. `GrantAllAndBind` is the debug default; `Grant`/`Owns`
are the acquisition loop's seam.

**The grimoire: macros as saved word lists (plan §12b; `Grimoire`,
`ExpandWords`).** A page is a name and a list of glyph NAMES and page NAMES.
Speaking it pushes its expansion onto the stack exactly as if you had spoken
the words, and the six rules apply to the result — that sentence is the whole
mechanic. Pages are fragments (`hellfire projectile` and `hellfire bomb` are
both live sentences; `hellfire hellfire` merges by R1), they nest to
`budgets.maxMacroDepth` (4) with a cycle check at save time that refuses with
the reason shown (`GrimoireWouldCycle`), and an expansion is capped by the
16-word stack: a page speaks as much as fits and the HUD says so (rule 2: no
unbounded expansion, ever). A word that no longer resolves drops with a log
line and shows as `?`; the page is kept (the DESIGN §8b contract). Two ways to
make one: `=` in magic mode CAPTURES the stack to a page auto-named from its
readout (`fire2-trail-projectile`), and the character screen's GRIMOIRE panel
COMPOSES — its own panel above the pack, in a third column beside the arsenal
(when the window is too narrow for three columns it falls back to a toggle on
the arsenal, over the pack). A page list on the left (the authored starters
from `glyphs.json`'s `conjoined` block appear read-only; `heal` = `blood
mend`, `firebolt` = `fire trail projectile`, `ward` = `transmute null aura
self`), and for the selected page a name, a word row you drag glyphs and pages
into and reorder (right-click or drag out to remove), the derived readout on a
dark page in small type, the price (`?` when it depends on `anything`), and
Save / Duplicate / Delete.

**What a drop on the word row DOES, and the fact that you can see it first
(2026-09-14).** A word or a page from outside lands BEFORE the cell under the
cursor (past the last word: appended). A word already in the row lands before
the word it was dropped on — EXCEPT onto an immediate NEIGHBOUR, where the two
SWAP. That exception is not decoration: moving a word one place right is
erase-then-insert-before-its-old-right-neighbour, which lands it exactly where
it already was, so the gesture answered leftward and silently did nothing
rightward. `ctrl` turns a move into a copy (`fire fire` is a real sentence).
Every cell PEEKS at the payload (`AcceptBeforeDelivery`) instead of waiting for
the release, so the row draws the edit it is about to make — a gold caret at
the seam, both cells lit with arrows between them for a swap, a red rim and a
reason on the status line for a refusal (full page, authored page) — and the
source cell is ghosted while its word is in the air. Every mutation of the row,
from wherever it comes, pushes onto a UI-owned undo stack (`ctrl+Z` /
`ctrl+Y`, 32 deep) that is DROPPED whenever the open page changes under it, so
an undo can never paste one page's words into another. A page is bound to a key
by dragging it from the list onto the key in the arsenal — the same gesture as
a glyph — and a bound KEY is itself a drag source: onto another key the two
bindings exchange (`UIState::BindMoveIntent`, the one gesture the single-slot
bind latch cannot say), out of the panel it unbinds. The row is
described through the same `DescribeSpell` the live sentence uses, so the
panel can never disagree with the game about what a page means. Editing a
page rewires every slot bound to it, because slots hold the page's name.

**PLYR v4** appends the grimoire (pages: name + words; the twenty slots as
(kind, name) pairs) after the v3 payload and still loads v3 (an empty
grimoire, the ten bound names landing in bank A). v2 and older stay refused,
as they were. Gate `grimoire` (CPU-only, own fixtures, beside `player-kit`):
expansion equals speaking (same cast list); nesting expands to depth and
stops past it; a cycle is refused with a reason; the overflow cap truncates
and reports; a missing name drops one word and keeps the page; the v4 round
trip compares by name, a v3 payload loads with bank A intact, a truncated one
is refused; a page bound to a key speaks its expansion; capture names the
page from the sentence. `--shot-inventory` writes a third frame,
`screenshot_inventory_grimoire.bmp`, with a page selected and its word row
populated.

Selftest gates `spells` (the trail's voxel budget respected exactly and the
projectile dead with it; an overcast resolving Fatal, emitting its own payload
and asking for the caster to be carved; `fire`×N throwing exactly N times the
matter of `fire` at exactly N times the price; the cast latch; the laws; the
bomb, the sustained things, the mend), `spells-oracle` (the parser against the
reference script, every entry) and `grimoire`.

### Items, and mouse-directed melee (2026-08-20; `game/item.h`, `game/melee.*`)

A hotbar and a sword, built as the melee counterpart to the spell system: the
same division of labour (main.cpp owns the player's inventory, the systems are
player-agnostic) and the same refusal to add a parallel damage path.

**DISCRETE STRIKES ARE THE DEFAULT NOW (2026-09-01; `melee.controlMode`).**
The mouse-steer experiment below is preserved intact as `controlMode = 1`, but
what ships is `0`: a click fires an **authored stroke program** — the same
`attack_styles.json` entries the NPCs replay, new `player_*` rows with short
windups and zero jitter — through the same `MeleeState`, with the **camera as
the basis**, so every strike's mid-travel passes
through the crosshair line. Direction is picked by the **flick at the press**
(`game/strike_pick.h`, quantized against the JSON `player` sector map; a
still-mouse click alternates the two horizontals), one strike buffers during
a live cut, and a click mid-windup is dropped — the windup is the commitment.
The phase machine itself was extracted to `StepStrokeProgram`
(`game/strokes.cpp`) so MobSystem and main.cpp step the identical runner:
**one driver, three feeders** (mouse, NPC program, player program), and
everything below the input layer — blade frame, damage sweep, parry, block,
Arrest/Nudge — is shared and unchanged. Two things the port surfaced, both
now structural: the **damage sweep keys on the program's cut phase, not on
the driver's Slash commit** (a thrust drives the radial channel, which
`commitSpeed` never sees — the NPC path always worked this way), and the
whole body got two long-wanted looks passes: a **torso lean** riding
`WeaponPose` (`torsoTwist`/`torsoPitch`, applied by `AnimApplySpineTwist`
pre-flatten, so NPCs inherit it) and a **head keep-out** clamp in the driver
(`melee.headClearM`; the blade's hand→tip segment is rigidly carried clear of
the wielder's own head, in both modes). The `player-styles` gate replays
every authored style through the player path on the real rig and asserts each
style's claims from its own authored numbers, head clearance included.

**THE AIM IS A POINT, NOT A DIRECTION (2026-09-16).** The strike tick used to
pass `(az 0, el 0, dist 0)` — "the camera IS the aim" — which is true of a
DIRECTION and false of a BLOW. A stroke is a bearing about the **arm's own
pivot**, and a shoulder sits ~2 voxels under the eye and ~2 to the side, so a
bearing copied from the camera sends the weapon down a line *parallel* to the
crosshair and a whole shoulder offset off it: a few degrees at sword reach,
and a collarbone instead of a head at arm's length. It now resolves a **point**
— nearest of the first dynamic body down the crosshair line (the rig's own
limbs excluded, for the reason the E-prompt ray excludes them), the first
solid voxel marched on the CPU mirror, or a fallback 40 voxels out that
reproduces the old camera-parallel aim to within a few degrees, so open air is
unchanged and the two cases meet continuously — and takes its bearing through
`StrokeAimAt` about `Mob::StrokePivotWorld`, both extracted from the pair
`MobSystem::StepStroke` was already doing by hand. The ray is
`player.EyePos()` + `cam.Forward()`, the brush's own pair, so the third-person
boom cannot move where a strike lands. The distance is real now too, which
arms the cut-radius clamp (`StepStrokeProgram`'s `toTarget`) that stops a fist
sailing past a chest two voxels away — it had never fired for the player.
`player-unarmed` gained an aim pass, a **differential**: one punch twice at
one fixed point, aimed and camera-parallel, scored as the closest the knuckles
came — 0.58 vox against 1.82.

**The pose is the hitbox.** A swing does not switch on a hitbox during an
animation window and it does not test a cone in front of the crosshair. The
blade's authored `edge` segment is read through its **live** transform and
swept from where it was last tick to where it is now; whatever that quad passes
through is cut, at the point it was crossed. So the location struck is the
location that loses voxels — which is only worth saying because §7's live-limb
carving already made "lose voxels *there*" expressible. Melee adds no gore
code: it reaches flesh through `Mob::CutLimb` / `BluntHit` / `BiteHit` and
debris through `MeltBodyAt`, and every one of those ends in the same
`CarveLimb` / `CarveLimbRadial` the laser and the blast already call.
Dismemberment stays geometric (a limb comes off when the cuts disconnect it,
not when a counter hits zero) — and a blunt blow refuses even that, see
*Damage kinds: cut, blunt, bite*.

**The mouse is the swing.** Holding the attack button hands the weapon arm to
the mouse, and past a speed threshold the blade commits a cut along the
direction actually moved — a diagonal flick is a diagonal cut. Damage scales
with the blade's measured tip speed, so the player's own motion is the damage
roll: no cooldowns, no swing timer, no randomness, and being slow is punished
by being ineffective rather than by being locked out. The HUD prints the phase
and the measured mouse speed, because an input this analogue has to be
*falsifiable* — the player needs to tell "the game misread my flick" from "I
misjudged the distance".

**The mouse is a HAND, not a pointer** (rewritten 2026-08-30). What the button
buys is *incremental control*: the arm keeps the pose the animation had it in,
and from then on each mouse pixel is a fixed amount of travel, **integrated** —
so the blade stays where it is put, a slow drag reaches as far as a fast one,
and the only bound is the arm's own reach (read off the rig's bone lengths, not
a constant). Two things follow that are worth stating because the first version
had neither:

- **Taking over an arm starts where the arm is.** `Mob::WeaponStrokePose` is the
  exact inverse of `SetWeaponPose`, so the driver reads the live hand AND the
  live point and hands the same values straight back: the first driven tick asks
  for the pose the blade is already in, weight can go to 1, and nothing moves.
  The original mapped mouse *velocity* to a lean off a fixed guard pose, so
  clicking teleported the arm to that pose — reported, accurately, as "the
  moment I click the arm shoots to the top right".
- **A velocity map has no memory.** With the pose a function of how fast the
  mouse was moving *this instant*, the blade could not be aimed and parked, any
  real flick saturated the clamp, and letting go dropped the arm back to guard.

**The mouse steers the POINT, not the fist** (rewritten 2026-08-31). Integrating
into a *hand position* fixed the input and left the geometry wrong. A hand
offset is three numbers fed to a two-bone solver with a fixed pole and a
strict-hinge elbow: shoulder roll, forearm pronation and the wrist have no
channel at all, and the blade's own orientation was computed, passed, stored and
then deliberately never applied. The sweep plane was therefore whatever the
elbow's authored hinge happened to allow, and pushing the mouse forward came out
as an elbow jab. The control surface is now the **blade tip on a reach surface
around the shoulder**, in the camera's frame:

    mouse x  ->  AZIMUTH of the point     mouse y  ->  ELEVATION of the point
    a separate bounded RADIAL channel  ->  thrust and draw-back

and everything else is derived from it. **Both mouse axes are angular on
purpose**: spending one on reach is what made forward mouse a jab, and a
right-to-left drag stops being a flat arc the moment the vertical axis does
double duty. The hand is the point minus a blade; the blade's angle to the
shoulder-to-point line is a **law of cosines**, not a taste constant, so the
derived hand is always somewhere the arm can be (a fixed lean put the hand at
the shoulder — this sword is longer than this arm). The flat of the blade faces
out of the stroke plane, so the edge leads the cut, and the damage sweep scales
by that alignment with a floor, so a flat still bruises.

**The arm serves the blade.** The weapon arm's IK is handed the driver's own
bend pole, built from the HAND's travel — a plane is only defined relative to
the chain that bends in it, and the point's tangent is a different direction
entirely for a long blade. The elbow stays a one-degree-of-freedom hinge with
its authored 0..130 range; what is steered is which PLANE that freedom lives in
(`anim.h` `PoseAxisOverride`), which is what shoulder roll buys in a real arm
and is invisible on a near-cylindrical upper arm. A horizontal cut then reads as
shoulder rotation plus elbow extension in the horizontal plane.

**The blade is steered by the WRIST, never by rotating the held part.** The
sword is a rigid child of the fist and rides the existing
`itemQ = handQ * gripRot` composition, so the only way to aim it is to aim the
hand. An early version re-aimed the held part directly; it looked like the sword
swivelling in the fist, and because the override wrote into the flattened pose
it also fought the animation pipeline and widened the walk until the legs failed
their own upright assertion — a "leg bug" whose cause was the thing the
character was holding.

**...and the wrist RAMPS with commitment** (2026-09-01, four owner-reported
regressions). The stroke always commands the blade along its law-of-cosines
direction, which is mostly radial — correct for a cut, and wrong for a hold,
because it wrenches the fist round to lay the sword along the shoulder-to-point
line whatever the player is doing. A stroke seeded from a hanging arm then had
that radius aimed at the floor, and holding a guard pointed the sword straight
down. So `WeaponPose::steerAmount` (0..1) throttles the applied alignment on the
tip's own speed: still is the authored GRIP pose, moving is full alignment, and
**only a committed Slash bypasses the ramp** (2026-09-01; Wind and Recover used
to as well, which made raising the sword for an overhead wrench the wrist round
to point the blade at the cursor — a raise IS Wind). The DRIVER owns it (it
knows the phase and the speed) and the RIG applies it, the same seam
`WeaponPose::wristMaxAngle` already crosses. Take-over stays continuous because
`SetStroke` re-seeds from the rig's actual blade every tick.

**Each joint smooths on its own clock** (2026-09-01). `melee.armSmoothing` eases
the integrated stroke (az/el/radius, follow-through included) before the tip is
built, so hand, bend pole and blade lag together as one rigid assembly;
`melee.wristSmoothing` owns the wrist's commitment envelope and its chase of the
commanded blade orientation (3x faster through a Slash, because edge alignment
is damage). Split because the joints tolerate lag differently: a lagging arm
reads as weight, a lagging wrist mid-cut costs the cut. The exact blade frame
still places the hand — the eased copy is only what the rig's wrist chases — so
`|tip − hand| = bladeLen` holds whatever the knobs say. And **the hand may not
go behind the body** (`melee.handBackFrac`): the azimuth window bounds the
commanded point, but the hand is that point minus a whole blade plus the lean,
and unbounded it sat voxels behind the shoulder's frontal plane at the stops —
the arm visibly behind the torso. The clamp holds the hand at the plane and
re-aims the blade at the commanded point from the clamped hand.

**World→rig is a pure un-yaw** (2026-09-01). `Mob::ApplyWeaponArm` carried an x
negation ("the rigs are authored mirrored") from the overhaul until it was
worked through on paper: camera-right un-yaws to model −X, which is where the
`.R` limbs are authored and what the raymarcher draws screen-right, so the
negation was a net MIRROR — mouse-right drove the rendered arm screen-left, and
azOut's generous weapon-side window was spent across the body. No gate saw it
because `WeaponArmPose` carried the matching inverse: every round-trip probe was
self-consistent whichever sign the pair had. The one in-code tell was the
ledge-hang solve, which un-yaws world palm targets with no negation and lands.

Two facts underneath it are worth keeping because both were got wrong first:
the GRIP is `[0,-90,0]` — blade out of the fist, perpendicular to the forearm,
and world-up when the arm is forward — and the along-the-arm `[0,0,-90]` grip
that replaced it briefly bought nothing (the wrist ask is 2.6..3.1 rad either
way) while costing the idle pose, which is what the player looks at for most of
a session. And **"the elbow bends both ways" is a fact about the POLE, not
about the hinge axis**: a two-bone solve has no backwards, since an elbow
"bending the wrong way" in plane P is the same arm SHAPE as one bending the
right way in the plane rotated by pi. What is wrong is which side the elbow
bulges to, so the bound is on the bend plane (`MeleeTuning::elbowPoleCone` —
down, up or out, never forward) and the hinge-axis override goes on reporting
the solve plane faithfully, which is what keeps `ClampHinge` from discarding
real solve.

**One driver, two consumers.** `MeleeState` consumes abstract control deltas
(`Feed(dx, dy)` / `FeedReach(dr)` / `Step(StrokeSample)`), not a mouse, and
`Mob::ApplyWeaponArm` is shared by the avatar's animation pass and the NPC one.
An authored attack is the same driver replayed from a polyline of deltas, so a
mob swings with the player's arm rather than an approximation of it.

**One sweep, one kerf, three callers** (integrated 2026-08-31). The damage half
of this system is `MeleeSweepDamage(EdgeSweep, MeleeTuning, wielder, ...)` in
`game/melee.cpp` — lifted out of main.cpp's tick loop the moment an attacking
NPC and a gate needed the same code the player runs. It is also the ONLY place
a `BladeCut` is built: the sweep finds what the edge passed through, and where
it used to call `CarveLimbRadial` it now fills the kerf described in the wound
model section above and calls `Mob::CutLimb`. Keeping that construction inside
the sweep rather than at each call site is deliberate — the player's cut, an
NPC's cut and the gate's cut are then the same cut by construction, and there is
no second copy of the depth formula to drift. Since 2026-09-15 the sweep
resolves THREE parts rather than one — `EdgeSweep::damage` became
`EdgeSweep::strike`, a whole `StrikeProfile` — and the kerf is only the first of
them.

The two halves meet at exactly one number. `MeleeSweepDamage` forms
`power = speedRamp * MeleeEdgeAlign(...)` once, and everything downstream reads
that value: the damage, the `BladeCutScope`, and the kerf's `depth` and
`length`. So a flat-on slap makes a shallower wound as well as a weaker one, for
free, and the wound model never has to know that edge alignment exists.
Multiplying `edgeAlign` in a second time when the kerf is built would square it.
`EdgeSweep::heft` carries the weapon's `ItemDef::HeftFactor` (default 1, so a
gate that only wants the geometry needs no item) and `EdgeSweep::tick` seeds the
wound's counter-based RNG.

`--gate swing` covers the mapping (CPU-only, milliseconds, its own fixtures);
`--gate swing-plane` drives the same driver through the real rig and asserts on
the sword's own world trajectory; `arm-readback` inside `--gate mob` covers the
inverse against a target the test chose, since a dropped yaw or a flipped x is
invisible on a rig standing at heading 0 and mirrors the answer.

**A held item is a rig part, not an object.** Equipping BORROWS A RIG SLOT: the
item's own geometry fills a real `MobLimb` parented to the socket's limb, so
while worn it is a rig part in every respect -- severable, non-vital, cheap to
knock loose. A dropped sword, a severed sword-arm, a burnt sword and a carved
sword are therefore all things the existing systems already do, and `ItemDef`
stays a name plus a behaviour kind plus its OWN `.vox`.

*(Corrected 2026-08-29. This paragraph used to say the sword was a part of the
avatar's own `.vox`, which is how it worked when it was written and stopped
being true when items became standalone assets -- `assets/items/sword.{vox,json}`
and the borrowed-slot note at the top of `game/item.h`. The distinction is
load-bearing rather than pedantic: it is exactly what stops a weapon inflating
the creature's own box, which is the trap recorded two paragraphs down.)*

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

  **A SLOT BASE IS WHAT THE WALK EMITTED, NEVER WHAT THE SYSTEM CONTAINS**
  (`game/bodyreg.cpp`, 2026-09-16). Three parallel arrays are indexed by body
  slot — the transforms, the cube instances and the compacted micro-body draw
  list — and an instance is `(slot, model)` drawn at `bodyXforms[slot]`, so a
  base that is one too high does not lose a body, it SWAPS two. The mob base
  used to be `DebrisSystem::BodyCount()` while all three debris walks stop at
  `kMaxBodies`, and `AdoptBody` takes no cap — a corpse hands over fifteen
  limbs at once and `bodies_` sits past the ceiling until the next `PostStep`
  cull. `DebrisSystem::SlotCount()` is the emitted count and is the only legal
  base; the mob walks return their next free slot for the same reason.

  **A BRICK RECORD HAS EXACTLY ONE HOLDER**, and handing a limb to
  `AdoptBody` means the slot forgets the index, not merely the ownership flag.
  `Mob::DetachLimb` and `Mob::Die` clear `carved` AND `microModel`: the slot
  outlives the hand-off (it holds the kinematic piece for the sever beat, and
  the avatar keeps its whole limb list past death), so a slot that kept the
  index is a second holder of a record `DebrisSystem` now owns — and
  `MicroBodyOwn` returns an already-owned record to whoever asks, so one later
  reskin starts editing the corpse's brick. Both failures are asserted every
  tick by `BodyRegistry::AuditMicroModels` + `BuildMicroInsts`, which name the
  two entities rather than counting; the `limb-alias` gate is the fixture that
  makes them fire (three duels, audited per tick, faults 15 -> 0).
### A creature knocked down gets back up: the live ragdoll (2026-09-09; `Mob::StartRagdoll`, `sim/tuning.h` Ragdoll)

Until now the only ragdoll was death. `Mob::Die` flips every limb dynamic and
hands it to `DebrisSystem::AdoptBody`, and there is no path back from that —
so an explosion beside a living creature could carve it (`CarveMobsRadial`)
but never MOVE it, because `Physics::ApplyRadialImpulse` walks the dynamic
body list and a living limb is kinematic. Blasts tore chunks off people who
stood there and took it.

**The live ragdoll is the same flip with the limbs kept.** `Mob::StartRagdoll`
makes every owned limb dynamic in place — joints stay, the intra-mob collision
group stays, `limb.body` stays the mob's — and sets `RagdollPhase::Limp`. The
NPC loop (`MobSystem::PreTick`) and the avatar driver (`PlayerAvatar::PreTick`)
then run nothing: no sense/intent/steer/drive, no animation, no submit.
`PostStep`'s read-back already places a dynamic body, so rendering, bleeding,
burning and carving all keep working on a limp creature because none of them
ever asked whether a limb was kinematic (`RebuildLimbBody` gained the one
exception: a carve mid-ragdoll rebuilds a DYNAMIC limb). Three things reach it
and nothing else: a blast (`Mob::BlastRadial`), freefall past
`ragdoll.fallSeconds` (NPC: `MobSystem::UpdateFall`; avatar: its own air
clock), and the dev panel ("ragdoll me"; "ragdoll all spawned" in the NPC AI
window). An NPC's limbs go through `ReleaseToWorldWhenClear` on the flip, for
the reason `Die()` documents — a body that goes dynamic inside the player's
capsule otherwise fires out of it.

**The launch is a velocity, not an impulse, and it is capped.** A blast's
other half runs beside the debris impulse in the explosion loop:
`MobSystem::BlastMobsRadial(ec, radius × blastRadiusScale, power ×
blastImpulseScale)`. Per creature: impulse at the pelvis by linear falloff,
divided by the rig's Jolt mass (`Physics::BodyMass`, readable on a kinematic
body), gives a speed in m/s; below `blastMinSpeed` nothing happens (a distant
boom rattles, it does not floor you), above `maxLaunchSpeed` it is clamped —
that clamp is the "across the room, not across the map" rule, so a massive
charge still tops out at 14 m/s (~20 m at 45°). The direction is
away-from-the-blast with `blastUpBias` of straight-up mixed in so a floor
blast arcs the body rather than skidding it. A grenade (power 380) sends a
~70 kg human about 5 m/s; mass comes from material density, so a heavier
creature flies less far from the same charge with no per-creature number.

**...and it TUMBLES, without any limb being launched on its own
(2026-09-11).** That whole-body speed used to be set UNIFORMLY on every limb,
because Jolt's per-body `AddImpulse` gives a hand ten times the velocity of
the torso and then the joints do the launching, badly — so a body floated away
from an explosion still standing to attention, whatever the charge was
standing next to. Both halves are now true at once. Each limb is measured at
its own CENTRE OF MASS (`Physics::BodyCenterOfMass`, not `GetTransform`'s pos,
which is the lattice's origin corner and on a thigh sits up at the knee) and
gets its own falloff and its own away-from-the-charge line, blended toward the
whole-body ones by `blastLimbBias`. The falloff enters as a RATIO against the
rig's mass-weighted mean, so the bias redistributes the launch instead of
adding to it — the mass-weighted mean scale is exactly 1 whatever the geometry.

Those per-limb velocities are never set. They are collapsed into the one
motion a jointed body can actually perform: the linear momentum they add up to
(`vCom = Σ m v / M`) and the spin their angular momentum implies about the
centre of mass (`ω = L / I`, with `blastSpinGain` correcting for a point-mass
`I` that leaves out each limb's own inertia, capped at `blastMaxSpin`). Every
limb is then set to `vCom + ω × r` and to the same `ω`. No constraint is
violated, so the tumble survives the first solver step intact instead of being
resolved away — and a charge at the ankles rolls the body one way while one
over the head rolls it the other. Measured at the stock 0.35 / 0.65: ±0.6
rad/s from the X-detonate charge, limb speeds spread 1.12x from slowest to
fastest. The gate arm is `ragdoll blast spin`, which runs both heights inside
one fixture so the claim is a reversal and not a number from another world.

**The tumble redistributes the launch; it never adds speed.** An outflung limb
carries `|ω × r|` on top of the centre's, so the whole rigid motion is scaled
(both halves together — it stays rigid) until no limb exceeds what the uniform
launch would have given it: ~0.94 at the stock gain, invisible on screen. The
launch ceiling is a statement about the creature, not about its centre of mass.

**Known, in-suite only: `ragdoll repeat blast` fails inside a full
`--selftest` and passes under `--gate ragdoll`.** That arm's subject settles
for 45 ticks in whatever world the gates before it left and reaches the first
charge at 15 of 15 limbs standalone but **12 of 15 (44.9 kg vs 52.4) in
suite** — and a charge 6 voxels away is close to lethal for the smaller one.
The flat launch left it alive by a hair (travel 109.4 against a 120 ceiling,
91% of the band); with the tumble it loses a vital limb on the launch tick and
dies, so the second-blast claim has nothing left to measure. The spin adding
speed is NOT the mechanism — capping the fastest limb at the uniform launch
speed (above) leaves the death exactly where it was. The arm now prints the
limb count it started with, the death tick and the death cause, and the pelvis
travel is measured to the last position the creature was ALIVE at, because a
dead mob's position probe answers from an empty list (that is where "the
pelvis moved 583 voxels in 1.3 s" — 56 m/s against a 30 m/s cap — came from).

**NPCs fall now.** The walk drive snapped `origin_.y` toward the probed ground
at 3 cm a tick and, with none within the 2.4 m scan, returned early — a
creature over a drop HUNG. `UpdateFall` runs before the drive: supported
(ground within a step) means the snap owns the height as before; further than
a step, or with nothing in reach, the creature falls under `physics.gravity`
(the number Jolt applies to its limbs, so a body that goes limp mid-fall keeps
its speed) and lands on the surface the probe reports. "I cannot see the
ground" is a third state (`GroundSense::groundUnknown`, from a new out-param
on `GroundHeightAt`): gravity waits on an unfetched column rather than
dropping the creature through terrain the mirror has not delivered — the
projectile trap in CLAUDE.md, mob edition. The gait is off while airborne
(it would IK the legs to the floor being fallen toward).

**...and a body can be THROWN, which is the lunge** (2026-09-15; `Mob::Launch`,
`AttackStyle::lunge`). Freefall used to be vertical only — "an NPC has no planar
velocity state, and a body that needs to fly is a ragdoll" — so "jump at them
and bite" had no expression at all: the walk drive resolves a whole step against
the body's box every tick and cannot leave the ground, so a zombie either stood
in reach or did not. `Mob::Launch(vel)` sets `airborne_`, puts the vertical on
`fallVel_` and keeps the horizontal in `airVel_`, which `UpdateFall` integrates
by the SAME rules the walk drive travels by — `FootprintFooting` against the
body's own box, another mob counted as a wall, each axis tried alone so a leap
that grazes a corner slides along it — because otherwise a pounce is the one way
in the game to get inside a rock. Hitting a wall in mid-air spends the planar
velocity and the body drops where it is. `launched_` is a one-tick latch: a
lunge is fired by `StepStroke`, which runs AFTER `UpdateFall`, so the first
`UpdateFall` to see it still has the body on the ground it left from — without
it a flat pounce has `fallVel_` driven negative by one tick of gravity before it
has moved anywhere, the landing test fires, and the creature twitches and stays
put.

A style fires one at the FIRST tick of its named phase (`"windup"` by default,
so the leap IS the telegraph), once per stroke. The authored `speed` is a
CEILING and not the magnitude: the horizontal is
`min(speed, (distance − reach) / flight time)`, so the same style is a long leap
from far out and a short hop from close in, and the body arrives at striking
distance rather than inside the victim. The `reach` there is the EFFECTOR's —
the arm or neck the driver is already bounded by — and not the style's, which
means how far out the creature is willing to COMMIT: measured with the style's
own 22, every launch computed `dist − 22 <= 0`, capped the horizontal at zero
and produced a creature that hopped on the spot. The loco state scales it
through `AnimStateRule::lungeScale`, which defaults at load to that state's
`speedScale` rather than to 1 — a body dragging itself on its elbows walks at a
fifth speed but can still throw itself half its own length — so a crawling
zombie pounces low and short from one number in its own state rule. Lining the
landing up with the cut is the AUTHOR's job. Gated by `lunge`: a zombie at twice
its reach leaves the ground, closes real distance and lands with the cut still
live; a crawler does the same lower and shorter, and neither sinks into the
ground it travels over.

**The get-up is procedural and nothing is authored.** `TickRagdollLimp`
watches the pelvis: past `minSeconds`, once it has moved slower than
`settleSpeed` for `settleSeconds` (or `maxSeconds` have passed and it is at
least not flying), `BeginGetUp` re-derives the creature from where it lies —
heading from the way the chest faces, or the way the head points if the chest
faces floor or sky; `origin_` from "where would the standing pelvis be for a
min corner here" solved through the same `LimbTargetFor` arithmetic
`SubmitPose` uses; height from the ground probe under that footprint — makes
every limb kinematic again exactly where it is, records that pose
(`getUpFrom_`), and re-plants the feet. `SubmitPose` then blends each limb
from the recorded pose toward its animated target with a modifier on the
body frame: the target starts as a CROUCH (pitched `getUpPitchDeg` forward
about the feet, hips down `getUpDropFrac` of the standing hip height) and
straightens over the back three quarters of `getUpSeconds`, while the
per-limb blend completes by 55%. So the limbs gather under the body into an
on-hands-and-knees shape, then the shape rises — a get-up rather than a corpse
levitating upright. The player's capsule is a passenger throughout
(`PlayerAvatar::RagdollFollow`): `main.cpp` skips `Player::Update`, rides the
pelvis while limp, and sits on the get-up's standing spot so the controller
resumes exactly where the animation ends. That is also why a body thrown by a
blast takes no fall damage on landing from the CONTROLLER — the controller
never saw the fall.

**A limp body still hits the ground (2026-09-12).** That last sentence used to
be the whole story, and it made a long fall SAFER than a short one: fall 2.9 s
and the sweep splatters you, fall 3.1 s and `ragdoll.fallSeconds` flips you limp
first and you land for nothing. The missing measurement is the same one
`Player::impactDeltaV` is — A SUDDEN DECELERATION — taken where it still exists,
off the solver rather than off a sweep: `Mob::TickRagdollArrest` differences the
rig's mass-weighted centre-of-mass velocity tick over tick, subtracts the gravity
step so free flight reads as zero, and keeps only change that OPPOSES the travel,
so neither gravity nor a blast (which SETS the velocity) can be read as a
landing. `PlayerAvatar::PreTick`'s limp branch hands it to the same
`ApplyFallDamage` and the same `player.fallDamageSpeed` / `fallSplatSpeed`
thresholds the driven branch uses. Three things about it are not obvious and all
three were found by measurement, not design:
- **A rig does not stop in one tick.** The player's AABB sweep refuses the whole
  velocity in the frame it meets the floor; a ragdoll meets it with its feet and
  the deceleration reaches the other fourteen bodies through the joints. A
  33 m/s landing showed 11 m/s in its worst single tick and the avatar walked
  away from what should have been a splat. So the arrest is summed over a RUN of
  consecutive braking ticks (≤ `kArrestRunTicks`, each above a floor that
  `debrisLinearDamping` cannot reach) rather than peak-held per tick.
- **A body cannot lose more than it arrived with.** Unbounded, the run goes on
  billing while the rig FOLDS — gravity presses it into the floor for as long as
  the joints take to collapse and the floor goes on refusing it, so a 15.0 m/s
  landing summed to 21.2 m/s. The run is clamped to the speed the rig had on the
  last tick nothing was touching it, which is exact and has no tuning in it.
- **One landing, one bill.** A rig hits the floor twice (legs, then torso): two
  events of 14.8 and 15.0 m/s were billed separately for three times what the
  same speed costs a walking player. The peak is HELD until the rig has stopped
  braking for `kArrestSettleTicks` and handed over once.
NPCs are deliberately unchanged — they have no fall-damage path of their own.

Every number is CPU-only float in the `ragdoll` tuning group (F5, no shader,
no rebaseline). Gate `ragdoll`: the X-detonate charge 6 voxels from a dummy
knocks it limp and moves its pelvis within a `tests/baseline.json` band in
1.5 s; it is back on its feet within `ragdollGetUpMaxTicks` with every limb
still its own and no body adopted by `DebrisSystem`; and a dummy spawned 2.2 m
up descends under gravity, goes limp past a fixture-short `fallSeconds`, and
stands up on the ground. Gate `ragdoll-falldamage` is the avatar half and is
THREE arms on purpose: a 15 m/s limp landing costs health and is survived, one
above `fallSplatSpeed` kills, and 0.4 s of upward flight at the same speed costs
nothing — the last is what fails if the deceleration test, the cap, the
settle-hold or the `SetLimbVelocities` reseed goes, and a fix that killed the
player for being in the air would be worse than the bug.

**One open finding the gate prints and asserts nothing on (2026-09-11).** On
the tick the burning avatar DIES, one of its own limbs can be on
`Layers::MOVING` deep inside the capsule while its siblings are still
correctly queued in `ReleaseToWorldWhenClear` — measured at 12.83 kg and 3.25
voxels in, one shove of 6.25 voxels, with five limbs still pending. It is not
the burn (the 265 living ticks before it push 0.00), it is phase-dependent
(three neighbouring tick phases never see it), and it clears the 5%-of-player
mass filter by design: a corpse's torso is meant to be able to shove you, just
not by a third of a metre in one tick. The avatar-on-fire arm's sample window
now ends at death, where its claim always was, and the death-tick shove is
printed on its own `post-mortem` line every run. Found by a blast-launch
retune that moved this arm's tick phase, not by anything that changed it.

**"Bodies zoom across the map" and "burning clothes launch me" (2026-09-10).**
Both reports arrived together after the ragdoll landed, and neither was the
launch clamp. The gate above called `BlastMobsRadial` on its own and so never
saw the rest of the explosion block; it now runs the block verbatim (`ragdoll
repeat blast`) and puts the player avatar on fire with the capsule proxy in
the world (`ragdoll player on fire`). What it found, in order of damage:

- *The carve's gobbets rammed the rig.* A blast big enough to carve
  (`explosionBodyDamageScale`) makes `EmitCarvedFragment` bodies of 0.05 kg,
  born inside the limb they came off. The per-body debris impulse then gave
  each one `impulse / mass` = 1000 m/s (Jolt caps at 500), and the same tick's
  launch had just flipped the rig dynamic, so they hit it. Measured: a carved
  upper leg at 41.8 m/s on the blast tick, 449 voxels of travel. Three
  elimination arms inside the gate (no carve / no impulse / no crater) named
  it in one run. `physics.explosionMaxSpeed` (30 m/s) bounds the SPEED the
  impulse may give any one body — a 2.5 kg stone voxel takes 20 m/s from the
  X-detonate charge and is untouched; only the confetti is.
- *A limp rig took the per-body impulse limb by limb.* Live limbs are in
  `dynamicBodies_` whether kinematic or not; standing they were skipped by
  Jolt's `IsDynamic` check, limp they were not, and the launch stacked on top.
  `Physics::ApplyRadialImpulse` takes a skip list (`Mob::AppendLiveLimbBodies`,
  every body a living creature still owns) and `BlastRadial` holds the stacked
  velocity under `max(maxLaunchSpeed, what it already had)`.
- *A carve while limp rebuilt the limb wrong twice.* `RebuildLimbBody`
  re-read Jolt's transform, discarding the rebase shift `ReskinLimbMicro` had
  just applied to `xf.pos`/`anchorLimb` (harmless on a kinematic limb, which
  is re-posed next tick; a dynamic one had its joint anchor one shift off and
  Jolt closed the gap by force), and the new body started at rest with no
  layer. It now keeps the carve's frame when limp, carries the old velocity,
  and `Physics::CarryLayer` carries the object layer plus the
  `ReleaseToWorldWhenClear` entry — the same helper `ReplaceBody` and the
  debris split now use, because a burning gobbet that shrank was rebuilt on
  the plain MOVING layer inside the player every time.
- *`PlayerPushOut` summed one hit per voxel box and took orders from
  confetti.* A gobbet born beside or above the capsule is released to MOVING
  at once (it does not overlap the proxy's AABB), the avatar's own kinematic
  arm sweeps it INTO the capsule, and `CollideShape` reported it as eight hits
  of the same depth, summed: 62 voxels in one tick, 432 before the player
  burned to death. The push is now the deepest hit per body, and a body under
  5% of `playerMassKg` cannot push the player at all (the 80 kg proxy pushes
  it instead). Standing on a log or being shoved by a corpse is unchanged.

### A held weapon is CARRIED, not simulated (2026-09-15; `Layers::PROP`, `Physics::SetBodyPropLayer`)

Reported as "holding a sword moves other mobs, walking into an inactive sword
moves me, fights feel clunky". Both halves are one mechanism, and it is not a
tuning number: **a held item is a rig limb** (`game/item.h`, "A BORROWED SLOT"),
so `Mob::EquipItem` gives it a Jolt body like any other limb — kinematic,
jointed to the hand, and until now on the ordinary `MOVING` layer for everyone
except the player.

A kinematic body cannot be moved BY a contact, only THROUGH one. So the held
weapon's contacts could never do anything except move other things:

- **Toward the player.** A kinematic limb reports its whole rig's mass, so an
  NPC's drawn blade sailed past `PlayerPushOut`'s `kPushMinMassFrac` gate —
  the one that exists so a body you could kick aside cannot move you — and
  depenetrated the capsule every tick it overlapped. Standing at sword's reach
  of an armed NPC shoved you off your feet.
- **Away from it.** The avatar's own sword was exempt from the player
  (`AVATAR`) but from nothing else, so a swing swept every DYNAMIC body it
  passed through: corpses, limp ragdolls and loose debris punted aside by a
  weapon you were merely carrying. Measured on bare bodies in `player-body`: a
  kinematic block dragged past a 10 kg sphere moves it **23.24 voxels** as an
  ordinary body and **0.00** as a prop.

`Layers::PROP` is the whole fix, and it is `AVATAR`'s argument taken to its
end — **the split is about CONTACTS, not about VISIBILITY**. It collides with
nothing at all, including another prop, and it stays in `DynamicLayerFilter`,
so every query still sees it.

**NOTHING IN COMBAT WAS ROUTED THROUGH THOSE CONTACTS**, which is why this
costs the swing nothing and is the part to check before touching it again:
- damage is ray probes tiled down the blade's own axis
  (`MeleeSweepDamage` → `CastRayBody`), and it is the TARGET's limbs those
  rays need, never the swinging weapon's collider;
- a parry is decided GEOMETRICALLY by `MobSystem::FindParry` on the two edge
  segments — `npc-block` already says in as many words that the collider
  cannot answer it, being a quarter of a voxel thick;
- melee applies **no impulses anywhere**. The only "force" a hit carries is
  `impactSpeed`, which feeds `severImpactSpeed` and the kerf depth.
- `Mob::WeaponEdge` needs the body's TRANSFORM, which is why this moves a body
  between layers rather than removing it.

**The flag is not set once.** A prop that stops being carried must come off the
layer or it falls through the world, and a prop that resumes being carried must
go back on:
| Path | What it does |
|---|---|
| `EquipItem` | sets it |
| `RebuildLimbBody` | re-applies it — a weapon is carved and burned like any other slot, and a rebuilt body starts on the default layer (the same trap `AVATAR` hit from every damage source; see the note there) |
| `StartRagdoll` | clears it, since the line above just handed the limb to the solver — and hands the AVATAR's back to `AVATAR` rather than letting it fall to `MOVING` inside the player's own capsule |
| `BeginGetUp` | sets it again, or a creature knocked down once would swing a shoving blade forever after |
| `Die` / `DetachLimb` / `TickSeveredHolds` / `DropItemToWorld` | already set the layer outright (`ReleaseToWorldWhenClear`, `AdoptBody`), so letting go needs no new code |

Asserted in two halves, deliberately: `player-body` pins the BEHAVIOUR on bare
bodies (push 4.00 → 0.000 → 4.00 vox as the flag goes on and off, the sweep
figures above, and — the arm that matters most — **a ray still hits it**, since
a "fix" that hid props from queries too would pass everything else while
silently deleting melee). `npc-block` pins the WIRING, because it is the one
gate with an NPC standing in the world holding a real drawn blade.

### Creatures do not stand inside each other (2026-09-15; `MobSystem::CrowdPush` / `ApplyCrowdSpacing` / `BlockedByMob`, `LocomotionDef::spacingMul`, gate `crowd`)

Owner report: creatures chasing one target "bunch up into the same space and
overlap". They did, and nothing in the locomotion pipeline had an opinion about
it — "walk at the target" is exactly what each of them was told and the target
is one point, so convergence is the correct answer to the question they were
each asked. The missing constraint is that the other bodies exist.

**Separation is a DRIVE, never a heading.** This is the whole design decision.
Folding a repulsion vector into `desiredHeading_` is the textbook boids answer
and it is wrong here: heading is what the stroke driver, the parry test and the
attack arbiter all read, so a crowded duelist would turn away from the thing it
is fighting in order to make room. Instead a crowded body **sidesteps** — it
keeps facing its target and gives ground laterally, which is also what a person
does — and the forward term is allowed only to **brake**, never to push. A
crowd behind you must not shove you forward into the enemy you were circling.

Two mechanisms, because either alone is wrong in a way the other is not:

| | what it is | why it is not sufficient alone |
|---|---|---|
| `CrowdPush` | soft, a drive, runs between `DecideIntent` and `Steer` | a fast body crosses the spacing radius within one tick before the push acts |
| `BlockedByMob` | hard, inside the drive's `fits()` move resolve | a blocked mob keeps trying, so bodies grind against invisible walls |

Together they give way and then stop, which is what a crowd does. The hard rule
sits in `fits()` specifically so the **existing wall-slide applies to it
unchanged**: a body that cannot step straight into its neighbour tries each axis
alone and slides around it, exactly as it already does to rock.

`BlockedByMob` has one escape that makes it safe: **a move which increases
separation is always legal.** Without it an overlap is a trap — two bodies that
somehow start inside each other (spawned on one column, shoved together by
terrain, teleported) would find every move refused and weld themselves in place
forever. With it, overlap is self-correcting and nothing can be permanently
stuck.

Spacing is `(rA + rB) * spacingMul` where `r` is each body's own footprint
radius — **a multiple of the bodies' own size, not metres**, unlike every other
budget in `LocomotionDef`. Those are terrain questions ("how big a ledge is a
wall") and a ledge does not scale with the creature; this is a body question,
and a critter inheriting a human's metre of personal space would refuse to enter
a corridor it fits in three abreast. The radius is the **mean** half-extent in
x and z rather than the max, because `worldSize` is the ART's bounding box and a
rig with its arms out would otherwise claim its wingspan. The hard floor is
`rA + rB` (the bodies touching) rather than the full spacing radius: spacing is
a preference the push expresses, contact is the geometry, and blocking at the
preference would fence a duelist at arm's length from everything it wants to
hit. `spacingMul = 0` disables both halves.

Height separates too — bodies more than half a body height apart in ground level
are on different storeys and ignore each other, which is self-scaling rather
than a constant that rots when the voxel size moves. Limp ragdolls are excluded:
a prone body is scenery, and the footprint model describes something standing.

Cost is `kMaxMobs` (16) squared at worst — 120 pairs of two compares and a sqrt,
cheaper than one ground probe, sleeping to nothing when bodies are apart. A
spatial index at that bound would be a second source of truth about where
creatures are for no measurable gain.

Gate `crowd` is **two arms, and the second is the point**: four duelists
converge on one target, and the control arm reruns the same fixture with
`spacingMul` zeroed *in the def* — data only, no rebuild, no test-only code
path. "No two bodies overlapped" is vacuously true of a fixture that never
crowded them, so the control arm has to overlap badly before the result means
anything. Measured: bodies touch at 3.38 voxels; **on**, the closest two ever
came was 3.39 with zero overlap ticks; **off**, 0.80 with 13.

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
| intent | `DecideIntent` | `desiredHeading`, `driveScale`, `driveStrafe` | **The AI seam.** The only stage allowed an opinion. A mob with an authored `behavior` is driven by the utility arbiter in `game/ai_behavior.cpp` (next section); one without falls through to the original wander-and-avoid, unchanged. |
| steer | `Steer` | `heading`, `turnVel` | The only writer of body facing, and it moves it at a bounded, ramped rate. |
| drive | `DriveLocomotion` | `origin`, `phase` | Translates a **local 2D velocity** — forward along the actual facing, plus a lateral term — with the alignment scale applied to the forward component only, then **resolves that step against the body's own footprint** (below). |

### A walking body is a box, and it has a budget (2026-09-10)

The drive used to be one line — `origin += vel * dt` — with the only restraint
an eight-way fan of single-column probes taken *before* the move. Reported as
"NPCs run into a hill and keep going underneath the voxels, and standing on a
ramp they're rotated 45 degrees". It was four independent faults that only
compose on sloped ground, which is why flat fixtures never saw any of them:

1. **The ground settle was symmetric and had no floor.** `clamp(groundY − y,
   −snap, +snap)` at 3 cm a tick against a humanoid walking 3.15 m/s: past
   roughly thirty degrees the body outruns its own settle and sinks.
2. **`Mob::GroundHeightAt` only ever scanned DOWNWARD.** A body one voxel under
   the surface asked "what is the first solid below me?" and was told "the one I
   am standing in", so the reported ground was *inside the hill*. The fan then
   measured its eight rises against that fiction, read flat in every direction,
   and the creature walked the rest of the way through. **This is what made the
   sinking self-sustaining rather than a one-tick glitch.**
3. **The mob step-up budget was 0.20 m against the player's 0.58 m.** A kerb the
   player strides over was a wall to an NPC: the forward probe went blocked, the
   steering deflected to the nearest clear bearing, and the creature crabbed
   sideways across ground it should have walked straight up.
4. **The gait counted every IK chain as a foot.** Every humanoid publishes two
   legs *and two arms*; the hands were given ground contact points, folded into
   the body-height average, and fitted into the plane the body's **tilt** came
   from. On flat ground all four sit at the same height and nothing shows; on a
   slope a quad of two feet and two hands has a normal nothing like the
   ground's, bounded only by "the normal's y is above 0.6" — 53 degrees, in any
   direction including pure roll. That is the "rotated 45 degrees" report, and
   `PlayerAvatar::UpdateGait` has filtered on the `leg` tag since it was written.

What replaced them:

- **`Mob::FootprintFooting` is the one place a walking body meets the terrain.**
  Nine columns over the body box, reporting the height it would *rest* at, a
  `wall` flag for any column standing more than a step above it, and headroom.
  `SenseGround`, the drive and the freefall test all read it.
- **The settle is asymmetric.** Downward is still eased (that is what a settle
  is *for*); upward is a hard clamp, because "do not be inside the ground" is
  not a preference. The rendered height `bodyY_` is eased separately, so nothing
  pops.
- **The move is resolved like the player's:** try the whole step, and if the box
  does not fit, try each axis alone. Wall *sliding* falls out of that, and so
  does climbing — "fits" includes a legal step up onto the destination.
- **The terrain budgets are authored per rig in metres** (`LocomotionDef::
  stepUpM` / `stepDownM` / `headroomM` / `tiltMaxDeg`, defaulting to the
  player's own numbers and clamped to the rig's leg span) and resolved once in
  `BuildRig`. The A* planner reads the same numbers through
  `ai::SelfView::stepUpCells`, so it and the drive cannot come to disagree about
  what a wall is — they used to be a literal in `mob.h` and a different literal
  in `behaviors.json`.
- **The gait owns leg-tagged chains only**, and the tilt is taken from the
  *ground* rather than from the contact points: two grades fore/aft and
  left/right of the footprint, clamped as a single total lean off vertical.
  Bipeds and quadrupeds now use one path (the old `>= 3 planted points` gate
  meant a biped never leaned at all), it does not jitter with the swing phase,
  and the bound is stated in degrees instead of falling out of a dot product.

**`GroundHeightAt` has three outcomes, not two.** A probe that starts inside
weight-bearing matter and cannot climb out of it has not failed to see
anything — it has seen rock and merely cannot measure it. Reporting that as
"unknown", which every caller correctly reads as *open*, let a duelist walk
through the middle of a stone wall whenever the chunk above the wall was not yet
cached. `outBlocked` is that third answer.

Gate: **`ai-slope`** — a real stone ramp, asserting the body climbs it, never
gets below the surface (0.00 voxels; the pre-fix code reached 2.70 for 53 of 700
ticks), and never leans past its rig's authored ceiling.

The drive stage takes a signed forward term and a lateral one rather than a
single forward scalar, and that is a *combat* requirement rather than a
generalisation for its own sake: a duelist that has to turn its back to give
ground reads as a rout, and circling a target while facing it is *pure* strafe.
Both are impossible with a forward-only scalar, since a negative one used to
mean "do not move".

The alignment scale stays on the forward term alone. `align` answers "how well
does my facing match where I want to *go*", which is exactly the wrong question
for a sidestep or a back-pedal — a duelist circling a target it is squarely
facing has an alignment of 1 and a step to the side to make. The per-direction
wall check follows the same split: the fan is in the mob's own frame at 45°
steps, so probe 0 is ahead, 2 its right, 4 behind and 6 its left, and a
back-pedal that only consulted probe 0 would reverse straight into the rock it
just stepped away from.

**None of this weakens the invariant.** `Steer` is still the only writer of
`heading_`; the drive vector only changes which direction the body translates
*relative to* that heading.

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

### NPC behaviour: a utility arbiter over named intents (2026-08-31; `game/ai_behavior.*`, `game/ai_nav.*`)

The seam above had one occupant — wander-and-avoid — and no notion of a target,
an aggro state or a reason to be anywhere in particular. This is the layer that
gives it those, and it is meant to carry the whole cast: guards, grazers,
fleeing critters, archers, pack hunters.

**The split that makes that possible is vocabulary versus character.** An
*intent* is a verb the engine knows how to perform (Idle, FaceTarget, Approach,
HoldRange, CircleStrafe, RequestAttack) — a scorer plus an actuator, and it has
to be code, because "circle the target at the outer edge of my band" is
geometry. A *profile* is a named creature character: which verbs it is allowed
at all, how much it wants each, how far it sees, what range it likes, how often
it swings. That is data — `assets/mobs/behaviors.json`, hot-reloaded on **R**
with materials, glyphs and items, and a mob sidecar opts in with one key,
`"behavior": "duelist"`.

There is deliberately no `enum MobKind` and no `switch (mob.type)` (design rule
4). An intent *absent* from a profile's `intents` map has weight 0 and is
disabled — that is the enable flag, rather than a second boolean nobody would
keep in sync with the weight. An empty profile is a blind, immobile statue, so a
creature only ever does what its JSON asked for. **Adding a creature is a JSON
edit; adding a verb is one enum entry, one scorer and one actuator, and every
existing profile is untouched.** The three launch profiles are exactly this:
`dummy` (no perception, no movement, no turn), `swordsman_static` (perceives,
faces, swings in reach, feet nailed down) and `duelist` (all of it), and they
share every line of code.

**The arbiter is utility scoring with three authored dampers.** Pure argmax over
continuous scores is a machine for producing twitching creatures — two intents
within a hair of each other swap every tick — so an intent gets a flat
`hysteresis` bonus while it is the incumbent, cannot be dropped before
`minDwellTicks` unless its own score has fallen to zero, and cannot be re-picked
for `cooldownTicks` after it ends (charged on *exit*, so a long circle is not
punished by its own duration). Perception carries its own hysteresis for the
same reason: a target already tracked is held to a larger radius than one being
acquired (`keepRangeScale`), or anything standing on the boundary is acquired
and dropped on alternate ticks and the arbiter above it oscillates for reasons
that look like a bug in the arbiter.

**Targets are actors, not "the player".** `MobSystem::PreTick` rebuilds one list
per tick from the player capsule plus every live mob, keyed only by faction, and
target selection scans it. Mob-vs-mob combat therefore needs no further code —
two factions fight the moment two profiles disagree about `faction`. The player
position is pushed in once per **tick** (`SetPlayerActor`), never per frame: the
tick loop runs 0–4 times per frame and a target sampled on the frame clock would
make an NPC's decisions a function of frame rate.

**Navigation is local and combat-scoped** (`ai_nav.h`): A* over walkable
*columns* inside the CPU mirror, string-pulled so the route is "walk to the
corner, then walk to the target" rather than a grid staircase, replanned on a
cadence and when the target walks off the end of its own path. Three things are
load-bearing. *Unknown is walkable* — the probe reaches past the mirror long
before it reaches anything interesting, and a planner that treats "I cannot see
it" as "there is a wall" builds a cage out of its own ignorance (the same rule
the steering layer already lives by); an unknown column inherits the height of
whoever reached it. *The probe is injected, not reimplemented* — `NavProbe` is a
pair of function pointers and the caller binds `Mob::GroundHeightAt`, so the
planner cannot come to disagree with the locomotion it is steering; the step-up
limit likewise has to match what the drive will climb, or the planner hands it a
path it refuses to walk — so since 2026-09-10 there is only ONE number, the
creature's own (`ai::SelfView::stepUpCells`, resolved from its rig), and a
profile's `maxStepUp` is an optional override rather than a second copy. *The straight line is tried first*, which
is both the cheap case and the graceful-degradation case: with no plan and a
clear line, "walk at them" is exactly right, and a failed search falls back to
direct steering plus the existing fan avoidance rather than to standing still.

**The attack seam is a request, not a swing.** This layer decides *when* and
*where* and emits an `ai::AttackRequest` (style id, target point, target id,
tick, commitment window) into `MobSystem::AttackRequests()`, drained by the
consumer exactly as sever and voice events are. It plays no clip, sets no pose
and deals no damage; while a request is live the arbiter pins the mob to
FaceTarget for `commitTicks` so a stroke system can assume the body is not
pirouetting mid-swing. The attack clock is advanced when the request goes out
rather than when a blow lands, because this layer must not learn whether a blow
landed — an AI that waits for hit confirmation stops swinging the moment the
seam is stubbed.

**A swing is a step forward, and it is aimed where the target will be
(2026-09-16; `attack.pursueSpeed`, `holdGroundFrac`, `leadTicks`).** Holding a
*facing* through a stroke and holding a *position* are different promises, and
the commit window was making both. Three separate things followed from that, and
a creature fighting a target that walks away lost to all three at once:

1. **The feet were nailed down for the whole telegraph.** `RequestAttack` wrote
   a heading and no drive; the commit override then replaced the winning intent
   with FaceTarget, which also writes no drive. `attack.pursueSpeed` is the
   fraction of walk speed a creature may spend closing *while* a blow is
   committed, applied after the arbiter (the only place that can reach past the
   override) and only ever as a `max`, so an intent that already wants to close
   harder keeps its own number and a too-close retreat is untouched.
2. **The step-off was a second retreat.** `disengageTicks` lifts the band floor
   to its ceiling after every swing — "hit and step off", and right against
   someone standing their ground. Against someone already leaving, the gap opens
   at the sum of both speeds and the creature never gets a second swing away.
   The window is now suspended while the target's *radial* velocity exceeds
   `holdGroundFrac` of the creature's own speed. That threshold is a **noise
   floor, not a judgement about pace** — sized as the latter (0.30) it never
   fired once against the crouchwalking player it was written for, because a
   crouchwalk is 0.25 of a `human`'s walk.
3. **The blow was aimed where the target had been.** `StartStroke` freezes
   `AttackRequest::targetPoint` and `StepStroke` re-derives the aim from that
   stored copy every tick — correct, because a committed cut must not home —
   which makes the request the *one* instant the destination is chosen.
   `leadTicks` (`-1` = use `commitTicks`, `0` = none) extrapolates along
   `Brain::targetVel` to where the edge will actually arrive: windup plus half a
   cut, about 15 ticks for the NPC sword styles.

`Brain::targetVel` is sampled only between **consecutive visible ticks of one
target**, because the two other ways a remembered position moves are both lies —
`Perceive` freezes `targetPos` while an alert decays, so a differenced position
reports a creature standing perfectly still, which is the most dangerous
available reading of the target that just broke contact.

The predicted distance (`Brain::leadDist`) gates the commit and feeds
`PickAttackStyle`, and is **clamped to the present distance so it can only bring
a commit forward, never push one back**. Unclamped it made the creature passive:
with the lead on and the pursuit off, a fleeing target predicts *further* than it
is, and the duelist issued one attack request in 360 ticks against the eight the
unmodified creature managed. Refusing to swing because the target might be out of
reach in fifteen ticks spends a whole cadence on a certainty of nothing, and the
refusal is not this layer's to make anyway — `BeginStroke` checks the drawn
style's own reach at the instant the stroke would start.

All three are authored per profile and all three are data, which is what lets the
`ai-pursue` gate carry its own pre-fix repro arm instead of needing a second
binary.

**Determinism.** Everything here is CPU-float gameplay state beside the gait and
the melee pose; the AI never writes a voxel, it writes a desired heading and a
drive vector. It runs only inside the fixed 30 Hz tick, and every random draw —
the attack jitter, the orbit direction — is `rng::Hash3(id ^ salt, tick, index)`,
so a replay of the same fight has the same rhythm.

One measured trap, and it is not a tuning question: **tangential speed is
bounded by the neck, not the legs.** An orbit of radius *r* walked at *v*
demands an angular rate *v/r*, and a body whose turn rate cannot follow spends
the whole circle looking off to one side — so it never gets its nose on the
target, never satisfies the aim tolerance, and has its forward drive scaled down
by an alignment it can never reach. Authored at `strafeSpeed` 0.5 on a creature
walking 60 vox/s, mina needed 3.3 rad/s against a 2.8 rad/s cap and issued
**zero** attacks in 240 ticks of holding range. The strafe is now capped at
`turnRate * 0.7 * r`, and the `ai-approach` gate asserts a minimum attack count
precisely so this cannot come back silently: the arbiter can score perfectly and
still never fire.

The three gates are built around what is actually falsifiable, since "it got
closer" and "its heading changed" are satisfied by almost every wrong
implementation: `ai-dummy` asserts *exactly* zero motion and zero turn with a
target in plain sight (the whole "behaviour is data" claim in one number, since
the dummy shares every line with the duelist and differs only in JSON);
`ai-face` asserts convergence on non-quantized bearings with the feet nailed
down; `ai-approach` asserts a real detour around a wall the mob can see over but
not walk through, then arrival *and residence* inside the band, no occupation of
the target's space, and swings actually issued. Thresholds are in
`tests/baseline.json`.

### NPCs swinging, and blades meeting blades (2026-08-31; `game/strokes.*`, `assets/mobs/attack_styles.json`)

The AI decides *when* and *where*; this is what turns that into a sword moving
through the world. **There is no second melee implementation.** An NPC's swing
runs through the same `MeleeState` the player's mouse drives, the same
`Mob::ApplyWeaponArm`, and the same `MeleeSweepDamage`; the only difference is
what feeds the deltas. `melee.h` was built for this — "the control law never
sees a mouse, it consumes abstract control deltas" — and an attack style is the
authored curve that fills them.

**An attack style is DATA** (`assets/mobs/attack_styles.json`, hot-reloaded on
R). A style is a stroke program in three segments: a **windup** — a pose,
driven closed-loop and deliberately under `commitSpeed` so the driver stays in
Guard — then a **cut**, a travel fast enough to commit, then a **recover** with
no input while the follow-through unwinds. Angles are radians in the mob's own
facing basis; reach is a position within the arm's own **reach band**
(`MeleeState::ReachBand`), not a fraction of the arm, because the band is where
the driver can actually put the point and the two are nothing like the same
length. There is no `enum SwingKind` anywhere: a profile lists opaque style ids,
`PickAttackStyle` draws one per attack, and a name the library has never heard
of gets a loud skip rather than a crash. Shipped: `horizontal_r`,
`horizontal_l`, `overhead`, `diagonal`, `thrust`, and — since a part of the
body can be a weapon — `punch_r`, `punch_l`, `hook_r`, `bite`, `bite_lunge`.

**The windup IS the telegraph.** There is no UI indicator by design: a style's
windup is 10–14 ticks of visible blade raise, and `npc-strike` asserts its
*length* precisely because an attack that resolved in two ticks would be
unreadable and unavoidable.

**The cut is centred on the aim, and the aim is taken once.** At the end of the
windup the target's bearing about *this mob's own shoulder* is frozen and never
refreshed, so a target that steps offline after the blade is moving is missed —
that is what makes a telegraph mean something. The windup's own target is
`aim − cut/2`, so the middle of the travel passes through the aim rather than
the beginning; a stroke aimed at its own start point cuts the air behind the
target every time.

**Variation is deterministic.** Style pick, start bow and tempo are all
`rng::Hash3(mobId ^ salt, tick, index)`, so ten swings differ and the fight
replays.

**A style can name a BODY CLIP, and clips have a shared library** (2026-09-01;
`AttackStyle::clip`, `assets/anims/`). The stroke program drives the weapon
arm; everything else the swing does with the body — the step, the shoulder
drop, the off hand — is an ordinary keyframed clip, started by `Mob::PlayClip`
at `BeginStroke` on both drivers (NPC `MobSystem::BeginStroke`, player
`main.cpp`), so it rides the clip layer's own mask/blend/mode and the arm claim
still wins on the arm. Clips used to exist only inside one creature's sidecar;
`assets/anims/<name>.json` is one clip per file in the clip-lane schema plus
`name` and `sidecarVoxelsPerMetre`, and `LoadMobDefs` compiles every file onto
every rig whose part names it fits (a sidecar clip of the same name wins on
that rig; a file with no usable track is skipped silently, because a critter
has no `armU.R`). The tuner's clip lane writes the library ("→ library") and
reads it back ("← library" copies a file into the open sidecar for editing);
the Attacks lane's `clip` picker lists it. The `mob` gate asserts every library
file compiled onto the human under its stem — that is the name a style uses.


**A style names WHAT SWINGS IT, and what it can reach** (2026-09-15;
`docs/PLAN_impact_unarmed.md` §5). Five fields, all optional, all data:

- `weapon` — `"held"` (the default, and what every style authored before this
  existed says) or the name of a natural weapon on the creature's own rig
  (`"fist.R"`, `"jaws"`).
- `fallback` — only drawn when there is nothing better. A duelist lists its
  punches beside its cuts; with a sword in its fist it must never draw one, and
  disarmed it must. `PickAttackStyle` resolves names, FILTERS by `StyleUsable`,
  then drops every fallback style as a GROUP if any non-fallback style survived,
  then draws. Filtering before drawing rather than after is what stops a
  disarmed duelist missing three turns in four while it rolls its way onto its
  one punch, which reads as a creature that has stopped fighting rather than one
  that has lost its sword.
- `reach` — world voxels, overriding the profile's `attack.reach` for this style
  alone. A lunging bite commits from 22 and a punch from 9, and a profile can
  state only one number.
- `lunge` — a ballistic opening; see the locomotion section above.
- `target` — weights over LIMB TAGS rather than names, so any rig that tags its
  parts answers it. `MobSystem::PickTargetLimb` accumulates over (tag weight x
  that tag's LIVE BASE limbs), so two arms do not make "arm" twice as likely as
  its authored weight, draws counter-based on (attacker, tick), then draws again
  for WHICH of that tag's limbs — a bite that always took the left arm would be
  a distribution over tags wearing a distribution over limbs. Worn shells and
  held items are excluded: aiming at one is aiming at somebody's coat. The
  choice is recorded on `NpcStroke::targetLimb` the instant it is made, so a
  gate asserts the DRAW instead of inferring it from where the wounds landed,
  and it is drawn ONCE per stroke — a blow that re-chose its limb every tick
  would be homing, and the whole point of a windup is that it is not.
  **And the stroke is then AIMED at it** (2026-09-19): `StartStroke` replaces
  the request's point — the victim's body CENTRE — with the drawn limb's own
  middle (`Mob::LimbCentreWorld`, the collider box's centre off the live
  transform; the limb's JOINT would aim a leg bite at a standing victim's hip),
  carrying the request's planar LEAD across and taking the height from the limb
  outright. Until that landed the table steered nothing at all: every authored
  weight was recorded and then discarded, most visibly on a crawler, whose
  `targetProne` sends nine bites in ten at a leg and sent all ten at a chest it
  could not reach. `targetProne` is the second table, drawn from instead of
  `target` whenever the BITER is prone (`Mob::LocoGroundAlign() > 0`).

**`StyleUsable` is the style vocabulary's one question about a rig**, declared
in `strokes.h` and defined in `mob.cpp` — the header is included BY `mob.h` so
it cannot see a `Mob`, but an author looking for "when does a style apply"
should not have to know the answer lives in the rig. `"held"` asks
`HeldSlot() >= 0`; a natural weapon asks that the def declares it, that its part
is alive, and — for a chain effector — that every part of the chain serving it
is alive, because a two-bone solve with the elbow severed is a hand hanging in
space off a shoulder. Refusing BEFORE the draw keeps the driver from ever seeing
a broken chain: a one-armed zombie's punches quietly stop being options instead
of becoming a pose bug. `MobSystem::ForceAttack` asks the same question, so a
gate forcing a punch on a handless body gets `false` and can assert on it.

**Reach reaches the AI without the AI learning what a style is.**
`MobSystem::AttackReachOf` is the longest reach the creature can ACTUALLY use —
the profile's, raised by the longest `reach` among its USABLE styles, so a
zombie whose head has come off stops standing 22 voxels away waiting to bite —
and it is handed to `ai::Think` on `ai::SelfView::attackReach`, exactly as the
creature's own step-up and headroom budgets are.

**The footwork band follows the WEAPON, and the draw knows how far it is**
(2026-09-16). Those two numbers together are the whole of a defect that read, from
outside, as the AI having gone passive: NPCs circling out of reach, a mace
swinging rarely, a dagger never swinging at all, and a zombie that would not
bite.

The cause is that `AttackReachOf` takes the profile's `attack.reach` as a FLOOR
and the footwork band (`movement.rangeMin`/`rangeMax`) was read as a fixed pair
of distances. `duelist` authors reach 10 and a band of 7..11 — all three are
SWORD numbers, because a sword is what it was tuned holding. Hand the same
creature a dagger and they are still 10, 7 and 11: it walks to 7..11, holds
there, commits on the floor of 10, draws a cut that lands at about 3.5, and
`BeginStroke` drops the swing with the cadence, commit and disengage clocks
already spent. Graded by blade length, that is exactly the reported symptom —
sword fine, mace marginal at the band's inner edge, fist and dagger never.

So there are now two reaches, because "how far do I commit from" and "where do I
stand" were one number answering two questions. `MobSystem::StrikeReachOf` is
the same maximum WITHOUT the floor — what this body can actually hit with right
now — and `ai::Think` slides the authored band, whole and keeping its authored
WIDTH, until its middle sits on it. Width is the character and survives; the
distance is a fact about the weapon and does not. INWARD ONLY, on both counts
that matter: a zombie's longest usable style is a 23-voxel lunge and anchoring
its band on that would have it orbit at 23, and `duelist`'s mid is (7+11)/2 = 9
against a sword that lands at about 9, so the armed case shifts by zero and
every fixture tuned around it is untouched. Only the weapons that were already
broken move.

`PickAttackStyle` takes the distance too, and filters on it BEFORE the draw
rather than after. The draw used to be uniform over everything usable with
`BeginStroke` re-checking the result against the same two numbers and binning
it on a mismatch — so a creature whose repertoire is long and short silently
lost a share of its swings equal to the share of short styles in its list. A
zombie is 50/50 between a 23-voxel lunging bite and a 3.3-voxel standing one, so
past about four voxels HALF its attacks were discarded by its own draw, once a
cadence, forever. The refusal in `BeginStroke` is kept as an assertion for
callers that pick their own style, and the "nothing reaches" case is no longer
silent: it reports through `ReportNoStroke` reason 2 with the distance and the
longest reach on the line, which is the difference between a positioning fault
and a content one. `ReportNoStroke` prints
once per (mob, reason) off a bitmask, because there are four reasons now and a
creature with nothing left requests an attack every cadence forever.

**The player has a second compass.** `StyleLibrary::playerUnarmed` is a
`PlayerStrikeMap` beside `player`, and it is a second map rather than a `weapon`
filter over the first because the two are different SHAPES: a sword's compass
has an overhead and a thrust, a fist's has a jab, a cross and a straight, and
asking one set of sectors to mean both makes every punch a re-labelled sword
cut. `main.cpp` picks between them on whether anything is drawn, and "this body
can punch" is answered by CONTENT rather than by a `FindNatural("fist.R")`
spelled in C++: the compass resolves and at least one style it points at passes
`StyleUsable` on the avatar. Lose both hands and it stops resolving on its own;
author a creature with claws and nothing in the frame loop changes.
`meleeArmed` still means "a weapon is drawn" and still decides what to equip;
`meleeReady = meleeArmed || meleeUnarmed` is what the driver, the program and
the sweep gate on, because a fist is as live as a sword.

The content that came with it: `punch_r` / `punch_l` / `hook_r` are
`fallback: true`, so the armed profiles that now list them are behaviourally
unchanged and `unarmed-attack` asserts in as many words that an armed fighter
never draws one; `bite` and `bite_lunge` are not fallbacks. `behaviors.json` gained a `zombie` profile whose every style
is a natural weapon, so a disarmed zombie is as dangerous as an armed one and
losing a hand costs it one style rather than all of them. It is also the first
profile whose `attack.reach` (9) is not the whole story: `bite_lunge` states 22
of its own and `AttackReachOf` hands the longer to the arbiter, so the creature
commits from three body-lengths out and the leap closes the gap. Putting 22 in
the profile instead would have it stand off at 22 to punch, which is the
coupling per-style reach exists to break.

**The player's swing is bound to the body like the head is** (2026-09-01;
`melee.aimYaw` / `aimReleaseYaw`, `ResolveSwingBasis` in `game/thirdperson.*`).
The stroke basis handed to the driver was the raw camera, so in third person —
where the body faces its travel direction — a camera orbited behind the
character made every strike cut backwards at the lens. Now the camera basis is
yawed back toward the body by the neck's own law: the camera may lead the body
by `aimYaw` degrees, past that the swing pins at the cone's edge, and across
the last `aimReleaseYaw` degrees before straight-behind the offset smoothsteps
to zero so a front-on camera swings the way the character faces. Pitch is never
touched, and the whole right/up/forward frame rotates rigidly so a diagonal is
still a diagonal. First person never reaches the cone (`ResolveAvatarHeading`
drags the body first). Gated in `mob` ("avatar swing cone").

**Blocking is EMERGENT: a blade physically in the path stops the blow.** No
block button, no block state, no defensive intent — an NPC's blade stops a cut
when a windup stance happens to put it in the way, and the player's blade does
the same by being where the player left it. A parry arrests the stroke (the
remaining cut ticks are abandoned and the driver drops into Recover with no
follow-through), charges the blocking weapon real item hp — items already carry
hp and `severImpactSpeed`, so a sword wears down and a badly-timed catch is
knocked out of the hand — nudges the defender's own stroke open by a bounded,
hash-seeded amount, and reports a `BlockEvent` out of the sweep for the audio
and FX to consume.

Two things about it are load-bearing and were both learned the hard way:

* **The parry is GEOMETRIC, not a ray cast.** `MeleeSweepDamage` finds bodies by
  casting rays along the swinging blade's own axis, which is exactly right for a
  torso and useless against another blade: a sword's collider is the item's own
  art at the item's own scale, about a **quarter of a voxel** thick, and a
  zero-radius ray through that is a coincidence rather than a test. Measured:
  two edges passing within 0.28 voxels, seven sweeps, zero hits — and a ray
  fired deliberately down the defender's own edge came back empty too. So
  `MobSystem::FindParry` asks the question of the two **edge segments**, which
  are the authoritative hitboxes anyway.
* **Armour is not a parry.** The three-way classification is the rig's own: a
  slot below `AppendedBase()` is flesh, one at or above it tagged `worn` is a
  garment, and the one at `HeldSlot()` is a weapon. A shell is strapped to the
  limb it covers and stopping a blow with it is what armour is *for*, so it
  keeps taking the cut it always did.

While fixing the sweep for this, one real defect surfaced in it: the damage
probes **sampled** the blade instead of **tiling** it — `along + 1` rays of a
fixed ~1 voxel from points 1.6 voxels apart, so 40% of the edge probed nothing.
Invisible on a torso, fatal on a sword. Each ray now runs exactly to the next
sample point plus the blade's own half-thickness.

**The player is a target.** `PlayerAvatar` is a `Mob` but is not in
`MobSystem`'s list, so the handle-keyed lookups could not find it and an NPC's
sweep melted the player's limbs as debris instead of wounding them.
`MobSystem::SetAvatar` registers it, and `FindLimb`/`FindOwner`/`Damage`/
`CutLimb`/`CarveLimbRadial` consult it **after** the mob list — every existing
caller that already checked the avatar first is bit-identical. Death is what it
already was: the parts are handed to `DebrisSystem`, the corpse settles like any
other debris, and `main.cpp` revives after `player.respawnDelay`.

Four gates (`selftest_combat.cpp`, at the end of the mob group):
`npc-strike` (the AI's own request becomes a windup, a cut and lost voxels; then
three scripted strikes of one style all land); `npc-block` (a guard across the
line arrests the stroke, takes the item hp, and leaves the defender's flesh a
graze at most against the *total* loss `npc-strike` measures unblocked);
`npc-styles` (every authored style sweeps the channel its own JSON claims —
the gate that keeps the content honest as it grows, and it names no style);
`duel` (two AI duelists, opposed factions, both engaging and both cutting).

Open, and the owner's calls: NPCs do not yet *choose* to parry, so blocking is
luck of the stance; and a committed sword cut through a torso severs it, so
whoever lands first usually ends the fight — measured, the duel is decided in
one exchange.

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

### Natural weapons and strike effectors (2026-09-15; sidecar `natural`, `Mob::SetStrikeEffector` / `ApplyAimPart`, `docs/PLAN_impact_unarmed.md` §3/§4)

The whole pipeline above began at `heldPartIndex_`, and that one assumption is
why this engine had no fists: an arm could only be driven to serve a blade, an
edge could only be read off a held item, and an unarmed creature therefore had
nothing to claim the arm WITH.

**A part of the body IS a weapon.** `MobNaturalWeaponDef` is exactly what an
item already is — an edge segment and a `StrikeProfile` — with the segment
living on a part of the rig instead of on a borrowed slot. `human.json` ships
three (`fist.L`, `fist.R`, `jaws`), and `zombie.json` inherits them untouched
through `extends`. Modelling jaws as an invisible held item was rejected because
a slot outlives a part: a headless zombie would still bite, a fist would be in
the parry table, and every rig would ship two `.vox` files. THE PART IS THE
AUTHORITY, which makes `StyleUsable` one check rather than a sync problem. The
`edge` is two POINTS in the part's own art frame — the Y-up,
origin-at-the-model's-min-corner frame `MobLimbDef::edgeFrom`/`edgeTo` use,
scaled by `artVoxelsPerMetre` at load exactly as those are — rather than an axis
plus two offsets, because a part has no hilt to measure from: a fist's edge runs
wrist to knuckles and a jaw's throat to teeth. A zero-length edge is refused at
load and asserted against by the `mob` gate, because its only symptom is a
creature that punches and never connects.

**A worn piece may REPLACE the profile.** `Mob::StrikeProfileFor` looks for a
shell parented to the weapon's own part whose item carries a `strike` block of
its own — `iron_gauntlets` is the first — and the four impact numbers move
across wholesale. An iron gauntlet is not "a fist plus iron"; it is a different
weapon that happens to be shaped like a fist, which is exactly why the swept
EDGE stays the fist's own. The covered limb is read off the RIG
(`parts[slot].parent`), not off the item's cover list, so there is no second
table to keep in step. The creature's own `bite: {infect, stain}` (`MobBiteDef`;
`zombie.json` says `rotflesh` + `ichor`) is then OR-ed on, and only onto a
profile that already bites — a zombie's fist carries no rot, because punching
somebody does not put your mouth on them. That split is the whole reason the
infection lives on the creature and the bite number on the weapon: neither file
has to know the other exists.

**The effector is one part index and one mode** (`Mob::SetStrikeEffector`,
`StrikeEffectorMode`), and every consumer — `ApplyWeaponArm`, `WeaponEdge`,
`WeaponStrokePose`, `WeaponArmPose`, `HeadKeepOut` — asks it instead of asking
the fist. `ResolveEffector` falls through to the held item when nothing is set,
so **the held path is unchanged by construction**: every Held branch is the code
that was there before, reached through one more `if`. `Mob::ArmForStyle` sets
the effector from the style's `weapon` and picks the MODE from the rig's own
shape — a part an IK chain can serve is a Chain, anything else is an Aim — which
is one rule instead of an authored mode field, and means a creature whose rig
later grows a neck chain starts biting through the IK with no content edit.

- **Held** — the arm chain is solved to the driver's hand target and the wrist
  lays the blade along `bladeDir`. Today's behaviour, untouched.
- **Chain** (a fist) — the same solve, minus three things. There is no blade to
  steer, so the wrist does not. There is no blade to LEAN, so
  `WeaponStrokePose` reports the TIP AT THE HAND: `MeleeState::RadiusBand` picks
  between two models on `bladeLen_`, and reporting a knuckle one voxel past the
  wrist as a blade collapsed a five-voxel arm's usable band to 1.75 voxels and
  leaked the radial drive into elevation through an ill-conditioned tangent.
  Nothing about the hitbox moves with it — `WeaponEdge` reads the authored
  segment off the live transform, a separate question from where the driver
  steers. And the head KEEP-OUT is off: that sphere models a long rigid segment
  swung about the shoulder that no pose limit knows exists, where a fist is a
  knuckle on an arm whose shoulder cone and elbow hinge are clamped every tick
  anyway — left on, a sphere wider than the whole fist shoved a chambered punch,
  which legitimately sits beside the chin, out and up.
- **Aim** (the jaws) — no rig puts the head in a chain and one that did would be
  claiming the neck is an arm, so the part is driven the other way a part can be
  pointed at something: `Mob::AimPartAlong` rotates it, and an authored share of
  the spine (`MobDef::aimSpineShare`, 0.35 — anatomy, so a rig fact and not a
  tuning one), until its forward lies along the stroke's bearing; the body's own
  travel closes the distance. **It is solved in the part's PARENT frame**
  (2026-09-19): the spine takes its share of the yaw error first, the part's
  world forward is then re-read and the remainder closed exactly, so nothing on
  this path assumes the chain above the part is upright. `ApplyAimPart`, the
  angle form, multiplied a yaw/pitch pair onto the part's LOCAL rotation — a
  delta about axes the parents have already moved — which is a few degrees of
  error on a standing creature and NINETY on a crawler, whose `crawl` clip
  pitches the hips 74° forward in override mode: commanded yaw arrived as pitch
  and commanded pitch as yaw. Measured by `bite-target`'s prone arm off the
  rig's own commanded/posed pair: 1.589 rad of yaw error under the old form
  (`SANDVOX_AIM_OLD=1`, the control arm), 0.000 under this one. The angle form
  stays for the avatar's head-look and for an aim effector with no edge. That bearing is the driver's `StrokeAz`/`StrokeEl`
  laid straight onto the part's forward, NOT `weapon_.bladeDir`, which is a
  rigid bar's law-of-cosines LEAN and therefore derived from a geometry the head
  is not in. The pivot is the part's OWN joint rather than its parent's:
  measured with the chest as the pivot, the head realised 0.08 rad of a
  commanded 0.51 because four of five and a half voxels of "neck" were rigid.
  Nothing on this path is smoothed — a four-tick cut cannot catch a goal through
  a 0.12 s halflife, and a stroke owns its part outright, which is what makes a
  bite snap. Both forms write the PRE-FLATTEN locals, so the aim runs at stage
  3.5 and `ApplyWeaponArm` returns immediately for an Aim effector.

`ApplyAimPart` is the avatar's head-look, moved down onto `Mob` verbatim in its
reasoning and generalised in two ways: the part is a parameter, and the rotation
is scaled by a weight so a stroke can fade its aim the way the arm claim does.
There is therefore ONE implementation of "turn a part toward something", and it
has a second use — **a creature with a target and no live stroke turns its head
toward it** (`Mob::SetAimLook`, at 0.6 weight, because a look is not a stare and
because the posed head also moves the keep-out sphere the driver clamps
against). It is read off the brain rather than passed out of `ai::Think`, since
it is not an intent: the AI has no opinion about necks, and giving it one would
be design rule 4's failure in miniature. It sells the bite for almost nothing,
because the head is already on the victim before the leap. The direction is
taken from where the head IS (`Mob::PartJointWorld`, off the live transform) and
not from `origin_ + model[head].pos`, which is a prefab-local offset added to a
world position with the body's yaw left out — a couple of voxels of error whose
direction rotates with the creature, the worst shape an error can have.

**The pose composes with whatever the body is doing**, because the effector
drive runs where the arm drive always ran, after the loco clip and the gait, so
a crawling body's punch is solved from wherever the crawl clip left the shoulder.
`MobBasis` takes the body's live `BodyUp()` weighted by the loco state's
`groundAlign` rather than world up: a prone creature is posed in a frame tilted
to the hill it lies on, and a stroke expressed about world up asks for an
elevation its shoulders do not have — solved correctly and landing in the wrong
place, by the grade of whatever it is lying on.

Gates: `unarmed-attack` (a disarmed duelist still lands a punch and loses zero
flesh doing it, and the style draw shrinks with the body — arms off leaves jaws,
head off leaves fists, both gone leaves −1 and one loud line), `bite-target`
(40 forced bites choose at least two tags, and not the head every time),
`player-unarmed` (the compass resolves, `ArmForStyle` lands a Chain effector on
the named fist, `WeaponEdge` reports its knuckles with NO flat — a fist cannot
land edge-on — and the punch is clamped by the same IK and pose limits a sword
swing is), and `npc-styles`, extended to measure a fist as the knuckles' world
PATH LENGTH and jaws as the forward vector's ANGULAR travel, because the posed
tip about the stroke's pivot is the SWORD's coordinate and neither of the others
is in it. The `mob` gate asserts every `natural` entry on every def names a live
part and has a real edge.

### Combat feel: hit-stop, hit flash, and the three melee cues (2026-08-31; `sim/tuning.h` `Tuning::CombatFx`, `audio/cues.*`, `assets/shaders/microbody.wgsl`)

**Everything in this section is PRESENTATION, and the separation is
load-bearing rather than tidy.** None of it changes what a blow does — the
damage is §8's melee sweep and the wound model above — and all of it changes
whether the blow *lands* for the person holding the sword. Three channels, kept
apart because they fail apart: TIME (a dip after a landed cut), LIGHT (a struck
limb flashes), SOUND (three slots). Every value is in `tuning.json` under
`combatfx.*`, hot-reloads on F5, and has a row in the in-game **Combat…** panel
and in `tuner_schema.js`.

**Hit-stop is not a sim change, and it must not be read as one.** The engine
runs a fixed 30 Hz tick off a frame-loop accumulator that already runs 0..4
ticks per frame depending on frame time. A dip scales the RATE that accumulator
fills at, so for a fifteenth of a second the world advances fewer ticks. No tick
computes anything different, nothing hashed moves, and the headless paths never
execute that loop at all — `--selftest`, `--shot` and both autofly harnesses are
structurally exempt, which is why no gate had to be taught about it.

**THREE TIERS, STRICTLY ORDERED, because the dip is peak-held:** a SEVER (a limb
came off) beats FLESH (a live creature voiced being hurt) beats CHIP (debris, a
dropped item, a weapon caught on a weapon). The peak-hold is `min` on the scale
and `max` on the duration — they are two spellings of "how much stop" that run
opposite ways, and combining them with one operator is how a sever ends up
stopping hard for a chip's 55 ms.

**The tier is read off the event queues, not carried on a return value.** The
melee sweep already fills `SeverEvents` and `VoiceEvents` as a side effect, both
frame-drained, so differencing their sizes across the call says exactly what
this blow added without widening `EdgeSweepResult` — which is the shared surface
two phases were writing at the time. Known and accepted: hurt voices
de-duplicate per mob per drain window, so a second cut into the same creature in
the same 16 ms reads as a chip.

**A PARRY IS THE FOURTH PRODUCER AND IT IS NOT ONE OF THOSE TIERS.** An arrested
sweep returns before any probe runs, so `bodiesHit` is 0 and the tiering above
never fires; the clang and the chip-tier dip for a blocked blow come off the
`BlockEvent` queue instead, at the drain below the AI readout, which is the only
place that knows a block from a chip. One blow, one cue, whichever way it ended.
That drain sits below the audio drain, so a parry's clang voices at the top of
the next frame — the same one-frame latency the hit-stop has from *every*
producer, so the two halves of a blocked blow stay together.

**The flash on a blocking weapon is not wired separately, on purpose.** The
sweep already charges a parry to the blocking item's own rig slot, and
`Mob::Damage` sets the flash for every cause with a two-tier split that an
`item`-tagged limb resolves to the chip flash. Flashing again from the block
drain would be a second writer of one fact. The ATTACKER's blade does not flash:
it takes no damage from being stopped and the event carries no handle for it.

**The flash rides a word the render path already had.** `MobLimb::hitFlash` is
packed into a reused padding word of `MicroBodyInstGpu` and read at
`@location(7)` in `microbody.wgsl`, added to the material's own emission — no
new buffer, no new binding, and 0 stays 0 so an unstruck limb is bit-identical
to before. It decays on the TICK, inside `MobSystem::PreTick`, not on the frame
clock: a frame-driven decay is never called by the selftest, so a flash raised
in a headless gate would stay lit forever and the probe two subtests later would
compare a different image. Ageing on the tick also means the flash slows down
with the world under hit-stop, which is what you want, since the two effects
describe the same blow.

**Cues are latched, never played where they are raised.** Audio drains once per
frame and the tick loop above it may have run four times or none, so each cue is
a one-shot request with PEAK-HOLD ON POWER — a frame in which the blade crossed
a body on three consecutive ticks is one blow to the ear, and it should be the
hardest of the three rather than three overlapping copies of nearly the same
sample. The position travels with the request because a killing blow can despawn
its victim before the frame drains. The three slots (`melee whoosh`, `melee
flesh`, `melee clang`) are declared in BOTH `assets/sound_schema.js` and
`Cues::kSlotPrefix`, which is the standing invariant for sound slots; the
`combat-cues` gate asserts they resolve to real sets and that
`Cues::Stats::combat` counts REQUESTS before the device is consulted, which is
the only half a silent headless run has.

**Whoosh fires on the EDGE into Slash, not while Slash is held** — committing is
a moment, and a per-tick test plays the sample five times over one cut. The edge
is remembered across frames so it survives a frame that ran no ticks at all.

**The third channel is WHICH WAY** (2026-09-16; `Mob::HitReact` /
`Mob::ApplyHitReact`, `combatfx.hitReact*`, gate `hit-react`). Hit-stop says
*something landed* and the flash says *here*; neither says which way, and a blow
with no direction in it reads as a light going on rather than as a thing
striking a body. A struck creature now rocks AWAY from the blade's own travel on
a critically damped spring and is back on its feet in about a quarter second.

*It is a pose, not a push.* Nothing in it moves `origin_`, `heading_`, `bodyY_`
or a collider — it writes `st.local[]`, the same pre-flatten channel the gait
bob, the spine twist and the aim write, at **stage 3.7** between the aim and the
flatten. Planted feet, personal space, the A\* plan and every hitbox are exactly
where they were, which is the whole licence for firing it on EVERY hit with no
animation to author, no budget to charge and no recovery state for the AI to
know about. A stagger that displaces a creature is a different feature. The
`hit-react` gate asserts the promise directly: origin and heading unchanged
across a blow.

*Three parts, one impulse.* The body LEANS (split root/spine by
`hitReactSpineShare`, the distribution law `ApplyAimPart` already uses), the root
is SHOVED (a fraction of the creature's **own height**, so one number means the
same lurch on a rat and a troll; the legs are IK'd to planted feet, so the body
travels and the legs absorb it), and the limb that was actually struck FLICKS
about its own joint — the only one of the three that says *which arm*. A hit on
an appended slot is redirected to its `wornHost`, because a shell is posed off
its host in `PostStep` and flicking the garment would leave the shoulder under it
still; a parried weapon has no host, so a parry spends lean and shove and no
flick. A limb inside an IK chain is overwritten by the stage-5 solve, and that is
correct: a planted foot does not fly up because a shin was hit.

*Why the spring is clamped as well as damped.* A cut is CONTINUOUS
(`EdgeSweep::struck`) — the blade is still in the wound next tick and the sweep
lands again, so the impulse arrives once per cut tick. `SpringDef::maxAngle`
bounds the displacement at the authored peak, so a long cut **holds** the lean
instead of pumping it four times over, which is what a blade dwelling in a wound
looks like. Within one tick the velocity is peak-held, for the reason the flash
and the dip are: several probes of one sweep meeting one limb are one blow.

*How hard comes from the profile, not from a name.* `StrikeProfile::Total()`
over `hitReactRefDamage`, capped, so a mace out-shoves a fist because it IS
harder and nothing in C++ knows a weapon by name (design rule 4). The whole
thing enters at ONE line in `MeleeSweepDamage`, placed BEFORE the three
resolvers because any of them can sever the slot being measured — so an NPC hit
by the player, the player hit by an NPC, and an NPC hit by an NPC all get it,
because all three swing through that function.

*The one number that had to be found by eye.* `hitReactLeanDeg` shipped at 6
first and at 6 a victim of a sword through the chest did not perceptibly move.
`--shot-strike human+sword horizontal_r human` is what settled it at 10 — a gate
can prove the body goes the right way and comes back, and cannot tell you that
the amount is invisible.

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
   the part's **rest** pose. Applying them before the normalize would let the
   weight division scale the delta away. The reference is rest and *not* the
   clip's own frame 0: a cyclic clip authored `+14 → −14 → +14` has its whole
   swing subtracted away by a frame-0 reference and becomes a one-sided
   `0 → −28 → 0`, so the pose never crosses rest and both arms sit permanently
   off to the same side — the "zombie arms" look. Rest is also the only
   reference under which two additive layers compose the way an animator
   expects.
4. **Flatten** parent→child in one linear pass; the loader topologically sorts
   limbs so a parent's index is always below its children's.
5. **IK** — two-bone analytic, in model space, strictly a **post-process**.
   IK is never a blended layer: blending two IK results yields a pose that
   satisfies neither end-effector constraint. Reach is clamped to
   `[|L1-L2|+ε, L1+L2-ε]`, every `acos` argument is clamped to `[-1,1]`, the
   root angle uses the `atan2` form, and the bend plane comes from an
   **explicit per-chain pole vector** with a fixed fallback axis when the
   cross product degenerates near full extension.
6. **Pose limits** (`AnimClampPoseLimits`) — clamp every part carrying an
   authored `poseLimit` back inside its range of motion, about the part's own
   **rest** frame, then recompose the subtree below it. Runs *after all IK*,
   never between solves: the IK is what puts a joint out of range, and a later
   solve would undo an earlier clamp.

   This exists because **the ragdoll limits do not bind an animated limb**.
   `MobLimbDef::minAngle/maxAngle` and the swing-twist cone are Jolt
   constraints, and Jolt only enforces them on a *dynamic* body — a live limb
   is kinematic and re-posed every tick, so nothing at all stood between the IK
   and an anatomically impossible leg.

   **Three shapes, because three kinds of joint.** Which one a limb gets is
   read off the authored `poseLimit` object, and no rig authors two:

   - **Axis** (`axis`/`min`/`max`) — clamps only the *twist* about the authored
     axis and leaves the swing free, so "the hip pitches between −20 and +85"
     can be stated without also constraining abduction. Right for the hip and
     the knee, where the solver only ever drives one plane anyway.
   - **Hinge** (the same, plus `hinge: true`) — one degree of freedom, not one
     *bounded* degree of freedom: the off-axis swing is **discarded**, so the
     joint's rotation is a pure rotation about the axis. This is the elbow. The
     two-bone solver picks its bend axis in *model* space from the chain's pole
     vector, and that axis only coincides with the forearm's own hinge axis
     under pure flexion or pure abduction — at any blend of the two it opens
     the elbow sideways, which the axis form cannot see because the on-axis
     component stays perfectly legal. Measured with the flag off, the solver
     takes the forearm **67° out of plane and 103° into hyperextension**.
   - **Ball** (`bone`/`reach`/`twist`, `PoseBallLimit` in `anim.h`) — bounds
     *where the bone may point* with up to two perpendicular half-spaces, plus
     a roll bound about the bone. This is the shoulder, and it is a different
     shape from the other two because the weapon arm is aimed by the **mouse**
     at arbitrary points on the sphere rather than swept through one plane. A
     cone cannot express it: the reachable set of a shoulder is very nearly
     "the sphere minus the region behind the torso minus a wedge across the
     midline", and any cone tight enough to exclude the back also excludes
     arm-straight-out-to-the-side. The normals must be perpendicular — the
     nearest legal direction has a closed form for an orthonormal pair and does
     not otherwise, and the loader rejects a tilted pair rather than solving
     the wrong problem quietly. Measured with the limits opened out, the upper
     arm reaches **89° behind the frontal plane and 60° across the midline**,
     i.e. through the ribcage.

   The limits are authored data (`assets/mobs/*.json`), and they should be
   **measured, not guessed**: `--selftest --gate mob` prints each joint's
   observed range next to its authored one — the hip and knee over a flat walk
   and a ramp climb, the shoulder and elbow over a fan of 14 hostile IK targets
   scaled to 0.72 of the rig's own arm (`arm-limits`). The fan is scaled rather
   than authored in world voxels for two reasons: a literal offset is a
   different fraction of an arm at every `kVoxelMeters`, and an out-of-reach
   target is not a hostile target at all — the solver clamps to its reach
   annulus and a clamped two-bone solve is a dead straight limb, so the elbow
   assertion would measure nothing.
7. **Physics blend / submit** through the existing `MoveKinematicBody` path.

**Gait** is the base layer and needs no per-gait table. Each leg's ideal
contact is `hip + fwd·strideBias + vel·leadTime`, snapped down through
`GroundHeightAt`; a foot unplants when it has drifted past
`stepThreshold·legLength` **and no other leg in its group is swinging**. That
one constraint *is* the gait state machine: singleton groups give a walk,
diagonal pairs give a trot, and losing a leg just means the survivors take
their turns sooner. Stride and lift scale by speed so an idle mob's feet are
genuinely still. **The gait owns `leg`-tagged chains only** — it walked
`sk.chains` whole until 2026-09-10, which meant a humanoid's hands were being
planted on the ground (see "A walking body is a box" above). **Body height is
derived from the foot average**, which is why walking up voxel stairs works with
zero slope-handling code; **body tilt is derived from the GROUND** — two grades
fore/aft and left/right of the footprint, clamped as one total lean — rather
than from a plane fitted through the contact points, which jittered with the
swing phase and never fired at all on a two-legged rig. Pelvis bob runs at 2× step
frequency (one rise per footfall), sway/roll at 1×, plus spine
counter-rotation and a progressive phase lag per hierarchy level.

**The avatar's gait differs from a mob's in three ways**, all forced by the
fact that the *player* owns the body rather than the gait:

- **Body height comes from the player**, not from the foot average — the AABB
  has already resolved against the terrain, and re-deriving height from feet
  whose goal falls back to that same height is a loop that runs away at ~9.5
  voxels/tick. The feet supply only the slope.
- **The pelvis therefore has to CROUCH to buy the stride any reach.** With the
  hip pinned at a fixed height above the ground, the reachable stride is
  `sqrt(reach² − span²)`, and on a rig authored standing that is nearly
  nothing: the stock human's hip sits 6.75 voxels over its ankle against a 6.79
  chain, so the foot could travel **0.7 voxels** before the two-bone solver hit
  its reach clamp — and a clamped solve is one fixed straight-leg pose for every
  target beyond it. `stanceCrouch_` drops the pelvis by the difference.

  **The demand is measured per foot, per tick, and it is a HORIZONTAL question.**
  Solving once for the far end of the stride and holding that height through the
  cycle is the wrong shape and backwards from a real walk, where the pelvis is
  lowest at double support and *highest* at midstance with the supporting leg
  vertical. Held, it put the leg at its most bent exactly where it should be
  straightest: measured 0.77 voxels of permanent crouch at walk pace (88% leg
  extension, ~56° of knee, every tick) and the `kMaxCrouchLegLengths` clamp from
  first step to last at a sprint — a character who walks flat ground kneeling.
  Taking the excursion from where each foot *actually is* makes the pelvis fall
  and rise once per step out of the geometry, with no oscillator to keep in
  phase with anything: 0.28↔0.44 voxels on the stock human at walk pace, stance
  knee at its authored ~9°, swing knee to 68°.

  Only the foot's **horizontal** offset is read; the vertical stays the rig's
  authored `span`. Using the foot's world height instead makes the crouch a
  function of the gap between the body's sole and the surface the probe found,
  so anything that floats or sinks the body relative to its footing is answered
  by burying the pelvis — and it would put `bodyY_` downstream of a probe
  height, which is the feedback path above. The horizontal descends only from
  `origin_.x/z` and the velocity lead, so it cannot close that loop.
- **The IK effector is the ANKLE, so the foot goal is raised by `restSoleY_`.**
  `GroundHeightAt` returns the surface the *min corner* rests on; handing that
  to the solver asks for the full hip-to-corner drop and clamps on every tick
  of every walk.

**One stride clock.** `gaitPhase` on the avatar is driven **by the feet**: the
rate is the measured period between touchdowns and the phase is pulled onto a
half-turn boundary at each one (`PlayerAvatar::SyncStrideClock`). The feet step
on a *drift threshold* while the bob/sway ran off `cadence × speedFactor` — two
clocks that disagreed by ~2.6× on the stock human, which at a 30 Hz tick is a
bob under four samples per cycle and reads on screen as a fast lateral jitter.
It cannot be tuned out, because the ratio itself moves with speed. With one
clock, `bobFreqMul 2` means "once per footfall" *by construction*. The walk/run
clips ride the same rate through `ClipInstance::rate`, so the arms stay locked
to the feet at speeds between the two authored clip periods. The NPC path
deliberately keeps the free oscillator — its swing is a flat `stepDuration` at
mob speeds, where the mismatch is small — and rigs with no leg chains
(`dummy.json`) keep it because they have no foot to lock to.

**The gait keeps its footing across a lost tick, and needs more slack than the
clips do.** `Player::grounded` is a 0.1-voxel positional probe, so on microvoxel
terrain it drops false constantly — cresting a bump, stepping down a voxel (a
genuine ~0.14 s of air at walk pace, longer than `avatar.airDebounce`). Losing
the gait for even one tick is not a subtle artifact: `UpdateAirPose` clears
`footInit_`, so both feet re-plant from scratch, and `gaitWeight_` fades the leg
IK out to the rest hang — the legs snap to standing in the middle of a stride,
over and over. So the gait runs on its own coyote window (`kGaitCoyoteSeconds`),
bounded by **distance** as well as time: air time alone cannot tell a kerb from
a launch, but a jump has left the floor by half a leg within about three ticks
and there is no footing left to keep. A hang or a mantle is support for the
clips but never for the gait — the feet are nowhere near a floor in either.

**Air-state clips fire on events, not on contact loss.** `jump` plays on
`Player::jumped`, a sticky latch set where the impulse is actually applied and
drained by `main.cpp` after the tick batch; losing contact is not a jump, and a
step-down at walking pace clears any debounce. `fall` additionally requires a
real **drop** below the height support was last held at (`avatar.fallMinDrop`),
and its flail is a weight **ramp** over `fallFlailDelay`/`fallFlailRamp` rather
than a pose that switches on — so a short drop never reaches the wide shape and
only a genuine fall arrives at it.

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

**THE AVATAR IS A MOB — one class, two drivers (2026-08-29 refactor).**
`class Mob` (`game/mob.h`) is the per-creature entity: the unified `MobLimb`
list, the animation state, and ONE implementation of every body mechanic —
damage, severing, dying, per-voxel carving (with connectivity splits and
collapse-severing), burning/dissolution, bleeding (gore-variance profile,
gouts, drip spray), item holding, kinematic pose submit and render appends.
`PlayerAvatar` **derives from `Mob`** and NPCs are plain `Mob` records driven
by `MobSystem`'s sense→intent→steer→drive stages. A chemical reaction, blast
or blade that works on an NPC works on the player through the same code by
construction; before the refactor the avatar kept parallel copies that had
already drifted (its explosion damage was an hp-only approximation with no
real carving, its burn flush never rebuilt the collider, its bleeding lacked
the variance profile and drip spray).

What the avatar does differently is confined to two seams. The DRIVER:
`PlayerAvatar::PreTick` takes position and facing from `Player` (plus its own
gait, ledge-hang arm IK, weapon-arm pose, footfall events, and the ANGLES for
its head look — the look itself is `Mob::ApplyAimPart`, which lives on the base
class because a creature pointing its jaws at your throat is the same
operation) where
`MobSystem::PreTick` runs the AI stages — everything else in the two PreTicks
is the same `Mob` upkeep calls. And the EXPLICIT-EXCEPTION virtuals on `Mob`:
`AvatarLayer()` (limbs ride the AVATAR physics layer), `OnBodyReleasedToWorld`
(strip that layer when a part becomes debris), `DropLimbListOnDeath` (the HUD
keeps per-part hp through the death screen) and `MarkInstancesDirty` (own
render slot range). Adding avatar behaviour anywhere else in shared mechanics
is the bug this shape exists to make impossible.

Item holding is BASE-CLASS scaffolding: `Mob::EquipItem` borrows a rig slot
exactly as the avatar always did, so a mob wields a sword through the identical
path (`MobSystem::EquipItem(mobId, ...)` is the id-keyed wrapper; mob combat
needs only an AI that calls it). Each creature owns a per-instance copy of its
skeleton and limb defs (`skel_`/`limbDefs_`) so an item slot can append to any
rig without growing the shared def.

**Damage is the same path as everything else.** A laser crossing a joint
anchor severs that part; the piece is handed to `DebrisSystem::AdoptBody` with
its `MicroBodyRef`, so it keeps its microvoxel detail and is then culled, burnt
and settled by the ordinary debris rules with no avatar-specific code. Bleeding
goes out as `BrushOp`s and `ParticleSpawn`s on the shared per-tick budget, i.e.
through the MutationQueue like every other world edit (rule 3). Every field in
`Mob`/`PlayerAvatar` is CPU-float presentation state and never touches the
hashed grid (rule 1) — `--selftest` determinism is unchanged with an avatar
standing, and the refactor kept the NPC pose/bleed timing byte-compatible
(`SubmitPose(writeXf=false)` preserves the PostStep-lagged transforms the mob
bleed positions always read).

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

**THE EXEMPTION IS PART OF THE HANDLE, and every new handle must re-apply it.**
The layer lives on a Jolt body, and three paths mint bodies for a live avatar:
`BuildRig`, `EquipItem` and — the one that was missed — `RebuildLimbBody`.
Every carve rebuilds a limb's collider, which REPLACES its handle, so the first
bite of acid, fire, laser or blast put that limb back on `MOVING`, inside the
capsule, and `PlayerPushOut` resumed shoving the player in a direction that
swung with the gait. `EmitCarvedFragment` had the same shape: a gobbet is born
inside the capsule, so it takes the avatar layer too and keeps it (a lump of
you that has stopped colliding with you is invisible; one that ejects you is
not).

**A corpse is not a severed limb and does not get released.** `DetachLimb`
strips the exemption only after `kSeverHoldSeconds`, by which time the piece
has swung clear. `Die()` has no such beat — every limb goes dynamic at once, in
the standing pose, entirely inside the proxy — so handing that to `MOVING` is a
dozen unresolvable penetrations and the solver fires the whole ragdoll out of
the capsule. `Mob::Die` therefore does **not** call `OnBodyReleasedToWorld`; the
corpse keeps the exemption for good and keels over where it stood.

**Damage from carving is INCREMENTAL** (`Mob::CarveLimb`). `voxelsAtSpawn` is
fixed, so `at0 - nowCount` is everything the limb has ever lost; charging that
on every carve makes N carves cost N(N+1)/2. Rare carves hid it — burning
exposed it, because `FlushBurn` expresses itself as a carve every
`max(12, n>>6)` voxels removed, so a burning limb carves dozens of times and
reached hp 0 having lost ~14% of itself. `MobLimb::voxelsCharged` holds the
previous numerator; the denominator stays `at0`.

**A straggler may not inherit a limb.** The connectivity split after a carve
gives the limb's identity to the component nearest the joint anchor — correct
between two real pieces, and wrong for a single loose voxel that is merely
*nearer* than the mass (the tie-break only fired on an exact distance tie,
which never happens). Burning is a machine for producing that voxel: it eats
holes, and holes strand specks. A wizard's head, 82% of its skin still there,
was handed to a ONE-voxel component and then severed by
`voxels.size() < kMinFragmentVoxels`; the head is vital, so the creature died.
Only a component that could still BE the limb — `kMinFragmentVoxels` and
`kLimbCollapseFraction` of the pre-split count — may now inherit it.

`MobSystem::WorstSeverFraction()` records how much of a limb was still there
when it came off, at the cut. That is the number that separates the two ways a
limb can leave — it ran out (small) versus something took it while it was still
a limb (large) — and it cannot be sampled from outside: `Die()` detaches every
limb at once, `DetachLimb` cascades to children, and a limb can lose half of
itself to a split inside the tick it is cut. The `mob-burn` gate's
"burn leaves char" asserts on it.

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

**A prone state is placed by the ground, not by an authored number.** A state
may set `groundAlign` (0..1); above 0 it is treated as *lying down*, and
`Mob::SettleClipOwnedBody` — one function, shared by the NPC loop and the
avatar — owns both halves of what that means.

*The fit.* A least-squares plane through a 5×3 grid of ground probes spanning
the creature's own **standing height**, which is how much ground a body covers
when it lies on it. The walker's slope lean is two probes over its footprint,
and that is the right instrument for a torso held near vertical over two small
feet; it is the wrong one for a body lying on the terrain, because two samples
have no redundancy. Real ground is a *mix* of grades, so a two-point estimate
crawling over ground that alternates between, say, 1:2 and 1:1 reports first one
and then the other and re-aims the whole creature every voxel it covers. The
temporal ease on `bodyUp_` sits on top of the fit and cannot substitute for it:
smoothing in time can only lag a spatially wrong estimate, never remove it.

*The grounding.* The body is then lowered until the lowest point of its **posed
core** touches that plane — core meaning parts no IK chain owns, because a
crawl's arms swing through a large arc and grounding on whichever hand is lowest
would lift and drop the whole body once per stroke, letting the pose drive the
terrain instead of the terrain driving the pose. The clearance is measured
**along the plane normal**, not in world Y: a prone body is as long as the
creature is tall, and laid along a grade its lowest *vertical* point is its
downhill end, so placing that end at the ground height under the body's middle
hangs the creature half a body length in the air. A single probe under the
contact point then floors the result, because real ground is not the plane
fitted to it and the residual is largest exactly where a prone body touches.

*The settling* (2026-09-14, the follow-up). A body grounded exactly on that
contact still reads as hovering, and it bobs. Both are the same two facts about
what is being measured. First, contact is the lowest **corner** of the lowest
core collider box, and a box corner touches before the art wrapped around it
does — so a prone body is laid `kProneEmbedVox` (1.5 voxels) *into* the surface
and the parts an IK chain owns are allowed to pass through it, which is also
what a body lying on grass, rubble and its own weight looks like. Second, the
two inputs are not smooth in time the way the fit is smooth in space: the posed
clearance moves with the stroke, and the contact probe is a single column
snapped to whole voxels that is *re-chosen wherever the pose is touching*, so it
walks to a new column whenever an arm moves and hands the difference between two
columns to the body height as a step. That step is the chaotic half of a crawl's
bob, and it reads as the arms shoving the body about, because the arms are what
moved the contact point. The clearance is therefore low-passed over a stroke
(0.6 s half-life, so the body is placed on the stroke's *average* rather than
its extreme — which is most of the remaining float, a minimum over a rocking
body being an extreme) and the probe's lift over 0.1 s, which is inside what
`EaseBodyY` can carry the body anyway. `SANDVOX_PRONE_RAW=1` restores the
unfiltered, unembedded placement as a control arm.

This replaces a per-state, per-rig `bodyYOffset`, which was that rig's hip
height written down by a person and was a third of the way there on the human
and half on the wizard: a crawl clip pitches the **root**, a root rotation
happens about the hip joint, so the torso swings out horizontally at hip height
and stays there. `bodyYOffset` survives as a small sink/lift for states that
are not prone (a hop), where it still means an offset from the walk drive's
ground column. `--gate crawl-slope` crawls a legless humanoid up a ramp mixing
1:2 and 1:1 grades and asserts all of it: the body holds 35.7° against the
fixture's true mean of 36.9° with 5.5° of wander, floats 0.48 voxels and sinks
1.21 over the ramp's interior (1.52 / 0.71 before the settling above — the
contact now straddles the surface instead of sitting over it), and the fitted
grade's standard deviation is
0.29× the footprint-span two-probe it replaced. (Its interior window is not
slack — a *rigid* body bridging the ramp's crest must have a gap, so contact is
only claimed where contact is possible; the crest clearance is reported.)

**Camera.** `Camera` stays orientation-only and is shared by both modes, so
mouse look, the picking ray and the walk basis are identical in first and third
person. `ThirdPersonRig` adds only the position policy: an orbit boom, swept
against the voxel world and pulled in to the first hit (unloaded space counts
as solid — the residency-window rule — or the camera backs out of the world).
Pull-in is instant and push-out is eased; easing inward would leave the camera
inside the wall for the duration of the ease, which is the artifact players
actually notice. In first person the body is hidden but the arms, hands and
staff are kept. The render eye is the only consumer — brush, laser, grenade,
physics and **the audio listener** all keep using `Player::EyePos()` /
`ViewEyePos()`, so no camera setting can move the world hash, and none can move
where you hear from either (§12b, "The ears are on the character").

---

## 9. Rendering

- **Ray traversal, not meshing.** Terrain geometry changes every tick; re-meshing
  churn would dominate. Raymarch the voxel grid directly (DDA through chunks,
  `nonEmpty` flags for empty-space skipping — the same flags the physics uses).
  The sim already lives in GPU memory, so the renderer reads it for free.
- **Pipeline: one fullscreen fragment shader that traces and shades inline.
  There is no G-buffer and no deferred lighting pass** — `raymarch.wgsl`'s `fs`
  marches the primary ray and shades the hit in the same invocation, so albedo,
  normal, depth and material never leave registers. (This bullet described a
  deferred pipeline that was never built; the cost model that follows from the
  real one is the reason W2-A exists — a fragment shader's occupancy is set by
  its worst path, so an inlined secondary-ray tracer is paid by every pixel.)
  Voxel face normals come from the hit axis; liquids take smoothed normals from
  the fullness-field gradient instead (see the water section below).
- **One `trace()` and one `traceOpaque()` (2026-09-01, W2-A).** The 26-field
  media-aware `trace()` in `raymarch.wgsl` has exactly one call site: the
  primary camera ray. Every SECONDARY ray — the shadow resolve pass, the
  cache-off `sunShadowAt` fallback, `traceReflection`, `traceRefraction`, the
  god-ray occlusion test — casts `traceOpaque()` in `common.wgsl`, a media-blind
  DDA returning `{hit, t, cell, axis, sgn, word}`, which is the 6 fields those
  callers actually read. `--render-budget` priced the difference: gating the
  reflection at runtime saved 0.09 ms, const-folding it away saved 3.58 ms, so
  the cost was never traversal — it was the register footprint of the inlined
  copy. `--selftest --gate shadow-cache` asserts the compute-stage and
  fragment-stage casts still agree.
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
  toroidal kFarN³ (512³ since 2026-08-29; was 256³) volumes centered on the player, one byte per
  cell (7 bits of FAR PALETTE SLOT + 1 conservative blocker flag; see below). The far grid is DECOUPLED from the window size (phase 5, when the
  window went 512³): level k cells span 2^(k + kFarShiftBase) fine voxels with
  the shift base chosen so level k's box edge is always 2^k WINDOW edges —
  cascade distances scale with the window at constant memory (1024 MiB total at
  kFarLevels = 8 and kFarN = 512, measured with `SANDVOX_GPUMEM=1`; outermost
  half-extent = 256× the window radius = 6553.6 m at the 512³ window and 10 cm
  voxels, asserted by the `far-fog` gate). kFarN went 256 → 512 on 2026-08-29
  to halve the apparent cascade block from ~6 px to ~3 px: because a cascade
  cell is POINT-SAMPLED at its centre voxel, cell size is also the size of the
  smallest authored structure that survives the horizon, which is the LOD's
  worst case and the one player-built content lands in.
  Levels are filled on the GPU by sampling `genCell()` at stride
  (worldgen.wgsl `far` — the "sieve"), recentered with hysteresis like the
  streaming window, and refilled a plane at a time (managed by
  `sim/farfield`; planes drain at `render.farPlaneFillRate` (256) in play, a
  wholesale reset at `render.farRefillRate` in the game and at `kFarListCap`
  in the headless drain loops).
  **The sieve skips the sky (2026-09-12).** `far` ran a full `genCellCol` for
  all 4,096 cells of a level chunk whatever its altitude, and a level box is
  `kFarN` cells tall — 13 km at level 8 against a few hundred metres of
  terrain — so most of a coarse level's fill was generating air. It now takes
  the ceiling `genChunk` has always had: above `max(farColTopFrom over the
  thread's columns, treeMaxTop())` a row stores the zero word it was going to
  store and skips the sample. Bit-identical output (the bound is exactly the
  three arms `farBlockerBitAt` and `farCellIsSolid` can answer above), and
  measured on one exe against two asset trees, `--frames 900
  --autofly-surface`, the same 265k entries over the same 306 ticks:
  `farField` GPU mean **15.90 → 6.65 ms/frame**, p99 65.5 → 37.3, max 150 →
  47 — 44.5 µs to 18.6 µs per sieve entry. That per-entry cost is what the
  fill caps trade against, which is why they moved with it.
  **The plane backlog is BOUNDED (2026-09-12).** Sustained travel queues
  planes faster than any per-tick cap drains them (measured in surface flight:
  ~1,065 entries/tick of demand), and nothing capped the queue — a flight left
  437,248 entries, 3.8 minutes of drain, still queued. `FarField::Update` now
  coalesces: a face `kFarNChunk` planes deep has turned over the whole box, so
  the level is reset instead, which costs the same entries, prunes the
  superseded ones (`PruneLevel`), drains on the bulk cap and leaves the level
  CORRECT rather than partly stale. Per-level backlog is then bounded by one
  level, and "the horizon is minutes behind" becomes the state a load leaves.
  **A wholesale refill is FINEST level first and is BUDGETED (2026-09-10).**
  A `FullRefill` — startup, `LoadWorld`, regen, a teleport past a level's
  window — is `kFarLevels × kFarNumChunks` = 262,144 sieve entries. Drained at
  `kFarListCap` that is 64 ticks of 267 ms, i.e. 2 fps for the ~16 s after the
  deferred `far` pipelines land mid-play, which was the largest stall in the
  frame and was reported as such. It now takes `render.farRefillRate` (1024)
  entries per tick and at most one slice per FRAME (`FarField::BeginFrame`,
  the rule `Stream::BeginFrame` already applies to window shifts, because a
  catch-up frame runs up to four ticks). Finest first, because the valid box
  makes coarsest-first actively wrong: with only level 8 filled every ray
  leaving the window marches 25.6 m cells, so the first thing outside the
  window is a field of house-sized blocks. Finest first, each landed level
  doubles the trusted radius (51.2 m, 102, 205, …) and the fog opens a band at
  a time. The trade is real in both directions — a smaller slice costs more
  total GPU, because a far dispatch has a large fixed cost — and the measured
  table lives on the knob (`Tuning::Render::farRefillRate`).
  **And the queue is HELD while those pipelines compile.** `EncodeFarFill`
  records nothing until `far`/`farpatch` land, but `PrepareTick` used to pop
  and drop the entries anyway, which emptied `pending_` — so for the whole
  compile (2.5 s warm, 20.6 s on a cold SPIR-V cache) the fog reported the
  full 6.5 km horizon and `FaceWord` reported every level marchable, over
  cascade buffers that were still zeros. Rays escaped through the ground and
  the sky, clouds included, was drawn UNDER the terrain. `Simulation::
  FarFillsDeferred` is true exactly when the fills would be dropped and the
  caller has opted into deferral (the game and the voxel tools; never a gate
  or a `--shot`, which block instead), and the frame loop then skips both
  `FarField::Update` and the drain, publishing only the UBO. The `far-fog`
  gate asserts it: nothing pops, every level reads "reset in flight", radius
  = the residency window. `SANDVOX_FAR_DELAY_S=<n>` holds the compile so the
  transient is reachable without a cold cache. **The renderer marches
  a VALID box, not the level box (2026-09-10).** A level is toroidal, so when
  its origin steps one level chunk the incoming face's SLOTS are the outgoing
  face's and hold the outgoing face's bytes until the sieve refills them —
  ~16 ticks per plane under the play cap, deeper under sprint flight. Marched
  from the full box the tick the origin moved, those bytes were the hillside
  BEHIND the player drawn ahead of them, and the underground of the bottom
  face drawn in the sky when the box stepped up. `FarField` now keeps one
  record per queued plane (FIFO beside the entry queue) and a count of planes
  outstanding on each of a level's six faces, released when a plane's LAST
  entry is dispatched; `FarField::FaceWord` packs the six counts (5 bits each)
  plus a whole-level-pending bit (30) into `FarParams.origins[k].w`, the word
  no other reader used. **The field must hold the whole box (2026-09-12):** it
  was four bits, saturating at 15 against a box `kFarNChunk` = 32 chunks
  across, so a face more than 15 planes behind published 15 and the renderer
  marched the other stale layers AS TERRAIN — the ground from behind a moving
  player, drawn in front of them until the sieve caught up, reached in about a
  second of flight. `world.h`'s `kFarFace*` block now derives the width from
  `kFarNChunk`, a count that will not fit escalates to the pending bit rather
  than clamping, and `scripts/check_invariants.py` pins the WGSL literals.
  raymarch.wgsl `farBox` unpacks it and every far reader (`traceFar`,
  `farShadowDist`, the far AO taps) marches the full box less those faces —
  empty during a reset — so a ray in an excluded slab leaves the level at the
  shrunken face and the next coarser level, which is filled, picks it up at
  the same t by the seam contract `traceFar` already keeps for a ray out of
  `farSteps`. Always conservative: a landed face is published a tick late, a
  reversed face is excluded on both sides until both records drain, and a
  reset voids the level's older records via an epoch. `SafeRadiusMeters`
  subtracts one level chunk PER QUEUED PLANE, not one: the excluded slab is
  that deep, and subtracting one fogged open over exactly the band the
  renderer was refusing to march. The `far-fog` gate steps the player one
  level-1 hysteresis in +x and asserts the +x field holds through the drain
  and clears after, then queues `kFarNChunk + 2` planes on one face and
  asserts the word is exact to `kFarFaceMax` and escalates past it. **Edits reach the far field
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
  payload of `(mat << 12) | cellIndex`), and a SECOND entry point, `farpatch`,
  applies them over the same dispatch extent immediately after the sweep. It
  used to be a block at the bottom of `far` behind one in-kernel
  `storageBarrier()`; it was split out for compile time
  (docs/PLAN_shader_compile.md package C — the patch loop is a second full
  `genColumn` inline copy, and NVIDIA's pipeline compile is superlinear in
  entry-point size), and the barrier is now the pass table's generated edge
  between the two `PT_FARFILL` rows. Same hazard, same order, same values; each
  entry publishes its own half of the `farOcc` word (the sweep stores its count
  and top, the patch folds its own in conservatively-high). The patch
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
  the in-window LOD handoff** (`TUNE_LOD_HANDOFF_DIST`, render group — **ships
  DISABLED at 26 m since the LOD-seam pass, 2026-09-04**; it was 24). The
  handoff is a `min()` clamp on `trace()`'s `tExit`, so it moves where the
  cascade takes over without touching the handoff machinery — the cascade
  start distance, the one-sided seam dither and the `tPrev` ordering all read
  `tExit` exactly as before. PRIMARY rays only (`wantMedia`): a shadow ray that
  gave up at 24 m would report "lit" for a receiver whose blocker is further,
  which unshadows terrain rather than coarsening it. Any value ≥ the window
  half-extent (25.6 m) disables it and the window BOX edge is the handoff.
  Measured worth ~8-11% of the offscreen frame, saturating at 22-24 m
  (`docs/PLAN_surface_flight_perf.md` A1) — and at 24 m it was THE visible
  ring: a `t` clamp on a normalised ray is a sphere around the camera, so every
  representation change the seam carries (cell size 10 → 20 cm, and every
  shading term listed under the seam pass below) landed on one circle on the
  ground at a constant 24 m, where a 20 cm cell is still 6 px. The box edge is
  25.6-44 m away, is not a circle, and is where the fine data genuinely ends.
  Turn the knob back down to buy the 8-11% at the cost of the ring.
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
  near field plus sky reflection on distant water top faces; the texture, AO
  and shadow terms are the near field's own since the seam pass below (phase 4
  had a 0.5 m palette hash, a one-sample AO and a flat ×0.3 shadow lift). Fog
  is aerial perspective: `applyAerial` converges surfaces exactly to
  `skyColor(rd)` (the old ×0.9 target left everything hanging slightly darker
  than the sky it should dissolve into, which read as a gray veil).
  **The LOD-seam pass (2026-09-04) — one rule: the only thing allowed to
  change across a seam is the cell size.** Studied against a shipped mesh-LOD
  voxel game (Fortune's Favor, `docs/refs/`), whose seams are invisible not
  because they blend (they hard-swap 62 m chunk meshes at 2^k block scale, no
  morphing, no crossfade) but because NOTHING ELSE changes: the texture tiles
  in world units at every LOD (`TexCoord * scale`), and lighting, shadows, SSAO
  and fog are deferred screen-space passes that do not know which LOD wrote
  the pixel. Our seam changed five things at once, and each is now matched:
  (1) **texture** — the far palette variant is `synthJitterState` at the fine
  voxel just inside the hit face (the exact positional formula worldgen writes
  into every pristine voxel's state nibble) and `surfaceGrain` on the same
  fine lattice, so a cascade cell wears the speckle the voxels under it would
  have shown; `grainAmpFar` ships equal to `grainAmp`. (2) **AO** — `farVoxelAO`
  is `voxelAO`'s four-tap rule over cascade cells at `aoStrength`, occluders
  being MATERIAL cells only. (3) **shadow** — `farShadowDist` returns the
  blocker distance and the far hit takes `shadowFromOpaqueHit`, the one
  softening law the terrain and the raster bodies share; levels ≥ 3 keep a
  floor at `shadowFarLift`. Its reach is `render.farShadowReach` in metres,
  converted to steps per level and capped at 64: it ships at 24 m (was 60)
  because once the blocker flag stopped being a caster an UNSHADOWED ray walks
  the whole reach — the owner's live flight measured 38 far-shadow steps per
  pixel against 136 far-march steps, on a frame that was 81% cascade — and a
  caster a cascade pixel can show is a canopy or a ridge within a few tens of
  metres. Likewise `render.farSteps` is an LOD handoff rather than a cliff: a
  ray that exhausts a level's budget hands the next level its STOP POINT, not
  the box exit (which left the rest of that level marched by nobody — a hole
  on a grazing hillside), so the budget may be tuned for the frame and its
  cost is 2× cells sooner along grazing rays. And the `farOcc` word is no
  longer only a count: bits 16..20 carry ONE PLUS the level chunk's highest
  non-empty row (`farOccPack`/`farOccTop`, common.wgsl; `far` measures it,
  `fardown` raises it by atomicMax and nothing lowers it, so an edit that
  clears a cell leaves it stale-HIGH, which only costs steps). Occupancy alone
  skips chunks with nothing in them, and every chunk of the surface band has
  something: a ray 12 m up pitched at the middle distance walked ~90 level-1
  cells of air per band chunk before it met the ground. Above that row
  `traceFar` and `farShadowDist` now jump to the chunk's exit face (ascending)
  or drop straight onto the row (descending) and resume the DDA there —
  exact, measured pixel-identical to the cell-by-cell march within the
  wind-animated noise of two frames. (4) **plants** — `farCellIsSolid` drops MATF_MICRO
  materials from the sieve, the downsample and the patch path: a grass tuft or
  a flower is a mostly-air cell the renderer fills with blades, and its centre
  sample had been a solid cube of the plant's palette (20 cm at level 1, 25 m
  at level 8 — the straw boulders on every far meadow). (5) **the handoff** —
  the 24 m sphere above is off. Measured on `screenshot_cascade`'s world,
  far-meadow mean RGB vs the near meadow's 126/172/110: before 108/138/99,
  after 126/165/109; with far shadows off entirely 128/168/111, so what
  remains is real cast shadow. What was tried and taken out: painting the
  surface cell under a tall-grass column with the strand material (the meadow
  at 25 m reads as skin between thin blades, and the strand palette made the
  far side darker than the near) — a blend would need a far-only proxy
  material, not a cell rule. What is still not matched, in order of visibility:
  the blades themselves end at the window edge (analytic plants have no far
  representation); the 2× cell step is a 3 px → 6 px block change at the seam
  and at every level box; the near field's one-bounce GI and openness terms
  have no far equivalent; far water is an opaque disc. The structural answer
  for the first two is the mesh game's: surface memory scales with area, so it
  keeps full-detail chunks out to ~500 m (1.5 px per block at the swap), where
  a dense cascade level costs volume — see `docs/PLAN_far_field_cascades.md`
  §5.6 for why `kFarN` is the only knob and what a sparse near level would
  take.
  **Those seven bits are a PALETTE SLOT, not a material id (2026-09-09):**
  a far cell byte names one of 128 entries in the FAR PALETTE — the fourth
  reserved run of the GPU material table (`kFarPaletteBaseGpu`, world.h,
  under the stain / art / tint runs and costing no new binding). Entry
  `FAR_PALETTE_BASE + slot` holds, in its `flags` word, the material id that
  slot paints. Two directions, two homes, neither of them a new buffer:

  - **material → slot** rides in every real material's own `flags` word at
    `MATF_FAR_PAL_SHIFT` (bits 24..30; `kMatFarPalShift` in materials.h is the
    C++ mirror). The three sites in `worldgen.wgsl` that write a far cell —
    the sieve `far`, the edit patch `farpatch` and the downsample `fardown` —
    read it through `matFarPal()`, so a writer pays one field of a material it
    was already fetching and no table lookup at all.
  - **slot → material** is `farPalMat()` (common.wgsl), used by
    `raymarch.wgsl`'s `farMatAt` and by `sim_gas.wgsl`'s `gasFarBlocked`. The
    "is anything here" tests do NOT translate: slot 0 is air, nothing else
    maps to material 0, so `farPalAt(..) != 0` is the same answer without the
    read, which is what AO, `farShadowBlocked` and the occupancy count use.

  WHY. Before this, the byte WAS the id, so `LoadMaterials` had to refuse a
  129th material outright — the voxel word had room for 4096 and this byte had
  room for 128 — and nothing else in the engine would have noticed if the
  refusal were deleted: the 129th material would simply have painted the wrong
  colour at distance and claimed a blocker wherever bit 7 landed. The
  indirection turns 128 into a budget on how many things may look DIFFERENT
  FROM EACH OTHER at cascade scale, which is a far-field question, instead of a
  cap on the material table, which is not one. Two materials that are
  indistinguishable at 50 m share a slot:

  ```json
  { "id": "sandstone", "class": "solid", "far": "sand" }
  ```

  resolved by name at load. An alias may not point at another alias (one hop,
  so the slot a byte names always belongs to a material that paints itself) and
  may not point at air (slot 0 means an EMPTY far cell, not a transparent one).
  The loader now errors only when the number of materials WITHOUT an alias
  exceeds 128, and says which knob fixes it; `check_invariants.py` counts the
  same thing off materials.json, since adding a material is a data edit that
  needs no build.

  Slots are handed out IDENTITY-FIRST — material `i` takes slot `i` while `i <
  128`, and only the ids pushed past seven bits take whatever the aliases
  freed. That is deliberate and load-bearing rather than tidy: on an unaliased
  table every far byte is bit-for-bit the byte the same worldgen wrote before
  the palette existed, which is why introducing the whole indirection moved
  neither the world hash nor a single smoke probe.
  **The far cell byte is 7 + 1, not 8 (13.2.2, 2026-09-01):** bit 7 of every
  far cell is a CONSERVATIVE BLOCKER FLAG — "pristine worldgen puts something a
  ray would stop on somewhere inside this cell's fine footprint" — and the low
  seven are a FAR PALETTE SLOT (`FAR_PAL_MASK` / `FAR_BLOCKER_BIT`,
  common.wgsl; every reader masks). The flag is a pure function of (coords, seed)
  (`farBlockerBitAt` in worldgen.wgsl) for the same reason `farSurfaceMat` is:
  the sieve has no live grid, so a flag derived from real voxels in the
  downsample would disagree with it at their shared boundary. Its cost is one
  comparison for all but ONE cell per column — the surface band, where the four
  corner columns are sampled — and its blind spot is edits, which reach the far
  field only through the material byte. `farShadowDist` does NOT treat it as a
  caster (it did, at every level, until the LOD-seam pass): the flag covers the
  whole band of cells the ground surface passes through, so honouring it
  shadowed most of the far surface at near-zero distance — the ×0.3 lift was
  hiding that as a ~20% general dimming. A primary ray honours it only up to
  `render.farBlockerHitLevel`, which ships at **0**. That default is a measured kill-criterion result, not
  timidity: at 2 the visible half does what it was built for (the half of every
  surface cell whose centre sampled air comes back, so a 60 m snow patch stops
  being a dithered smear) but the same one-cell lift buries the single-cell
  ground cover standing on that slope and paints flat facets where a cell hits
  on the flag alone. The distant ridge is not the casualty — the sky silhouette
  is pixel-identical at 0 and 2, because levels ≥ 3 never take the flag.

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
  half-extent of the FARTHEST COMPLETE one, so a cold start or teleport fogs
  out the bands still in the queue instead of showing sky through them,
  clamped between the full-horizon pin (`kFarFogDensity`) and a ceiling at
  level 2's half-extent (`kFarFogDensityMax`, so the residency window is never
  fogged away) and eased over a few frames. *Farthest complete*, not "the one
  below the innermost incomplete", which is what it read until 2026-09-10: the
  levels are nested boxes all centred on the player and `traceFar` tests each
  independently, so a level `farBox` has collapsed is skipped and the next one
  picks the ray up at the same `t` — a complete level covers everything inside
  its half-extent as well as at it. Reading it the pessimistic way pinned the
  fog on the residency window for the whole of a refill and then snapped it to
  6.5 km, which is the "everything suddenly goes extremely foggy" half of the
  report that also produced the two paragraphs below. Determinism is
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
  - **The body is LIT (2026-09-02).** The palette colour is an albedo and goes
    through the same hemisphere ambient x openness + key light x cached sun
    shadow as an opaque hit. It went into the frame raw before, and because
    `tonemapHdr` + gamma put a lit surface at about a third of its authored
    value, unlit blood rendered two to three times brighter than the same hex
    on the wall next to it — a #3c0909 pool measured (133,72,72) on screen
    with every reflection and sheen term removed. That is why "make the
    palette darker" could not fix it, and why the material now matches the
    stain it leaves (which was always lit, being an albedo change).
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

  Perf note (historical): `heatSpill()`, the four-tap molten-light stand-in,
  cost ~13 ms of a 30 ms frame unguarded and ~1 ms gated. It was DELETED on
  2026-09-02 by P3 of `docs/PLAN_gi.md`: emission is part of the irradiance
  sample both injection paths deposit (`irrSample`), and the one-bounce gather
  (§9.y) lights the rim rock at no per-pixel cost beyond the gather itself.

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
solid cell whose material carries `MATF_MICRO`, the cell is RECORDED and the
march continues; after the march resolves, a nested Amanatides–Woo DDA runs over
the `subdiv³` brick in cell-local space for each record (see "The detail model
is resolved in a SECOND PASS" under analytic plants below — the same two-phase
machinery carries bricks, analytic plants and anything later that spans more
than one cell). A hit reports the sub-voxel's material and the face it was
entered through; a **miss lets the ray continue past
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

### Analytic plants and the trample field (2026-09-04)

The brick path above gave every plant ONE 10 cm cell of 8³ voxels and a
two-frame flipbook, and a meadow of it read as scattered prefab clutter that
twitched. The tall grass had already left that path (`MICROF_STRANDS`, 2026-08-22:
analytic blades, ray-shear sway); this generalises it. `MICROF_PLANT` replaces
`MICROF_STRANDS`, a material's `micro.plant` block replaces `micro.strands`, and
`tracePlant` (raymarch.wgsl) intersects four kinds of plant in closed form:

| kind | primitives | column / tile |
|---|---|---|
| `grass` | tapered flat blades (six half-planes linear in t), seed-head ellipsoids | column |
| `flower` | a stem blade, leaf boxes, a head: disc with petal notches / bells / spike of buds / cup | column |
| `mushroom` | cone-frustum stem, clipped ellipsoid cap (flat gill underside), hashed spots | column (clusters) or tile (one big toadstool) |
| `fern` | a rosette of quadratic-Bézier rachises, each three oriented boxes perforated per leaflet | tile |

**Column vs tile.** A column plant lives in one column and every cell of the
column rebuilds it from `microColumnHash` — the contract the strands had. A
TILE plant is wider than a cell, so worldgen paints its whole footprint (`foot`²
columns, `base+1..base+h` cells) with one material and the renderer rebuilds the
same plant from the tile hash in every one of those cells. `plantTileAt` in
`common.wgsl` is the ONE function both sides ask "which plant, where": tile size,
footprint, height range and a hashed centre strictly inside the tile (so two
tiles' footprints never overlap). Worldgen's `plantColumnAt` evaluates it once
per column and `Col.plant` carries the answer to every cell; the base is the
CENTRE column's ground so the plant is rigid across a slope. The species
constants (`PLANT_FERN_*`, `PLANT_SHROOM_*`) live in `common.wgsl`, are
transcribed to `sim/plants.h` for the `--shot` harness, and restated per
species in `materials.json` for the loader — `check_invariants.py plants` holds
the three equal.

**One evaluation per TILE plant per ray — and NOT per column.** A fern or a big
toadstool is one organism across a 3×3×H footprint and `tracePlant` intersects
all of it in one call, so phase 1 records it once (`plantTileKey`, compared
against the last record) and every later cell of the same tile is free. A
COLUMN plant may NOT be collapsed this way, and one attempt to was reverted on
2026-09-11: `tracePlant`'s column branch intersects only the segment of the
plant **inside the cell it was called for** (`yTop = min(1, sH - rise0)`, Y
half-planes in cell-local coordinates), so a remembered hit can only lie in the
cell it was computed in and every later cell of the column resolves to a miss —
the blades above the first cell a ray enters are simply never drawn, visible in
the `plants` gate picture as horizontal bands of missing blade at every cell
boundary (37,097 px over 24 SAD). Collapsing a column needs `tracePlant` to
intersect the whole column in one call first. The clip bound for a column is
therefore the ray's exit from the CELL, NOT 1e9: `tracePlant`'s per-blade
footprint cull derives `segLo`/`segHi` from it, and 1e9 would silently disable
the cull that makes the blade loop cheap. A tile plant's bound IS 1e9, because
`plantTileAt`'s non-overlapping footprints already bound it.

**The detail model is resolved in a SECOND PASS, after the march (2026-09-11).**
`trace()` used to call `tracePlant`/`traceMicro` from inside its DDA loop, and
that placement — not the kernel — was most of the frame. The march carries ~25
loop-live values plus `out : Hit` (26 fields), all live across the call; the
allocator capped the fragment at 168 registers and spilled, so the loop's own
state moved to scratch and **every pixel in the frame paid the fill traffic,
sky included**. The `meadow` `--render-budget` camera measured `microMaxPerRay`
→ 0 at **8.95 ms of a 19.73 ms frame (45%)**, on a frame whose counters read
`rmMicroEnters = rmPlantEvals = 0.0` — nothing was evaluated; removing the code
was what saved the time.

So the fix is not to make evaluations cheaper or rarer. It is to get the kernel
out of the loop. **Phase 1**, inside the DDA: a primary ray entering a
`MATF_MICRO` cell within the LOD records `(cell, tEnter)` into one of
`DETAIL_SLOTS` fixed scalar pairs — window-local cell packed 10 bits per axis —
and keeps marching, treating the cell as air, which is what a MISS already did
and a miss is the common outcome. **Phase 2**, at the function's tail: walk the
records in `t` order, evaluate each, take the first hit nearer than the opaque
hit the march resolved. Records are scalars and never an array, because WGSL
puts a dynamically indexed array in local memory — the exact thing being
removed. The three in-loop `return out` became `break` so the tail has a single
exit to run in.

| | Register Count | local memory | meadow | noon |
|---|---|---|---|---|
| in-march | 168 | +160 B/thread | 19.73 ms | 16.54 ms |
| **two-phase** | **128** | +160 B/thread | **9.35 / 8.69 ms** | **9.54 / 9.49 ms** |

**The mechanism is OCCUPANCY, not spill.** The spill did not move by one byte;
the register count fell 168 → 128, which is 16 warps per SM instead of 12 —
and `trace()`'s own `sgn3` note already measured that trade in the other
direction (hoisting a dynamic index bought 128 → 168 regs and zero spill, and
cost 3.5 ms). What moving the call bought was the allocator's freedom to take
the low-register plan, plus the fact that whatever spill traffic remains is now
executed only by the pixels that recorded a detail cell. Image parity on the
`plants` gate fixture (grass, fern, toadstool): 678 px over 24 SAD against a
same-shader re-run control of 617 — measured at the SAME gate scope on both
arms, which matters: the same frame taken inside a four-gate `--verify` instead
of alone differs from itself by 162k px.

Two things phase 2 must do that the in-march form did not. It must **unwind the
volumetrics accumulated behind a winning surface** — the march walked through
the cell, so a water interface, a glacier or a plume past it was recorded for a
backdrop the detail hit now covers, and `fs()` shades `liqT` with no ordering
test against `h.t`; latched crossings at or beyond the hit are cleared with
their path, and the media accumulators restore to a snapshot taken at the first
record. And it must keep **clipping the model to the cells that exist**: a hit
that falls in a cell worldgen never painted — dug out, cut by a trunk — is not
reported, which is exactly the clipping a partial plant must have.

Only records charge `microBudget`; cells collapsed onto an existing record are
free, and `render.microMaxPerRay` is clamped to `DETAIL_SLOTS` (4). The
traversal cost of a REAL meadow remains unmeasured — the harness world holds
756 plant cells and no leaves at all, so everything above is a PRESENCE cost.
Full arm tables in `docs/PLAN_frame_perf.md`. Past
`TUNE_MICRO_LOD_DIST` only the centre column of a tile plant stands in as the
solid proxy; the outer eight pass as air, or a distant fern is a 30 cm cube.
Column plants take the SHORTER of that and `render.plantLodDist` (16 m): an
evaluation is a wind sample plus six to eight blade tests, charged for every
cell a grazing ray crosses up to `microMaxPerRay`, and a blade is sub-pixel
long before its cell is — at 40 m a meadow ran at a third of the frame rate of
snow (2026-09-04). Inside the grass loop each blade's chord box is tested
against the ray's XZ footprint through the cell before `hitBlade`, exact and
conservative, so most of a tuft's blades cost two hashes and a compare.

**Density is a look knob and it was halved (2026-09-04).** Every ground-cover
rate — `flowerAt`'s per-mille thresholds and tall-grass stand density, the
`UG_*_CHANCE` undergrowth rows, `PLANT_FERN_CHANCE` / `PLANT_SHROOM_CHANCE`
for the tile plants, the shore/pond `worldgen.*Chance` tuning rows and the
`chance` of every biome cover row in `assets/biomes/*.json` — is half what the
plant overhaul shipped with. Not for the raymarch: while flying the live
telemetry showed ~0 micro steps per pixel (plants are cubes past
`plantLodDist`). For WORLDGEN and the far refill, which were 8 + 16 ms of a
53 ms GPU frame in flight: a column inside a fern footprint pays a second
`landColumn` and a 25-tile tree scan (`plantSiteAt`) in `genColumn`, which the
`far` sieve runs 256 times per level chunk, and every placed cell is one the
renderer treats as a micro model. Trees are untouched.

**What the flipbook could not do and this does:** continuous displacement in
time (the wind is sampled once per plant at its base, every part blends the two
gust bands with its own hash weights), taper, per-blade yaw and static lean,
seed heads on a third of the tall blades, real flower heads, mushrooms of
varying size in a cell and one knee-high toadstool per 7×7 tile, world-space
normals (`MicroHit.n`, `curved`) so a cap shades as a curved surface, and a
per-PART palette key (`MicroHit.key`) so a cap spanning nine cells is one colour.

**The trample field.** A bounded ring of footprint stamps (`sim/trample.h`,
`kTrampleCap` = 48) rides `RenderParams` exactly the way the wave-impact ring
does: the frame loop presses one under the player and every mob each frame
(refreshing the stamp it stands in, laying a new one every ~half radius), and
`trampleAt` in `common.wgsl` sums them at a plant's base — flat under the foot,
soft rim, held while pressed, recovering as `(1-r)²` over `render.trampleRecover`.
Plants compress by `render.trampleDepth` and lean away by `render.trampleLean`.
Render-only and never hashed, by pointer like the wind primitives (it dynamically
indexes a uniform array in the per-cell plant path). The honest limit, stated
so nobody re-derives it: a column plant cannot leave its column, so a trampled
stand is a crushed mat that leans, not blades lying flat across their
neighbours; lying flat needs the directional over-march of upstream columns,
affordable only inside the stamp and not built.

**Determinism (rule 1) is untouched**: all of this is render-side, keyed on
`(seed, cell)` hashes and `R.time`; worldgen's half is integer hashes on the sim
input stream, and moving the placement moved the pinned hash once, as any
worldgen change does.

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

**A VOLUME NEEDS A DEPTH TOO, and it is the MEDIAN one.** The raymarcher runs
first and the raster passes draw on top of the depth it wrote. Gas is not a
surface — `trace()` accumulates it and keeps marching — so a pixel full of fire
reported the depth of the SOLID BEHIND the flame, or the far plane for a ray
that crossed the plume into sky. Every mob, debris chunk and dropped item
therefore passed the depth test against fire it was genuinely behind and drew
straight over it: fire never composited in front of a rigidbody at any distance
or density, and a burning creature had its own flames painted behind it.

Writing depth at the plume's FIRST cell is worse than the bug — a single wisp of
smoke would then hard-clip a body standing behind it. `Hit.gasHalfT` is the
point where the accumulated GAS crosses half opacity (`MEDIA_HALF_TAU`, ln 2;
~1.8 cells at fire's authored opacity 150/255, ~4 at smoke's 70/255), which is
the honest single depth for a volume: a wisp never reaches it and nothing
changes, geometry beyond it is more hidden than shown, and geometry in FRONT of
it still wins the reversed-Z test and draws over the flame. Liquids are excluded
— they have a real interface (`liqT`) and already report it. Ordering geometry
*inside* a plume still needs order-independent transparency.

Gated by `--selftest --gate fire-depth`, which renders a stone block behind a
fire slab and the same block in front of it and compares both against the flame
alone: **56 px vs 93,433 px**. Both arms are the claim — a first-cell depth
would also pass the "behind" half while wrecking the "front" half, so an arm
that only checks the fix is not a test.

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

**Shadows: received, not cast** (since 2026-09-04; was "none, v1"). The micro
pass casts one sun ray per fragment against the VOXEL GRID
(`bodySunShadow` in `common.wgsl`), so a limb in a building's shade is lit like
the ground it stands on. The old rule survives intact where it mattered — shadow
rays still never iterate MODELS, so no body shadows itself or another body and
the per-fragment cost does not scale with the number of micro bodies. A coarse
render-side occupancy proxy remains the stretch goal for body-on-body.

**The smooth normal, and what a brick boundary means** (added 2026-09-11;
`BODY_SMOOTH_N` in `microbody.wgsl`, `MicroBodyModelGpu::cutFaces`). The DDA's
face normal is the last-stepped axis, so a rounded arm made of 8 mm skin voxels
shaded as a staircase of six flat tones. The cure is `shadeViscous`'s: 26 taps
of the occupancy field the brick already stores, weighted 1/|d|, mixed with the
face normal at `BODY_SMOOTH_N` so a deliberately square limb keeps its edges.
26 taps and not a 6-tap central difference, because on a binary field the latter
can only quantise the normal to the 26 directions the staircase already had.

That gradient has to decide what lies one cell OUTSIDE the brick, and there are
two right answers. Past the SIDE of an arm is air, and assuming so is what
rounds the silhouette. Past its END is the FOREARM, and assuming air there
rounds the cap too — which put a hard light/dark ring at every shoulder, elbow,
hip and knee, swinging with the joint as it animated while the two overlapping
OBBs traded depth ties under the TAA jitter (owner report: "the conjoining seams
of limb nodes pulse and flash"). The cases are locally identical — a solid
boundary plane with unknown beyond — and shape does not separate them either:
on chunky voxel art the flat side of an arm is as solid a boundary plane as its
cap. What knows is the ART: every limb of a mob is one model of ONE prefab in
one shared frame, so a joint is exactly a face another model is pressed against.
`MicroBodyCutFaces` (`sim/microbody.cpp`) measures that once at load (half the
solid boundary cells covered is the line, so no stray voxel crosses it and no
ordinary joint fails) and packs six bits onto the model record's spare word. The
shader clamps the field across a cut face and treats every other face as air. A
single-model prefab — an item, a debris chunk, a carved COW clone — gets 0 and
the pre-2026-09-11 behaviour exactly. (`return 0u` from
`microBodyCutFaces()` in the shader is the oracle: one WGSL line, no rebuild.)

**The rule is about MULTI-MODEL PREFABS, not about creatures** (2026-09-12).
It lived in `mob.cpp` for one day and that cost something: a WORN ITEM has
exactly the same shape — one named model per covered limb, in one shared frame
(`game/item.h ItemCover`) — so every garment packed with `cutFaces = 0` and grew
a rounded end cap on each sleeve, yoke, cuff and hem, with the caps at a seam
shading away from each other as the joint they straddle swings. It is now
measured for item covers too, carried on `ItemCover::cutFaces` so the FIT
resample (`Mob::AppendWornShell`) hands the same mask to the re-packed brick —
a resample moves cells, never the question of which plane is a seam.

**Two bricks that own the same cell.** A garment's panels deliberately OVERLAP:
`scripts/gen_stock_armor.py` gives the armpit cells to BOTH the sleeve and the
body panel, because whichever side cedes them shows a stripe of bare skin
through half the gait (the sleeve's inner wall is what you see when the arm
swings forward, the torso's side column when it swings back). That was free
while both bricks shaded a shared cell identically — the generator says as much,
"z-fighting is only a defect between things that look different" — and
`BODY_SMOOTH_N` made them look different, because each brick differentiates its
OWN field and cannot see the other. The tie that now decides which shading you
see is float noise: the two panels ride different limbs, so the same world plane
is reached through two different quaternions, `tCur` agrees only to rounding,
`GreaterEqual` picks per PIXEL, and every idle-sway frame re-rolls it (owner
report 2026-09-12: the overlapping parts of a robe pulsing and swapping). So the
order is made explicit — `BODY_Z_PRIORITY` in `microbody.wgsl` pulls each body
toward the camera by a relative slice of its view depth keyed on its render
slot. Any consistent winner removes the flicker; both panels still cover the
body, so the choice only decides which one's shading shows at the seam, and a
seam that holds still reads as a seam. Relative and not absolute, so it is a
fixed number of depth steps at any distance: the largest bias is 1.9e-3 of view
depth, four thousand times the rounding it must beat and under a hundredth of a
voxel at arm's length.

**Bounds and cost.** The per-fragment DDA is hard-capped at `3·maxDim + 4` steps
(worst-case diagonal of the brick) with no data-dependent loop bound anywhere.
The draw list is CPU-compacted, so the instance count IS the number of micro
bodies and a world with none does no upload, no bind and no draw. Determinism is
untouched: the pool, the model table and the draw list are bound to the
microbody pipeline and to nothing else, and `--selftest` reports an unchanged
world hash with a scale-2 critter walking through the scene.


### 9.x The openness grid — the ambient's only spatial term (added 2026-09-02)

Plan of record: **`docs/PLAN_gi.md`** §2 (phase P0 of indirect light). This is
the binding summary.

**The problem.** `ambientAt(n)` (raymarch.wgsl) and its raster twin
`ambientAtP` (common.wgsl) are a hemisphere lerp on `n.y` between
`TUNE_AMB_GROUND` and `TUNE_AMB_SKY`. Pure functions of the NORMAL: no term in
either knows where the receiver is. So a cave floor was lit exactly as brightly
as a meadow, a room exactly as brightly as the field outside its door, and
`voxelAO`'s three in-plane taps cannot see a ceiling 3 m up. It was also the
reason a large flat single-material face — a wall, a table top, bare dirt —
rendered as a dead-uniform polygon with one hard edge and no gradient
(`PLAN_gi.md` §0's look test).

**The data.** `openness`: one BYTE per (chunk SLOT, 4³ sub-occupancy block,
face) = `kNumChunks × 64 × 6` = 12 MiB, plus `opennessGen`, one word per slot
(128 KiB). Byte index `((slot * blocks + block) * 6 + face)`, viewed through an
`array<u32>`. The value is the unblocked fraction of that face's hemisphere
within `render.opennessReach` metres.

Six values per block, not one, because this world is full of one-voxel walls,
floors and trunks and a scalar would average a wall's lit side with its dark
one. A receiver reads only the face that faces it, keyed on the same
`face = axis*2 + (sgn > 0)` the shadow cache uses.

Render-only derived data, with exactly the standing of `shadowCache` and the
far-field cascades: **never hashed, never saved, never read by the sim**, and
`determinismHash` unmoved is P0's cheapest correctness proof.

**The writer** is `sim_openness.wgsl`, two `pass_table.def` rows on the TICK
table — `opennessDirty` (indirect over the tick's compacted dirty list,
immediately after `occupancyDirty` so the mask it marches is the one that tick
rewrote) and `opennessRefresh` (a flat `render.opennessChunksPerFrame` slots per
tick, round robin from a cursor derived from `T.tick`). One workgroup per chunk,
`OPEN_WORDS_PER_CHUNK` threads, one whole u32 per thread — four consecutive
(block, face) pairs, computed and stored with a plain store, because 6 bytes per
block does not divide a word and a thread per (block, face) would be a
read-modify-write race.

The march is **`traceOpaque` in coarse mode from t = 0**, i.e. the existing
media-blind DDA stepping 4³ blocks over the blockers-class sub-occupancy mask.
Not a new DDA: two block-steppers that must agree is the bug `common.wgsl`'s
tracer block exists to prevent. Five directions per face (the normal, weighted
double, plus four at 45° toward the tangents), so a face's value is
`unblocked / 6`.

**Why it is on the tick table and not the per-frame shadow table.** Its input is
the tick's dirty list; recorded per frame it would re-walk the same chunks two
or three times per tick for an identical answer, i.e. cost that scales with
FRAMERATE. The compute → fragment hop still gets its barrier, from the global
memory barrier every command buffer opens with (`vulkan_barrier_graph.md` §3.4).

**Staleness has two clocks and both are one-sided toward the old look.**
`opennessGen[slot]` holds a hash of the WORLD CHUNK COORD the bytes were
computed for; the window is toroidal, so a slot is silently reused as it walks,
and a reader whose stamp does not match falls back to the plain `n.y` lerp. And
an edit only dirties the chunks it WRITES: a roof stamped 1.4 m above a floor
does not re-walk the floor, which darkens when the rolling refresh reaches it
(`kNumChunks / render.opennessChunksPerFrame` ticks). Dilating the dirty list
instead is not available — a 12 m reach dilates to a 15³ chunk neighbourhood.

**The refresh skips a world where nothing changed** (2026-09-05,
`docs/PLAN_frame_perf.md` §3 item 4). `opennessGen` carries two more planes
(`kOpennessGenWords`; `OPEN_WALKED_BASE` / `OPEN_TOUCH_BASE` in
`common.wgsl`): the tick of each slot's last FULL walk, and per slot COLUMN
(x, z) the tick something within `opennessReach` of that column last changed
geometry. Every dirty walk stamps the 17×17 columns around its chunk
(`openTouchAround` — column STAMPS, not the chunk WALKS the paragraph above
refuses), and so does the refresh when it meets a chunk that arrived in a slot
with a stale stamp (its neighbours' faces were marched against whatever the
slot held before). A refresh visit whose column was not touched since the
slot's last full walk keeps its bytes and does only the irradiance
maintenance — the coarse sun re-sample for a face that marched, the decay for
one that could not — because that half must never stop (the
charger-with-no-expiry bug). "Touched more recently than walked" is compared
modulo 2³², so a tick clock that jumps between harness fixtures, or a slice
the pass was not recorded on, cannot age a touch away. A solid sentinel chunk
(`UNIFORM`/`JITTER` of a ray blocker) settles every interior face as buried
without the four-column origin search; only a face on the chunk boundary can
be exposed. The roof-over-floor case above therefore darkens the floor on its
next visit rather than never, which the old flat refresh also did — the skip
changed the cost, not the answer.

**The readers.** `ambientAt`'s near-field terrain hit multiplies the hemisphere
ambient by `opennessScale(opennessAt(...))`, alongside `ao` and never the sun
(the sun has its own shadow ray). Far-cascade hits keep the plain lerp — the
grid is keyed on residency slots and a cascade hit is outside the window by
definition. Micro hits sample the cell BELOW with the +Y face, because a grass
tuft is not a ray blocker and its own block has no entry. `microbody.wgsl` uses
`opennessScaleAtBody`, which walks down at most six blocks to the ground the
body stands on and takes that surface's value: a body is not in the voxel grid,
so its own block would read "open sky" and a mob would glow in a cave. The
sample is bilinear over the four blocks in the FACE PLANE; nearest-only tiles
visibly at 40 cm blocks in the middle of a smooth wall.

**A MULTIPLY, not `mix(ambGround, ambSky, openness)`.** The mix form makes a
fully enclosed surface read as `TUNE_AMB_GROUND`, which is what a downward-facing
surface already gets in open daylight — so a cave floor would come out exactly
as bright as the underside of an outdoor overhang, and it throws away the `n.y`
shape cue entirely. Multiplying keeps hue and shape, makes enclosure actually
darken, and leaves a fully open face BIT-IDENTICAL to the pre-P0 image.

**The resolution trap, and the rule that came out of it.** A face whose
neighbouring block already holds a blocker skips the march — but it writes
"no opinion" (255), NOT "fully enclosed" (0). At 40 cm blocks over 10 cm voxels
the mask cannot tell "buried inside rock" from "there is a one-voxel terrace
step in front of me", and this terrain is a staircase of one-voxel steps.
Writing 0 painted every riser on every hillside hard black — measured on the
tall-grass shot, mean luminance −12.5/255 with 43% of pixels moved, which is
precisely the banding `wrapDiffuse` exists to remove. With 255 the same shot
moves −1.2 with 17% of pixels, and nothing P0 exists to darken is lost: a cave
floor, a room floor and the ground under an overhang all have AIR in the block
in front of them, so they march. **The conservative direction for a lighting
grid is the one that never darkens something wrongly.**

**THE AMBIENT IS A SUM, NOT A FLOORED PRODUCT** (2026-09-11). The reader used
to be `ambientAt(n) * ao * max(openness, render.opennessFloor)`, with the floor
at 0.3 standing in for the bounce light P1 could not yet carry. The bug was not
the 0.3, it was WHAT it was 0.3 OF: `ambientAt` is the day/night signal and
nothing else, so a sealed cave was lit at a fixed fraction of the SUN and got
brighter at noon through solid rock. Measured in the `--shot` stone room, mean
linear luminance noon vs midnight: back wall **8.6x**, deep floor **7.7x**,
against 14.6x for the open meadow outside — and removing the floor alone took
the noon wall from 0.0568 to 0.00136, so **97.6% of it was that one term**.

`ambientOpen()` (`common.wgsl`) is the fix and it is two lights instead of one:

    ambientAt(n) * openScale  +  render.enclosedAmbient * (1 - openness)

The sky term keeps the visibility multiplier with nothing under it; the second
is a small constant that does not move with the sun, the moons or the sky, and
it is what decides how bright a cave is. At full sky its weight is exactly 0, so
open ground is bit-identical; `opennessStrength` scales both, so the
`noopenness` arm still folds the whole feature away. This is the shape the
literature uses for sky injection (`L = L_local + V_sky · L_sky`), and the
failure it fixes is the one CRYENGINE's SVOGI documents for its own constant
"diffuse bias" leaking into interiors.

**A full-sun reading is no longer exempt from `shadowLiftCap`.** The cap passed
`v = 1` through untouched, on the argument that a ray which hit nothing saw the
whole solar disc. In a cell whose hemisphere is entirely blocked that is a
contradiction, and several paths return 1.0 when they have not MEASURED
anything — the shadow cache returns exactly 1.0 for a patch nobody has
requested yet. So every surface a camera saw for the FIRST time inside a cave
shaded with full daylight for a frame, and the P2 write-back recorded it into
the irradiance grid where it stuck. That was the whole of a sealed box's
remaining 2.7x swing, with every openness byte and every injected word reading
0. The exemption is now gated on the face seeing some sky (a smoothstep over
the bottom 5% of openness), which covers every consumer at once — the raymarch,
the resolve pass's deposit, the walk's sun sample and the raster body path.

**Knobs** (`render.*`): `opennessReach` (12 m), `opennessChunksPerFrame` (256
slots/tick), `opennessStrength` (1.0 — and 0 is an EXACT off switch on both
halves: it const-folds the reader and makes `C_OPENNESS` false so neither row is
recorded), `enclosedAmbient` (0.012, 0.013, 0.016 — roughly what a moonless
night gives open sky), `opennessFloor` (**0**; the pre-2026-09-11 leak, kept
only as the A/B arm for the old look), `opennessBilinear` (1).

**Gate:** `--selftest --gate cave-time` stamps a sealed 24-voxel stone box,
puts the camera inside it and renders the same view at the sun's highest and
lowest tick. Four arms, because "the cave does not change" is satisfiable by
rendering black: the noon frame must clear a floor, the noon/midnight ratio must
be ≤ 1.25x (it measures 1.1–1.2x), the box must stay under 15% of the open
meadow outside, and the probed faces must hold no irradiance an earlier gate
left behind — that last one reports as a FIXTURE failure, not a lighting one.

**Cost**, RTX 3060 Ti, 1080p, 2026-09-02. The compute pass, from `--perf`'s
`openness` node: p50 0.015 ms idle, 0.008 ms flying (streaming does not light it
up), 0.19–0.30 ms under explosions / water / a burning canopy. The per-hit read
in the raymarch, from `--render-budget`'s `noopenness` arm at noon: 0.89 ms of a
12.25 ms baseline (7.3%) — the reads dominate the pass by an order of magnitude,
and `render.opennessBilinear = 0` is the lever if that ever needs to come down.

**Verified by** `--selftest --gate openness`: the +Y face of the ground under a
45×45 stone slab 1.4 m up reads 0.00 (bound 0.30) while the slab's own top face
reads 1.00 (bound 0.75), and both slots' stamps match `sandvox::OpennessStamp`
computed on the CPU — which is what keeps that hash and `common.wgsl`'s
`opennessStamp` from drifting. Thresholds live in `tests/baseline.json`.

**What P0 does not do.** There is no bounce: a sealed room goes to zero ambient
and stays there, which is honest for a sky-visibility term and is what P1's
one-bounce gather (below) is for. Debris cubes and sprites (`debris.wgsl`) read
the grid since 2026-09-02 too: one `opennessScaleAtBody` walk from the cube's
position, with `occupancy`/`openness`/`opennessGen` widened to the vertex stage
in `renderBGL_` — a burning ember in a cave no longer glows at full sky ambient.
Particles, sprites and fluid still take that walk per VERTEX; the BODY path
takes it once per cube (at the centre) and shades per FRAGMENT — see §9.z.

### 9.s The penumbra window — the sun is not a point (added 2026-09-11; `shadow_resolve.wgsl`, `src/sim/world.h` `kShadowHistBytes`)

**The defect.** One ray per patch answers a yes/no question, so the shadow cache
could only publish "lit" or "shadowed at this blocker's lift". A shadow edge was
therefore a hard step one patch wide however far away the blocker was, and
walking the sun across the sky advanced that step one patch at a time — a shadow
that visibly expands in jumps (owner report 2026-09-11). Subdivision does not
fix it: a finer grid makes the step smaller, not softer.

**The fix costs no extra rays.** The resolve pass jitters its ray inside a cone
of half-angle `render.shadowSunAngle` and averages the last `kShadowSamples`
= 16 verdicts, so a patch publishes the FRACTION of the solar disc it can see.
The penumbra width then falls out of the geometry — a blocker `d` away spreads
the cone over `2·d·tan(angle)`, so a kerb stays crisp and a canopy 10 m up
softens over most of a metre.

**`render.shadowSunAngle` is the ARTISTIC size of the sun, not the real one.**
`dayNight.sunAngularRadius` (0.3°) is the star, and it drives the drawn disc and
eclipse geometry; at 0.3° a canopy 10 m up softens over one voxel, which is
physically right and reads as the hard edge this replaced. It ships at **1.0°**.
0 restores the single-ray hard shadow exactly — the same value, the same code
path, verified by the `shadow-cache` gate's agreement with the per-pixel
reference tightening from 0.67 to 0.50 mean |dL| when the cone is switched off.

**A sliding WINDOW, not an exponential blend, and that is the whole design.** An
EMA over a cycling sample set never settles — it oscillates with the sequence's
period forever, i.e. a shadow that pulses, which is the defect next door. A
window over a FIXED 16-point sunflower sequence is exact after 16 frames and
then stops moving entirely while the scene and the sun hold still, because the
same slot is rewritten with the same bit. `--gate shadow-cache` measures
**0 pixels moved** between two warmed frames, and its warm-up is 20 frames
rather than 4 for exactly this reason: anything under 17 measures the fill.

**Storage is one word per bucket in its own buffer** (`world.h kShadowHistBytes`,
4 MiB): 16 sample bits, a 5-bit fill count, and the 8-bit lift. Its own buffer
and not two more words on the cache slot, because the fragment shader probes a
whole 8-way set on every lit pixel and that set is exactly one 64-byte cache
line; widening the slot would put the hot path on two lines to carry state only
the resolve pass touches. The reader is unchanged — it still reads one byte.

**The lift is refreshed from ONE deterministic ray**, the undeflected one, on
the frame a patch's window wraps (plus immediately on a fresh slot, so a newly
visible patch never spends a frame at the reset value of contact black). There
are no spare bits for a second 16-sample window, and a fixed ray is what keeps
the byte — and therefore the published value — bit-stable in a static scene.
The two compose as `open + (1 − open)·lift`, which degenerates to the pre-cone
value exactly when every sample agrees.

**A window belongs to a PATCH, not to a slot.** The slot's `valid` bit is the
only signal that distinguishes "my samples" from "the samples of whatever patch
held this slot before me"; nothing but the resolve pass sets it, so `!valid` is
exactly "claimed since the last publish" and the window resets. The write-back
is unguarded (unlike the value publish) because a slot that changed hands
self-corrects on the next frame's verifier mismatch.

**The window has seventeen levels; the sun has none** (added 2026-09-12,
`SHADOW_GLIDE` in `shadow_resolve.wgsl`). The cone made the shadow a gradient in
SPACE and left it a staircase in TIME: 16 binary verdicts estimate coverage at
1/16, so a patch holds a value and then steps ~9 of 255 when a blocker's edge
crosses one of the sixteen sample directions. `dayNight.cycleMinutes` is 6, so
the sun crosses a degree of sky per SECOND and sweeps the whole 2° cone in two —
sixteen levels in two seconds is a visible step every seven or eight frames, for
as long as the shadow moves (owner report 2026-09-12, "the pixels still
discretely jump in a bunch of small steps"). More samples cannot fix it and
there is no room for them: 29 of the word's 32 bits are already spoken for, and
64 levels would still step.

So the published byte GLIDES: each frame it moves `SHADOW_GLIDE` of the way to
the window's answer, which turns each 1/16 jump into a short ramp, and the ramps
of a moving shadow run into each other. **This is not the EMA the paragraph
above refuses** — that one would average the raw SAMPLES, a cycling sequence
whose EMA oscillates forever; this averages the WINDOW'S OUTPUT, which in a
static scene is one number, bit-identical every frame, and a low-pass fed a
constant converges and stops. It is computed in integer units of the stored byte
with a minimum step of one unit, so the fixed point is exact and a settled patch
publishes a delta of exactly zero (a float blend landing 0.4 units short rounds
back and forth forever — a 1/255 flicker, and "0 pixels moved" is a claim about
zero). Past `SHADOW_GLIDE_SNAP` = 64/255 the new answer is taken whole, so a
mined block or an opened door is instant and only the sun creeps. Below
`|d·rate| = 1` the step floors at one unit per frame, so a steadily moving shadow
settles into a constant-VELOCITY slide with a standing error of ~1/rate units —
a couple of centimetres of shadow position, and the smoothest thing this can be.

`--gate shadow-cache` gained a **creep arm** for it: a pinned camera, one TICK
of sun per frame (the real rate — the walk arm's 1.4°/frame changes the answer
so much that the glide correctly snaps and measures nothing), 32 frames, and the
cache buffer read back each frame so every slot that is still the same patch
contributes its per-frame delta. Measured: **0.8%** of slot-frame moves step by
more than 5/255, against a bound of 25%. With `SHADOW_GLIDE = 0` — the oracle,
one WGSL const and no rebuild — it is **99.2%**, largest step exactly 16/255,
and the number of slot-frames that move at all falls from 43,928 to 7,146.
Moving a little every frame rather than a lot occasionally IS the fix.

**What is NOT here.** Soft shadows on the raster body paths. `bodySunShadow`
casts one ray per FRAGMENT with nowhere to accumulate, so a limb still takes a
hard edge; the terrain under it does not. Per-fragment cone jitter would dither
it at the cost of noise on a moving body, which is the artifact this pass exists
to avoid — the honest fix is a body-side cache, and it is not written.

### 9.z Raster body shading parity (added 2026-09-04)

**The defect.** Rigidbodies are not raymarched; they are rasterized cubes
(`debris.wgsl` `vsBody`/`fsBody`) and micro-body bricks (`microbody.wgsl`),
shaded by `litColorS` in `common.wgsl`. That function had **no shadow term at
all**, so a body received 100% of the key light wherever it stood. Modelled at
shipped tuning against the terrain combine, a body was ~0.94x in open sun (fine),
**3.2x** too bright in an ordinary cast shadow, **5.3x** with contact AO and
**10.4x** inside a room — which is exactly the reported symptom, "fine in full
sun, washed out everywhere else". Four smaller terms were missing with it: the
per-face tint, wrapped diffuse, and a fog that used a hardcoded `0.0128` and
ignored `R.fogDensity` (so `main.cpp`'s portrait renders, which pass 0.0, were
fogged anyway).

**The structural blocker, and the fix.** `voxels` (binding 0) and `pageTable`
(binding 9) are **Fragment-only** in `renderBGL_`, and `vsBody` shaded in the
VERTEX stage — so the body path could not reach the world at all. Widening those
to the vertex stage would hand the world grid to every particle and sprite vertex
for one consumer, so instead the body path alone ships its surface across the
interstage boundary (`BodyVSOut`: flat albedo/normal/emissive/openness,
interpolated world position) and lights it in a new `fsBody` entry point. That
is the ONLY pipeline in `debris.wgsl` with its own fragment entry.

**One law, not two.** The softening curve was lifted out of `raymarch.wgsl`'s
`sunShadowAt` into `shadowFromOpaqueHit` in `common.wgsl`, and `wrapDiffuse`
moved to `common.wgsl` with it. Both paths now call the same functions: a body
and the ground under it cannot disagree about how a shadow edge softens. The
lift is capped by raw openness through the same `shadowLiftCap`, which is why
`opennessAtBody` returns the scale AND the raw byte from one walk.

**What a body still does not get, and why.** No `voxelAO` — it samples the eight
cell-scale neighbours around an axis-aligned hit face, which a rotated cube does
not have; a wrong occlusion on every tumbling block is worse than none. No GI
bounce. And **bodies still cast nothing**: the ray marches world voxels only, so
there is no contact shadow under a rubble pile and no body-on-body shadowing.
Particles, sprites and fluid deliberately stay per-vertex and shadowless — foam
is 1/6 of a cell and there can be ~260k of them.

**The gate.** `--gate body-shade` (`selftest_render.cpp`) stands a 5³ stone body
on a 45×45 deck under a 45×45 roof at noon and renders three arms of one world
state: body draw count 0 (which yields the body pixel mask by difference), then
shadows off, then shadows on. It compares how much each surface dims, as a
fraction of its own lit level. Measured: the body dims **0.532**, the deck under
it **0.562**, ratio **0.95**. With the shadow term forced off the body dims
**0.000** (bit-identical frames) and the ratio is **0.00**. Thresholds are in
`tests/baseline.json`. `determinismHash` is unmoved — all of this is render-only
float math on render-only data.

### 9.y The irradiance grid — one bounce of direct light (added 2026-09-02)

Plan of record: **`docs/PLAN_gi.md`** §3 (phase P1). Binding summary:

**The data.** `irradiance`: one RGB9E5 word per (chunk slot, 4³ block, face) at
the SAME index as the openness byte (`irrIndex` in `common.wgsl`), under the
SAME per-slot stamp (`opennessGen`), `kIrradianceBytes` = 48 MiB. The value is
the direct-lit RADIANCE leaving that block-face: albedo × sun colour × Lambert
× lit, averaged over what has been seen of it. Render-only derived data with
exactly the openness grid's standing — never hashed, never saved, the sim has
no binding for it — and `determinismHash` unmoved is P1's cheapest proof.

**Two writers, one blend.** The shadow resolve pass (`shadow_resolve.wgsl`)
deposits for every patch it publishes — it already knows the patch's cell, face
and shadow term, and one voxel-word read gives the albedo — so anything on
screen is injected every frame. The openness walk (`sim_openness.wgsl`)
deposits ONE coarse sun sample per face it marches (a `traceOpaque` block ray
from the face centre; the albedo is the first blocker voxel found in FIVE
columns of the face — its centre and the same 2x2 quincunx `openValueAt` uses
to find its ray origin), so faces nobody is looking at follow the sun within
`kNumChunks / opennessChunksPerFrame` ticks. A face the walk cannot measure —
it could not march (a blocker in front), or none of the five columns holds a
blocker — fades by `render.giDecay` per visit, and a block with no surface
reads 0. **A face the walk cannot measure must never be left alone.** The
resolve pass is a charger with no expiry: it deposits full sun for anything on
screen and stops the moment the camera looks away, so the walk's visit is the
only thing that can ever discharge a word. Sampling the centre column alone and
skipping the deposit when it held no blocker froze exactly the faces of blocks
whose surface passes off-centre — every slope, bank, trunk and cliff, since a
block is 4 voxels wide — at whatever value the last daylight frame put there:
they went on lighting their neighbours grass-green all night, in scattered
patches that tracked the steep ground (2026-09-02). Gate `gi-nightfall`.
Both blend into the word with an EMA (`irrDeposit`: 1/16 per resolve deposit,
1/2 per walk sample) rather than summing — 256 patches per block-face per
frame overflow any bounded sum-and-count, and a per-frame reset needs a frame
stamp the word has no room for. The blend converges within a frame where
deposits are dense, over a few frames where they are sparse, is bounded by
construction, and its races (plain read-modify-write between patches of one
face) cost at most a lost sample. A slot the window has reused starts its
blend from zero (the stamp read in the resolve pass is for that).

**The reader** is `giGather` in `raymarch.wgsl`, at every near-field hit:
nine `traceOpaque` block rays from a point pushed clear of the receiver's own
4³ block along the normal (the normal, four 45° tangent diagonals, four
corners), each stopping at the first blocker block, whose faces that face the
receiver are read (one per axis the direction leans on, weighted by that
axis's share) and summed with cosine-weighted solid-angle weights (0.25 for
the normal's 30° cone, 0.09375 for each ring ray). The result is a mean
radiance of what the surface sees; `albedo × ao × gathered × render.giStrength`
is added to the shade. Two lessons the gate taught, both now in the code: the
origin MUST leave the receiver's block (a wall's block column reaches three
voxels in front of its face, and a ray starting on the face reports the wall
as its own emitter), and the emitter face MUST be chosen from the direction,
not the block-entry axis (a 45° ray enters a floor block through its side as
often as its top). Five rays weighted by cosine and distance gave a wall beside
a sunlit floor 6% of the floor's radiance; the nine-ray quadrature gives 0.28
against a true form factor of 0.5, and `giStrength` carries the rest.

**Knobs** (`render.*`): `giStrength` (2.0 — at 1.0 the ruin walls beside the meadow did
not change to the eye, and the quadrature's 0.28 against a form factor of 0.5 is the
reason; 0 is an exact off switch — gather,
resolve deposit and walk sample all const-fold, the `nogi` `--render-budget`
arm), `giDecay` (0.25), `giFeedback` (0, P2; `LoadTuning` keeps it strictly
below `giDecay`), `giGatherBlocks` (**12**), `giCachePeriod` (**16**; 0 is the
uncached per-pixel gather, the `nogicache` `--render-budget` arm).

**The gather reached 1.2 m, which is less than a room** (2026-09-11).
`giGatherBlocks` is a STEP budget rather than a distance — `traceOpaque` jumps a
whole chunk wherever that chunk holds no blocker, so open air costs a quarter of
the steps a wall does — and at the shipped 3 the bounce could not cross a 10 m
room: a wall ten metres from a sunlit doorway measured 0.00136 with GI on and
0.00133 with it off. That shortfall is *why* the openness floor above existed.
At 12 it crosses a room, and it is free: the cost is (rays × steps) /
`giCachePeriod`, so doubling the period 8 → 16 absorbed the entire increase —
0.78 ms on the noon overlook, against 0.79 ms before. What buys the reach is
LATENCY: the bounce now trails the sun by up to 16 frames rather than 8.

**Two obvious improvements were built and both removed the same day, for the
same reason: this fragment shader has no register headroom.** (1) A three-level
chunk-local mip pyramid over plane 0, so a distant hit reads a prefiltered cell
instead of point-sampling one 40 cm block-face. (2) Scaling the step budget by
the receiver's own sky visibility, so reach is not spent outdoors where
`ambientAt` already answers analytically. Each measured **128 → 168 registers**
on the `raymarch` fragment stage (`--shader-stats`), past its occupancy cliff,
and slowed `--render-budget` cameras containing no indirect light at all; a
branchless level select measured 168 too, and a CONSTANT budget of the same size
measured 128, so it is the live state and the dynamism rather than the branches
or the count. **The step budget must stay a value the shader can const-fold.**
The thing to change, if this is revisited, is WHERE the gather runs — a compute
pass over block-faces, which the per-block-face cache below already makes
natural — not what it reads or how far it goes.

**The gather is cached per block-face** (2026-09-05, `docs/PLAN_frame_perf.md`
§3 item 1). The nine rays are a function of the block-face and the geometry
around it, not of the pixel, and they read a grid that is itself an EMA over
frames — so `irradiance` has a second plane (`kIrradiancePlanes`,
`GI_CACHE_BASE`) at the same index holding the INCOMING irradiance gathered at
the block-face's centre (the same origin `sim_openness.wgsl`'s own rays start
from), RGB9E5 with its low bit forced on so that 0 means "never gathered".
`giBounceAt` re-runs `giGatherRays` only on a chunk slot's scheduled frame —
slots are staggered over `giCachePeriod` frames, per SLOT rather than per block
so the branch stays uniform across a warp — or for a word that reads 0, and
reads the four block-faces in the face plane bilinearly, as `opennessAt` reads
its bytes. The openness walk zeroes the cache word on every full walk and for
a reused slot, so moved geometry re-gathers on the next frame that looks at
it; a slot the walk has not stamped gathers live, as before.

**Verified by** `--selftest --gate gi-bounce`: a white `bone` wall on the -X
edge of a floating 41×41 `leaves` slab at noon; the wall's +X face rendered
with `giStrength` at its default and at 0 differs by R +2.26, G +5.80,
B +1.01 per 255 over the middle of the frame (G ≥ 2.0 and G − R ≥ 1.0 from
`tests/baseline.json`), and the slab's own +Y word reads green-led. The
`shadow-cache` gate pins `giStrength = 0` for its arms: its reference arm has
no resolve pass and so no injection, and the bounce would otherwise be the
whole disagreement.

**P2 write-back (2026-09-02).** After the gather, `fs` blends the pixel's outgoing
radiance (`albedo × sun + bounce`) into its own block-face at `render.giFeedback`
(0.2), so the next frame's gather at a neighbour sees light that has already
bounced — multi-bounce as a fixed-point iteration over frames. `irradiance` is
the third buffer a fragment shader writes. Bounded because each bounce is
albedo × 0.28 × `giStrength` of the last; `LoadTuning` keeps `giFeedback <
giDecay` and `giStrength ≤ 3` while feedback is on. `--gate gi-bounce`'s
convergence arm reads the wall's word after 500 and 600 frames (0.0301 both).

**P3 emissives (2026-09-02).** Emission is part of the one sample both injection
paths deposit (`irrSample`), so lava and embers light their surroundings through
the same gather that lights a wall from a meadow, on every dirty tick. `heatSpill`
and `render.heatSpillStrength` are gone.

- Later: volumetrics for gases; a bilinear over the emitter faces if the
  block-scale edge of a bounce patch ever matters.

### 9.g The glow field — emitter light for the paths a ray cannot reach (added 2026-09-08; `assets/shaders/sim_glow.wgsl`, `src/sim/world.h` `kGlowBytes`)

**The problem is NOT that emission is derived ad hoc.** That was true and §9.y's
P3 fixed it: `heatSpill` and the per-ray ember probe are deleted, every site
funnels through `burnTint()`, and `check_burn_tint_sites` in
`scripts/check_invariants.py` refuses the raw `emission / 255.0` idiom outside
one. Terrain receives emitter light through the irradiance grid.

**The problem is that three consumers are outside that path, for structural
reasons rather than for want of wiring.** `giGather` is nine coarse DDA rays
over `occupancy`; it needs `voxels`, `occupancy` and `pageTable` bound in the
FRAGMENT stage, and its reach is `render.giGatherBlocks` march STEPS — about a
room at the shipped 12, and 1.2 m before 2026-09-11.

* `debris.wgsl`'s rigid-body and particle cubes shade in the **vertex** stage,
  where `renderBGL_` marks `voxels` (0) and `pageTable` (9) Fragment-only. They
  can neither march a ray nor read a voxel (§9.z).
* `microbody.wgsl`'s mob limbs shade per fragment but already pay for a sun ray
  and an openness probe per limb voxel.
* Neither irradiance injector can **see** a body at all: `shadow_resolve.wgsl`
  and `sim_openness.wgsl` both read the voxel grid, and a rigid body is not in
  it.

So a mob standing in a lava pit, a severed limb beside a fire and the burning
crown that falls off a tree were lit by the **sky alone**.

**What it is.** A coarse world-space field answering "how much emitter light is
at this POINT" from nothing but a world position, in **one buffer load and no
ray** — the only shape of answer those paths can consume. Two regions of one
buffer (one binding per layout, not two):

| region | granularity | contents |
|---|---|---|
| source | per chunk slot (1.6 m), 4 words | RGB9E5 emitted radiance, `OpennessStamp`, emitter cell count, changed flag |
| field | per 4³ block (40 cm), 1 word | RGB9E5 incident glow |

8.5 MiB, against 12 MiB of openness and 48 MiB of irradiance. RGB9E5 rather
than a scalar because `packRgb9e5`/`unpackRgb9e5` already exist (no second
packing convention), the HDR exponent lets a lava lake and one ember differ by
three orders of magnitude with no global scale constant, and it carries the
COLOUR that makes lava orange and `gem_arcane` violet.

**Why the two granularities differ, which is the design.** The field at a point
is a sum over emitters within `render.glowReach`, and gather cost goes as the
CUBE of the reach in source cells: per-voxel sources at 2.4 m would be a 48³ =
110,592-tap gather per output. Quantising the SOURCE to a chunk makes the same
reach a 27-tap gather over a 512 KiB region. Evaluating the FALLOFF at 40 cm is
what keeps the result looking like light rather than a stack of 1.6 m cubes.
Source quantisation costs precision about *where* the emitter is, invisible at
1.6 m against a 2.4 m falloff; a coarse field would cost the *shape* of the
falloff, which is not. Blocks reuse the sub-occupancy index, so the reader adds
no arithmetic and the feature needs no new prelude constant.

**It cannot latch.** The irradiance grid is an EMA with two writers and only one
that can discharge, and a face its walk declines to measure keeps yesterday's
sun forever (§9.y, and the night-glow bug that found it). This field has **no
memory**: every visit recomputes the source word from the voxels standing in the
chunk now and overwrites it. Remove the emitter, the removal dirties the chunk,
the source goes to 0, the change flag fires, and the whole 3×3×3 ring of field
words is rewritten to 0 on the same tick. There is no decay constant because
there is nothing to decay, and the gate asserts EXACTLY zero rather than a
tolerance for that reason.

**Three ceilings, none of them a tuned threshold** (rule 2):

1. The source is a MEAN over the emitting cells times a SATURATING fill factor,
   so no amount of lava can make a chunk brighter than one lava voxel's own
   surface. A sum would have had no ceiling at all.
2. The 27-chunk ring rewrite — the only expensive branch — fires only when the
   quantised source word actually MOVES. A rippling lava pool takes it zero
   times; a settled world does no work.
3. Even when a grove catches fire at once, only the first
   `render.glowRingBudget` workgroups of the compacted dirty list take it. A
   prefix of a slot-ordered list, so which chunks win is stable rather than
   scheduling-dependent, and no atomic and no allocation are involved.

It writes no voxel, no dirty flag and no page-fault word, so it cannot wake a
chunk — the same claim §9.x makes for the openness walk, and `--gate sleep`
would notice.

**Two dispatches, and the barrier between them is the point.** `src` writes every
dirty chunk's source word; `field` gathers the 3×3×3 neighbourhood of those
words. The second reads what the first wrote for OTHER chunks on the same list,
so merging them into one entry point would be a race. The `RW(Glow)` →
`RW(Glow)` edge `vk_record` derives from the two `pass_table.def` rows is what
makes it a read-after-write. A third entry point, `refresh`, walks
`render.glowChunksPerFrame` slots per tick doing both stages, as the backstop
for a cold start, a streamed-in slot and a chunk the ring budget turned away.

**Both sides of the toroidal seam take the stamp.** `glowSrcOf` refuses to READ a
neighbour whose slot holds a different world chunk; `glowWriteField` refuses to
WRITE one. The write side is the half that corrupts: the ring rewrite touches 26
neighbours, and at the window edge `chunkSlotIndex` folds some of them onto
slots holding chunks 51.2 m away, where the stamp is valid and simply names
something else.

**Not occluded, and said out loud.** A glow word says what emitters within reach
are throwing at a point, not whether a wall is in the way. At 1.6 m source
granularity and a 2.4 m reach the leak is bounded to about one chunk through a
thin wall, and the term is additive and soft, so the failure mode is a faint
warm haze behind a lava pit's rim rather than a light in the wrong room. If it
ever needs occlusion the honest fix is to attenuate along the line by the
sub-occupancy mask, not to shorten the reach until nobody notices.

**`render.glowTerrain` defaults OFF, and that is a correctness call.** Terrain
already receives emitter light through `giGather`, so sampling the field at the
near-field hit as well DOUBLE-COUNTS every emitter inside the gather's 1.2 m. On,
it extends emitter light to `glowReach` on terrain at the cost of that overlap.
The raster paths, which have no other source at all, are on unconditionally.

**Verified by** `--selftest --gate glow`, four passes over one fixture: a 4³
block of `crystal` (a solid mineral, emission 120, no reactions) buried 12 cells
under the surface, so nothing can move it and the floating-voxel pass cannot
lift it into a rigid body. A: the source counted all 64 cells and is bright.
B: the field one block away, across a chunk boundary, is nonzero. C: the field
two CHUNKS away — outside the gather stencil by construction — is zero, which is
what stops a producer that returned bright everywhere from passing A, B and D.
D: the emitter is replaced with stone and both go back to exactly zero.
`determinismHash` unmoved: no sim pass has a binding for this buffer.

- Later: a `--shot` fixture that puts a rigid body or a mob beside an emitter.
  None exists today, so the shipping default's visual effect has no camera and
  the gate's field values are the claim. Also: reconsider whether the irradiance
  grid should stop carrying emission and let this field own it end to end,
  which would make `glowTerrain` free of double-counting — that is a change to a
  cache with its own convergence and wants its own package.

### 9.t TAA and temporal upscale (added 2026-09-07; `assets/shaders/taa.wgsl`)

`render.renderScale` renders the world at a fraction of the window and blits it
up with NEAREST. That is the biggest single lever in the renderer — the
`halfres` budget arm saves ~70% of the raymarch — but what it produces is a
SMALLER PICTURE, not a cheaper one: at 0.7 you are looking at 0.7-resolution
edges and nothing ever fills them back in. `render.taa` replaces that blit with
a resolve pass that does.

**The chain, per frame, when `render.taa` is on:**

```
  (CPU) ApplyTaaJitter  -> perturb the Camera by this frame's R2 sub-pixel
                           offset; MEASURE the resulting image shift in pixels
  WriteRenderParams(jittered camera)
  Simulation::WriteTaaParams   (before any render pass opens)
  ... the ordinary world render pass, into the offscreen target at render res
  Simulation::EncodeTaaCapture -> CopyTextureToBuffer colour AND depth
  Simulation::BeginTaaRenderPass + DrawTaa -> resolve into the swapchain
  Simulation::BeginOverlayRenderPass -> the UI, at native res, on top
  Simulation::FlipTaaPage
```

**Four things about it are decisions, not details.**

1. **Storage buffers, not textures.** The rhi has no sampled-texture or sampler
   binding at all — `BindGroupEntry` carries a `Buffer` and nothing else, and
   the only image anything samples in this engine is ImGui's avatar portrait,
   through ImGui's own descriptor path. The finished frame therefore reaches the
   resolve the way everything else reaches a shader here: an image-to-buffer
   copy into a plain storage buffer, indexed by hand. The alternative was four
   new concepts (sampled images, samplers, RGBA16F, their layout transitions) in
   the one file the whole engine sees the GPU through. The cost is one copy of
   the render-resolution frame, colour and depth, ~8 MB at 0.7 of 1080p.

2. **The jitter is a yaw/pitch nudge on the `Camera`, not a shear of the basis.**
   Every path that draws — the raymarch's ray construction AND `projectView` for
   bodies, particles, sprites and the debug arrows — derives from the same
   `RenderParams`, so perturbing the camera that is written into it moves all of
   them by exactly the same amount. There is no second place to keep in step and
   no way for the raster and the ray to disagree. `cam` itself is never
   modified: picking, the brush ray, the player's movement basis and every
   mirror query still see the true camera, so nothing that reaches the sim can
   see the jitter.

3. **The shift is MEASURED, not derived.** The resolve must know where the image
   moved to within a fraction of a pixel or its reconstruction filter is centred
   on the wrong texel. `ApplyTaaJitter` projects the UNJITTERED forward direction
   through the JITTERED basis and reads off where it lands — sign, `cos(pitch)`
   term and perspective included. The small-angle algebra that produced the
   nudge only has to get the amplitude roughly right.

4. **Accumulation is weight-proportional, which is what makes it an UPSCALE.**
   The history stores accumulated WEIGHT, not a frame count, and each frame
   contributes `w / (W + w)` where `w` is the reconstruction filter's response
   at this native pixel — how close that frame's jittered sample landed.
   Blending at a constant rate instead converges to the average of the filter
   over the jitter sequence, which is a WIDER filter than any single frame's: it
   would spend the jitter on blur. The history is ping-ponged because the
   resolve reads it bilinearly at a REPROJECTED position (other pixels' texels)
   while writing its own.

**Rejection is the colour box and nothing else.** There is no history-depth
buffer and no per-pixel surface id: a disocclusion reprojects onto a pixel whose
colour is outside the 3x3 range of what is actually there now, the clamp pulls
it in, and where it had to move a long way the accumulated weight is cut so the
pixel reconverges in a few frames. `render.taaClamp` is the one knob that trades
ghosting against flicker.

**Render-only, in the strict sense.** The history, the colour copy and the depth
copy are derived data in the §9 sense — never read by the sim, never hashed,
never saved, and the jitter never reaches the camera the game logic uses. The
world hash is identical with `render.taa` at 0 and at 1.

**Gated by `--gate taa`**, which ASSERTS one thing and REPORTS the rest. The
assertion is alignment: the resolve at 1:1 with no jitter must be closer to a
plain render than a ONE-PIXEL SHIFT of that render is (0.32 against 0.89 as
shipped). It is calibrated against the frame itself rather than against a pinned
number, so it means the same thing on any scene or resolution, and every
plausible bug in the pass — a dropped half-pixel, an inverted Y, a jitter sign
measured the wrong way round — fails it by a mile.

**The reconstruction claim is NOT established, and the gate reports it instead
of asserting it.** Measured at 1080p from 960x540, over 48 accumulated frames:
whole-frame mean absolute error against a full-resolution render is 1.42 for the
resolve against 1.20 for a NEAREST blit, and on the top 20% of pixels by
gradient 3.73 against 3.69 — level. Worse, the error RISES with frame count
(1.19 at 16 frames, 1.42 at 48), and an accumulator cannot get worse with more
samples unless it is converging to something other than the reference. What it
converges to is the weighted mean of every sample inside the reconstruction
filter: a blur about half a native pixel wide. Against a point-sampled reference
a blur and a half-pixel shift score about the same, so MAE cannot separate them
— which is the deeper reason this was never going to be the deciding
measurement.

Ruled out on the way, and worth not re-testing: a stale reference. The gate now
renders its reference to 64 frames so the irradiance EMA sits at its fixed point
before anything is measured, and the numbers did not move at all.

`render.taaSharpness` is the dial that would settle it, and it is a UNIFORM lane
rather than a shader constant precisely so that sweeping it costs a
`tuning.json` edit and one `--gate taa` — no rebuild, no shader recompile. The
assertion goes back in when either that sweep finds a width where the 48-frame
error falls BELOW the 16-frame one (which is what converging looks like), or
somebody replaces the metric with one that can tell a blur from a blocky shift.
**`render.taa` therefore ships at 0.**

**What is NOT here.** A separate SVGF-style denoiser for the lighting terms,
which was the other half of the package this came from. That port assumes one
jittered sun-occlusion ray and one stratified AO ray per pixel per frame; this
engine traces neither. The sun term is the §9 shadow cache (one ray per visible
surface PATCH per frame, resolved by a compute pass), sky visibility is the
world-space openness grid (§9.x), ambient occlusion is `voxelAO` — three voxel
fetches, no ray — and one-bounce indirect is `giGather` over the world-space
irradiance grid (§9.y). A screen-space denoiser needs screen-space noise, and
this renderer's noise is in the QUANTISATION of those world-space caches, which
is what the temporal accumulator above softens as a side effect.

### 9.u The shading-LOD filter — the "denoiser" that is not one (added 2026-09-12; `assets/shaders/denoise.wgsl`, `raymarch.wgsl` `lodShadeFade`)

Reported as "a ton of noise overlaying everything in the first far-field /
medium-field ring, especially in daylight". Measured on `screenshot_ground`
and `screenshot`: the noise is **not stochastic**. Every lighting term the
raymarch evaluates is deterministic and evaluated once per pixel at one point
on one voxel face — the sun through the shadow cache, AO from three fetches,
the far shadow from one any-hit march. What the eye reads as noise is
GEOMETRY: past ~20 m a hillside is a staircase of 10 cm voxels (40–160 cm
cascade cells) whose treads are contact-shadowed by the riser above them,
whose creases carry full AO and whose ±X/±Z faces take different lamberts,
each face 2–6 px on screen. Point-sampling that staircase once per pixel is
aliasing of the lighting, and it is the same picture every frame. The
`denoise` gate puts a number on it: mean absolute luminance residual against
the 4-neighbour mean over the 30–400 m band, **16.5** (0..255) at mid-morning
against 3.0 at midnight — it is a sunlit artefact.

**Why none of the five candidate denoisers was used.** OptiX (every mode,
AOV-guided and temporal included) is CUDA and NVIDIA-only and would need
CUDA↔Vulkan interop; DLSS Ray Reconstruction is RTX-only behind the NGX SDK;
NRD/REBLUR is cross-vendor but wants a G-buffer this renderer does not have
(normals, roughness, motion vectors, split diffuse/specular with hit
distance), sampled images and samplers the rhi does not have, and HLSL
through a toolchain the build does not have. None of that is the real
objection. The real one is the MODEL: all five treat the input as a noisy
estimate of a smooth signal and accumulate it over time, and a deterministic
staircase has no variance to average away — the temporal stage would see the
same value every frame and change nothing, and their spatial stages are
guided by NORMALS, which would preserve exactly the riser-vs-tread contrast
that is the defect. A denoiser needs noise; this is LOD.

**What was built instead is the prefilter a texture mip chain gives a
rasteriser for free, in two halves that share one law.** The law is
projected size: `fpx / viewZ` (pixels one fine voxel covers at this pixel's
depth, `fpx = renderH / 2·tanHalfFov`), ramped from 8 px per voxel (off) to
2 px (full) — at 1080p / 70° that is 10 m to 39 m — and written in PIXELS so
the band follows resolution and fov. Because the far cascade keeps a constant
~6 px per CELL (§9's resolution law: 1.5 px per fine voxel at level 1, less
beyond), everything past the window edge is at full strength. And because
the law is a function of hit distance alone, it is continuous across the
window edge — a far cell one voxel past the seam gets the fade the near voxel
one voxel inside it got, which is what keeps this from being a new LOD ring.

1. **In the raymarch (`lodShadeFade`, applied in both the near and the far
   shade): the CONTACT terms fade.** A contact shadow lifts toward
   `LOD_SHADE_SHADOW_LIFT` (0.7) via `max()`, so a real cast shadow — a ridge
   over a valley, whose blocker is distant and which the near law already
   holds at `TUNE_SHADOW_LIFT` — is untouched and only the contact-dark
   artefacts move; the crease AO fades to 1. The direct lambert of the cube
   normal is deliberately NOT touched: its average over the footprint IS the
   smooth surface's lambert, which the second half reconstructs. Measured:
   raw band residual 16.5 → 13.5 from this half alone. Three WGSL consts in
   `raymarch.wgsl`, not `TUNE_` rows: only that kernel reads them, and
   `common.wgsl` is the nine-minute file. `--shader-stats`: the raymarch
   fragment stage stays at 128 registers.

2. **A screen-space pass (`denoise.wgsl`): a depth-guided à-trous kernel over
   the finished world frame, IN PLACE, at render resolution, between the
   world pass and TAA / the upscale blit.** 5×5 B3-spline taps dilated
   2^i per iteration (Dammertz 2010 — the SVGF spatial stage), with weights
   from three things: the projected-size STRENGTH above; a DEPTH stop that is
   relative and per pixel of offset (`|Δz| < denoiseDepthTol · z · dist`,
   because a grazing ground plane changes depth ~1 %/px at 100 m and a fixed
   tolerance stops on every tread or on nothing — this is what keeps a crest
   from bleeding into the hill behind it, a mob from bleeding into the ground,
   and the sky, depth 0, out of everything); and a CHROMA stop, deliberately
   not a luminance one — the speckle is luminance (lit vs shadowed faces of
   one material), a material boundary is mostly hue (sand/water, grass/rock).
   No history: nothing ghosts, nothing resets, a still frame is bit-stable.

**The plumbing is TAA's, reused.** No sampled images: the world colour and
depth attachments are copied into two storage buffers, the pass reads them
and renders into the world texture, and between iterations the texture is
copied back into the colour buffer. Every barrier in that chain is DERIVED —
`CopyImageToBuffer` routes its destination through the extras tracker (so
the second copy WARs against the previous pass's fragment read, which
`FlushForRenderDomain` recorded) and `TransitionImage` carries the
attachment-write → transfer-read → attachment-write on the texture. There is
no hand-written barrier and `--vk-validation` is clean. Like TAA it forces
the offscreen path at render scale 1. It runs in `--shot` too, in place over
the shot texture, because a look pass the look harness did not show would be
untunable.

**Shipped values, and what the alternatives looked like** (all from
`--gate denoise`, which writes `build/denoise_before.bmp` / `_after.bmp`
every run so a tuning.json edit plus one gate is the whole loop):

| arm | band residual | verdict |
|---|---|---|
| raw, no fade | 16.5 | the report |
| fade only | 13.5 | contact stipple mostly gone; the ±X/±Z face mosaic remains |
| fade + 1 iteration, strength 1 | 2.3 | in focus, but the 6 px mosaic is still a mosaic |
| **fade + 2 iterations, strength 0.7** | **0.7** | **shipped: mosaic averaged, terrace bands and relief survive** |
| fade + 2 iterations, strength 1 | 0.6 | softer than it needs to be |
| 3 iterations, strength 1 | 0.14 | every hillside reads out of focus |

**The owner's verdict on the pass, 2026-09-12: it ships OFF** (`render.denoise`
0). Even at two iterations the filtered hills read as out of focus. The
in-raymarch fade (half 1) is what ships; the pass stays as the A/B arm and the
gate keeps it working. The next lever for the remaining ±X/±Z face mosaic is a
smoothed normal for the lighting at distance, in the raymarch, not a blur.

The 15–31 m ramp of the first cut measured as a visible line where crisp
voxels met filtered ones — the LOD-ring defect the cascade seam dither exists
to break — hence the 10–39 m ramp. Cost, same gate, 1080p: **+1.2 ms** for two
iterations (copies included); at `render.renderScale` 0.7 it is half that.
`render.denoise` 0 is the A/B arm.

**Gated by `--gate denoise`**, which draws one mid-morning valley view, copies
the frame out BEFORE and AFTER the pass (one frame, so the irradiance EMA
cannot differ between the two — the first cut compared two arms and found a
3/255 lighting drift in the near band), reads the depth the world pass
wrote, and asserts by distance band: pixels nearer than 10 m and the sky are
**bit-identical** (`denoise.nearMaxDiff` 0 — the ramp is exactly zero there
and the shader returns the source texel), the 30–400 m band's residual falls
to at most `denoise.midHfMaxRatio` (0.6) of the raw frame's, and that band's
mean luminance moves by at most `denoise.midMeanMaxDelta` (3). Every threshold
is baseline.json; the observed ratio, raw residual and per-frame cost are
recorded for `--rebaseline`.

**What is NOT here.** An albedo guide: the pass reads colour and depth only,
so a distant structure of a similar hue to its ground (the ruins at 100 m in
`screenshot`) is softened along with the ground — the fix is a second
attachment or a fragment store of albedo from the raymarch, which the chroma
stop is standing in for. Soft shadows on the raster body paths (§9.s's open
item) are unaffected either way — a body's pixels are nearer than the ramp.
And nothing temporal: `render.taa`'s accumulator would integrate the same
staircase over the jitter sequence and is the right next lever if the
residual 0.7 still reads as texture at some resolution.

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
  oriented and coloured by magnitude. **F4 cycles off → wind → current → off**
  (`UIState::fieldViz`; the current arm is §9d.8), seeded from
  `wind.dbgWindField` / `render.dbgCurrentField` at startup and on every F5.
  Nothing is uploaded for it — the vertex shader derives each lattice
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
acceptance gate. **M1-M5 are all landed — the registry, the drain ledger and the
surface shave, the discharge law and the local excite, the current field and
waves (§9d), and the container sweep that lets a basin the player dug into
re-derive itself and split. None of them moves the pinned world hash**, because
`sim.waterBodyMode` ships at 0 and mode 0 records no pass row at all. What
follows describes what exists, and says plainly what does not (§5b.7).

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
  rejected. **Since P-F (environment truth, 2026-09-06) the bowl is the water
  PRESET's profile, not a parabola:** `bowlDepth` interpolates seventeen Q8
  knots in d² (sampled at sqrt(k/16) at load, forced non-increasing), and the
  registry's `Profiled` kind inverts THAT by bisection over d² against
  `World::BowlDepth`, the mirrored function itself — still integer, still no
  fourth copy. The disc, its preset and the preset's berm ride in
  `PondDisc`; `World::WaterSiteDisc` publishes the map's authored lakes
  beside the rolled tarns.
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

### 5b.5 M3: the discharge law and the local excite (added 2026-08-29)

M2 left the only drain source a development tap. M3 replaces it with a HOLE:
components 6 (the discharge law) and 7 (local excite at the throat). This is the
milestone where the feature becomes a feature.

**The law, and the one rule that makes it safe.** A hole under a body of water
is an orifice and the body behind it is a head:

```
h = level - hole.y            (integer, voxels)
v = sqrt(2 g h)               (integer sqrt; no f32 in the kernel)
Q = Cd * A * v * 8            (eighths/tick; A is the orifice's cell count)
```

> **ONE evaluation of `h` produces BOTH the emitted particle momentum and the
> ledger debit.** They are computed together in `wbLedger` and published as two
> ledger words (`WBS_EMIT`, `WBS_JETV`) that `wbDrain` reads. There is no second
> rule anywhere that could disagree with the first — emit by one and decrement by
> another and the pair is a mass pump under every edge case.

`sim.drainCd` and `sim.drainGravity` are human-unit floats in the sanctioned
`sim.fluid*` lane, const-eval'd to fixed point at the top of
`sim_waterbody.wgsl`; `sim.drainMaxEighthsPerTick` and `sim.drainExciteRadius`
are integers. Same five-place `TUNE_*` pipeline as everything else.

**The FLUID_VMAX trap, made structural.** `spawnAppend` clamps spawn velocity to
±`FLUID_VMAX` (0.45 cell/substep), and a Torricelli velocity under real head
exceeds it — at `pondDepth` 26 the exit speed is 7.1 m/s. The clamp is correct
and stays. So `h` is capped at `DRAIN_H_MAX = vmax² / 2g` *before* Q is computed,
which makes the momentum asked for equal the momentum granted by construction.
At the shipped 9 substeps that cap is `h ≤ 8` voxels and the jet leaves at
exactly `FLUID_VMAX` (measured: `WBS_JETV` = 262144 Q16.16 = 4.0 cells/tick).

**Where the hole comes from.** `wbHole` runs LAST in the water-body row block,
one thread per (x,z) column of a listed chunk, and it returns immediately unless
the chunk is dirty or holds an MPM block — the chunk-dirty path, because holes
appear when someone digs and digging is a mutation. The predicate is the
**water/void interface**: a cell holding the body's liquid whose cell below is
air. That set is empty in an intact basin by construction, it is exactly the
shaft mouth when someone bores through, and it tracks the mouth downward as the
shaft empties, which is the head growing.

> The first version took "any air cell with air below" and found the FLOOR OF THE
> CAVERN the shaft opened into: A = 473 for a 5×5 shaft, because the chamber
> under it was 25 cells across. `A` is then not an orifice and Q is an arbitrary
> rate. The gate caught it as a −66,773-eighth conservation failure.

The lowest candidate wins by `atomicMin` over a key packing (y, x, z), so the
answer is order-independent (integer min is associative; the `atomicCAS` ban is
about the other kind). Two copies of the record, CURRENT and NEXT: the scan
reports into the NEXT half and the ledger promotes it next tick — §3.3's "never
read a tally in the pass that writes it", at pass granularity. A hole outlives
its last sighting by `WB_HOLE_TTL` = 8 ticks, which is what makes *plugging* a
hole stop the drain rather than a timer the drain depends on.

**Emission goes through the existing seam, and nothing new.** The jet is
`FluidSpawnOp`s consumed by `spawnAppend`, which already charges the pool budget
and refuses past `FLUID_CAP`. What is new is only who FILLS them: the CPU cannot
author these ops, because `h` comes from a level the GPU owns. So the CPU
RESERVES a block of `kWaterDrainOpsPerBody` (512) slots per proposed body,
immediately after this tick's real pours, and `wbDrain` fills every slot — live
while the hole flows, DEAD (`mat` 0, which `fpAlive` already rejects) after it. A
slot the pass skipped would keep a stale particle that compaction counts as live.
`FA_SPAWNDEAD` counts the dead tail so a conservation gate can subtract it from
`FA_LIVE` and see the real in-flight mass.

The reservation is rule 2 charged BEFORE emission, on the CPU, deterministically:
the ledger refuses the discharge outright to any body without a block
(`b < T.waterDrainBodies`), so a granted eighth always has a particle behind it.
And when the per-tick cap binds — it does; the analytic Q at a 7×7 orifice is 941
eighths/tick against the 512 cap — the debit is what was WRITTEN, never the
analytic Q (§3.2).

**Component 7 is a SHELL, not a ball.** A draining hole hands a region around
itself to the solver while the rest of the body stays a number; the trigger lives
in `exciteDetect` (trigger (e)) so it inherits `sim.fluidExciteCeiling` and
`sim.fluidExciteRate` unchanged, and `waterBodyState` moved into the SLIM bind
group so the seam can read it. The region is the free-surface annulus at the
body's level out to `sim.drainExciteRadius` plus a narrow throat column over the
hole — not a solid hemisphere, which at the r ≈ 25 a real vortex implies is
~33,000 particles against a ~40,000 largest-measured scene (plan §9 item 2, and
the mitigation was UNMEASURED there). Measured at radius 6: **14,468 cells
converted cumulatively over a 90-tick drain, 161/tick**, against a standing
ceiling of 8,000. The shell only fires while `WBS_EMIT > 0`, so a plugged hole
does not leave an excite source standing open over still water.

**§9's ranked-first risk is REOPENED, and this time the mechanism is there.** M2
measured the surface shave producing 0 excite candidates against 10.9 M cells
inspected — the shave removes from the TOP, so it creates no air-below. A real
jet at a real throat is a different question and the answer is different:

```
exciteDetect LOOKED AT   5,897,839 settled liquid cells   (90 draining ticks)
excite CANDIDATES           51,346                        (570.5 / tick)
```

That is the same order as WP5's own 169,616 over 400 ticks (424/tick) on
`worldlake`. It is BOUNDED rather than absent — the ceiling and rate hold it, and
refusal is graceful because refused water is still settled water and the CA moves
settled water. `--gate waterbody` carries a 3,000/tick bound in
`tests/baseline.json`, ~5× the measured value, so a regression into an unbounded
burst fails and the normal case does not.

**Pass H, and why its identity is not pass A's.** Pass A's sum is about the LAKE,
where the only movers are the shave and the tap and both report what they
granted, so it closes at exactly +0. Pass H punches a 7×7 shaft into a sealed
29×29×18 chamber and the water does not leave the WORLD, it leaves the lake:

```
boxVoxelEighths(t) + inFlightMpm(t) - debit(t)  ==  boxVoxelEighths(0)
```

Two arms. **H1** turns the excite seam and the splash coupling off, so only the
discharge and the shave can move an eighth: **35,381 eighths drained through a
real hole, residual −37 (0.10%)**. **H2** is the shipped configuration with the
shell live: 26,476 drained, residual −305. `capped` is 0 in both, so the shave
was never short and the ledger debited what it granted every tick — the residual
is entirely downstream of the ledger, in a churning MPM pool where the CA's
thin-film handling, the sun/water evaporation rule and the always-on wake trigger
all act on water the water-body system no longer owns. The bound is an assertion
that the discharge is not a PUMP, not a claim that a churning pool is lossless.

**The known M3 cost, named rather than discovered.** The shave makes RESTING
voxels move, so its footprint has to be declared to the page table before the
command buffer exists — and the CPU cannot ask whether a hole exists. What it can
see is the thing that MAKES holes, so any world mutation opens a 900-tick window
(`kWaterDrainHotTicks`) during which a governed body declares its footprint.
Outside that window a still lake declares nothing and materializes nothing, which
is M2's zero-idle-cost property kept. Inside it, a body materializes its WHOLE
footprint rather than the two Y layers the shave can write; narrowing that needs
the CPU to know the live level, which is exactly what M2 moved onto the GPU.

**`sim.waterBodyMode` is still 0 by default, so the pinned world hash does not
move at M3 either.** Every row's condition is false at mode 0, no op block is
reserved, and `T.waterBodyCount` 0 makes component 7's loop a zero-trip.

### 5b.6 M5: the container sweep, and a lake that splits itself (added 2026-08-29)

`docs/PLAN_water_master.md` components 2 (case 2) and 10 — the last milestone.
M1-M4 governed the basins WORLDGEN makes, which are closed forms: a tarn is an
integer parabola and an authored pool is a flat-floored cylinder, so their
area-per-height tables are free and exact. M5 is what happens when the player
takes a shovel to one.

**The four outputs of one sweep.** Plan §2 asks for a height-ordered union-find
sweep producing `area(y)`, the spill elevation, the split elevations and the
split children — one pass, four answers, because the merge tree of a basin read
downward IS its split schedule. Here:

| output | pass | word | how |
|---|---|---|---|
| `area(y)` | `wbSweep` | `SW_AREA0..` | `atomicAdd` per CONTAINER cell (air or the body's liquid) at the swept level, inside the disc |
| spill elevation | `wbSweep` | `SW_SPILLY` | `atomicMin` over the ring one cell OUTSIDE the disc — water leaves a basin over its rim, and the rim is not in the basin |
| split elevations | `wbSplit` | `SW_SPLITY` | `atomicMax` over every level whose wet region is disconnected |
| split children | `wbSplit` | `SW_SPLIT0..` | a 2-bit component index per grid cell |

Both accumulate over a whole CYCLE — one level per scheduled tick — so the two
reductions are published through a current/next pair (`SW_SPILLYN`,
`SW_SPLITYN`) promoted at the cycle boundary. Accumulated in place they were
worse than useless: a reader mid-cycle sees the maximum over however many levels
happened to have been walked, which for a draining lake is "roughly the live
level" and looks exactly like a correct answer.

**It is a kernel, and that is a rule-1 decision rather than a performance one.**
Plan §2 allows an async `voxregion` readback as a second choice. It is not
available here: the table decides which pool a cell belongs to and therefore
which cells the shave takes an eighth off, so a table whose ARRIVAL is set by
fence retirement puts scheduling inside a voxel write's control path — §5b.4's
hazard through a different door, and one that two runs in a single process
cannot catch because they share a fence cadence. So the sweep is a compute pass,
its outputs are consumed by compute passes, and the CPU's entire contribution is
a SCHEDULE: which body, which level, both pure functions of the tick.

**No union-find, and no atomicCAS.** Classical connected-component labelling
wants path compression, which wants `atomicCAS`, which rule 1 bans outright
because a CAS loop's outcome depends on which thread arrived first. Min-label
propagation reaches the same fixpoint without it (integer `min` is associative
and commutative, so the fixpoint is unique), and running it inside ONE workgroup
— read phase into registers, `workgroupBarrier()`, write phase into cells this
invocation alone owns — makes the whole thing a pure function of the input
bitmap. It costs one under-occupied workgroup on a pass that runs once per
scheduled tick. Every barrier sits in uniform control flow and every early-out
that depends on a storage read is a FLAG rather than a `return`, because WGSL
treats a storage load as possibly non-uniform and rejects the alternative.

**The connectivity grid is a downsample, and the direction of its error is
chosen.** Labelling runs on a `kWaterSplitGrid` squared grid laid over the
basin's column AABB (one grid cell is about 3 columns for the harness lake), and
a grid cell counts as OPEN if ANY of its columns is. That is the LIBERAL
direction on purpose: it can only ever UNDER-split — report one pool where there
are two — and under-splitting is the status quo, which the CA already handles.
OVER-splitting would strand water in a puddle nobody drains, and that is the
direction that is not safe. Components smaller than `WB_SPLIT_MIN_CELLS` grid
cells are folded back into the parent for the same reason: at the top of a disc
the circle's edge clips two or three cells, and ranking components by grid index
alone once handed the parent that speck and the child the entire lake.

**A split is not new arithmetic.** Nothing divides anybody's water. The map
changes which cells each body OWNS and the existing ladder does the rest: the
child's adoption reduce measures its own voxels, the parent's re-audit
re-measures what is left, and both are voxel sums — so `held(parent) +
held(child)` equals the parent's pre-split content BY MEASUREMENT rather than by
a division that could round. §5's "both directions must be mass-exact" reused
instead of re-derived. `--gate waterbody` pass B measures it at **+0 eighths**.

**The re-audit closes M2's named leftover.** A body adopted once carried the
volume it had at adoption; anyone who dug into it made that number a lie, and it
bounds the discharge through `held = VOLUME - DRAINED`. On the first level of
each sweep cycle the ledger arms `WBS_REAUDIT`, the adoption reduce refills
`WBS_RSUM`, and the next tick writes

    VOLUME := RSUM + DRAINED - DEBIT

The form is the correctness argument: the standing invariant is `voxels == held
+ debit`, so folding the debit in is what keeps it true. `DRAINED` and `DEBIT`
are untouched, because they are the cumulative terms passes A and H balance
their identity on and a re-audit that reset either would read as a leak of
everything the body had ever drained. The arm and the consume are two ticks
apart for plan §3.3's reason applied inside one kernel: written as a single flag
it fired on the tick it was armed, read the sum it had just zeroed, and set
`held` to zero — which refuses every drain.

**Discovery is a latch on the tick stream.** A mutation landing in a chunk the
registry LABELLED marks that basin's curve dirty for `kWaterDrainHotTicks`. A
dirty basin, and only a dirty basin, proposes a split child and takes a slot in
the sweep rotation. Everything else proposes nothing and records neither sweep
row, so a world nobody has dug in pays exactly what it paid at M4.

**Where it all lives.** The sweep's outputs sit past the end of the ledger in the
SAME buffer (`waterBodyState`, world.h's `kWaterCurveBase` block) rather than in
a buffer of their own — a deliberate refusal to add a binding, since every
accumulator the sweep needs (`atomicAdd`, `atomicMin`, `atomicOr`) is a
sanctioned order-free atomic and the buffer is already an `A(WaterBodyState)` row
in every water pass.

### 5b.7 What is NOT here

`sim.waterBodyTestDrain` survives from M2 as a development tap of a known size in
eighths per tick, 0 in every shipped world — it exists so the ledger and the shave
could be proved exact before there was a hole to be exact about, and pass A still
uses it for the one identity that closes at +0.

M3's own gaps, still open: ONE hole per body (the descriptor has room for a list;
the ledger carries the deepest), no wall holes with lateral jets (the exit
velocity is straight down), and the hot-window footprint declaration above.

M5's own, and the first is the one worth knowing before building on this:

* **An outstanding debit is not divided across a split.** The parent keeps all of
  it and pays it out of the part it kept. Mass stays EXACT — the identity is over
  both bodies and neither `DRAINED` nor `DEBIT` moves — but the pacing is wrong:
  a body carrying a large debit when its footprint halves descends at twice the
  rate it should. `WB_MAX_STEPS` bounds the damage to one voxel per tick (rule 2)
  and at every shipped rate the debit is under one eighth-step anyway, so this is
  reachable only by a development tap sized past the surface it drains. The exact
  fix is a proportional transfer at the child's adoption, which needs both
  bodies' reduces on one tick.
* **The split names at most three components**, and a basin with more leaves the
  extras with the parent — the same safe degradation every refusal here takes.
* **A basin the player digs from NOTHING is still not a basin.** The registry
  knows exactly two kinds, the authored pools and `pondAt`'s tarns, and M5
  re-derives the container of a basin the player MODIFIED. A hole dug in flat
  ground that fills with water is a CA pond, as it was before.
* **The container curve is not persisted and not saved**, like every other
  derived structure in this design. A window rebuild re-derives it.

### 5b.8 W1: relevel — a body finds its level (added 2026-09-14)

`docs/PLAN_water_relevel.md` §3. The surface shave of §5b.4 only ever moves a
lake DOWN, uniformly, because a drain is a global potential with one number
behind it. Relevel is its mirror image: every column of a disturbed body relaxes
toward the body's own MEAN surface.

**Why it cannot be a CA rule, and this is proven rather than tuned.** The
equalize branch in `sim_step.wgsl` fires only at a 2-eighth difference between
neighbours, and `bridgeLevel` extends its reach to a distance-2 pair. **A ramp of
1 eighth per 2 cells is therefore a stable fixed point** — over an 80-cell radius
that is 40 eighths, so a 5-voxel cone is PERMANENT. Above that slope it flattens
by diffusion at order r² ticks per eighth, which at r ≈ 80 and 30 Hz is minutes
per eighth. Lowering the threshold to 1 was refused with proof (the (k, k+1) pair
is the integer diffusion equilibrium and every reach-1 tie-break ratchets or
oscillates). Flatter than reach 1 allows needs a global operation, and the body
system already is one.

**Three passes, in the existing row block**, ordered `wbShave → wbRelevel →
wbSurface`:

| pass | shape | what it does |
|---|---|---|
| `wbSurface` | one thread per column of a listed chunk | finds the column's free surface in the band and adds its height to the body's histogram |
| `wbLedger` (extended) | one thread per body | turns LAST tick's histogram into THIS tick's give/take cutoffs, and banks the credit |
| `wbRelevel` | one thread per column of a listed chunk | gives or takes eighths in its own column per the cutoffs, and REPORTS what it moved |

The measure runs after every writer in the block so next tick's ledger sees the
surface this tick settled on; the ledger consumes it one tick later — §5b.4's
"never read a tally in the pass that writes it", at pass granularity, exactly as
the shave report works. `wbRelevel` runs after `wbShave` so a draining body's two
writers can never reach the same cell: the shave owns `level` and `level-1`, and
the relevel skips a column whose surface is in that band while
`WBS_STEPS | WBS_FRAC` is nonzero.

**One chunk layer owns each column.** The band is up to
`sim.waterRelevelDepth` (32) voxels deep and straddles two or three of a body's
listed chunk layers, so unlike the shave's two-Y band a naive dispatch would
measure one column several times. The layer holding the body's `level` owns it
and walks DOWN through the page table; every other listed chunk returns after
three scalar loads. The walk stops at `floorY + 1`, which is both physically
right (a pocket under the basin floor is the MPM's and the CA's) and a page-table
argument: the body's chunk list covers its water AABB and a write below it would
be a lost eighth reported as a page fault.

**The arithmetic, all integer.** `m = RVSUM / RVCOUNT` is the mean surface in
eighths; `k(s) = clamp(|s − m| / gain, 1, max)` is what a column at height `s`
may move this tick; supply and demand are summed over the histogram with
`min(k, |s − m|)` so no column can overshoot the mean. The ledger then walks the
buckets from the bottom for takes and from the top for gives and publishes a
cutoff plus a dither for the partial bucket, exactly as it publishes
`WBS_STEPS`/`WBS_FRAC` — the dither key is the WORLD column, never a chunk-list
index, because a list reorders and an index used as an identity must be the
stable one.

**The credit is a stored field, for the reason the debit is.** The histogram is
one tick stale, so a take can outrun its gives by a bounded amount; `credit +=
GIVEN − TAKEN` records it, and while `credit < 0` the ledger publishes NO take
cutoff and the gives repay it first. That is the `WB_RELEASING` argument — a
released body keeps shaving until the ledger is square — applied in the other
direction. `--gate waterbody` pass R's identity carries the term, and closed at exactly
+0 on both arms of the landing measurement:

    voxelEighths(t) + drained(t) − debit(t) + credit(t)  ==  voxelEighths(0)

**Where it lives.** The eleven `WBS_RV*` words plus `WBS_RVBASE` extend the
ledger (`kWaterBodyStateWords` 27 → 39; they are `RV` and not `R` because
`WBS_RSUM` is already the adoption reduce's running sum and a re-audit can run
while a body relevels). The histogram is 272 words per body appended past the
sweep scratch in the SAME buffer — no new binding, and deliberately not at
`kWaterCurveBase + cap·kWaterCurveWords`, which is the shared openness bitmap's
address.

**Idle cost is zero, not small.** The CPU's hot latch (`kWaterDrainHotTicks`,
900) is what declares the footprint to the page table, and the relevel rides it
through one uniform word: `TickParams::waterRelevelMax` is
`sim.waterRelevelMax` on a tick the footprint is declared and **0** on every
other, so the arm and the rate are one number with one owner. The latch itself
now fires on `drainMax > 0 || relevelMax > 0` rather than on the drain knob
alone — a knob about jets must not switch off a rule about surfaces. At
`sim.waterRelevelMax = 0` both kernels return on their first comparison and W1
is an exact identity.

**Pass R, and why its fixture has the MPM off.** A 17x17x6 crater is bored into
the lake's own floor and goes nowhere — sealed stone, no discharge, and
`sim.fluidExciteMode` 0. That last one is §5b.5's H1 discipline reused: this is
a rule that moves SETTLED water between columns, so the fixture must contain
nothing else that moves settled water. Measured with the seam live and a 33x33x8
pit, the solver and the evaporation rule took 54,091 eighths out of the lake
WITH THE RELEVEL DISARMED and left 64 chunks awake — every number the pass
reported was then a statement about the solver. Seam off: the CA alone leaves 15
eighths of spread after 150 ticks, the relevel leaves 1 (budget 2) and is flat
at 45, 12,855 eighths given equals 12,855 taken, credit 0, capped 0, 0 chunks
awake, identity +0. The pass runs BOTH arms and fails if the control also meets
the budget — a fixture the CA would have closed anyway proves nothing.

**A cost stated rather than discovered.** Because gives are dithered across every
above-mean column, any disturbance that opens real demand scatters single-eighth
writes over the body's whole surface and wakes most of its surface chunks for the
active ticks. Bounded and brief — at rate 4 a 10-voxel cone is flat in ~20 ticks
against a 900-tick window — and correct for a crater. If a measured case ever
makes it matter the knob is a demand floor in the ledger, not a change to the
apply.

### 5b.9 W2: surface momentum — the pond overshoots (added 2026-09-14)

`docs/PLAN_water_relevel.md` §4. W1 RELAXES: every column drifts toward the
body's mean and stops, which is water finding its level and is not what water
looks like. A real pond OVERSHOOTS — the water beside a fresh crater accelerates
into it, arrives carrying momentum, piles past the level and rings back out. W2
is that momentum, and it is one extra integer per column FACE and nothing else
new.

**Four owned outflow pipes per column, never two shared signed faces.** Each
column owns `q[+x], q[-x], q[+z], q[-z]`, all non-negative (the Mei/O'Brien
virtual-pipes layout); the reverse of a pipe is the NEIGHBOUR's own pipe, not a
sign on one shared word. That is not a packing preference, it is the whole
write-hazard argument: the outflow clamp has to scale a column's outflows
against what that column may give, and with shared faces half of them run
through a face the neighbour owns — so "write the scaled flux back" would be two
writers per word, the exact mark/apply hazard everything else in this subsystem
avoids. With owned outflows **every transfer amount lives in exactly one word,
written by its owner and gathered by its receiver**, and conservation between two
columns is exact by construction rather than by agreement between two copies.

**The store** (`world.waterFlux`, `world.h` `kWaterFluxWords`). A dense XZ grid
over the residency window, six u32 per column — four pipes, the free-surface
height in eighths, and a validity stamp — indexed `(z & WORLD_MASK) * kWorldN +
(x & WORLD_MASK)` like every other window-relative buffer. 6 MiB at 512², one
binding (36 in `simBGL_`), one pass-table resource. Derived data in guideline
#3's sense: not hashed, not saved, dropped on a window shift.

**The stamp is the window-shift answer, and it is one word.** The residency
window is toroidal, so a slot is silently reused by the column `kWorldN` voxels
away as the window walks. `(tick + 1) << 4 | pageX << 2 | pageZ`: the tick half
is freshness, the page bits are identity. The tick half alone would ALMOST do —
but a shift moves the origin by one CHUNK, so the departed column's stamp can be
exactly one tick old on the tick its slot is re-read, and two columns sharing a
slot differ by a multiple of `kWorldN`, which is what those two bits separate.
Same shape as `opennessStamp` (a hash of the world chunk coord) and `reposeSnap`
(a per-slot tick). **A column ENTERING the valid set has its pipes zeroed by
`wbSurface`**, which is what also makes the store safe across a world reload, a
`--sweep` arm and the determinism gate's second pass — all three put a record
from another world in the slot, and a stale pipe read as fresh would let session
history decide a voxel write.

**Three passes, and the apply is FOLDED IN.** `wbSurface` (W1's measure) writes
the height and the stamp on the same walk that fills the histogram. `wbFlux`
runs between the shave and the apply: per owned column it gathers five heights,
accumulates `q += G·depth·Δs` in Q8, clamps at 0, damps, and applies the outflow
clamp against `min(relevelMax, topFullness, s - floorS)`. `wbRelevel` then
carries BOTH contributions — the relevel's give/take plus pipe inflow minus pipe
outflow — so a column is written **once per tick by one pass**. A second apply
pass would be two writers of one word one tick apart, each having read a fullness
the other was about to change.

**The physics, and the one discretisation guard.** `G` is the sanctioned
human-unit lane (`sim.waveGravity`, vox/s², const-eval'd to Q8 cells/tick² at
30 Hz exactly as `DRAIN_TWO_G` is). The wave speed is `sqrt(G·depth)`; at real
gravity and the shipped `sim.waveDepthCap` of 10 voxels that is 1.04 cells/tick,
the CFL limit — so the DEPTH is capped rather than the speed clamped afterwards,
which keeps the pipes consistent with the transfer they are allowed to make.
`WAVE_HEAD_EPS` (2 eighths, `sim_waterbody.wgsl`) is the guard and it is not
optional: **measured on the first run of pass S, without it Σ|q| rose to 9.1 M Q8
and STAYED there for 150 ticks** — every pipe in 14,493 columns pinned at the
clamp, the surface 8 eighths from flat instead of 1, 85 chunks awake, the body
never asleep. The physics was right and the discretisation was not: real gravity
over a one-eighth head accelerates at 1.09 eighths/tick², a two-cell checkerboard
cycles in two ticks, and the CA's own equalize rule leaves exactly that
checkerboard everywhere by design. Two eighths is `TUNE_LIQUID_EQUALIZE`'s own
threshold, so the wave stops where the CA stops and the two cannot fight.

**Sleep, and how it wakes.** `wbFlux` reports `atomicMax(|q|)` and `Σ|q|` into
the ledger; a body that is STILL (max under `sim.waveSleepEps`, shipped 256 —
which is not a tolerance, since a pipe moves `q >> 8` whole eighths and under 256
moves nothing) **and** FLAT (the measure's own histogram spread ≤ 2 eighths) for
`sim.fluidSettleTicks` consecutive ticks publishes flux-asleep, and both W2
passes then return after three loads. Both halves are needed: stillness alone
sleeps a ring at the instant it passes through its own mean, flatness alone
sleeps a lake with a fast shallow ring crossing it. Waking is the FLATNESS going
false, which is why the spread is measured in the ledger rather than inferred
from the pipes — an asleep body records no pipes, so nothing derived from them
could ever wake it.

**`sim.waveMode = 0` is an exact identity in the strong sense**: `Cond::WaterWave`
leaves the `waterFlux` row unrecorded entirely, so the pinned hash cannot see a
pass that was never dispatched. The wave rides the relevel's arm
(`TickParams::waveMode` is zeroed with `waterRelevelMax`), because the apply it
folds into is the relevel's and may only run on a tick whose chunks were declared
to the page table.

**Pass S**, on pass R's fixture exactly — same lake, same 17×17×6 crater, no
drain, no test tap, MPM seam off — so the only difference between the two passes
is the pipe layer. Measured at the W2 landing: Σ|q| peaks at 842,799 Q8 in bin 3
of 12, decays monotonically to 61,038 and then to 0, the body publishes
flux-asleep at **112 ticks** (budget 150), the surface ends **1 eighth** from flat
(budget 2), **the mass identity closes at +0** with 36,764 given / 36,524 taken
and a credit of 240, 0 chunks awake, 0 page faults. The dissipation test is
BINNED rather than a per-tick monotone: Σ|q| is an integer sum over dithered
columns and a ring reflects off the bank and focuses at the centre, which can
lift the sum for a tick or two without creating anything.

### 5b.10 W-D: discovery — a body the PLAYER made (added 2026-09-14)

`docs/PLAN_water_relevel.md` §8. §5b.7's third M5 gap — "a basin the player digs
from NOTHING is still not a basin" — is closed. A pit dug and filled by hand, or
a pool a drain leaves behind, becomes a real body with a ledger, a level and
W1's relevel; a puddle stays CA and costs nothing to ignore.

**The constraint that shapes all of it.** The CPU may not look at voxels. The
mirror is 3×3×3 and a readback on the frame path puts fence retirement in a
voxel write's control path, which is §5b.4's whole argument. So discovery takes
the M2 authority split verbatim:

* **The CPU proposes, from the tick stream only.** Every liquid-placing edit
  rides the MutationQueue, so "how many eighths of which liquid were placed,
  where" is exact arithmetic on the tick's op list. A replay reproduces it and
  the twice-run gate compares it by construction.
* **The GPU disposes.** The existing WB_MEASURING reduce measures the real
  water; the ledger adopts it or parks it in a new sticky `WB_REFUSED`. The CPU
  never learns the verdict and does not need to.

**Where the accounting hooks, and why there.** `WaterBodySystem::NoteMutations`,
called from `SubmitTick` immediately before `WaterBodySystem::Tick` — the one
function the game frame loop, every gate and both smoke harnesses hand their op
lists to. The player's brush arrives as `ops`; mobs, debris, prefabs, tree
felling, the world edit layer, spells and every gate's hand-built list arrive as
`cells`. There is no other door into the MutationQueue, which is what makes a
gate's CellOp pour a statement about what a player would do.

**Evidence is a heuristic and is allowed to be wrong in both directions.** It
over-counts (a `kCellOpIfAir` op the grid then refuses still counts — the CPU
cannot know without the readback that is banned) and it under-counts (water that
ran laterally out of every probe disc is missed, a stated v1 limit; that water
stays CA, which is the old behaviour and not a regression). Neither can cost an
eighth: evidence decides only whether a DISC IS PROPOSED.

**The shape.** Placed eighths accumulate per coarse XZ cell
(`kWaterEvidenceGrid` = 32 voxels) per liquid material, capped at
`kWaterEvidenceCap` with lowest-evidence eviction. A promotion scan — run only on
ticks where the evidence or the registry moved — flood-fills the grid into
clusters of one material, bounds each with a circle padded by
`kWaterDiscoverPad`, and either GROWS the probe already covering it (re-dirtying
the entry, which is the same latch shape as `curveDirtyUntil_`) or, past
`sim.waterDiscoverMinEighths`, raises a new one. A probe enters `basins_` as a
flat-disc cylinder whose curve is a PREDICTION in §5b.4's sense, so an
inaccurate container costs pace and never mass; the sweep re-derives the real
`area(y)` if the player keeps digging in it.

**Refusal is a state, not a retry.** A discovered probe whose measurement fails
the volume floor or the new `sim.waterAdoptMinArea` size gate goes to
`WB_REFUSED` and stays there: four ledger loads a tick, no footprint pass, no
chunk wake. Sending it back to WB_CANDIDATE — which is right for an authored
basin, whose container is a closed form the registry vouches for and whose water
may still be on its way — would re-run the ONE whole-footprint pass every
`sim.waterBodyQuietTicks` forever for a puddle. It leaves the state only on
`WBF_REPROBE`, a ONE-TICK pulse the CPU sends when new evidence lands in the
disc, so the tick it re-measures on is tick-deterministic too. A latch held over
several ticks would reset it to candidate on every one of them and it could
never accumulate the quiet ticks adoption needs.

**The size gate needed a measurement that did not exist.** §8.4 asks the ledger
to adopt iff the measured surface area clears a floor, and `wbReduce` measured
only the volume and the level. `WBS_RAREA` (`kWaterBodyStateWords` 41 → 42) is
that count, filled by the reduce on the single tick a body spends in
WB_MEASURING. The walk was inverted to run DOWNWARD so the previously read word
IS the cell above — one read per cell as before, plus one per column. `WBS_AREA`
at adoption is the CPU's ANALYTIC seed, and for a probe that is the area of a
cylinder the CPU drew around some evidence: gating adoption on it would be the
candidate's own guess deciding whether the candidate is real.

**The registry is the one thing here that is SAVED.** Everything else in
`waterbody.h` is derived — reconstructible from (seed, window, voxels),
disposable, never saved. A probe disc is the residue of what the player did, so
it is authored-equivalent truth (guideline #3) and rides the world save as the
`'WTRB'` entity section: a few ints an entry, no ledger, no level, no curve, no
verdict. On load the entries are re-proposed and the GPU re-adopts each one by
re-measuring the restored water, which is the same path it took the first time.

**Rule 2, charged before emission.** `kWaterDiscoveredCap` = 16 probes inside
`kWaterBodyCap`, and `kWaterDiscoverChunkShare` = a quarter of `kWaterChunkCap`
across all of them — a probe's analytic cylinder over-predicts, so it sorts
early in Classify's biggest-first order and would otherwise be entitled to take
the chunk list out from under the lake beside it. An evicted probe is proposed
with `WBF_RELEASE` for `kWaterDiscoverReleaseTicks` before being dropped: never
drop a descriptor cold, or the slot is reused against a ledger still carrying
the old body's state.

**Two asymmetries a probe gets that an authored basin does not**, both because a
proposal's safe degradation is to not exist:

* A straddle between two WORLDGEN basins refuses both — neither is more entitled
  and the CA simulates both correctly. A straddle involving a probe refuses only
  the probe. Refusing the harness lake because somebody made a puddle on its
  bank would be a regression bought with a feature.
* The size gate and the sticky refusal apply only to `WBF_DISCOVER` bodies, so
  `sim.waterDiscoverMinEighths = 0` is an exact identity: no probe exists, no
  descriptor carries the flag, and no branch behind it is reachable.

**Gate `waterbody` pass N**, four arms. A 33×33×13 pit dug and filled with
CellOps 150 voxels west of the harness lake raises one probe, which adopts at
109,016 eighths against 113,256 poured (−3.7%) over 1,107 measured surface
cells. A second pit filled to 1,936 eighths — under half the 4,096 threshold —
raises nothing and puts nothing in the ledger. The `'WTRB'` block round-trips and
the GPU re-adopts by re-measuring, at exactly the same 109,016 eighths. And pass
R's crater, bored into the CREATED body, leaves it flat to 2 eighths with 0 page
faults — W1 reaching a body the player made, which is the owner's actual ask.

**One interaction found and not fixed here.** The crater makes the probe's basin
curve-dirty, which arms M5's sweep; this pool's disc holds a second open region
(its water reached 1,107 columns against the 1,089 dug, so it found a way out
sideways), the sweep names two components, and the child adopts the water while
the parent keeps component 0. That is M5 working as designed for an ADOPTED
body — but a body that has to RE-ADOPT from scratch under a live split map can
end up with the empty component and refuse. A real load does not reproduce it
(the ledger buffer comes back zeroed with the world, so no stale map survives),
so pass N runs its round trip before the crater rather than after. Whether M5's
ladder restart under a live map deserves its own fix is a question for the split
machinery, not for discovery.

### 5b.11 W3: what disturbs the surface (added 2026-09-14)

`docs/PLAN_water_relevel.md` section 5. W2's pipes carry a ring the HEIGHTFIELD
started - a crater, a bank giving way. W3 is the three sources that start one
from OUTSIDE it, plus the render half that reads the pipes back.

**THE STACK SHIPS ON FROM THIS COMMIT.** `sim.waterBodyMode` and `sim.waveMode`
both default to 1 in `assets/materials/tuning.json`. That is the ship decision
for the whole relevel / discovery / slosh / W3 package and it is what moved the
pinned world hash. Every off switch remains an exact identity - mode 0 records
no GPU pass at all, `waveMode` 0 does not record the `waterFlux` row, and each
of the four W3 knobs is independently an identity at 0.

**One record, one door, two callers.** A blast and a swimmer are the same
statement - *at this column, for one tick, push the surface this way this hard*
- so there is one `WaterImpulse` (x, z, radius, Q8 strength, Q8 direction;
(0,0) means radial outward), one bounded queue on `WaterBodySystem`, one
`TickParams` block (`kWaterImpulseCap` = 8 records x two `vec4<i32>` rows) and
one term in `wbFlux`. Two record types would be two things to keep in step for
no gain.

**`sim_explode.wgsl` does not touch the flux buffer, and that is rule 3 plus
section 4.1.** Rule 3 keeps the explosion's writes in the mutation path;
section 4.1's whole argument for owned outflows over shared signed faces is that
every pipe word has exactly one writer. A compute pass reaching into
`waterFlux` from the explosion kernel would break both. What crosses instead is
the EVENT, on the tick input stream, through `SubmitTick` - the one function the
game frame loop, every gate and both smoke harnesses hand their op lists to, so
the crosshair detonate, grenade fuses, spell blasts and a gate's hand-built list
all arrive by one path. The swimmer enters at the same door from `main.cpp`'s
TICK site (not from `player.Update`, which runs per FRAME: a frame-rate emitter
would shove the lake harder on a fast machine than on a slow one).

**The drain sink is the exception, and has to be.** "This body is emitting, this
many eighths, through a hole HERE" is derived by the ledger from a level the GPU
owns (`WBS_EMIT`, `WBS_HOLEKEY`). The CPU could learn it only from the async
readback, which is rule 1 through the back door - the same hazard M2 moved
quiescence off the CPU for. So the sink is generated in `wbFlux` from the
published words, behind `TUNE_WAVE_DRAIN_SINK`.

**Everything is added BEFORE the outflow clamp.** An impulse is another term in
the head `wbFlux` already integrates, so it passes through the same wall test
(`ns[d] == WB_RV_NOTAKE` zeroes the pipe whatever the blast said) and the same
per-column give limit. A W3 push therefore changes WHERE the water is and never
how much, and gate pass T carries the section 3.5 conservation identity on every
arm to say so. It is also added before `max(v, 0)`, so an impulse against a
standing head drains that pipe through zero exactly as an opposing head does.

**Two bugs the gate found, both about a lake being asleep.** The flux sleep
(section 4.3) is what makes a settled lake free: both wave passes return after
three loads. That is correct for a lake nobody is doing anything to and exactly
wrong on the tick a grenade lands in it, so `wvBodyAwake` overrides the flag for
one tick on an impulse or a live discharge - one uniform compare in a still
world, and it clears itself by the normal rule (the ledger sees `WVMAX` over the
epsilon next tick and resets `calm`). The second took two runs and a three-stage
counter to name: **the impulse arrived a tick too EARLY**. `wbFlux` is a pure
gather over the column heights `wbSurface` stored at the END of the previous
tick, and a lake nobody has touched has not run `wbSurface` for as long as it
has been still - so on the tick a splash arrives there is not one valid column
in the body and every pipe reads `WB_RV_NOTAKE`. A record accepted on tick N is
now shipped on N+1: accepting it arms the footprint latch, N measures, N+1
spends a heightfield that exists. That is the same one-tick CPU->GPU latency the
rest of the mutation path carries. A blast alone would have hidden it forever
(an explosion is a mutation, so it arms the latch by accident); a swimmer, who
writes nothing, would have been silently dead in the shipped game.

**Its own hot window.** `kWaterImpulseHotTicks` = 240 rather than the dig's 900:
a dig's consequence is a drain that runs for minutes, a splash's is a ring pass
S measured asleep after 112 ticks. Holding a lake's whole footprint materialised
for thirty seconds every time somebody swam past it is rule 2 with the sign
flipped. `max`, not assignment, so an impulse during a live drain cannot shorten
that drain's window.

**The render half (section 5's last bullet).** `waterFlux` is bound READ-ONLY to
`renderBGL_` at binding 22 - declared on the sim side for `gasOuter`'s stated
reason, and needing no pass-table row of its own because the writes have theirs
and every command buffer opens with a global memory barrier. Section 9d.5's rule
stands: `liquidColumn()` derives the surface from the VOXELS and nothing
overrides it. But that height is quantised to eighths, and the two things it
therefore cannot express are exactly what the pipes carry - sub-eighth tilt
BETWEEN the steps, and which way the surface is going. So `waveFluxAt()` returns
the column's net pipe flux, the normal leans down-flow by `render.waveSimSlope`,
and a column running hard foams by `render.waveSimFoam`, joined to the shoreline
and convergence terms by `max()`. Gerstner stays the far/idle texture. The
validity test is the stamp's PAGE BITS (the toroidal-aliasing half, which is the
one that matters for a pixel) plus a loose freshness bound, not the exact tick
match the sim readers demand - a stale pipe there would decide a voxel write,
here it would tint a pixel. **Both knobs at 0 const-fold the entire block away,
buffer read included**, which is not bookkeeping: this fragment shader has no
register headroom (two memory notes record a small change cliffing it) and that
is the arm `--shader-stats` is compared against. Measured: +177 SPIR-V
instructions on 159,487.

**Gate `waterbody` pass T**, every arm a PAIR. Blast: sum|q| peaks at 1,028,008
Q8 with the knob on and is EXACTLY 0 with it at 0 - nothing queued, nothing
shipped, no pipe touched. Wake: 53,739 against 0. Sink: a DIFFERENTIAL and not
an identity, because the shaft disturbs the surface either way - 16,227,477
against 16,217,094 over the same 60 draining ticks - and the arm FAILS rather
than skips if the shaft never emitted, since a sink generated from `WBS_EMIT`
cannot be measured in a fixture with no emission. The conservation identity
closes at +0 on all four impulse arms, with 0 page faults. The failure messages
name the three stages separately (queued / shipped / footprint declared),
because "sum|q| was 0" is a bare count and it cost two runs before it was not.

**Two things W3 does not do.** MOBS have no submersion or swim state at all
(`game/mob.h`), so the swimmer wake is the player only - the door is generic and
a mob that gains one is a single call. And pass H now disarms `waveMode`
explicitly: it measures whether the DISCHARGE LAW is mass-exact, and the
surface-momentum layer became a second mover of settled water the day the
default shipped at 1 (H1's ledger-only identity went +0 to +1225 eighths against
a 256-eighth slack with nothing else changed). That is the same discipline
passes R, N and S already state in their own words; the wave's own conservation
is asserted exactly, on its own fixture, by S and T.

## 9d. The current field, and surface waves (added 2026-08-29)

`docs/PLAN_water_master.md` components 8 and 9 (milestone M4, "it looks alive").
Read §9b (Wind) first: this is a deliberate clone of that system and every
structural decision here is inherited rather than re-argued.

M1-M3 made a still lake a NAME and a drain a real hole (§5b). None of that is
visible: a governed lake and an ungoverned one look identical, and so does a
lake with a hole in it until the jet reaches the frame. M4 is the part you can
see -- flow, whirlpools, waves that move like water -- and it is the part with
the least architectural risk, because **the current field owns no mass**.

### 9d.1 The current field is the wind field, applied to water

`src/sim/currentprim.{h,cpp}`, `currentPrims` in BOTH `TickParams` and
`RenderParams`, `currentPrimEvalF` / `currentPrimEvalQ` in `common.wgsl`.

The cloning is the design, not an accident of authorship. Water flow is the same
KIND of object as wind: a bounded list of parametric shapes summed analytically
at a sample point, like point lights. So it gets the same cap (32), the same
three-row `vec4<i32>` packing, the same union-AABB whole-loop reject, the same
float/integer transcription pair sitting adjacent in one file, and the same
`ptr<uniform, T>` rule. Inventing a second shape for it would have meant a second
set of overflow arguments to get wrong.

**Four primitives, and the set is closed because they are SUMMABLE.**

| Kind | Field | What it is for |
|---|---|---|
| `CPRIM_SINK` | `1/r^2` radial in, clamped at a core radius | A drain's throat |
| `CPRIM_SOURCE` | `1/r^2` radial out | A river mouth, a jet dissipating into a basin |
| `CPRIM_VORTEX` | `Gamma/2*pi*r` tangential about an axis, plus a 22% inflow share | The whirlpool |
| `CPRIM_STREAM` | uniform along an axis | A reach of river, from the bed gradient |

**Superposition only. No neighbour coupling, no stored field, no relaxation.**
The behaviour that motivated the field -- a river running into a pool and
dissipating outward -- is what a point SOURCE does for free under superposition.
Implementing it as real vector diffusion would mean stored state, a solver,
per-tick cost, determinism exposure and a system that does not sleep.
Superposition of sources, sinks and vortices is a real solution of Laplace's
equation, not a hack: incompressible irrotational flow away from boundaries is
approximately what pond water does.

**The sink/vortex asymmetry is the whole look.** The sink is `1/r^2` and is only
a couple of voxels wide at any realistic discharge; the vortex is `Gamma/2*pi*r`
and reaches far. That is why real whirlpools look enormous while the actual
suction is a small throat: the visible danger is the tangential term, the
lethality is the sink. `--gate current` pass P asserts both profiles by ratio
rather than by eye, because a field with the two swapped would still look busy in
a screenshot and would be wrong in the one way that matters.

**There is one square root, and the wind block has none.** `windPrimEvalQ` gets
away without one because every wind profile is quadratic in the distance
(`1 - r^2/R^2`), which `r^2` already gives. `Gamma/2*pi*r` and `1/r^2` are both
about the true radius, and faking them with `r^2` would give a whirlpool the
SINK's falloff -- i.e. delete the "reaches far" property the vortex exists for.
So `curISqrt` is paid once per primitive per sample, and sink, source and vortex
all consume the same radius.

### 9d.2 The authority line, which wind did not need

A wind primitive is authored by an op, so its parameters are trivially a pure
function of the tick input stream. A water current wants to be seeded from things
the CPU can only learn ASYNCHRONOUSLY -- most obviously whether the GPU ledger's
`WBS_EMIT` is non-zero this tick, which arrives (if at all) through a readback
scheduled by fence retirement. That is exactly the hazard §5b.4 and
`PLAN_water_master.md` §1.1 correction 2 name: "seeded when the CPU got around to
noticing" is a scheduling-dependent outcome, and it becomes a rule-1 violation the
moment a kernel reads it.

So every primitive carries `kCurrentPrimSim`, and it is set ONLY when the
primitive's parameters are a pure function of (seed, window, tuning, tick).
`currentAtQ` skips primitives without the licence; `currentAt` sums all of them.
A render field cannot write a voxel, so the split costs nothing.

**The drain seeder is where this bites, and the answer is that the CPU asks a
different question.** It cannot see the hole the ledger picked. What it CAN see,
on the tick stream, is the MUTATION that made the hole -- holes appear when
someone digs or explodes, and every one of those arrives through the mutation
queue. `WaterBodySystem::HoleHint` records that cell against the body whose chunk
it landed in, and the sink and the vortex sit there. Exact in the case that
matters (a player boring a shaft), approximate in the case that does not (which
of several digs the ledger called deepest), and free.

**Gamma and chirality come from `hash3` of the hole position.** This is
physically legitimate rather than a fudge: a real bathtub vortex is not created by
the drain, it is residual ambient circulation being concentrated as fluid moves
inward. `Gamma` is conserved, so `v_theta = Gamma/2*pi*r` blows up as `r` shrinks
-- the swirl is an INITIAL CONDITION. Drawing it from the position means not every
drain in the world spins the same way, which is the giveaway a single constant
would produce.

**Gamma decays when flow stops** (`sim.currentVortexDecay`, 3 s). A primitive
carries `spawnTick` for its attack ramp and `seenTick` for its release ramp, so a
seeder can re-assert a live whirlpool every tick without restarting its attack,
and the tick the digging stops the swirl starts winding down. Without this a
funnel stands open in still water, which is instantly and obviously wrong; the
gate asserts the envelope reaches exactly zero and that the primitive is then
dropped.

### 9d.3 The stream arm, and the slope trap it walks past

`CurrentPrimSystem::SeedStreams`. Manning/Chezy: `v = C * sqrt(slope * depth)`,
direction from the bed gradient. Genuinely independent of components 1-7 -- no
descriptor needed, and it would work in a world where the ledger did not exist.

`World::Column::slope` IS `Land.slope`, which `worldgen.wgsl:550` states is `g2`:
accumulated through the HILL octave and deliberately not through detail and
grain, because `d(slope)/dcolumn` through the grain octave is 96 Q8 -- the whole
of a gate's range in ONE column. A current built on the fine gradient is
per-voxel noise. So the MAGNITUDE reads that field directly.

The DIRECTION needs a signed gradient, which `slope` (an absolute sum) does not
carry and which is not exposed -- the signed `g2` pair lives inside the block
`check_invariants.py` token-compares against the shader, and widening it would be
a change to the mirror rather than to this system. So the direction is a central
difference of the ground height over a +-32 voxel baseline, which is the same
low-pass by another route: over that span the grain octave (cell 8 voxels,
amplitude 4) can contribute at most 0.06 voxel/voxel, where a +-1 difference --
the actual trap -- would give it 2.0.

The probe is scheduled by the WINDOW, not by the tick: the answer is a pure
function of (seed, window, tuning) and cannot change between window moves, so
re-probing every tick would be ~1,600 terrain hashes for a result already on the
list.

### 9d.4 The consumers, and the third transcription

* **Render surface advection** -- component 9 evaluates wave phase at
  `position - current*t`. This is what makes flow read as flow.
* **Foam on convergence lines** -- `currentConvergeAt`, a central difference of
  the field. Four evaluations of a function that early-outs to one compare
  outside the AABB, so a still lake pays nothing. Deliberately NOT a symbolic
  divergence: that would be a THIRD transcription of every profile with nothing
  checking it.
* **MPM particle drag** (`sim_fluid.wgsl`, `FLUID_CURRENT_DRAG`) -- the first sim
  consumer, and the only current knob a shader reads. Gated on
  `T.currentMode` rather than on a zero field, for the same reason the wind block
  beside it is: a drag term with a zero field still pulls every node toward a
  standstill, which is not "no current" but "infinite still water", and it would
  move the pinned hash through the settle seam. Unlike wind there is no exposure
  test -- air touches the skin of a body of water, a current runs through it.
* **Debris in and on water** (`sim_particle.wgsl`, added 2026-09-14) -- the
  buoyancy pass's viscous damping, re-aimed from zero at the local current. It
  needs NO knob of its own: the coefficient is the material's authored
  `fluid.drag`, which already answers "how hard does a liquid grab this", and
  "how hard does a MOVING liquid grab it" is the same number. Unlike the MPM
  arm this one is an exact identity at mode 0 twice over -- `currentAtQ`
  returns zero, and the term it replaced aimed at zero -- so the gate there is
  for the primitive loop's cost, not for the hash.

  **The wind is EXCLUDED from the same particles, and that half is the bug
  fix.** A floater at the waterline is not submerged -- its cell is the air
  above the topmost water cell -- so every force keyed on the cell the particle
  is IN switched off exactly when it started floating, leaving the wind acting
  alone on wood (derived response 8) and leaves (15). Chips of an exploded tree
  skated across a pond. `liquidBelow` is the missing predicate, and the rule is
  an exclusion rather than a blend on purpose: blending needs a submersion
  fraction and a grid voxel has no sub-voxel position to measure one from --
  the same structural fact that put buoyancy in the particle ring rather than
  in the CA (§5).
* **The player** (`player.cpp`, in the `inLiquid` block) -- a drag toward the
  local flow scaled by SUBMERSION, which that file already computes as a
  fraction. Vertical included, deliberately: the downward limb of a drain's
  vortex is the dangerous part.

That last one needs `CurrentAtCpu`, and **it is a third transcription of the four
profiles, named as one.** `common.wgsl` carries the float evaluator (the
renderer) and the integer evaluator (the sim), adjacent, with a standing
obligation between them. The CPU copy exists because the player is CPU physics
and the alternatives were to push the player from a field the renderer draws
differently, or not to push the player at all -- and "the current does not move
you" is the difference between a whirlpool and a painting of one. It is bounded
as a copy in the way that matters: it reads the RESOLVED rows, so the envelope,
the cap, the AABB and the packing are shared code and only the four formulae are
transcribed.

### 9d.5 Surface waves are render-only, and the boundary is absolute

`waveSlope` in `raymarch.wgsl`. A Gerstner sum evaluated where a ray HITS the
water surface -- never per sample through the volume. That is the one expensive
mistake the plan names: the perf audit identified the raymarch media march as
what collapsed the frame rate during fires, and this field's cost is O(water
pixels), not O(volume).

**A render wave can never push anything.** The body's LEVEL is sim (the ledger
owns it) and its DISPLACEMENT is render, and the CA never sees the displacement.
The moment a wave height is fed back so a boat bobs, a render field has become
authoritative for sim (design guideline #3). `waveSlope` returns a SLOPE for a
normal rather than a height anyone could sample, which is what keeps that honest.

**Per-octave speed from the local depth is the highest-leverage constant choice
in the whole render tier, and it costs nothing but the choice.**

```
w^2 = g*k*tanh(k*h)      k = 2*pi/lambda, h = local depth
  deep    (h >> lambda):  tanh -> 1     => c = sqrt(g*lambda/2*pi)
  shallow (h << lambda):  tanh(kh)~kh   => c = sqrt(g*h), lambda cancels
```

If every octave scrolls at one speed the surface reads as a moving texture. At
`pondDepth` 26 (2.6 m) the spread across the bands worth rendering is 4x -- 0.88
m/s at 0.5 m against 3.48 m/s at 8 m. And because `h` is the LOCAL depth the same
`tanh` pays a second time: approaching a bank at 0.3 m the long swell slows to
1.70 m/s while the short chop barely changes, which is shoaling. Green's-law
amplitude gain rides the same term. `render.waveDispersion` mixes between the two
regimes, and at 0 it reproduces the per-band speeds this shader shipped with
exactly -- so the claim is an A/B rather than an assertion.

**What this is NOT.** A sum of fixed-direction waves does not REFRACT: the crests
do not physically turn to run parallel to the shore, they only slow and steepen
there. Directional refraction would need the wave vectors to be functions of
position, which is a different field.

Depth is measured, not assumed: `waterDepthM` is a geometric probe, 8 taps at
increasing stride reaching 26 voxels with 1-voxel resolution near the surface.
The descriptor could supply it for a governed body, but the renderer has to be
right on the 95% of water that is not governed -- a puddle, a flooded cellar, the
CA's own transient.

Amplitude fades to zero below `render.waveShoreDepth`: a sum of sinusoids cannot
reflect off a bank and shallow water damps chop anyway, so the cheap fix is also
the physically right one. Without it the waves march straight through a
shoreline.

**Impact ripples are the one part that reads state**, because a ripple is the
memory of an event. A BOUNDED ring of 16 recent impacts (`WaveImpactRing`), each
drawn as an analytic expanding ring with amplitude decay and a `1/sqrt(r)` spread
-- a pure function of `(eventList, t)`, the `windAt()` idiom. The upgrade path (a
per-body 2D wave-equation texture) is stored state plus a solver; the plan says
DO NOT START THERE, and this does not. The event source is the rising edge of the
player entering liquid, sized by entry speed.

### 9d.6 The off switch, and why the look ships with the hash pinned

`sim.currentMode` is **0** by default and `currentAtQ` returns the zero vector
before reading anything else, so no sim kernel can see the field.
`RenderParams::currentRenderOn` is its own word rather than a copy of that mode,
and it ships **on** -- because a renderer cannot write a voxel. That asymmetry is
the whole shape of M4: the look is visible and the pinned world hash cannot move.

`--gate current` proves both halves in one invocation, in THREE arms
(mode 0 / mode 1 / mode 0) over an identical fluid pour with an identical
whirlpool standing in it. Two arms cannot tell "mode 1 changed the world" from
"arm 1 inherited something arm 2 did not". Unlike `--gate waterbody` pass D, arm
2 is REQUIRED TO DIFFER: pass D proves an off switch, this proves an off switch
AND that the knob reaches the kernel, which is the half `--sweep` cannot
establish in a world with no primitives in it.

### 9d.7 Knobs

Every current knob except one is CPU-side, and that is what this system's shape
makes correct rather than an exception: the shader reads resolved PRIMITIVES, so
a `TUNE_CURRENT_*` constant would be a second, never-read copy of a number.
`sim.currentDrag` is the exception, because `sim_fluid.wgsl` const-evals it.

`sim.currentMode`, `sim.currentVortexGamma` (m^2/s), `sim.currentVortexDecay`
(s), `sim.currentVortexRadius` (cells), `sim.currentSinkSpeed` (m/s),
`sim.currentStreamScale`, `sim.currentStreamMinSlope` (Q8), `sim.currentDrag`
(/s).

The wave knobs are RENDER-side and are `.def` rows, because `raymarch.wgsl`
evaluates the Gerstner sum itself: `render.waveDispersion`, `waveSteepness`,
`waveShoreDepth`, `waveFlowScale`, `waveFoamThreshold`, `waveFoamGain`,
`waveImpactSpeed`, `waveImpactDecay`, `waveImpactLen`, plus the arrow overlay's
`dbgCurrentField` / `dbgCurrentSpacing` / `dbgCurrentRadius`.

### 9d.8 The arrow overlay

`assets/shaders/debug_current.wgsl`, a clone of `debug_wind.wgsl`. It has to be a
clone: the arrow geometry, the axial fade and the near-plane cull are three bugs
already paid for once (see the notes in that file), and re-deriving them for a
second field would pay for them again. What differs is one line -- it samples
`currentAt`, the SAME function the waves advect with and the foam reads its
convergence from -- and the ramp's full-scale speed, 4 m/s rather than wind's
24 m/s, because at a wind scale every current in the world is one shade of blue.

It is only EVIDENCE because it is the identical function. A visualiser with its
own copy of the field would be a picture of a different current, agreeing with the
world only until someone edited one of the two, and it would be exactly as
convincing while wrong.

**Reached in-game by F4**, which cycles off → wind → current → off rather than
giving each field its own key. One cycle because the two fields are read by
COMPARING them -- does the raft answer the air or the water? -- and because two
arrow lattices composited into one frame read as noise, so only one is ever
live. `UIState::fieldViz` holds the state; the headless draw path in `main.cpp`
still reads `wind.dbgWindField` and `render.dbgCurrentField` directly, since it
has no UIState and cannot press a key, and `FieldVizFromTuning` collapses that
pair into the cycle's starting position (current wins a file asking for both).

The current arm draws **regardless of `sim.currentMode`**, because
`currentRenderOn` is its own always-on gate (§9c) -- so the overlay shows the
resolved primitives whether or not the sim is being pushed by them. What it
cannot show is a field with no primitives in it: `SeedStreams` and `SeedDrains`
run every tick, so a world with rivers or draining bodies has arrows and a world
with still water is honestly empty.

### 9d.9 What is NOT here

* **No wind-stress term.** Plan component 8 lists it as optional (a surface layer
  downwind with a return flow beneath, driven by the existing `windAt`). It is one
  extra primitive kind with a depth-dependent sign and it is not built.
* **No source seeder.** `CPRIM_SOURCE` exists and evaluates; nothing places one
  yet. The natural author is M3's jet where it lands, which needs the impact
  point, which is a GPU fact.
* **No refraction**, per §9d.5.
* **The stream arm places nothing in the shipped world**, because the authored
  pools and `pondAt`'s tarns are flat-floored basins and the slope gate refuses
  them. The arm is correct and will fire on standing water over a real hillside;
  there is not yet any worldgen that makes such water.
* **`sim.currentMode` ships at 0.** Flipping it is an owner decision and a
  rebaseline commit, exactly as `sim.windMode` was.

## 9c. The terrain viewer and the edit layer (added 2026-08-27)

Two things live here: a way to LOOK at generated terrain per voxel, and a way to
CHANGE it that composes with worldgen instead of replacing it.

### 9c.1 Why the column map was not enough

`--heightmap` renders a grid of `World::TerrainColumn` — ground height, slope,
sediment depth, water depth per (x, z) — and the tuner's Worldgen tab drew it
(the World map page's preview pane, since P-I).
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
`world.editLayer` (the map page's edits selector).

It is deliberately none of the three things it could have been:

* not a **ChunkStore save** (`world.svd/`), which is a live world pinned to one
  seed and one history and cannot compose with a worldgen change — and the whole
  premise of the preview pane is that you are still editing the map;
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
why `world.editLayer` ships empty and no gate sets it.

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

## 8b. The character screen (added 2026-08-29)

The screen the armour, weapon, pickup and spell-acquisition systems land into.
`I` opens it; `Esc` closes it before it does anything else. Three regions over a
dimmed but **still running** world:

* **left** — a LIVE avatar portrait with armour slots flanking it, sheath and
  quick slots beneath, and a `CHARACTER`/`HEALTH` toggle that swaps the same
  frame between equipment and an injury inspector.
* **top right** — the arsenal: every glyph the player owns, and the ten bound
  slots that **are** the magic-mode number row.
* **bottom right** — the pack (4x8) and the hotbar row, with drag between all of
  them.

The sim keeps running while it is open. This is WoW, not single-player
Minecraft: the engine is real-time and the MutationQueue is a future network
stream (§10), so a pause would be a lie the moment a second player existed.

### The portrait is the avatar, not a picture of one

`src/main.cpp` renders a **second camera** at the player's own rig into a
320x448 offscreen target once per frame while the screen is open, and ImGui
samples it. That is the whole reason the panel needs no portrait art and no
paper-doll layer: the avatar is one live copy-on-write micro-voxel body, so
missing voxels, char/cook material transitions (§6), severed limbs (§7),
dismemberment poses and whatever is in its hand all appear **for free**, and
nothing in the UI knows about any of them. Cost is ~2 draw calls over ~18 OBB
instances, no shadows, no raymarch — the honest cost is the second submit.

Three things about it are load-bearing and were each learned the hard way:

* **`world.renderUBO` is ONE buffer**, so two cameras cannot share a command
  buffer. Each pass writes its own params and is submitted separately; queue
  writes drain at the head of the next command buffer, which is exactly what
  makes that work. The main camera is re-written after the portrait submits.
* **The instance lists are shared too.** In first person the body is hidden and
  the arms are not (§8), so the portrait re-uploads with an empty hide mask,
  draws, and hands the real mask back — the same write-then-submit pattern one
  level down.
* **A sampled render target needs a layout.** `vk::Image::sampled` (set from
  `TextureUsage::TextureBinding`) makes the recorder leave it in
  `SHADER_READ_ONLY_OPTIMAL` at `Finish()`, exactly as `presentable` does for
  the swapchain. Without it the attachment ends the pass in
  `COLOR_ATTACHMENT_OPTIMAL` and ImGui's descriptor describes a layout the image
  is not in.

The portrait's light is a **fixed studio sun** independent of the world clock:
`--shot-mob`'s own comment says why — midnight is the worst possible light for a
silhouette, and a character sheet that goes unreadable at night is one you
cannot use half the time.

### Equipment is a slot TABLE, and the table is the schema

`src/game/equipment.h` holds `EquipSlots()`: one row per slot, each naming the
`ItemKind`s it accepts. That table **is** the armour system's schema. When
`ItemKind::ArmorHead` arrived the change WAS one row and no branch anywhere,
which is the claim this section made while the rows were still empty. A slot
that refuses still says why — `MoveResult::WrongKind` carries the sentence the
tooltip shows. A move is always a **swap**, never an overwrite, and validates
BOTH ends, so no mis-drop can destroy an item.

## 8c. Armour and equippables (added 2026-08-29)

### A worn piece is a set of borrowed rig slots

Wearing appends ONE RIG LIMB PER COVERED BODY PART — a **shell**: parent = the
covered limb, fixed joint, `tag: "worn"`, not vital, its own hp, its own
voxels, its own micro brick (`ItemCover` in `game/item.h`, `Mob::WearItem`).
It is the held-item trick N times instead of once, and it inherits, with no new
code: burning and dissolving, per-voxel carving, severing with the limb it is
strapped to (a cut strap drops the pauldron), dropping as debris,
live-transform hitboxes, and rendering. "Degraded armour shows the body
underneath" is automatic — the shell encloses the limb, so a hole in the cloth
IS the skin.

Rejected: folding armour voxels into the body limb's own lattice, the way
mina's robe works. Burn ordering would be free, but unequip, per-piece hp,
sever-as-a-piece, drop and persistence all become entangled bookkeeping in one
lattice. Separate slots keep one owner per fact.

Cover entries bind by **limb name**. Every humanoid rig here names its parts
the same way, so one authored helmet finds the right part on any of them and a
wearer lacking a named limb simply skips that shell — which is what makes
"goblin helmets look right on anyone" content rather than code.

**The appended tail is the one real refactor armour needed.** Shells and a held
weapon share the region past `Mob::AppendedBase()`, so removing a piece removes
a group from the MIDDLE of it. `RemoveAppendedSlots` ERASES that range and
fixes up the indices that referred past it, rather than tearing the tail down
and re-appending the survivors: an appended slot is never a parent, so nothing
can be orphaned, and — the reason that matters — a survivor's lattice is never
let go of, so removing the robe cannot mend the boots.

### Protection is geometry and materials, never a number

There is no armour class, no resist field and no damage mitigation anywhere —
with one exception, and it is a mechanism rather than a mitigation: a blunt
blow transmits `gear.bluntThrough` of itself through a shell to the limb
underneath, because a plate deforming is how the energy arrives (see *Damage
kinds: cut, blunt, bite*). The dent it leaves is real geometry like everything
else here.
Cloth burns because it IS `robe_cloth` (chance 200/1000 against skin's 90);
steel stops acid because `steel` carries no `tag:dissolvable`, so acid's rule
never matches it. The one genuinely new mechanic is **occlusion**: the burn
pass reads the world around a limb, and a shell's voxels are in neither the
grid nor the limb's lattice, so without help fire lapping at a sleeve reads to
the arm underneath exactly as fire lapping at the arm.

`Mob::WornAlong` closes that, and it returns the occluding shell's MATERIAL
rather than a bool — which is what keeps the behaviour emergent. The flesh's
neighbour simply becomes "cloth" instead of "fire"; cloth-over-flesh semantics
fall out of the ordinary authored table; and the moment the shell burns through
the probe returns nothing there and the skin is exposed. No integrity
threshold, nothing to tune.

**Asked along a SEGMENT, not at a point.** A limb is a rounded tube inside a
garment cut to its box, so on any diagonal there are several empty cells
between the flesh and the cloth: the obvious "is the cell one step outside me
inside a shell" test reads correct and leaks completely. Measured, before the
fix: a fully enclosed arm caught fire three ticks *before* the bare one beside
it.

**And it only works at the scale the grid can resolve.** An arm is 0.76 world
voxels across and its coat adds 0.24; the grid cell is one world voxel, so for
a limb thinner than a cell there is no "outside the coat" and no probe can put
one between the fire and the flesh. This is a property of a grid-coupled body,
not a bug to fix: armour occludes on the torso and on anything larger, and on a
forearm it does not. `--gate armor-react` therefore measures the torso, with a
second undressed creature as the control — and it measures 114 skin voxels lost
bare against 0 under a steel plate.

### Fit: authored at stock size, resampled per wearer

Each cover entry records the `fitBox` it was drawn against. At equip, the
wearer's own limb box divided by that box gives a per-axis rational, and the
shell lattice is resampled by nearest neighbour in integer math
(`ResampleLattice`, `game/item.h`) — the only non-uniform scale in the engine,
and the smallest one that can express "a goblin is not a small human, it is a
wide short one". A resampled shell packs its own copy-on-write brick and frees
it on unwear; at ratio 1 (the stock set on the stock human) nothing is
resampled and the def's brick is shared, exactly as a body limb shares its.

### The sheath is the weapon slot

A blade is either DRAWN (a real rig part in the fist) or STOWED (an entry in
the Sheath slot and nothing else). `Q` toggles; drawing forces the melee tool
and stowing puts the previous one back; a weapon that leaves the sheath while
drawn stops being drawn (`SheathState`, `game/equipment.h` — three cases, all
easy to get subtly wrong, so they live in one testable struct rather than in
the frame loop). The hotbar keeps the number row and stops being where a weapon
comes from.

Sheathing is still **visually** data only: the slot holds a weapon, it does not
draw it on the avatar's back. That visual is a `sheath_back` socket in the rig
plus a matching grip context on the item — `ItemGrip`'s context map
(`game/item.h`) already anticipates exactly that, so it is content, not code.

### Ground items are debris that remember their name

A dropped item IS an ordinary `DebrisSystem` body: it falls, settles, burns,
dissolves and can be blown apart, none of it written twice. The only thing
debris cannot carry is IDENTITY, and that is the whole of `WorldItems`
(`game/worlditems.h`) — body handle to item name, by name because library
indices die on every R reload. Dropping is a drag out of the character screen;
picking up is `E` and a short camera ray filtered through the registry, so a
body the registry does not know is scenery and is left alone.

The registry MUST NOT outlive the body: Jolt reuses handles, so a stale entry
would eventually re-match a new body and hand the player a sword they picked up
off a rock. `DebrisSystem::SetOnBodyGone` is the one seam that keeps the two in
step, and it means a robe that burns up on the ground is simply GONE.

### Damage persists exactly

Not a durability percentage — the holes themselves. While a piece is on, its
wounds are the shells'; the shells die with the slots, so `Mob::CaptureWorn`
reads them out one call before the rig forgets and `WearItem` puts them back.
Off the body they live in `PlayerKit::wornDamage`, keyed by ITEM NAME (not by
slot, or dragging the robe through the pack would mend it; not by instance,
because an `ItemStack` has no identity and giving stacks one is a much larger
change than armour needed). `PLYR` is at v3 for the map, `ITMS` carries a
ground item's lattice the same way, and an older payload is refused rather than
half-applied.

**Condition is a summary, not the record.** The lattice above stays the truth —
it is what puts the wear back in the right places, and on armour whose whole
mechanic is "the world reaches you through the gap" that is not a cosmetic
distinction. What v3 adds beside it is two counts per shell (voxels at spawn,
voxels live), because the character screen has to say something about a piece
sitting in the PACK, where there is no shell to measure and re-deriving the
denominator would mean re-running the per-axis fit resample against a wearer
who is not wearing it. `Mob::WornCondition` answers the same question off the
live rig and the two must agree; both are volume-weighted across a piece's
shells, so a burnt-off sleeve does not weigh as much as the body of the robe.

Measured in VOXELS rather than hp, because protection here is geometric: a
shell protects by being in the way, so how much of it is still in the way is
what its condition means. hp also falls to blunt trauma and is not what the
occlusion probe reads. Below `gear.ruinedCondition` a piece is **ruined** —
still wearable (whatever remains still covers whatever it still covers), but
that is the line a repair, and anything that scales with condition, is expected
to refuse. There is deliberately no repair and no enchantment system yet; this
is the substrate either would read.

### A shell is a rig slot, and the gore path did not know that

The borrowed-slot trick buys burning, dissolving, carving and severing for
free — and it bought the GORE path too, which was wrong in three ways at once,
all of them reported as separate bugs. A robe burning off sprayed the WEARER's
blood (`CarveLimb` topping up a drip budget on a garment), cried out in their
voice, and was handed to `DebrisSystem` as a dynamic body spawned overlapping
the wearer's own capsule — outside `Layers::AVATAR`, so resolving that overlap
fired the player across the room.

`Mob::IsWornSlot` is the missing distinction, asked by TAG (`"worn"`, the same
one `BodySlotFor` already keys the HUD off) rather than by index, so there is
one spelling of "wardrobe, not anatomy". A garment neither bleeds nor voices,
and one consumed BY FIRE is not adopted as debris at all — there is nothing
left to fall off. Rags cut loose by a blade still drop, because that is a piece
of gear hitting the floor.

**Fire cauterises**, and that half applies to real limbs too. `FlushBurn`
expresses a tick of burning as a carve and fires every `max(12, n>>6)` voxels
removed, so a limb alight carves itself dozens of times a second and each one
topped the bleed budget back up — being on fire read as haemorrhaging. Both the
drip and `Sever`'s arterial gout now refuse while `inBurnFlush_` is set. The hp
charge is deliberately outside the exclusion: fire still kills you, and a burnt
shell still loses its own durability. Only the blood is refused.
`mob-burn`'s `fire does not bleed` asserts zero droplets and zero open wounds
across 630 ticks of a creature burning to death.

### The stock set

`scripts/gen_stock_armor.py` emits hood / robe / sash / pants / boots in cloth
and leather, and iron_helm / iron_cuirass / iron_greaves / iron_sabatons in
iron. The geometry is DERIVED, not drawn: each shell is the stock human's own
silhouette dilated outward by one authored micro with the body subtracted back
off, importing `gen_human.py`'s limb table rather than restating it. So the
garment fits by construction, is strictly outside the body, and
re-proportioning the human re-proportions the coat. Colour is art-palette slots
in `.col` layers, never materials — painting with materials is what makes
mina's sash burn on a different schedule from her sleeve. The plate set is the
same builders over a different material (a helm with an eye slit for the hood,
a short straight fauld for the skirt), which is the point of deriving: a second
suit is a second row of one table.

**What a per-z dilation cannot produce, and what was done about it (2026-09-04).**
The tube is a ring per row and cannot cap anything, so every lid is authored
explicitly — and three of them were wrong in ways only a dressed figure shows:

* *Shoulders.* The torso's lid was its own top silhouette grown by one, and the
  upper arms end on the SAME row, so the top of each arm was bare skin. The yoke
  is now the torso's top row and both upper arms' top rows grown by one, on the
  TORSO shell rather than the arms': the arm's anchor is its top
  (`joint_top`), so a raised arm rotates in place under the yoke instead of
  carrying a lid off with it.
* *Neck.* The neck is three rows of the HEAD limb and the hood started above
  them, so a dressed figure showed a stub of bare neck standing in the yoke's
  hole. The torso shell now rings those rows as a collar and the hood and helm
  start where the skull starts, on a seam with it; `neck_rows` derives the
  count from the head geometry (the run of identical bottom rows) so both sides
  read one number.
* *The sash.* It was dilated from EVERY robe cell at waist height, and the
  forearms hang beside the hips at waist height, so it ringed the sleeves too:
  a band six micro wide on each side with a loop around each arm that stayed on
  the hips slot while the arm swung out of it — "the hands go inside the belt".
  It now seeds from the torso and skirt shells only, one micro proud of the
  robe, and is INTERRUPTED where an arm hangs flush against the hips, because
  there is no cell between them for a belt to pass through. At rest the arm
  covers the break. That is the honest geometry of a figure whose arms hang
  flush, and a better trade than a belt inside the sleeve (two shells in one
  cell) or around it.

The sleeves SHARE cells with the torso shell — the armpit corners and, at the
waist where the torso tapers, a whole column — and that is deliberate. Giving
those cells to one side was tried and put a stripe of bare forearm on every
walking figure: the sleeve's inner wall is what shows when the arm swings
forward and the torso's side column is what shows when it swings back, so
whichever side cedes is wrong in half the gait. Two shells of one material in
one colour coinciding at rest is invisible; z-fighting is only a defect between
things that look different, which is why the sash does subtract the robe.

**Iron, not steel.** `steel` carries no `tag:dissolvable`, so acid cannot
touch it at all — that is what a sword is made of and what `armor-react`'s
steel arm measures (0 voxels lost). A suit that acid eats SLOWLY is a different
fact, and here a fact is a material: `iron` (materials.json, id 121) is not
flammable, not organic, and carries exactly one rule of its own,
`acid + iron -> air` at 10 per-mille a tick against 250 for flesh and cloth.
The rule is APPENDED after acid's other rules rather than placed
specific-before-generic: nothing else matches iron, and a rule inserted earlier
renumbers every acid rule after it — rule index is part of the reaction RNG
stream — so the world hash does not move for a material worldgen never places.
`armor-react` arm (e) holds an iron plate in the same acid bath as the steel
one for 60 ticks and asserts it is eaten — at all, and with at least 40% left.
"At all" rather than a floor, because how much acid actually stands against a
torso plate is bath luck (2.3% in 25 ticks in one run, 0.6% in 60 in the next)
while steel's figure in the same bath is exactly zero every time; the figure is
printed beside the verdict for anyone retuning the rate. Nor is it conditioned
on the bare creature losing anything: nothing but acid removes iron, so the
count is its own evidence, whereas the steel arm needs the bare creature to
know its bath was acid — and on some terrain the bare creature stands where the
acid never pools.

### A blade chips iron; it does not carve it (2026-09-04)

The kerf `Mob::CutLimb` builds is sized for flesh, and a shell is one authored
micro thick, so a sword went through a cuirass and out the other side: the
connectivity split found two halves, and the cut-through rule took "the entire
centre piece" off in one blow. That is what a sword does to a robe; to a plate
it does the other thing armour was built for, and skates.

On a WORN slot the slot is scaled by `gear.cutHardnessRef` (8, skin's) over the
shell material's `hardness` — the same 0..255 field the blast crater and the
dig read — floored at `gear.cutHardnessMin` and at one skin cell in every
direction, so cloth (5) and leather (14) are cut about like flesh, iron (160)
takes a chip, and no strike costs a plate nothing. A suit still wears through
under a patient enemy; it takes a fight rather than a stroke, and it wears
through in HOLES, which the occlusion probe already reads as exposure. The
impact-speed knock-loose (the sword-from-the-hand rule) no longer applies to
worn slots: a strap does not snap because the blow was fast. Held blades were
never carved (the sweep skips the held slot) and flesh keeps the wound model
the `wound` gate pins. `armor-wear` 3c cuts a steel cube and a cloth cube of
identical geometry with one kerf.

**The answer to a plate is not a better edge.** Armour that could not be
answered was a wall rather than a mechanic, which is what the blunt half of
*Damage kinds: cut, blunt, bite* exists to fix: a mace beats the same shell in
over `gear.bluntDentRadius` and puts `gear.bluntThrough` of itself into the
body underneath either way.

### What leaves a body cannot launch anybody

Everything that leaves a creature — a severed limb, a cut strap's plate, a
sword knocked from a hand, a carved gobbet, a corpse's limbs, an item dropped
from the pack — is created exactly where the creature is, which for the player
means INSIDE the capsule proxy. A severed piece is also KINEMATIC for its
0.25 s hold, frozen where it was cut: an NPC's arm cut mid-swing was frozen
inside the player who cut it, on the normal contact layer, an overlap the
solver could not move, so `PlayerPushOut` moved the PLAYER a body-width a tick
for fifteen ticks. That was "I dismembered him and flew across the field", and
the avatar's own pieces had already been exempted once (`Layers::AVATAR`).

The rule now belongs to the body, not to who it came off.
`Physics::ReleaseToWorldWhenClear` puts a body on the no-player-contact layer
and remembers it; every step, each remembered body whose world AABB has left
the proxy goes back to `MOVING` and is forgotten. So a piece never shoves the
creature it came off, and the moment it has fallen clear it is ordinary debris
that can be stood on, kicked and picked up — the avatar's corpse no longer
keeps its exemption for good either. Bounded at 256; past that the oldest is
released unconditionally. The avatar's `OnBodyReleasedToWorld` override is
gone with it. NPCs were never pushed by anything: their limbs are kinematic and
they have no push-out. `player-body` asserts a block born inside the proxy
reads no push while it overlaps, and reads one again once the proxy has
walked away.

### A piece cut loose is a thing on the floor

A cut strap was `DetachLimb(adopt)` like any severed limb, which made the shell
an anonymous debris body: not the robe, nothing `E` could see, and the wearer's
slot still said "robe". Now a piece's IDENTITY shell — the same panel a dropped
copy is made of (`ItemGroundVoxels`) — takes the piece with it when it leaves
by blade: its other shells fall as rags, the `WornPiece` entry is erased, the
loss is reported through `Mob::LostGear` with the piece's damage captured one
call before the shells forget it, and `MobSystem::SetOnItemShed` registers the
body under the item's name in `WorldItems`. `main.cpp` drains the report at the
top of the tick, BEFORE the sheath and the wear loop read the kit — or those
seams would faithfully pull a second sword out of the sheath and put the
cuirass back on — clears the equip slot, and files the damage by name, so a
piece picked back up and re-worn has exactly the holes it had. A sleeve alone
is still a rag, and the piece goes on being worn without it. A sword knocked
from the hand takes the same road and the creature is unarmed from that
instant (`heldSlot_` clears in `ShedGearBeforeDetach`, not at a later
`EquipItem`). A shell consumed by fire registers nothing: there is no body.
The dead slots are swept out of the appended tail once the severed hold is
over, so `LimbCount` is not a history of what was worn. `armor-wear` 3d.

The half of this that is still open: a piece knocked off an NPC and picked up
by the player comes back as authored, because `WornDamage` is keyed by cover
index and the ground registry carries only a name and a lattice.

### Corpses keep what they fell with, and E says what it will do (2026-09-12)

`Mob::Die` hands every limb to `DebrisSystem` and the husk is swept on the next
tick, so a tick after a creature falls there was no Mob left to ask "what was it
wearing" — the robe was still THERE, a debris body jointed to the torso it fell
with, but debris carries no identity. So Die now makes a one-time
`CorpseReport` the instant before the handover, while the rig still knows which
appended slot is the robe: the bodies the corpse became (any of them under the
crosshair means "this corpse"), and one entry per piece with the body that IS
the piece (the identity shell, `IdentityShellOf`; the borrowed slot for a held
sword), its other shells as rags, and its damage from `CaptureWorn`. Delivered
through `MobSystem::SetOnCorpse` to `Corpses` (`game/corpses.h`), which is
`WorldItems`' shape over the same bodies and hangs off the same
`SetOnBodyGone` hook: a piece whose body burns off the corpse leaves the list,
a corpse with no bodies left is forgotten, and the list is capped at 64 oldest-
first. Not fired for the avatar (its kit lives in `PlayerKit` and the wear loop
re-dresses the respawn from it). Not saved: a loaded world has heaps, not
corpses, the same call the `'ITMS'` re-drop made.

**The reach ray runs every frame, not on the press.** The prompt is the
feature: `E  pick up robe` / `E  loot goblin` under the crosshair is what tells
the player the thing on the floor is a thing at all, and the ground registry is
asked first so a shed robe lying in its heap reads as the robe. Reach went from
5 to 24 voxels: the ray starts at the EYE and the avatar is 17 voxels tall, so a
thing at your feet was a body height out of reach. Two things the ray must NOT
do, both measured with a walking rig on 2026-09-12 (a fly-mode harness has no
rig and shows neither): hit the player's OWN HEAD — the eye is inside the head
collider and Jolt reports a convex shape the ray starts in at fraction 0, so
every cast answered "your head" and E was dead — and miss the crosshair in
third person, where the camera is on a boom metres behind the head and a ray
from the head along the camera's forward runs parallel to the crosshair line
but metres off it (16 of 16 corpse bodies missed). So the cast skips the
avatar's live limb bodies (`Physics::CastRayBody` with an ignore list) and
starts at the RENDER eye, with a hit counting only if the point it lands on is
within reach of the head. It is the one ray that reads the camera: a UI query
against Jolt bodies, not a sim input, so the "picking rays use `player.EyePos`
so the camera cannot change what the sim sees" contract does not apply to it.
E over a corpse opens the
character screen with a LOOT panel where the grimoire sits (wide) or the
arsenal (narrow), one `KitSlotUI` per piece through the same mirror, and
`KitSpace::Loot` is one more address the same drag can name. `TakeCorpseLoot`
executes it beside `PlayerKit::Move` rather than inside it — a corpse is not
one of the player's containers and has no `ItemStack` to swap — with Move's
discipline: `EquipSlotAccepts` on an equip destination, a sentence per refusal,
an occupied destination sends its stack to the pack rather than overwriting,
a full pack refuses with the piece still on the corpse. On success the
identity body and rags are destroyed (the robe visibly leaves the heap), and
the damage is filed by name with the identity shell's lattice re-read off the
body as it is NOW, so a robe that went on burning after its wearer died comes
off burnt. Right-click takes into the pack; a drag onto an equip slot wears it;
a drag OUT of the panel is `ShedCorpseLoot` — the piece comes off the corpse
and its body is registered as a ground item, nothing created. A drag INTO the
panel is refused: a thing put on a corpse would be data with no body. The panel
closes with the screen, on its button, when the corpse is gone, or past 40
voxels from any of its bodies. The HUD now also draws a fresh `kitMessage`
under the prompt, so "picked up" is seen in play rather than only on the
character screen. `--gate loot` (42 checks, no window); `--shot-inventory`'s
fourth frame is a dressed human killed and opened.

### Cross-limb heat respects the coat

`BuildCrossLimbHeat` lets a burning limb warm its siblings through faces where
the grid holds air — and that is exactly where `WornAlong` was never asked,
because the probe ran only against a THREAT in the grid. A burning bare hand
lit the wrist inside its sleeve, and the fire walked up the arm under the
plate: "the character is on fire inside the armour". The probe now runs on
cross faces too, with the same march and reach; the sibling's flame reads as
the shell's material, which is not hot, and whether the shell catches from it
is the shell's own pass's business. What can still catch is what the grid can
see: the face behind a helm's eye slit, a bare hand, and the wrist opening of a
sleeve along the arm's own axis.

### Mirror in, intent out

`ui/inventory_ui.cpp` is a drawing function. It reads `UIState`'s mirrors and
writes `UIState`'s one-shot intent latches; it never touches an `Inventory`, an
`Equipment`, a `GlyphInventory` or the avatar. `main.cpp` consumes the latch,
calls the real method, and next frame's mirror shows the result. The one address
both sides speak is `KitRef` (`game/kitref.h`) — a dependency-free header
precisely so the UI can name a slot without pulling the item system in, and so
there is one definition rather than two that must agree.

Binding a glyph in the arsenal rewires the live number row, because the panel
and the keys read one mirror.

### Everything crosses by NAME

Item and glyph slots hold indices into `ItemLibrary::items` /
`GlyphLibrary::glyphs`, both **file-order dependent** and renumbered by any edit
to `items.json` or `glyphs.json`. So every crossing — the `R` hot reload, the
`PLYR` save section — snapshots names and re-resolves them. A name that no
longer resolves empties the slot with a log line: content legitimately
disappears between saves, and that is the contract, not a failure.

This closed two live bugs as well as preventing new ones: the hot-reload path
carried a comment promising the hotbar was "re-validated below" and there was no
such code, and `GrantAllAndBind` threw away the player's spell bindings on every
`R`.

### Persistence: `entities.sve` section `PLYR` v1

Registered in `game/persist.cpp` beside `DBRS`/`MOBS`/`AVTR`. A bundle of
references rather than a system with its own `SaveState`, because no single
object owns all of it — the hotbar predates the pack and the caster is
deliberately separate from the player. Length-prefixed strings with explicit
counts, so a build whose `kItemSlots` or `Bag::kSlots` has changed reads an older
file correctly instead of walking off the end. A truncated or unknown-version
payload is REFUSED, not half-applied.

### Verification

`--gate player-kit` (`src/test/selftest_playerkit.cpp`) is CPU-only and builds
its own two-item / three-glyph fixtures, so it asserts on the RULES rather than
on today's content: the refusal matrix in both directions, swap-never-overwrite,
binding by ownership, name re-resolution across a simulated library reorder, and
a `PLYR` round trip compared **by name**. It runs in milliseconds and needs no
GPU, which is what makes it the whole iteration loop for the equipment model.

`--shot-inventory` runs the windowed game, damages the avatar on a fixed
schedule and writes two frames — `screenshot_inventory.bmp` (gear) and
`screenshot_inventory_health.bmp` (inspector). The screen's job is to be looked
at, so the harness that judges it produces a picture; it prints the image's
pixel sum, because "wrote the file" is true of an all-black rectangle too.

### The commoner wardrobe: nine patterns times any colour (added 2026-09-15; `game/dye.h`, `scripts/gen_peasant_clothes.py`, gate `dye`)

> **The art is a PATTERN. The colour arrives at runtime.** A village needs forty
> outfits; authoring forty is not the answer and authoring one and accepting
> that everybody matches is not either.

`assets/items` ships nine plain garments — `tunic` / `smock` / `jerkin`,
`trousers` / `breeches` / `hose`, `shoes` / `clogs` / `footwraps` — built by the
same derived-geometry rules as the wizard's kit (imported from
`gen_stock_armor.py`, never restated) and differing from it in exactly one way:
**they are painted in greyscale, and every cell's grey is a MULTIPLIER rather
than a pigment.** A cell at 0.70 renders as exactly the colour the player
picked; the weave noise, the hems, the lacing and the mends are all ratios of
it and therefore survive being recoloured. Nine silhouettes times a continuous
colour is the whole variety budget.

**Where the colour lives: `MicroBodyInstGpu`'s fourth word.** It was padding,
it is already uploaded, and the struct must stay 16 bytes (its `static_assert`
is the only mechanical guard the CPU/WGSL pair has, because
`check_invariants.py` cannot see a hand-written mirror). There are now no spare
words; the next per-instance value needs a second buffer.

The chain is `ItemStack::dye` → `Mob::WearItem` → `MobLimb::dye` →
`AppendMicroInsts` → `microbody.wgsl`, where albedo becomes
`dye * luma(art) / DYE_REF`. Four consequences fall out of that placement
and all four are the reason for it:

* **Two hundred differently-dressed villagers share nine bricks.** Nothing is
  cloned, nothing is re-packed, and the cost of a dye is one word per instance.
* **The colour is CONTINUOUS.** The rejected alternative — allocating art
  palette slots per dye and baking it into the voxels — costs nothing at shade
  time and has a hard ceiling: the merged art palette is 255 entries *total*
  across every prefab in the game (`kArtPaletteSlotsGpu`), shared with every
  creature's skin, so a six-tone ramp buys about forty simultaneous colours.
* **A severed sleeve keeps its colour.** The dye rides `MicroBodyRef`, which is
  a render description and is what `DebrisSystem::AdoptBody` already takes — so
  the hand-off path needed no new argument.
* **A dye is PAINT, not a material.** Dyed linen burns, tears, dissolves and
  soaks blood exactly as undyed linen does. Nothing in the sim reads the word
  and it is never hashed (rule 1). The stain is applied *after* the dye, because
  blood goes on the cloth rather than being scaled by it.

**Luminance, not a per-channel multiply.** The two are identical on the
greyscale art this exists for. They differ on art that is *not* greyscale, where
per-channel tints (the wizard's robe stays black, its gold trim goes muddy) and
luminance re-colours outright — which is what somebody dyeing a thing expects,
and which makes a dye applied to an un-dyeable piece merely wrong rather than
invisible. `ItemDef::dyeable` (authored in the sidecar, beside the art that
makes it true) is what the wardrobe UI offers a colour for; it is a fact about
the art, not a permission.

**THE REFERENCE TONE IS STATED THREE TIMES.** `kDyeRef` (`game/dye.h`, the UI),
`DYE_REF` (`microbody.wgsl`, the GPU) and `DYE_REF_GREY`
(`gen_peasant_clothes.py`, what the art is painted at). Disagreement is not a
crash — it is every dyed garment in the game coming out uniformly too bright or
too dark with the cause two files away — so `--gate dye` parses the shader and
compares, and the generator asserts against its own ramp.

**Authoring surface.** The Wardrobe panel (mob tool → `Wardrobe...`) is a
colour wheel and three combos mirrored off the live item library, so a tenth
pattern is a generator run and an `R`, with no C++ edit. `--shot-mob
human:+tunic#B4472A,+trousers#3A5470` photographs an outfit headlessly, which is
the only way the *picture* — the one thing no CPU assertion can see — gets
judged.

`--gate dye` covers the rest: the constant across the seam, the packing
(including black, which is why there is a flag bit at all), that every painted
cell of every dyeable piece is a grey against the MERGED palette the renderer
actually reads, that three patterns exist per slot, that the dye reaches every
shell's GPU instance and nothing else's, that a stack is one colour, and that
the word survives `PLYR` v5.

## 9d. Biomes and water-body presets — the Environment tab (added 2026-09-01)

> **A biome SELECTS from component libraries and says how often and where.
> Species and water presets are edited once, in their library; a biome edits
> the row.** Plan and research: `docs/PLAN_biomes.md`.

### The shape of the thing

Three kinds of file, one owner per fact:

| File | Owns | Edited in |
|---|---|---|
| `assets/trees/<species>.json` (+ `.svtree`) | what a tree LOOKS like; what ground it physically tolerates (altitude band, slope, shade) | Environment → Trees |
| `assets/water/<preset>.json` | the SHAPE of a body of water: footprint, bathymetry curve, fill, berm, bed, shore + aquatic vegetation by depth; its default tile/rarity | Environment → Water bodies |
| `assets/biomes/<biome>.json` | WHICH species and presets appear in the biome, at what weight / rarity, under what extra conditions; ground cover; cave bands; terrain overrides; climate coordinates | Environment → *biome* |

Every feature row of every stack carries the same **placement chain**: a rarity
(a weight for trees, 1-in-N tiles for water, 1-in-N columns for cover — one
form authored, the others shown read-only beside it: percent, per hectare, per
km²) and **conditions** (`minY`, `maxY`, `maxSlope` in Q8, `nearWaterMin/Max`
in metres, `patchThreshold`). This is Minecraft's placed-feature modifier list,
the one model modders already read (PLAN_biomes.md §2 has the survey).

### What is live and what is scaffold — stated where the user can see it

* **LIVE (world map P1, 2026-09-04): the biome RECORD TABLE.** `worldmap.cpp`
  packs every `assets/biomes/*.json` into the `worldMap` storage buffer
  (binding 31, both sim layouts; `src/sim/worldmap.h` is the layout) and
  `worldgen.wgsl` reads it through `wmBiome()/wmCover()` for: the ground skin
  and its depth, the wedge's topsoil (`cover.skin/skinDepth/subsoil`), the
  tree spacing and density (`trees.tile` + `trees.density`, P-D of
  `PLAN_environment_truth.md`, 2026-09-05: worldgen scans ONE lattice, the
  FINEST tile among the biomes that grow trees, and thins each biome on it to
  `density × (T/tile)²` in Q16 — `kB_TreeChanceQ16` — so trees per hectare
  are the page's number by construction; `TREE_TILE`/`TREE_SCAN`/
  `TREE_CAND_MAX` are load-time prelude constants from `treeatlas.h
  TreeLatticeFor`, `worldgen.treeTile` is gone, and `LoadTreeAtlas` refuses a
  crown wider than the 5x5 candidate cap on that lattice), the per-row
  `conditions` on tree and cover rows (`minY`/`maxY`/`maxSlope`/
  `nearWaterMax`/`nearWaterMin`/`patchThreshold`, packed per (biome, species)
  into the atlas's condition table and onto the cover row; `nearWater` is
  `waterDistAt`, a pond-rim distance with the row's own band), the per-biome
  cover stack (`cover.plants[]`, rolled in order, first hit wins, one hash
  salt per row, patch-masked through `vnoise2d`), the cave thresholds
  (`caves.features` near_surface/deep), and three flags that replaced the
  hard-coded `biome == B_DESERT/B_PINE` gates — `cover.groundFlora` (the
  canopy-inverted undergrowth + flower layer), `cover.cacti`, `cover.sandCap`.
  The desert tussock/scrub and pine heath floors that were shader blocks with
  five `worldgen.*` knobs are now rows in `desert.json`/`pine.json`; the four
  `treeChance*` knobs are gone. The alpine-cushion snowline block is still in
  the shader (it is an ALTITUDE rule, not a biome's; it moves when the
  landform plane lands, P4). A cover row's `maxSlope` is packed but not yet
  enforced (P4 gives `Col` a slope).
* **LIVE: tree species and weights, WITHOUT the bake.** `LoadTreeAtlas` takes
  the biome set and builds the per-biome weight table from each biome file's
  `trees.species[]` by name; the `.svtree`'s baked weight words (12..15) are
  no longer read, so a weight edit reaches the world on the next launch and
  `placement.biomes` in a species file is informational. **The biome id space
  is the files:** `index` values must be exactly 0..N-1 (`ValidateBiomeSet`,
  `check_invariants.py biome order`); the tree atlas and the record table are
  laid out in that order and the shader reads the count from each header.
  Eight biomes ship: forest, meadow, pine, desert, tundra, swamp, alpine,
  ocean. `worldgen.wgsl` still names the first four by id (`B_*`) as the
  (identity, folded-out) height curve's input until P4.
* **LIVE (world map P2a, 2026-09-04): THE BIOME COMES FROM THE PAINTED MAP.**
  `assets/worldmap/<world.mapLayer>/` — `map.json` (cell size, extent,
  origin cell, sea level, ocean fade, warp amplitude, the palette of biome
  NAMES, sites, rules) beside `map.svmap` (three u8 planes: biome, landform,
  moisture; four cells per word on the GPU). `LoadWorldMap` resolves the
  palette by name against the biome files and `PackWorldMap` appends header
  + planes to the same buffer as the records; `mapBiomeAt` in
  `worldgen.wgsl` is `biomeAt` now. **Tier A is seed-independent** (the
  plane), **Tier B takes the seed** (the boundary warp, two `vnoise2d`
  samples, amplitude ≤ cell/4 — the loader refuses more, so a one-cell
  region can never pinch below two tree tiles). Outside the planes the biome
  is `ocean`. `World::MapBiomeAt` is the CPU twin; the `worldmap` gate holds
  it to the plane at cell centres and to the GPU's ground skin in-window;
  `check_invariants.py` (`world map layout`) holds `worldmap.h`'s word
  offsets to the shader's `WM_*` consts. A missing or invalid map REFUSES to
  start. The shipped `default` is `scripts/seed_worldmap.py`'s 20 km
  starting layout (tundra north, desert east, alpine NW, swamp SE, ocean
  ring past ~9 km, forest forced around the origin for the fixtures) — a
  starting point for the World Map tab (P3), not a generator: the map is
  authored data and every edit to it moves the world hash.
* **DELETED (world map P2b, 2026-09-04): the hand-coded origin set pieces.**
  The wood deck, the combat arena (+ its ivy, ramp, screenshots), the oil
  pond and the lava pool, the per-tile ruin scatter (+ pad, shell, moss,
  ivy, the `ruin*`/`wallIvyDensity`/`mossFace` knobs and the terrain gate's
  A8), `inSpawnClearing`, `onFixturePad` and `pondInfo`'s literal keep-out
  box and discs are gone from `worldgen.wgsl`, `world.cpp` and `main.cpp`.
  What survives is ONE authored site read from the map: the **harness pad**
  (`map.json` `sites[]`, kind `pad`, a world-voxel box; `WM_H_HARNESS_*` in
  the buffer header, `inHarness`/`crownMeetsHarness` in the shader,
  `World::InHarness` on the CPU) that keeps the fixture columns clear of
  trunks, crowns, tarns and cover, and the **harness tarn** at (420,420) —
  the one authored pool left, because the `waterbody` gate's `Basin(1)`,
  the water screenshots and `perfsuite` all read it
  (`World::kAuthoredPools` is 1). `landColumn` is now `landColumnBare`
  with no pad to blend in. Fixture columns no longer get a loose sand cap:
  they stand on their biome's skin like everything else — DESIGN §6 already
  said the `armor-react`-style fix is a levelled pad, which P5's site table
  gives every site. `RESEARCH_worldgen` §8.2's "the first landmark should
  introduce a proper table" is P5.
* **LIVE (world map P3, 2026-09-04): the World map page.** Environment →
  World map (`assets/editor/map.js`) paints the biome plane (palette = the
  biome files), the landform plane (0..255, soft brush) and the harness pad
  box, with pan/zoom, stroke undo and a cell/world readout, and saves both
  files through `/api/worldmap` + `/api/worldmap/planes`
  (`scripts/tuner_server.py`, bare names, format-checked, write-then-
  rename). The page shows the planes as painted; its preview pane (the old
  Worldgen tab's heightmap/voxel views, P-I) shows what worldgen makes of
  them. Every save moves the world hash.
* **LIVE (environment truth P-A, 2026-09-04): THE ENVIRONMENT HOT-RELOADS,
  and the game says what it was generated from.** `ReloadEnvironment`
  (`test/support.cpp`) re-reads the biome files, the map named by
  `world.mapLayer` and the tree atlas, validates them exactly as boot
  does, and pushes them through `Simulation::UploadEnvironment` (a table
  that grew gets a new buffer and the two sim bind groups are rebuilt) and
  `worldmap::SetCurrentWorldMap` for the CPU twins. A refusal keeps the old
  tables and names the file. Callers: **F7**, the overlay's "reload
  environment + regen world", `--voxserve RELOAD`, and the Environment
  tab's **Apply to game** over the telemetry socket (`{"cmd":
  "apply-environment"}`; `Telemetry` now reads client frames). Boot and
  every reload print `environment: map <name> <hash> | biomes <hash> |
  trees <hash>` (`biomes::StampEnvironment`, FNV-1a over the files, mirrored
  by `tuner_server.py /api/environment/hashes`), and the tab shows whether
  the running game is behind the disk. Gate: `env-reload`. Plan:
  `docs/PLAN_environment_truth.md`.
* **LIVE (environment truth P-C, 2026-09-04): SPAWN IS A SITE ON THE MAP.**
  The player used to start at a literal (140, 140) — inside the harness pad,
  which refuses every trunk, crown, tarn and cover for the fixtures' sake, so
  the first thing seen was 77 m of bare grass whatever the biome said.
  `map.json sites[]` now carries `{kind: "spawn", at: [x, z]}` (one per map;
  the loader refuses two, and a map without one defaults to (140, 140) and
  says so), packed into header words `kHSpawnX/Z` (`WM_H_SPAWN_X/Z`).
  `main.cpp` boots and regenerates (F7) from `CurrentWorldMap().spawnX/Z`
  and centres the residency window on it before the boot worldgen; the
  selftest's player proxies keep their literals — they are fixtures on the
  pad. The calm home area (`spawnPlain*`) centres on the spawn through
  `spawnCentre()` inside the height mirror, with a second fade from the pad
  box's edge so the pad's ground does not move (the paragraph under
  "Derivative attenuation" above). The default map's spawn is (900, 900):
  260 voxels past the pad's edge (more than the widest crown reach, 115), in
  the forced-forest cells, ground y≈200 over a sea at y112. The World map
  page draws the spawn diamond and has a `Spawn` tool that moves it by
  click. Gate: `spawn-site` (authored, outside the box and every site cell,
  above the sea, not under a tarn, not ocean). `spawnPlain*` stays in
  `tuning.json` until P-G moves it to `map.json terrain.homeArea`.
* **LIVE (world map P4, 2026-09-04): THE LANDFORM PLANE OWNS THE CONTINENTAL
  RUNG, and the sea is a plane.** `landAt`'s `o0` is `landformOctave(x, z)`
  on both mirrors: `dev = ((mapLandformQ8 - 32768) * contAmplitude) >> 16`,
  so `worldgen.contAmplitude` still says how tall the world is (one landform
  unit = amplitude/256 voxels, 4 at the default) and the map says WHERE;
  the gradient feeding the finer rungs' domain warp is the cell-to-cell
  difference of the plane. `mapLandformQ8` is a Q8 bilinear over the four
  cells around the column (cell value = its centre), clamped at the plane's
  edge and faded to 0 over `oceanFadeCells` beyond it; it and `seaLevelY()`
  live OUTSIDE the height mirror in both languages and the mirrored code
  calls them by name (the `terrain` gate's C1 is the per-voxel proof). The
  range/hill/detail/grain octaves and the spawn-plain fade are unchanged.
  **The sea**: `landColumnBare` fills `fluid = water, fluidTop = seaLevelY`
  wherever ground is under the map's `seaLevelY` (one global plane,
  `RESEARCH_worldgen` §6.5 option (a)); the sediment wedge is zero under
  it; `TerrainColumn` reports the sea as water. Landform units are coarse
  against the fine octaves' ±84-voxel dip, so a painted map has to keep
  land ≥ ~132 above a sea level of 112 — the default map does; the World
  map page's landform brush is where that is authored. `LAVA_LID` is not
  needed yet: no biome record carves caves under the sea (ocean's cave
  thresholds are 255) and lava only pools in caves.
* **LIVE (world map P5, 2026-09-04): THE SITE TABLE.** `map.json sites[]`
  of kind `stamp` (a `.vox` from `assets/prefabs/`, a world column, a
  rotation, a pad margin) and `rules[]` (`{kind: stamp, template, biome,
  perKm2, minSpacing, rot|-1, padMargin, salt}` — resolved at load with the
  world seed, integer hash per cell in row-major order, greedy spacing, so
  the same seed places the same sites everywhere; Tier B). The loader packs
  each (template, rotation) once into columns of runs in the tree atlas's
  encoding, writes one `kS_*` record per site and a per-cell SITE INDEX
  plane (`site id + 1`). In the shader `wmSiteAt` is one plane read per
  column; **`sitePadAt`** (inside the height mirror, identical in
  `world.cpp`) levels the ground under the footprint to the site centre's
  height and ramps back over the margin — `ruinPad` generalised, and
  `World::TerrainHeight` applies it too, so the height contract holds;
  **`wmStampCell`** overlays the template's runs above the pad in
  `genCellIn` (non-air replaces, air leaves the world alone) — pure
  worldgen, so the far cascades show a stamped building at any distance
  with nothing to patch, and the sky early-out / far blocker band include
  `wmSiteTopAt`; **`siteKeepOut`** (the harness box or any site cell)
  suppresses trunks, tarns and cover. A missing template refuses to start.
  The World map page places/deletes stamp sites (`Stamp site` tool).
  `assets/prefabs/` ships no `.vox` yet, so the default map has none; the
  first authored one exercises the whole path. Not yet: `proc:` kinds
  (the ruin shell is gone; a generator per kind is the follow-up plan),
  slope-gated rules, sites larger than 512 voxels a side.
* **THE LIVE MANIFEST** (`assets/editor/envlive.js`, PLAN_environment_truth
  P-B, 2026-09-05). One table says, per JSON path of a biome file, a water
  preset and the map, `{read: true}` or `{read: false, package, why}`; every
  row builder goes through `envui.liveMark`, so an unread field renders
  DISABLED (greyed, not hidden, tooltip = the package that reads it), and
  `test_environment.mjs` §7 walks `BG.defaultBiome()` / `defaultRows()`,
  `WG.defaultParams()` and `map.json` and fails on a field the manifest does
  not list — no third state. The truth is derived from `PackBiomeTable` +
  `worldgen.wgsl`, never from the plan. Read today: skin / subsoil / depth,
  patch mask, the three flags, cover rows with `minY` / `maxY` /
  `patchThreshold`, `trees.density`, species weights, cave thresholds. Not
  read: `trees.tile` and every other `conditions` field (P-D), the water
  rows and every preset field (P-F geometry, P-E vegetation),
  `terrain.overrides` (P-G), climate and the moisture plane (later). The
  trees/ha stat uses the ENGINE tile (`worldgen.treeTile`).
* **THE BAND STRIP IS GONE** (P-G, 2026-09-07): `biomeAt` is `mapBiomeAt`,
  and the five knobs it drew (`meadow/pine/desertThreshold`, `biomeLog2`,
  `biomeBlend`) are deleted with the rest of `worldgen.*`; the biome page's
  Relief section (the curve + multipliers) took its place.
* The `biomes` gate (`src/sim/biomes.*`, `selftest_biomes.cpp`) loads every
  file and refuses an unknown species, preset or material, a biome `index`
  that is not worldgen's id for its name, a stale species mirror, a preset
  whose berm exceeds its shore lift. The swatch on the biome page composes
  all of it, read or not.

### The generators are the preview AND the future truth

`assets/editor/watergen.js` and `biomegen.js` are pure modules (no DOM, hash
RNG keyed on the column, Node-runnable) in the treegen.js mould. A water body
is a superellipse footprint (`squareness` is the Lamé exponent) with an fBm
domain warp, optional smooth-min'd lobes and subtracted islands; a **depth
profile curve** over the normalised radius (monotone cubic through authored
points — parabola, bathtub, littoral shelf, cone and flat are the presets);
a fill material and level; the engine's structural berm; bed materials by
depth; a shore band with distance-ordered plants; and emergent / floating /
submerged bands by water depth. Band WIDTHS are consequences of the
bathymetry, not knobs — a steep kettle gets a one-cell reed fringe, a marsh is
all fringe — which is what limnology says and what keeps the parameter count
sane. `columnAt()` is the one answer to "what is at (x, z) of this body"; the
standalone preview and the biome swatch both call it.

The engine's pond today is still `pondAt`'s parabolic disc, inside the
CPU-mirrored `height` block. PLAN_biomes.md §5 orders the wiring seams by
risk: cover blocks and `treeInfoAt` chances first (outside every mirror), then
`biomeAt` reading a table, then pond geometry last (it needs a C++ twin of the
table under the mirror's token compare).

### The swatch is a scale ladder, composed off the main thread (2026-09-01)

The biome page's swatch runs from 16 m to 128 m on a side. Up to 32 m it is
1:1 with the engine (10 vpm). Past that it does NOT get a bigger grid: it
bakes COARSER, at the finest INTEGER vpm that keeps the side inside
`MAX_SWATCH` = 384 cells (`biomegen.swatchScale`: 8 vpm at 48 m, 6 at 64,
4 at 96, 3 at 128), and the viewer draws the one region at `lod = 10 / vpm`
so it lands in world metres. The reason is the dense volume: a swatch is one
`nx*ny*nz` Uint16 array copied twice more on the way to the screen (the
viewer's cells, the mesher's lent copy) and once into a 3D texture, and a
96 m forest at 10 vpm would be 16x the 24 m swatch's 32 MB per copy — over
2 GB in flight before the mesher ran. Integer vpm because treegen bakes at
integer vpm only, so every species goes through the real generator at the
ground's scale and the composition stays exact rather than resampled; the
tree cache is keyed on vpm, so the first swatch at a new scale bakes every
species it places (seconds), reported as progress, and later ones do not.

Composition runs in `assets/editor/swatch_worker.js`, a module worker that
owns the libraries and the tree cache and returns the cells as one
transferred buffer already remapped to engine material ids
(`biomegen.remapToMaterials`). One request in flight; a request made while
one runs waits as the single queued one, so a slider drag costs at most one
extra compose and the page never stops answering. If the worker cannot be
built the page composes inline as it did before.

The viewer side is `worldview.js`'s greedy mesher, rewritten the same day:
occupancy through a 4096-entry LUT built once per palette, both draw layers
in one sweep, each plane read from the volume once (the x and y sweeps are
strided gathers), the merge bounded to the rows and columns that hold a
face, quads into growable typed arrays. Byte-identical quads to the first
mesher, with and without neighbour slabs, at 5–6x the speed (a 16M-cell
swatch: 2.2–3.6 s → ~0.45 s). Vertex positions are Uint16 now; the first
mesher packed them as bytes, which folded every region past 255 cells on a
side back over itself — the 320-cell 32 m swatch and any water body wider
than 25 m drew wrong, with no error anywhere.

### Verify

`node scripts/test_environment.mjs` (data: determinism of both generators,
preset sanity, biome validity, the species mirror, the swatch, the scale
ladder and the in-place remap),
`bash scripts/check_environment.sh` (the tab in real Chrome: mount, sidebar,
three pages, WebGL meshing, framebuffer, plan/profile canvases, undo, deep
links, save routes), `--selftest --gate biomes` (the engine's side),
`python scripts/check_invariants.py` (biome order). `check_tabs.sh` and
`check_trees.sh` still pass: the tree editor mounts into a `div#view-trees`
inside the Environment section, without class `view`.

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

### The performance suite (added 2026-08-30; `measure/perfsuite.*`, `measure/perfnodes.h`, `assets/perfview.js`)

"Instrument from day one" above was satisfied by an on-screen overlay and two
harnesses that each answer half a question. `--measure` reports per-pass GPU time
averaged over three synthetic scenarios, blocking after every tick — so its wall
clock is a latency, not a frame rate, and it says so. `--selftest` reports one
advisory render number. Neither answers the question anyone actually has, which
is **"where did THIS frame go, and which part of the engine do I fix?"**

`--perf` answers it, and the tuner's **Performance** tab draws the answer.

**The taxonomy is shared with the architecture map.** `measure/perfnodes.h` is
one table mapping every `PASS()` row in `pass_table.def` and every CPU scope the
frame loop opens onto an `ARCH_NODES` key from `assets/tuner.html` — so a bar on
the Performance tab is the same component as a box on the Engine tab, by
construction. `scripts/check_invariants.py` (`perfnodes`) fails the build if a
node here is not a box there, if a claimed pass is not a real row, if two nodes
claim one pass, or **if any timed row is claimed by nobody**. That last one is
the important one and nothing else would catch it: an unattributed dispatch is
GPU time that silently disappears from the page, and the bars still agree with
each other while quietly ceasing to agree with the frame.

**Five scenarios, each on its own world.** idle (rule 2's floor), a 30-second
canopy fire on the tallest real worldgen tree near the window, a streaming
flythrough, an explosion storm, and a draining authored lake. Every one
regenerates and re-settles the world and reloads the residency window first, so
any subset produces the same numbers as the full run — which is the only
property that makes `--scenario <id>` useful for iterating.

**Three measurement problems had to be solved before any of it meant anything,
and each produced a plausible wrong number first. They are recorded because the
wrong numbers all looked fine:**

1. **Granularity.** `prep(mutate+explode+compact)` is ONE pass group spanning
   THREE architecture components, so a page keyed on groups cannot answer "is it
   the mutation queue or the explosion". `PassTimer::SetRowGranularity` moves the
   timestamp pair onto the individual row. A timestamp brackets different
   dispatches but reorders none — and rather than leave that as an argument, the
   harness refuses to start unless an untimed and a row-timed 60-tick run produce
   the same world hash.
2. **Pacing.** With no bound on frames in flight the CPU races ahead of the GPU
   and the measured "distribution" is a queue depth: measured p50 **0.64 ms** /
   p95 **196.56 ms** over 900 frames, which is not a frame-time distribution at
   all. A 3-deep fence ring — what a swapchain does — turned that into p50 39.07
   / p95 43.02.
3. **Independence.** Scenarios shared one `World`. The flythrough left the
   residency window 900 voxels away, and the lake scenario then reported "no
   pond in the window" about a world that has one. `Stream::ReloadWindow` per
   scenario; `SetWindowOrigin` alone is not enough, because the origin is
   `World`'s and the residency is `Stream`'s.

**CPU BUSY and CPU WAITING are never summed.** The `present` scope is the CPU
parked in front of a full frame queue. It is not a cost, it is the *shape* of the
bottleneck: large means the GPU is the limit and the CPU has headroom. Adding it
to a "total CPU" bar is the fastest way to read this page backwards.

**Live mode is the same charts.** `--telemetry` broadcasts one `PerfSample` per
rendered frame — the same struct `--perf` records — so the page draws a live
session and a recorded run with the same code. GPU pass times come back through
`PassTimer::KickDeferred`/`PollDeferred`, a fence ring that lets the numbers
arrive two or three frames late **tagged with the frame that produced them**;
the frame path never waits on a timestamp, and a frame whose queries have not
landed is marked rather than drawn as a GPU that cost nothing.

**The render pass is seven spans, and the raymarch has an inside (2026-09-01).**
The whole render pass used to be ONE timestamp pair billed to `raymarch`, which
on the idle scenario is the entire GPU frame — a bare count. The frame loop now
writes a pair around each draw inside the dynamic-rendering scope (legal; only
the query reset and resolve must stay outside) and bills them through
`kPerfRenderSpans` in `perfnodes.h` to the boxes that issue them: `raymarch`
(the fullscreen shader alone), `drawParticles`, `drawBodies`, `drawMicro`,
`drawSprites`, `drawDebug`, `uiOverlay`. Both ends of a span are ALL_COMMANDS
stamps, because draws overlap in the pipeline and a top-of-pipe start can land
before the previous draw's fragments retire. The shadow-cache resolve runs before
the pass and is timed by its own table rows, so it is no longer counted twice.

Inside the world draw nothing can be timestamped — it is one fragment shader — so
`raymarch.wgsl` counts instead. Under the `RENDER_STATS` prelude const
(`gpu/resources.h`, on only for `--telemetry`) every trace call site adds its DDA
steps and every shading path its pixels into `World::renderStats`, a 4 KiB
striped `atomic<u32>` buffer (`world.h kRenderStat*`), on a 1-in-16 pixel
sample; the shipping shader compiles all of it out, because `trace()` is
register-footprint bound and a live flag would keep the accumulators resident.
The counters are MONOTONIC and differenced on the CPU (`measure/renderstats.h`),
which keeps a clear and its barrier out of the frame. They reach the page as the
`rm*` counters and the tab draws them as an ESTIMATED millisecond split of the
raymarch span (step share x span) with the estimate labelled as one; the exact
per-feature answer stays `--render-budget`. The copy-out goes through
`rhi::CommandEncoder::CopyRenderWritten`, the one place a render-domain write is
declared to the barrier tracker (`barrier_graph` §2.6 otherwise assumes draws are
read-only): it sets the buffer's last writer to the fragment stage and derives
the fragment->transfer barrier from that, exactly as `CopyTracked` derives its
own.

**`readback` and `readbackStall` are two rows (2026-09-01).** The async readback
row used to hold both the non-blocking map pump and the paged mirror's staleness
fallback in `SubmitTick` — a fence wait on the oldest in-flight readback when the
newest snapshot is more than `kPagedSnapshotMaxGap` ticks old. After a lake was
disturbed the row read as "async readback spiking for a long time"; the pump
never blocks, and what spiked was the fallback, firing every tick while the GPU
was more than a frame behind and the loop was catching up at 4 ticks/frame. That
is a relabelled GPU wait — the CPU would have spent the same time in `present` —
so it now has its own row and its own two counters: `snapshotStalls` (the waits)
and `readbackDeclined` (readback requests the ring refused). Declines
without stalls mean the ring is the limit; stalls with declines mean the GPU is.

**The GPU-lag throttle (2026-09-09).** The 4-tick catch-up is right for a CPU
hitch and wrong for a GPU one: every tick submits a plane of worldgen, a CA
pass and a snapshot copy, so paying three of them into a queue already a frame
behind lengthens the next frame, which owes more ticks — the loop that ends in
the two blocking waits on the frame path (the staleness fence above, and
`Stream`'s T+`kWakeLatency` wake fence, billed to World Storage). The tick loop
now reads the lag instead of guessing it — `rhi::Device::PendingMapCount()`,
the snapshot readbacks whose fence has not signalled, is exactly the ticks the
GPU has not finished — and runs a second tick in a frame only while the GPU
owes fewer than two, dropping the surplus debt (sim time dilates by those
ticks) rather than banking it into a burst. Pure pacing: which ticks run and
what they compute is unchanged, nothing hashed moves, and the headless
harnesses never reach the branch. `--frames` prints the count as `gpu-lag
throttle`; the live page shows it as `ticksThisFrame` sticking at 1.

**Verify the page, not just the numbers.** `scripts/check_perfview.sh` drives the
real tab in real headless Chrome and asserts both content (charts built from the
data, passes attributed, nothing unattributed) and layout (nothing overflows its
column, no zero-height chart) — because none of the numbers being right makes the
page readable, and a chart 300 px wider than its column is invisible to every
assertion about its DOM.

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

### The ears are on the character, not on the camera (2026-09-04)

`ListenerPose` is published once a frame from **`Player::ViewEyePos()`** —
the head, at ear height — with the orientation taken from `Camera`'s look
direction. That split is deliberate and each half was once wrong:

- **Position must not be the render eye.** It was, until this note. In third
  person the render eye is an orbit boom several metres behind the body, so
  flipping the camera key silently moved every distance, every doppler shift
  and, worst, the occlusion ray's origin — and the boom is frequently pulled
  into the wall behind the player, which muffled the whole world. Third person
  now hears exactly what first person hears.
- **Position must not be the avatar's head joint either**, tempting as "the
  model's ears" sounds. That transform is one tick latent out of Jolt and rides
  the gait's bob and sway; a listener parked on it turns every footstep into a
  doppler wobble. It is the same three objections that stop the camera from
  orbiting the head joint (§8, Camera). The player's own eye is authoritative,
  frame-current and already step-smoothed.
- **Orientation must be the LOOK direction, not the body heading.** In third
  person the model faces where it *runs* (`ResolveAvatarHeading`), so ears
  welded to the torso would swing the stereo image away from the picture every
  time the player strafed.

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
