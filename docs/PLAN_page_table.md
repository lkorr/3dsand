# PLAN: the software page table for the voxel buffer

Phase 7 of `docs/PLAN_vulkan_port.md`, rewritten per `docs/ROADMAP_scale.md` §1
(user-reviewed): a **flat u32 software page table with `EMPTY` and
`UNIFORM(material)` sentinels**, not `VK_KHR_sparse_binding`, not an octree.

Status: design, 2026-08-22. No code has been written. Companion docs:
`docs/vulkan_barrier_graph.md` (the barrier design this must extend),
`docs/vulkan_pass_map.md` (the buffer inventory), `src/sim/pass_table.def`
(the table this must add `uses` entries to).

This document is load-bearing in the sense CLAUDE.md rule 1 means. Every branch
introduced here sits on the **address path of the hashed domain**: it decides
which u32 a sim kernel reads and writes. A translation that is right on the
RTX 3060 Ti and subtly different elsewhere is a desync, and a *write through a
sentinel* is a lost voxel that the hash notices one tick later at a location
that has nothing to do with the bug. Anything marked **[JUDGMENT]** is a call
made here that the reviewer should re-derive rather than accept.

Verified against the live tree 2026-08-22: `assets/shaders/common.wgsl:663-712`
(the address helpers), `sim_step.wgsl`, `sim_mutate.wgsl:87-107`,
`sim_occupancy.wgsl:35,64`, `worldgen.wgsl:2574-2612`, `src/sim/world.h`,
`src/sim/stream.cpp`, `src/sim/pass_table.def`, `src/test/support.cpp:104-173`,
`src/sim/world.cpp:105-235`.

---

## 0. Ground truth this is computed from (re-verify before acting)

| fact | value | source |
|---|---|---|
| window | `kWorldN = 512`, `kChunk = 16` → `kNChunk = 32`, `kNumChunks = 32768` | `world.h:20-31` |
| chunk payload | `kChunkVol = 4096` words = **16 KiB** | `world.h:31` |
| dense voxel buffer | `kVoxelCount = 512³` u32 = **512 MiB** | `world.h:32`, `pass_map §3a` |
| occupancy, settled | **11.25% non-air**; 27,794 / 32,768 chunks empty (**84.8%**); 2,338 fully full; 2,636 mixed | port plan "measured baseline" |
| 64 KiB-page estimate | 6,801 / 8,192 pages empty → **86.9 MiB** resident | port plan §Why |
| `maxStorageBufferRange` | **4294967295** = 4 GiB **minus one byte** | phase 3a capability record |
| settled tick, Vulkan | **229–236 µs**, of which ~120 µs is 54 empty indirect dispatches | phase 4a D4 / phase 6 |
| golden hash | `tests/baseline.json` `"determinismHash": "7cfa2420"` over 200 ticks | phase 3a D0 |

**Note on stale comments.** `world.h:29-30` says `kNChunk = 16` and
`kNumChunks = 4096` in trailing comments; both are wrong since the window went
to 512³ (the real values are 32 and 32768, and the expressions above them are
right). `vulkan_pass_map.md §3a` repeats the error. Phase 7 touches `world.h`
anyway; **fix those two comments in the first commit** so nobody sizes a pool
from them.

---

## 1. Goal and non-goals

### 1.1 What lands in phase 7

**Paged residency for the CURRENT 512³ window, behaviourally identical to
dense, with the memory saving measured.** Concretely:

1. A `pageTable` buffer of `kNumChunks` u32, one entry per **chunk slot**,
   holding either a page index into a pooled physical voxel buffer or a
   sentinel (`EMPTY` / `UNIFORM(mat)`).
2. Address translation inside the existing WGSL helpers in `common.wgsl`, so
   that **no sim kernel's own code changes**. The CA remains unaware there is a
   page table (ROADMAP §1: "the CA is unaware of it").
3. Page materialization driven from the CPU **before** the dispatch that could
   write into a page, because a GPU kernel cannot allocate mid-dispatch.
4. Page deallocation tied to the occupancy readback the CPU already receives,
   with hysteresis — so a settled world pays no per-tick scan (rule 2).
5. Gates: paged-vs-dense-vs-Dawn hash equality on the `--vk-smoke-loud`
   scenario, plus a new `page-roundtrip` selftest gate.
6. A reported resident-memory number against the 86.9 MiB estimate.

Success is defined negatively, and that is the point: **the world hash sequence
must not move**. `7cfa2420` at tick 200 stays `7cfa2420`. Every saved `.svd`
still loads. `--vk-smoke-loud`'s 19 probes still read `f97ba745 … cb036bd1`.
A page table that changes any of those has changed the simulation, which is not
what it is for.

### 1.2 What explicitly does NOT land in phase 7

Each of these is a ROADMAP follow-on with its own measurements. Bundling any of
them makes the hash-equality gate meaningless, because a hash that moves for two
reasons cannot be attributed to either.

- **Window growth to 1024³ or 2048³.** ROADMAP §6 sequences it *after* this
  phase and requires fresh occupancy measurements (§5.1-2) first. It also
  multiplies the dirty/occupancy metadata by 8× and needs a `kFarShiftBase`
  retune. Phase 7 changes where voxels live, not how many there are.
- **`kVoxelMeters` 0.10 → 0.05.** ROADMAP §2 is explicit that this is a project
  (avatar height, CPU-mirror reach, explosion radii, reaction rates, every
  save), not a constant edit.
- **The 4 GiB virtual buffer.** `maxStorageBufferRange` is 4 GiB − 1, so a
  single 4 GiB binding is illegal on this hardware today. Phase 7's pool is
  sized for the *current* window (§3.7) and lands far under the ceiling. The
  split-binding / BDA escape is designed for when the window actually grows.
- **Cell-level active bitmasks** (ROADMAP §3.1), **temporal rate LOD** (§3.5),
  **skip-encode / merged submissions** (§3.4, port phase 8). All independent.
  In particular: the ~120 µs of empty CA dispatches is a *phase 8* target, and
  phase 7 must not be credited or blamed for it.
- **Dropping the indirect staging copies or `simSlimBGL_`** (barrier_graph
  §4.10). Separate, hash-gated.
- **Removing Dawn.** Phase 6 says Dawn is retained *through* phase 7 as the hash
  oracle. §6 decides what happens after.

### 1.3 The one thing this phase buys that hardware sparse cannot

`UNIFORM(material)`. Hardware sparse gives you "unbound reads as zero" — air,
and only air, and only with `residencyNonResidentStrict`. A software sentinel
can encode *any* material, so a chunk that is 4,096 identical stone words costs
**4 bytes** instead of 16 KiB. At the current seed that is 2,338 fully-full
chunks, some fraction of which are single-material; at a window grown downward
into solid bulk it is most of the volume. §3.6 decides how much of that this
phase actually implements.

---

## 2. Address translation

### 2.1 Where the indirection lives, and why that is the whole design

The survey that makes this phase cheap: **every world-coordinate voxel access in
every sim shader already routes through `cellIndexW`**, and there are exactly
three access shapes that do not.

```
$ grep -c "cellIndexW|cellIndex(|chunkIndexW|chunkIndexOf" assets/shaders/*.wgsl
  common.wgsl 8   raymarch.wgsl 21   sim_step.wgsl 17   sim_particle.wgsl 5
  sim_explode.wgsl 4   sim_mutate.wgsl 1   sim_pick.wgsl 1   worldgen.wgsl 1
```

Every `voxels[...]` subscript in `sim_step.wgsl` resolves to a local `di`, `si`,
`idx` or `ni` that was assigned from `cellIndexW(...)` — verified line by line
(`:104`, `:128-129`, `:375`, `:398`, `:528`, `:770`). Same for
`sim_explode.wgsl:119`, `sim_mutate.wgsl:59` (the `main`/brush entry),
`sim_particle.wgsl:202`, `sim_pick.wgsl`. The three exceptions are all
**chunk-linear**, not world-coordinate:

| shape | site | nature |
|---|---|---|
| `voxels[op.cellIdx]` | `sim_mutate.wgsl:97,99` (`cells` entry) | `cellIdx` is already a **slot** index built by the CPU |
| `voxels[base + i]`, `base = dirtyList[wg.x] * CHUNK_VOL` or `wg.x * CHUNK_VOL` | `sim_occupancy.wgsl:35,64` | whole-chunk sweep, one workgroup per chunk |
| `voxels[slot * CHUNK_VOL + i]` | `worldgen.wgsl:2589` (`genChunk`) | whole-chunk fill, slot given |

So the indirection needs **two** entry points, not one per call site:

- `voxWordIndex(c : vec3<i32>) -> ...` — the paged replacement inside
  `cellIndexW`, for world-coordinate access.
- A per-chunk base resolve, for the three chunk-linear paths, which already
  have the chunk index in hand and want it hoisted out of their inner loop
  anyway.

**This is the seam, and it is the only seam.** ROADMAP §1's "the CA is unaware
of it" is not aspiration — it is a property that holds because `sim_step.wgsl`
never names a buffer offset it did not get from `cellIndexW`.

**Standing obligation, stated here because it is now load-bearing:** a sim
kernel that computes a `voxels[]` subscript by any means other than these two
helpers bypasses the page table and reads physical memory that may belong to
another chunk. Under Dawn-dense that is invisible. §5 gives the checker rule
that catches it.

### 2.2 The table layout

```
pageTable : array<u32>       // kNumChunks entries, one per CHUNK SLOT
```

Indexed by **slot chunk index** — exactly what `chunkIndexOf` /
`chunkSlotIndex` / `World::SlotChunkIndex` already produce. Not by world chunk
coordinate: memory is slot-indexed and never shifts (`common.wgsl:663-671`), so
the table shifts the same way the voxels do, which is to say not at all. A
window shift recycles slots in place and the table entry for a recycled slot is
rewritten by streaming, exactly like the voxel data.

128 KiB total. It is read by every sim kernel and is cache-hot by construction.

**Bit layout of an entry:**

```
 31 30                                                                   0
+---+-------------------------------------------------------------------+
| K |                            payload                                 |
+---+-------------------------------------------------------------------+

K = 0  RESIDENT.   payload bits 0..30 = PAGE INDEX into the physical pool.
K = 1  SENTINEL.   payload bits 12..30 = tag, bits 0..11 = material id.
```

Concretely, as WGSL constants generated into the prelude from `world.h`
(`ShaderConstantPrelude`, per the "world constants are generated from world.h,
never redeclared in WGSL" invariant):

```wgsl
const PT_SENTINEL_BIT : u32 = 0x80000000u;   // bit 31: 1 = sentinel, 0 = page index
const PT_MAT_MASK     : u32 = 0x00000FFFu;   // bits 0..11: material id (12 bits, matches voxMat)
const PT_EMPTY        : u32 = 0x80000000u;   // sentinel, material 0 (air)
const PT_PAGE_MASK    : u32 = 0x7FFFFFFFu;   // bits 0..30 when bit 31 is clear
const PT_UNRESIDENT   : u32 = 0xFFFFFFFFu;   // see below
```

Four consequences of choosing these exact values, each deliberate:

1. **`EMPTY` is `UNIFORM(air)`.** `PT_EMPTY == PT_SENTINEL_BIT | kMatAir` and
   `kMatAir == 0`. There is *one* sentinel decode path, not two, and "empty" is
   not a special case anywhere in the shader. This removes a whole class of
   "the empty branch and the uniform branch disagreed" bug, which is precisely
   the class §7 risk 3 is about.
2. **The material field is 12 bits and shares the voxel word's material
   position** (`voxMat(w) = w & 0xFFFu`, `common.wgsl:798`), so synthesizing a
   word from a sentinel is a mask, not a shift-and-repack.
3. **Page index gets 31 bits.** Absurdly more than needed (the pool is ~4k
   pages, §3.7) and free, because the alternative — a narrow field with spare
   bits — invites someone to overload the spare bits, which is the
   "state nibble is not spare space" mistake in a new place. If a future entry
   needs flags, it takes them from the sentinel's tag range (bits 12..30, all
   currently zero), never from the page index.
4. **`PT_UNRESIDENT = 0xFFFFFFFF` is a sentinel with material `0xFFF`** — a
   material id that cannot exist (`kMaterialSlots = 4096`, ids assigned from
   the bottom up, ~48 used, and the top 136 entries are the stain/art palettes
   which are inert and unreferenceable). It is **not used in the tick path**;
   it is the debug/assert poison value (§7 risk 1) and the value a page-table
   entry holds between "freed" and "rewritten". A read through it synthesizes
   material `0xFFF`, whose material-table entry is zeroed — class 0 (solid),
   density 0. That is deliberately *visible*: it makes a translation bug show
   up as a wall of impossible solid rather than as silent air.

### 2.3 The read path — exact synthesized words

```wgsl
// Physical word index for world cell c. Callers must have checked inWindow().
// Returns PT_NO_WORD when the chunk is a sentinel: there is no physical word,
// and the caller must take the synthesized value from voxWordAt instead.
fn pageBaseOfChunk(chunkSlot : u32) -> u32 { return pageTable[chunkSlot]; }

// THE read accessor. Replaces every `voxels[cellIndexW(c)]` in sim kernels.
fn voxWordAt(c : vec3<i32>) -> u32 {
  let s = vec3<u32>(c & vec3<i32>(WORLD_MASK));
  let e = pageTable[chunkIndexOf(s)];
  if ((e & PT_SENTINEL_BIT) != 0u) { return synthWord(e); }
  let lo = s % CHUNK;
  return voxels[e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x];
}

// The synthesized word for a sentinel chunk. THIS IS THE HASH CONTRACT (§4):
// it must be bit-identical to what a physically materialized page would hold.
fn synthWord(entry : u32) -> u32 {
  let mat = entry & PT_MAT_MASK;
  if (mat == MAT_AIR) { return 0u; }
  return packVox(mat, uniformStateFor(mat), STAMP_NEVER);
}
```

**The exact u32 for `EMPTY`, spelled out: `0x00000000`.** Material 0, state 0,
stamp `STAMP_NEVER` (which is 0, `common.wgsl:333`), no stain, bit 31 clear.
This is not a coincidence to be relied on loosely — it is *the* reason the
empty case is free: an all-air materialized page is a `vkCmdFillBuffer(0)`, and
`vkCmdFillBuffer(0)` is what §4.8 of the barrier graph already does to every
buffer at creation. Zero is air by construction and always has been
(`pass_map §6.14`). The invariant to state and check: `synthWord(PT_EMPTY) ==
0u`, and a materialized EMPTY page is 4,096 zero words.

**The exact u32 for `UNIFORM(mat)`:** `packVox(mat, uniformStateFor(mat),
STAMP_NEVER)` with no stain bits, i.e.
`(mat & 0xFFF) | (state << 12) | (0 << 16)` → the top 16 bits are **all zero**.
Two parts of that need justifying:

- **`STAMP_NEVER`, not a live stamp.** `world.h:183-191` is unambiguous:
  `kStampNever` is the only value a non-CA path may write, because a voxel
  born with a live code sits out a substep. A sentinel chunk is by definition
  one that has not been simulated in place, so every voxel in it must be free
  to act on the first tick it is dispatched. Writing a live stamp here would
  reproduce exactly the `0xFF`-masked-to-7 bug that comment exists to warn
  about — and it would be *correlated across a whole chunk*, which is the
  worse variant the `STAMP_NEVER` note describes.
- **`uniformStateFor(mat)` and why the state nibble is a problem.** The state
  nibble is per-material in meaning (`world.h:174`): liquid fullness 1..8, or
  a render palette variant `state % 3`. A `UNIFORM` sentinel carries only 12
  bits of material and therefore cannot represent a chunk whose cells have
  *different* state nibbles — and a chunk of stone almost certainly does, since
  worldgen assigns `rnd % 3` palette variants per cell (`sim_mutate.wgsl:74`
  and `genCell`). **This is the real constraint on UNIFORM, and it is why §3.6
  scopes UNIFORM the way it does.** `uniformStateFor(mat)` returns 0 and the
  promotion rule (§3.6) refuses to promote any chunk whose words are not all
  bit-identical to `packVox(mat, 0, STAMP_NEVER)`. Promotion is by **whole-word
  equality**, never by material equality.

**`inWindow` is unchanged and still the outer guard.** Out-of-window space is
solid and inert; that test happens *before* translation and is untouched. A
sentinel is a statement about a resident chunk's contents; out-of-window is a
statement about residency. Conflating them is the bug `gotcha-cpu-mirror` warns
about in a different register — they are two different tests and stay so.

**Cost.** One dependent u32 load per chunk entered, from a 128 KiB buffer that
every workgroup in the dispatch is hammering. Within one CA workgroup all 216
threads act on cells of *one chunk*, so it is one broadcast load, not 216.
Neighbor probes crossing a chunk face pay a second. **[JUDGMENT]** I expect this
to be within noise of the 229 µs settled tick and a few percent on an active
one; it is not measured, and §8's final step measures it. If it is worse than
that, the mitigation is hoisting the own-chunk entry into a workgroup-uniform
`let` at the top of `sim_step:main` — which is legal (all threads in the
workgroup share the chunk) and does not change any value.

### 2.4 The write path — a kernel must never write through a sentinel

This is the invariant the whole phase rests on, and it is stated as a *shape*,
not as a rule to remember:

> **There is no writable accessor that takes a sentinel.** The write accessor
> takes a physical word index, and the only way to obtain one is a function that
> returns a distinguished no-word value for a sentinel chunk.

```wgsl
const PT_NO_WORD : u32 = 0xFFFFFFFFu;   // not a valid word index

// The ONLY way to obtain a writable word index. Returns PT_NO_WORD for a
// sentinel chunk — which is a BUG at every sim call site, because §3
// guarantees every chunk a kernel may write is materialized before dispatch.
fn voxWordIndex(c : vec3<i32>) -> u32 {
  let s = vec3<u32>(c & vec3<i32>(WORLD_MASK));
  let e = pageTable[chunkIndexOf(s)];
  if ((e & PT_SENTINEL_BIT) != 0u) { return PT_NO_WORD; }
  let lo = s % CHUNK;
  return e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
}

// Every sim write goes through this. A sentinel write is a NO-OP, plus a
// detectable assertion when PAGE_ASSERT is on.
fn voxStore(idx : u32, w : u32) {
  if (idx == PT_NO_WORD) {
    if (PAGE_ASSERT != 0u) { atomicAdd(&pageFaults[0], 1u); }
    return;
  }
  voxels[idx] = w;
}
```

**`PAGE_ASSERT` is a generated prelude constant, not a preprocessor define** —
WGSL has no preprocessor. It joins `WORLD_N`, `CHUNK` and the rest in
`ShaderConstantPrelude()` (`gpu/resources.cpp`), emitted as
`const PAGE_ASSERT : u32 = 0u;` or `1u;` from a C++ flag. Two consequences,
both good: the branch is a compile-time-known constant so Tint folds it away
entirely in the off case (zero cost, verifiable in the SPIR-V), and flipping it
is an F5 shader reload rather than a rebuild. The `pageFaults` binding is
declared unconditionally and bound to a 16-byte buffer either way — a bound
buffer nothing writes costs nothing, and a conditionally-declared binding would
mean two bind-group layouts, which is exactly the "two things that must agree"
shape to avoid.

Three properties, in the order that matters:

1. **A sentinel write is a no-op, not an out-of-bounds store.** `PT_NO_WORD` is
   not an index into `voxels`; it is a value `voxStore` tests before indexing.
   The failure mode is a *lost voxel* (bad, visible in the hash) rather than a
   *corrupted stranger* (worse, invisible until it isn't). §7 risk 1 argues
   why this is the right choice of failure.
2. **It is detectable, cheaply, and only when asked.** `SANDVOX_PAGE_ASSERT` is
   a shader-prelude define (the prelude is regenerated per load, so this is a
   flag, not a rebuild) that binds a tiny `pageFaults` atomic counter. The new
   selftest gate (§4.4) runs with it on and asserts the counter is **zero**.
   Production runs bind nothing and the branch compiles out. This converts
   "structurally impossible" from a claim into a measurement.
3. **`cellIndexW` keeps its name and meaning for readers.** The mechanical
   edit is: every `voxels[cellIndexW(X)]` **read** becomes `voxWordAt(X)`;
   every `voxels[<idx>] = W` **write** becomes `voxStore(<idx>, W)` where
   `<idx>` came from `voxWordIndex`. Sites that do both (`sim_step`'s
   `tryMove`, which reads `voxels[di]` then writes it) resolve the index once
   and use it for both — which is what they already do.

**Why not just let the write land somewhere harmless?** Because there is no
harmless somewhere. Any physical index is *some other chunk's* voxel. The
no-op is the only write that cannot corrupt a bystander, and a lost write in a
chunk that was provably never going to be written (§3) is a contradiction the
assert catches.

### 2.5 Integer-only discipline, restated for this path

Rule 1 applies here with full force, and the specific hazards are:

- **No `f32` anywhere in translation.** Every operation above is `u32` mask,
  shift, compare, multiply-add. There is no arithmetic a compiler can contract
  or reassociate into a different answer.
- **No scheduling dependence.** `pageTable` is **read-only during a dispatch**.
  It is written only by CPU-driven fills between dispatches (§3), never by a
  sim kernel, never by an atomic. Two threads translating the same address in
  the same dispatch get the same answer because the input did not change. This
  is the single most important structural property in the document: *the page
  table is not sim state, it is dispatch-invariant configuration.*
- **No allocation inside a kernel.** There is no GPU-side free list, no atomic
  bump allocator, no CAS to claim a page. Those are all
  first-come-first-served, which rule 1 bans by name. Allocation is a CPU
  decision recorded before the command buffer.
- **Same inputs translate identically on every machine.** The table's content
  is a pure function of the CPU-side allocation history, which is itself a pure
  function of the tick's inputs (§3.8 makes exhaustion deterministic, which is
  the one place this could have failed).

**The table is NOT hashed and NOT saved.** It is derived state — see §4.2. Two
different page assignments for the same logical world are the same world.

---

## 3. Page lifecycle

This is the crux. GPU kernels cannot allocate, so **every page a kernel might
write must exist before the command buffer is submitted**. That turns the whole
problem into one question: *at encode time, what is the set of chunks that this
tick could write?*

### 3.1 The writers, enumerated

Not "the ones I can think of" — the enumeration is taken from
`pass_table.def`'s `uses` sets, which is the authoritative list of what writes
`Voxels`:

| row | writes `Voxels` | reach |
|---|---|---|
| `mutate` | `RW(Voxels)` | brush sphere, radius ≤ op radius, CPU-known center |
| `mutateCells` | `RW(Voxels)` | exact slot cell indices, CPU-authored |
| `explodeApply` | `RW(Voxels)` | ≤ `EXP_R_MAX = 20` voxels of a CPU-known center |
| `ca` ×54 | `RW(Voxels)` | dirty chunks **∪ ≤1 cell into neighbours** |
| `particleResolve` | `RW(Voxels)` | anywhere a particle's flight ends |
| `worldgen` | `W(Voxels)` | all 32,768 slots |
| `worldgenList` | `W(Voxels)` | the `genList` slots |
| *(off-table)* `Stream::FillSlots` | `queue.WriteBuffer(voxels, slot*16KiB)` | the refilled slots |
| *(off-table)* `LoadWorld` | whole-world upload | all slots |

Plus one non-writer that still matters: `occupancyFull` **reads every slot**
(`D_CHUNKS` = 32,768 workgroups), which §4.1 addresses.

### 3.2 (a) The CA — and the one-tick-late dirty problem

The CA is the hard case and everything else is easy, so it gets the proof.

**What the CA can write.** `sim_step`'s workgroups are dispatched indirectly
over `dirtyList`, which `sim_compact` built from `dirtyIn`. A workgroup handles
one dirty chunk and its threads act on cells of that chunk. Writes reach ≤1
cell (the rule-1 invariant, asserted in `sim_step.wgsl:1-9`), so a cell on a
chunk face can write into the **face-, edge-, or corner-adjacent** chunk.
`markDirty` (`sim_step.wgsl:66-88`) confirms the reach precisely: it marks the
2×2×2 combination of the cell's own chunk with up to three ±1 offsets, i.e. up
to the 26-neighbourhood.

> **Correction to the brief.** The brief says "up to 1 cell into face-neighbor
> chunks", and the materialization set it proposes (`dirty ∪ face neighbours`)
> is **not sufficient**. A cell at a chunk *corner* — local coordinate
> (0,0,0) or (15,15,15) — writes to a destination that differs on all three
> axes, landing in the corner-diagonal chunk. `markDirty` marks exactly that
> chunk, which is the code telling us the reach. The materialization set must
> be **dirty ∪ their 26-neighbourhoods**, not 6. This is the kind of
> off-by-one that produces a lost voxel on ~1 in 4,096 cells at a chunk
> corner, which is far too rare to catch by looking and immediately fatal to
> the hash.

So: `writes(N) ⊆ N26(dirtyIn(N))`, where `N26(S)` is `S` plus every
window-resident chunk adjacent to a member of `S` (including diagonals), and
`dirtyIn(N)` is the buffer `compact` reads at tick N.

**What the CPU knows at encode time.** The snapshot carries
`dirtyFlags[kNumChunks]`, copied by `EncodeDirtyCopy` (`world.cpp:173-181`)
from `Simulation::DirtyNext()` — which the comment there states is *the tick's
`dirtyOut`, before the page flip*. And `FlipPage` after submit means tick N+1's
`dirtyIn` **is the same buffer** as tick N's `dirtyOut`. So:

```
dirtyFlags in a snapshot stamped tick N  ==  dirtyOut(N)  ==  dirtyIn(N+1)
```

That is an *exact* identity, not an approximation. If the snapshot for tick N
were always available before encoding tick N+1, the CPU would know
`dirtyIn(N+1)` exactly and the problem would be over.

**It is not always available**, and there are two independent reasons:

1. **The ring can decline.** `EncodeReadbacks` returns false when all three
   slots are in flight (`world.cpp:110-113`), and `EncodeDirtyCopy` is guarded
   by `if (lastSlot_ < 0) return;`. On such a tick, `dirtyOut` is **never
   copied**. The next snapshot to arrive is a *later* tick's, and the
   intervening `dirtyOut` generations are simply never seen by the CPU.
2. **The map is asynchronous.** `KickReadback` fires `MapReadAsync`; the
   callback runs in `ProcessEvents()` at a frame boundary
   (`main.cpp` frame loop), and the frame loop runs **up to 4 ticks per frame**
   (`main.cpp:1723`, `ticksThisFrame < 4`). So even without a skip, the
   snapshot in hand while encoding tick N can be from tick N−4 or older.

So the CPU's knowledge is "some `dirtyOut(M)` for an unknown `M ≤ N−1`", which
is *not* `dirtyIn(N)`. **Materializing `N26(dirtyOut(last snapshot)) ∪
CPU-op-targets` is therefore NOT sufficient, and the brief's proposed rule is
refuted.** Concretely: a fire that spreads for three ticks while the ring is
saturated has a dirty frontier three chunks beyond anything the CPU has seen.

**But the frontier propagates at a bounded rate, and that closes it.** The
argument the skip-encode idea uses does apply — it just has to be applied with
the right latency bound:

> **Closure lemma.** `dirtyIn(N+1) = dirtyOut(N) ⊆ N26(dirtyIn(N)) ∪ C(N)`,
> where `C(N)` is the set of chunks marked by CPU-driven ops at tick N.

*Proof.* Every writer of `dirtyOut` is one of: `markDirty`/`markBoth` called
from a kernel, or a CPU path. `markDirty(c)` is only ever called with a cell
`c` that the calling kernel touched; for `ca` and `particleResolve` those cells
are within `N26(dirtyIn(N))` by the write-reach argument above, and `markDirty`
itself reaches ≤1 chunk further — but a chunk marked by a cell *in* chunk `X`
is in `N26({X})`, and `X ∈ N26(dirtyIn(N))` gives `N26(N26(dirtyIn(N)))`. That
is a 2-ring, not a 1-ring. ∎ — **and that is the honest bound**: one tick of
unknown dirty state costs a 2-ring dilation, not a 1-ring. (`mutate`,
`mutateCells`, `explodeApply` and `particleSpawn` are all in `C(N)`, being
CPU-op-driven; `particleResolve`'s landing cell is bounded by the particle's
CPU-unknown flight, which §3.4 handles separately.)

Iterating the lemma `k` times gives: if the CPU's newest snapshot is from tick
`N−k`, then

```
dirtyIn(N) ⊆ N26^(2k) ( dirtyFlags(snapshot) )  ∪  ⋃_{j=N-k}^{N-1} C(j)
```

A `2k`-ring dilation. **[JUDGMENT] I reject this as the mechanism**, for two
reasons that are worth stating because the reviewer may disagree:

- `k` is unbounded in principle (a long GPU stall saturates the ring
  indefinitely), so the dilation radius is unbounded, so the "conservative
  set" degenerates toward the whole window. A rule whose worst case is "all
  32,768 chunks" has no rule-2 story.
- Computing a `2k`-ring dilation over 32,768 chunks on the CPU every tick is
  itself a full-world scan — the exact rule-2 violation the phase is supposed
  not to add.

**The mechanism instead: make the CPU's dirty knowledge exact by construction,
not by inference.**

> **Decision: maintain a CPU-side mirror of `dirtyOut` incrementally, from the
> same information the GPU has, and materialize from it.** Specifically, the
> CPU keeps `cpuDirty[kNumChunks]` (a bitset, 4 KiB) and updates it each tick
> with the *superset rule*:
>
> ```
> cpuDirty(N+1)  =  N26( cpuDirty(N) )  ∪  C(N)          // CPU-side, no GPU dep
> cpuDirty(N+1) ←  dirtyFlags(snapshot)                  // EXACT reset, when a
>                                                        // snapshot for tick N
>                                                        // actually arrives
> ```
>
> The first line is the conservative propagation the lemma licenses, applied
> **one tick at a time** so the dilation is a 1-ring per tick rather than a
> `2k`-ring in one go. The second line is the correction: whenever a snapshot
> lands, its `dirtyFlags` are the *exact* `dirtyIn` for the tick after the one
> it was stamped with, so the CPU replaces its conservative estimate with
> ground truth and the accumulated over-approximation is discarded.

Why this is the right shape:

- **It is exact in the common case.** The ring almost always has a slot; the
  snapshot almost always lands within a frame. In a settled world `cpuDirty`
  is empty and the reset confirms it empty — the settled cost is *zero
  materializations and a scan of an empty set*, which is the rule-2 story.
- **It degrades by one ring per missed tick, and self-heals.** Three missed
  snapshots cost a 3-ring around the true frontier, i.e. a handful of extra
  chunks materialized around an active fire. The next snapshot clamps it back.
  There is no unbounded growth path that a *live* world does not already have
  (a spreading fire's true dirty set grows too; the over-approximation is a
  constant ring around it).
- **It is a set operation on a bitset, not a scan.** The `N26` dilation is
  performed over the *members* of `cpuDirty` (a small vector of indices in
  practice), never over all 32,768 slots. A settled world iterates zero
  elements.
- **It never under-approximates**, which is the only property correctness
  needs. Over-approximating costs a materialized page that turns out empty and
  is freed by §3.6's hysteresis a few ticks later.

**[JUDGMENT] The alternative I considered and rejected: a GPU-side
materialization pre-pass.** Shape: a kernel reads `dirtyIn`, computes the
26-dilation, and writes a "needed pages" list; the CPU reads it back and
allocates. This is exact and needs no CPU mirror — but it requires a **readback
in the middle of the tick**, i.e. a CPU↔GPU round trip on the frame path, which
CLAUDE.md forbids outright ("never add a synchronous readback to the frame
path"). Making it async re-introduces the same latency the CPU mirror already
handles, with more machinery. The CPU mirror wins because the information is
CPU-derivable and the GPU has no monopoly on it.

**[JUDGMENT] A second alternative worth recording: pre-materialize a fixed ring
around every dirty chunk permanently.** Simpler, but it makes the resident set
a function of activity *history* rather than activity, and nothing frees it
promptly. Rejected on rule 2.

### 3.3 (b) sim_mutate / sim_explode / particleSpawn

All three take CPU-authored op streams, so their targets are CPU-known **before
the ops are written to their buffers** — which is the same place `SubmitTick`
already computes `opsCount`/`expCount`/`cellCount` (`support.cpp:113-136`).

| writer | materialization set | when |
|---|---|---|
| `mutate` | every chunk intersecting the sphere `(op.x±r, op.y±r, op.z±r)`, per op | as the `BrushOp` vector is assembled |
| `mutateCells` | `op.cellIdx / CHUNK_VOL` per op — already a slot chunk index, no arithmetic needed | as the `CellOp` vector is assembled |
| `explodeApply` | chunks intersecting the `41³` box around each op center (`EXP_BOX`), clipped to the window | as the `ExplosionOp` vector is assembled |

`explodeMark` is a **pure read** and needs no materialization: it reads
neighbours (which may legitimately be sentinels — a sky-boundary explosion is
exactly the §4.4 test case) and writes only `expMask`. That asymmetry is worth
noting because the mark/apply split means the *read* half sees sentinels and the
*write* half must not — which is the split working as designed.

All three also feed `C(N)` in §3.2's recurrence, since they mark chunks dirty
(`markBoth`), which is what makes tick N+1's CA reach them.

**Precision note.** Materializing the sphere's bounding-box chunks rather than
the exact sphere over-approximates by a few chunks per op. Accept it: the ops
are bounded (`kMaxOpsPerTick = 64`, `kMaxExplosionsPerTick = 8`) and the
over-approximation is freed by hysteresis.

### 3.4 (b cont.) particleResolve — the one writer with no CPU-known target

`sim_particle:resolve` writes `voxels` where a particle **lands**, and the
landing cell is computed on the GPU from integrated fixed-point flight
(`sim_particle.wgsl:180,202`). The CPU does not know it. This is the writer the
brief flags as "can land anywhere along a flight path", and it is real.

Three facts bound it:

1. **Particles are capped**: `kParticleCap = 262144`, and `particlesActive` is a
   CPU-known boolean (`support.cpp:111`) that is false in a settled world.
2. **Velocity is capped**: `PART_MAX_VEL = TUNE_PART_MAX_VEL` (~6 voxels/tick
   terminal). A particle moves at most a bounded number of *voxels* per tick,
   hence at most a bounded number of *chunks*.
3. **Particles are spawned by CPU-known events** — explosion ejecta
   (`explodeApply`, whose center the CPU knows) and `spawnOps` (CPU-authored) —
   and thereafter integrate deterministically.

**Decision: track a CPU-side conservative particle-occupancy set, seeded from
spawn sites and dilated by the max flight distance per tick, and materialize
it — with a hard fallback.** Specifically:

```
particleChunks(N+1) = Ndilate( particleChunks(N), ceil(PART_MAX_VEL / CHUNK) + 1 )
                      ∪ chunks(spawnOps(N)) ∪ chunks(explosion centers(N))
particleChunks(N+1) = ∅   when the readback says particleCount == 0
```

`ceil(6/16)+1 = 1`, so the dilation is a 1-ring per tick — the same shape as
`cpuDirty`. And `snap.particleCount` (already read back, `world.h:487`) gives
the exact reset condition: no live particles means the set is empty, which is
the settled case.

**[JUDGMENT] The fallback, and I want the reviewer's opinion on it.** The
dilation is only sound if `PART_MAX_VEL` genuinely bounds per-tick travel and
the sweep is subdivided (it is — anti-tunneling subdivides the sweep, per the
budget gotcha in CLAUDE.md). If a future change lets a particle move further,
this silently under-approximates and we lose a voxel. Two ways to make that
non-silent:

- **(i)** `voxStore`'s no-op path is already the containment (§2.4) and the
  `SANDVOX_PAGE_ASSERT` counter catches it in the gate. Cheap, but only in the
  gate.
- **(ii)** Add a `static_assert`-shaped CPU check: the dilation radius is
  computed from `TUNE_PART_MAX_VEL` at load rather than hardcoded, so raising
  the tuning value automatically widens the ring. Since `TUNE_*` values are
  hot-reloadable (F5), this must be recomputed on reload.

**Take both.** (ii) is the mechanism, (i) is the detector. The general principle
matches the repo's habit: derive the constant, don't restate it.

**A simpler option exists and I am not taking it:** materialize the whole window
whenever `particlesActive`. That is correct and trivially safe, but a single
explosion would then un-sparse the entire world for as long as its debris
flies — turning the most common "something is happening" case into the
worst-case memory footprint. That defeats the phase.

### 3.5 (c,d,e) worldgen, streaming fill, LoadWorld

These are the easy ones, because they all **replace whole chunks** and are
CPU-scheduled.

**(c) `worldgen` / `worldgenList`.** `genChunk` writes `voxels[slot*CHUNK_VOL+i]`
for all 4,096 cells of a slot. Options:

- **Materialize every target slot before the dispatch**, then let `genChunk`
  write through a resolved base. Simple, but worldgen's `main` targets *all*
  32,768 slots — materializing all of them makes the pool momentarily need to
  be dense, which is the whole thing we are avoiding.
- **Let `genChunk` write to a sentinel by *demoting after the fact*.** No: the
  kernel cannot allocate.
- **Decision: give `genChunk` a two-phase shape.** Phase A (CPU, before the
  dispatch): every target slot is set to `PT_EMPTY` in the page table — free,
  no allocation, and correct because `genChunk` overwrites the whole chunk.
  Phase B: `genChunk` writes into a **materialized page for slots the CPU
  chose to materialize**, and for slots left as `PT_EMPTY` it... cannot write.

  That does not work either, and the reason is instructive: worldgen does not
  know in advance which chunks it will fill with air. **So worldgen is the one
  path that gets a scratch page.**

  **Final decision: worldgen materializes into a per-workgroup scratch page,
  and a CPU-driven compaction pass promotes.** Concretely: `genChunk` writes
  into a materialized page as today (so *all* target slots must be
  materialized), and immediately after the worldgen submit, a **compaction
  pass** (§3.6's demotion machinery, run once eagerly rather than on the
  hysteresis cadence) frees the ~84.8% of pages that came out all-air. The
  peak footprint is dense-for-one-submit; the steady state is 86.9 MiB.

  **[JUDGMENT] This is the least elegant part of the design and the reviewer
  should push on it.** A dense transient at worldgen means the pool must be
  sized for 32,768 pages during startup, which is 512 MiB — i.e. no saving at
  all at the moment of worldgen. The alternative that actually fixes it is to
  make `genChunk` compute its chunk's occupancy *in-kernel* (it already does —
  `worldgen.wgsl:2589` region writes `occupancy`) and have the CPU do worldgen
  **in batches** of, say, 2,048 slots: materialize a batch, dispatch, read
  occupancy, demote the empties, reuse those pages for the next batch. That
  bounds the transient to batch size and costs `32768/2048 = 16` submits at
  startup, which is nothing. **Recommend the batched form**; it also
  generalizes to a grown window, where the dense transient would be 4 GiB and
  simply impossible. Worldgen is not on the frame path, so 16 submits is free.

**(d) `Stream::FillSlots`.** Two branches (`stream.cpp:258-276`):

- **Store hit**: the CPU has the decoded 16 KiB in `data` before it uploads.
  It therefore knows, exactly and for free, whether the chunk is all-air or
  all-one-word — it is already looping over every word to compute `occ` and
  `blockers` (`stream.cpp:263-268`). **Extend that existing loop** to also
  track "all words equal" and "all words zero", and:
  - all-zero → set `pageTable[s] = PT_EMPTY`, **skip the 16 KiB upload
    entirely**. This is a bandwidth win on top of the memory win, and it is
    free.
  - all-equal to `packVox(mat, 0, STAMP_NEVER)` → `pageTable[s] =
    PT_SENTINEL_BIT | mat`, skip the upload.
  - otherwise → materialize a page, upload into it, `pageTable[s] = page`.
- **Store miss** (`genSlots`) → the `worldgenList` path, which is (c) at batch
  size = the genList count. Already bounded by the shift plane.

  Note the ordering constraint this creates: the page-table writes and the
  voxel upload are **both** `QueueWrite`s on the pending-upload queue
  (barrier_graph §4.1), issued in that order, draining into the same command
  buffer. §5 covers the hazard.

**(e) `LoadWorld`** (`worldio.cpp`, one of the two sanctioned rule-3 bypasses).
Same shape as (d) at whole-world scale: the CPU has every chunk's words in hand
as it decodes the save, so it classifies each chunk as it goes and uploads only
the non-uniform ones. This is strictly better than today — a save of a mostly-sky
world becomes a mostly-sentinel load with almost no upload traffic. `LoadWorld`
already drains and resets (`EncodeLoadReset`), so there is no in-flight state to
reconcile.

### 3.6 Deallocation, demotion, and hysteresis

**Who decides.** The CPU, from the occupancy readback it *already receives*
every tick in the snapshot (`snap.occupancy[kNumChunks]`,
`world.cpp:205-208`). No new readback, no new scan, no new GPU work. This is
the rule-2 answer: **a settled world pays nothing new**, because the data was
already arriving and the decision is a comparison against a per-slot counter
the CPU already keeps.

**The free condition.** `occTotal(occ) == 0` for a resident slot means the
chunk is all-air and its page is releasable. That is a *sufficient* condition
and exactly the `EMPTY` case.

**The demote-to-UNIFORM condition — and my recommendation on scope.**

`occupancy` packs only two 16-bit counts (`common.wgsl:785-787`); it cannot
tell you whether a full chunk is one material, let alone one *word* (which is
what UNIFORM needs, per §2.3). ROADMAP §5.1 lists exactly this as an open
measurement ("how many 'fully full' chunks are single-word uniform vs
mixed-material/stained — the last unknown in the 2048³ memory budget").

> **[JUDGMENT] Decision: implement UNIFORM as a *representable and
> constructible* sentinel in phase 7, but implement demotion (dense page →
> UNIFORM sentinel) ONLY on the paths where the CPU already has the words in
> hand — streaming store-hit and `LoadWorld` (§3.5 d,e). Do NOT add a GPU
> uniformity scan and do NOT demote from the tick path in phase 7.**

Reasons, and this is the call I most want challenged:

1. **The payoff is unmeasured.** ROADMAP §5.1 says so outright. At the current
   seed the sparse win is "almost entirely sky" — 84.8% empty chunks vs 2,338
   full ones, and an unknown fraction of *those* are single-word. Building a
   GPU uniformity scan to chase at most 2,338 × 16 KiB = 36 MiB, of which
   probably a minority qualifies (worldgen assigns `rnd % 3` palette variants
   per cell, so a stone chunk is almost certainly **not** single-word), is
   effort spent ahead of evidence.
2. **The measurement is cheap and should come first.** ROADMAP §5.1's ~20-line
   `--measure` histogram answers it. **Add it as commit 0 of §8** and let the
   number decide whether tick-path demotion is worth a later commit.
3. **The read path costs nothing to support UNIFORM anyway.** `synthWord`
   handles it, `EMPTY` *is* `UNIFORM(air)`, and the streaming/load paths
   construct it for free from a loop they already run. So the sentinel is live
   and tested from day one; only the expensive *discovery* mechanism is
   deferred.
4. **Downward window growth is where UNIFORM pays**, and that is a later
   milestone (§1.2). When the window grows into solid bulk, the chunks that
   matter arrive through **streaming and worldgen**, which are exactly the
   paths that do get demotion here.

**Hysteresis.** A page at a boundary must not thrash. The rule:

```
A resident page is freed when it has reported occTotal == 0 on
kPageFreeTicks CONSECUTIVE snapshots AND its slot is not in cpuDirty.
```

`kPageFreeTicks = 8` **[JUDGMENT]** — about a quarter second at 30 Hz. Both
conjuncts are needed and each blocks a different thrash:

- The consecutive-count blocks the *sampling* thrash: a chunk that empties and
  refills within the readback latency.
- The `!cpuDirty` conjunct blocks the *causal* thrash: a chunk that is empty
  right now but is adjacent to activity and is about to be written into.
  Freeing it would force a re-materialize on the very next tick. This conjunct
  is the one that makes §7 risk 6 (materialize–demote oscillation) structurally
  impossible rather than merely unlikely: a chunk cannot be simultaneously
  "in the materialization set" and "eligible to free".

**Who scans, at what cadence, at what cost when settled.** The consecutive-zero
counter is maintained in the snapshot callback, which already walks all
`kNumChunks` entries (`world.cpp:200-210` — an existing loop, not a new one).
The free *decision* iterates only slots whose counter just reached the
threshold, which in a settled world is **zero slots** after the first quarter
second and stays zero forever. Concretely: a settled world does one extra
`uint8` increment per chunk inside a loop it was already running, and takes no
further action. That is the rule-2 story, and it is honest — it is not free,
it is 32,768 increments inside an existing 32,768-iteration loop.

**What a freed page must do to the physical memory.** Nothing. The page is
returned to the free list and its contents are stale garbage. When it is next
allocated, the allocation path zero-fills it (§3.7). Freeing is a page-table
write plus a free-list push; no GPU command at all.

### 3.7 The pool, the free list, fragmentation

**Layout.** One `voxels` buffer, `kPoolPages × kChunkVol` u32, unchanged in
usage flags from today. **No fragmentation is possible**: every allocation is
exactly one page and every page is exactly `kChunkVol` words. This is the
single biggest reason the flat table beats anything hierarchical — the
allocator is a stack of indices.

```c++
std::vector<uint32_t> freePages_;   // LIFO stack of page indices
uint32_t              pagesInUse_;
uint32_t              pagesHighWater_;   // reported by --measure
```

**Allocation is a pop; free is a push.** LIFO deliberately: a recently-freed
page is the one most likely still resident in whatever cache hierarchy cares,
and — more importantly — LIFO makes the sequence of page indices a *pure
function of the allocation/free order*, which is a pure function of the tick
inputs. That matters for §7 risk 4: it makes page assignment reproducible run
to run, which is not required for hash equality (the table is not hashed) but
makes divergence debugging tractable.

**Initialization on allocation.** A freshly allocated page must be filled with
the synthesized content of the sentinel it is replacing, **before** the
consuming dispatch:

| replacing | fill | mechanism |
|---|---|---|
| `PT_EMPTY` | 4,096 × `0x00000000` | `vkCmdFillBuffer(voxels, pageOff, 16 KiB, 0)` |
| `PT_UNIFORM(mat)` | 4,096 × `synthWord` | `vkCmdFillBuffer(voxels, pageOff, 16 KiB, word)` |

`vkCmdFillBuffer` takes a 32-bit pattern, so **both cases are one fill command**
— UNIFORM costs exactly what EMPTY costs. This is a small, real argument for
the sentinel design over hardware sparse, which can only produce zeros.

Under the RHI seam these go through `FillTracked` (`pass::Buf::Voxels` +
offset), which is the mechanism phase 4a established for off-table commands
(`rhi_record.h`, barrier_graph §8) — so the hazard is derived by the tracker
rather than hand-placed. §5 covers the ordering.

**Pool sizing.** Measured: 4,974 non-empty chunks (32,768 − 27,794) at the
default seed = **15.2%**.

| sizing | pages | bytes | note |
|---|---|---|---|
| measured non-empty | 4,974 | 77.7 MiB | the steady state |
| 64 KiB-page estimate from the port plan | — | 86.9 MiB | the number to beat |
| **`kPoolPages = 8192` (25%)** | 8,192 | **128 MiB** | **recommended** |
| dense equivalent | 32,768 | 512 MiB | today |

**Recommendation: `kPoolPages = 8192`, i.e. 128 MiB, a 1.65× headroom over the
measured steady state and a 4× reduction from dense.** **[JUDGMENT]** The
headroom is for: the materialization over-approximation (a 1-ring around every
dirty chunk), transient activity (a large explosion fills chunks that were sky),
worldgen batching (§3.5c), and a different seed. It is a `constexpr` in
`world.h` so it is one edit to retune once `--measure` reports the high-water
mark on real scenarios — which §8's final step does.

**A note on what "128 MiB" means against the 86.9 MiB estimate.** The estimate
is *resident content*; 128 MiB is *reserved pool*. They are not comparable
numbers and the acceptance criterion in §8 must report both: the high-water
`pagesInUse_ × 16 KiB` (compare to 77.7 MiB) and the pool reservation (128 MiB).
Conflating them is how a phase claims a win it did not get.

### 3.8 Exhaustion — and why it must be deterministic

If `freePages_` is empty when the materialization set needs a page, something
must give. **Any behaviour that changes voxel state is hash-relevant**, so this
is a rule-1 decision, not an error-handling detail.

The options and their consequences:

| option | consequence |
|---|---|
| **Refuse the op** | a brush stroke silently does nothing. Deterministic *if* the refusal order is deterministic — and it is not obviously so, because the pool state depends on allocation history. |
| **Fall back dense** | requires 512 MiB of pool to exist, i.e. the phase bought nothing. |
| **Grow the pool** | a `vkCreateBuffer` mid-frame, a full copy, and a descriptor rewrite while command buffers are in flight. |
| **Drop the write** | `voxStore`'s no-op. Deterministic but silently loses matter. |

> **Decision: exhaustion is a hard, loud, deterministic failure — the tick is
> refused before it is encoded, the engine logs and aborts in the selftest, and
> in the game it clamps by refusing *the lowest-priority members of the
> materialization set in a fixed order*.**

Spelled out, because "deterministic" is the whole requirement:

1. **Priority order is fixed and total**, and it is by *provenance*, not by
   slot index: (1) CPU-op targets — refusing these loses an authored mutation;
   (2) chunks in `cpuDirty` — refusing these loses CA writes; (3) the
   over-approximation ring — refusing these is *free*, because a chunk in the
   ring that is not truly dirty was never going to be written. Within a tier,
   ascending slot index. This is a pure function of the tick's inputs, so two
   machines make the same refusals.
2. **The ring tier is where the clamp lands in practice**, and refusing it is
   sound because the ring is an over-approximation: if a write *does* arrive at
   a refused ring chunk, `voxStore` no-ops it and `pageFaults` increments. So
   the failure escalates from "free" to "detectable" rather than to "silent".
3. **Tiers 1 and 2 refusing is a hard error.** The engine logs
   `page pool exhausted: N needed, M free` and, under `--selftest`, **fails the
   run**. In the game it continues with the refusal (matter is lost, the world
   diverges from a replay) but the log line is the evidence. It must not be
   swallowed.
4. **The pool is sized so tiers 1 and 2 cannot plausibly exhaust it** (§3.7's
   1.65× headroom over a measured steady state), and `pagesHighWater_` is
   reported so the margin is observable rather than assumed.

**Recording this in the doc trail:** because exhaustion behaviour *can* alter
voxel state, it must be listed in DESIGN.md's determinism section as a
condition under which the hash guarantee is void — the same way out-of-memory
is. **[JUDGMENT]** I would rather have a loud, documented "the guarantee does
not hold here" than a quiet fallback that makes the guarantee false everywhere.

---

## 4. What is hashed and what round-trips

### 4.1 The world hash over a paged world

The hash lives in `sim_occupancy.wgsl:64-103`, `main` entry:

```wgsl
let base = wg.x * CHUNK_VOL;
for (var i = li; i < CHUNK_VOL; i += 64u) {
  let w = voxels[base + i];
  let v = (w & 0xFFFFu) | ((w & STAIN_BITS) >> 8u);
  let m = v & 0xFFFu;
  if (m != MAT_AIR) {
    count += 1u;  if (isRayBlocker(materials[m])) { block += 1u; }
    if (T.hashEnable != 0u) { h += pcg((base + i) ^ (v * 0x9E3779B9u)); }
  }
}
```

Three properties of that code decide everything below, and the first is the
lucky one:

1. **Air contributes nothing.** The `if (m != MAT_AIR)` guard means an all-air
   chunk adds exactly `0` to `count`, `block` and `h`. So an `EMPTY` chunk's
   analytic hash contribution is **provably zero, with no arithmetic at all** —
   not "approximately zero", not "zero if we compute it right". The analytic
   path for EMPTY is: skip the chunk.
2. **The hash is keyed on `base + i`, the PHYSICAL slot word index.** Not the
   world coordinate. So the contribution of a chunk depends on *which slot it
   is in*, which is what makes the hash window-relative — and it means the
   analytic path must key on the same `wg.x * CHUNK_VOL + i`, not on a page
   index. Getting this backwards would make paged and dense disagree
   immediately, which is at least a loud failure.
3. **The sum is commutative** (`h +=` then `atomicAdd`), which the file's own
   header comment relies on for order-independence. A per-chunk analytic
   contribution therefore composes correctly with physically-scanned chunks.

**Decision: the analytic path, for both sentinel kinds.** `sim_occupancy:main`
becomes:

```wgsl
let e = pageTable[wg.x];
if ((e & PT_SENTINEL_BIT) != 0u) {
  let mat = e & PT_MAT_MASK;
  if (mat == MAT_AIR) {
    // EXACT, and no arithmetic: the dense loop's `if (m != MAT_AIR)` guard
    // means an all-air chunk contributes 0 to count, blockers AND hash.
    if (li == 0u) { occupancy[wg.x] = packOcc(0u, 0u); }
    return;
  }
  // UNIFORM: same 4096 hash evaluations the dense path does, but the word
  // comes from synthWord instead of memory. Spread across all 64 threads
  // exactly as the dense loop does — there is no reason to serialize it.
  let w = synthWord(e);
  let v = (w & 0xFFFFu) | ((w & STAIN_BITS) >> 8u);
  let base = wg.x * CHUNK_VOL;
  var h = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) { h += pcg((base + i) ^ (v * 0x9E3779B9u)); }
  if (T.hashEnable != 0u) { atomicAdd(&wgHash, h); }
  workgroupBarrier();
  if (li == 0u) {
    occupancy[wg.x] = packOcc(CHUNK_VOL,
        select(0u, CHUNK_VOL, isRayBlocker(materials[mat])));
    if (T.hashEnable != 0u) { atomicAdd(&worldHash[0], atomicLoad(&wgHash)); }
  }
  return;
}
// ... existing dense path, with `base` resolved from the page index ...
```

Note the `wgHash` workgroup atomic must be zeroed in the same `li == 0`
prologue the existing entry point already has (`sim_occupancy.wgsl:57-62`) —
i.e. the sentinel branch is taken *after* that prologue and its
`workgroupBarrier`, not before it.

**Is this bit-identical to a materialized chunk?** For `EMPTY`: yes,
trivially and provably — both contribute nothing to any of the three
accumulators. For `UNIFORM(mat)`: yes, **provided `synthWord(e)` is exactly the
word a materialized page would hold**, which §3.7 guarantees by construction
(the materializing fill uses the *same* `synthWord`). The two paths share one
function; that is the mechanism, not a coincidence to be re-verified.

**Two things the analytic UNIFORM path costs, honestly stated:**

- **`EMPTY` is free; `UNIFORM` is not.** The EMPTY branch does no work at all
  and the whole-world hash tick gets *cheaper* in proportion to sky — 84.8% of
  chunks skipped is a direct cut into the 95–98 µs full-scan that ROADMAP §3.4
  lists as a rule-2 violation. That is a real side benefit and should be
  measured (§8 commit 6), but it is a **consequence**, not a goal, and it must
  not be used to justify the phase if the memory number disappoints.
- **`UNIFORM` is not a closed form.** The `pcg((base+i) ^ k)` sum over `i` has
  no algebraic shortcut, so a UNIFORM chunk still costs 4,096 `pcg` evaluations
  on hash ticks — it saves the 16 KiB of memory *traffic*, not the ALU. Spread
  across 64 threads as above it is the same shape as the dense path minus the
  loads, so it should be slightly faster, not slower.

**The alternative — materialize before hash ticks — is rejected.** It would
mean the world periodically becomes dense every 15 ticks, which is the phase
undoing itself, and it would make the hash tick a materialization storm. The
only argument for it is "the analytic path might diverge", and the answer to
that is the shared `synthWord` plus the §4.4 gate, not abandoning sparsity.

**`mainDirty` needs the same treatment**, and it is easier: it is dispatched
over `dirtyList`, and every chunk in `dirtyList` is in the materialization set
by §3.2 — **so `mainDirty` can never see a sentinel**. It should nonetheless
handle one, by taking the same analytic branch, because "can never" arguments
that are load-bearing deserve a cheap belt. Under `SANDVOX_PAGE_ASSERT` the
branch increments `pageFaults` — a sentinel in the dirty list means §3.2's
closure argument is broken, and that is exactly what we want to hear about.

### 4.2 The page table is derived state and is never serialized

Stated in the header of whatever file owns it, in the `farfield.h` style the
repo already uses:

> Derived data only: the page table is a physical-layout index, not world
> state. It is not hashed, not persisted, and not replicated. It is rebuilt
> from the chunk contents on every load, stream-in and worldgen.

Consequences, each checked against the existing machinery:

- **Saves are unchanged.** `kPersistMask` (`stream.h`) still strips bits 16..23
  and RLE-encodes 32-bit words (`stream.cpp:36-49`). RLE already compresses a
  uniform chunk to a **single 2-word pair** — `{4096, w}` — so the save format
  needs *no* change to benefit; a sentinel chunk and a materialized uniform
  chunk produce byte-identical RLE. That is a strong consistency check in
  itself, and §4.3 uses it.
- **Eviction is unchanged in shape but gains a fast path.**
  `Stream::EvictSlots` copies 16 KiB per slot to staging
  (`stream.cpp:170-172`). For a sentinel slot **there is nothing to copy**: the
  CPU already knows the chunk's entire content from the table entry and can
  synthesize the RLE (`{4096, synthWord}`) without touching the GPU. This
  removes the largest single source of streaming traffic on a shift plane that
  is mostly sky — a real win, and it drops the copy from the tracked path,
  which §5 must account for.
- **Nothing in the MutationQueue changes.** Ops are world-coordinate; the page
  table is invisible to them. Rule 3 is untouched.

### 4.3 A free consistency check

Because RLE round-trips a uniform chunk identically whether it was sentinel or
materialized, the existing `region-store` and `save-load` gates already exercise
the equivalence — a world saved paged and loaded dense (or vice versa) must
produce the same hash. Assert it explicitly in the new gate rather than relying
on it implicitly.

### 4.4 The gates

**Gate A — paged-vs-dense hash equality on the loud scenario.** Reuse
`--vk-smoke-loud`'s 19-probe driver verbatim, per the phase-6 handoff. The
scenario is already exactly what this phase needs to stress: brush + melt ops,
three explosions at t45/t52/t75 (the mark/apply split, the whole particle
chain), exact-cell stamps anchored to `world.WindowOrigin()`, the readback ring
live every tick, and an **8-shift streaming walk with eviction and procgen
refill** at t85–t100. That last leg is the page alloc/free/realloc path under
streaming, for free.

The comparison machinery in `src/gpu/vk_smoke.cpp` is already backend-agnostic
once labelled: `RunScenario(kind, loud, ...)` at `:128` and
`CompareAndReport(name, a, b, validation)` at `:214`. What needs changing:

- `RunScenario` gains a residency mode parameter (see §6 for the flag).
- `CompareAndReport`'s hardcoded `"  stage              Dawn         Vulkan\n"`
  header (`:228`) and `KindName` (`:123`) need label parameters. Small, and the
  right change regardless.

**One scenario gap to close.** The loud scenario's explosions are placed
relative to the window origin but not specifically at a **sky boundary**, which
is the case the checkpoint calls for: an explosion whose blast box spans
materialized terrain *and* sentinel sky, so `explodeMark` reads sentinels and
`explodeApply` writes into chunks that were sentinels one tick earlier. Add one
explosion at `windowOrigin + (kNChunk/2, terrainTop + 2, kNChunk/2)` in world
cells — deliberately straddling the surface — and a probe on the tick after.

**Gate B — a new `page-roundtrip` selftest gate.** Registered as a row in
`src/test/selftest_sim.cpp`'s `SimGates()` **and** a name in `kOrder`
(`selftest.cpp:47-54`) — both, or it prints a warning and never runs. What it
asserts, in order:

1. **Alloc**: paint a brush ball into provably-empty sky. Assert the target
   chunks' page-table entries went from `PT_EMPTY` to resident, and
   `pagesInUse_` rose by exactly the number of chunks the ball touches.
2. **Content**: read back the chunk and assert the words match a dense run's.
3. **Free with hysteresis**: erase the ball (brush with air), tick
   `kPageFreeTicks + 2`, assert the pages returned to the free list and
   `pagesInUse_` fell back. Assert it did **not** fall before the threshold —
   the hysteresis is the thing under test, and a gate that only checks the
   end state would pass with hysteresis removed.
4. **Realloc**: repaint, assert reuse (LIFO ⇒ the same page indices).
5. **Streaming**: shift the window so those slots evict and refill; assert a
   store-hit refill of an all-air chunk sets `PT_EMPTY` and uploads **zero
   bytes**, and that a refill of a mixed chunk materializes.
6. **Explosion at a sky boundary**: assert the hash matches a dense run.
7. **`pageFaults == 0`** under `SANDVOX_PAGE_ASSERT` for the whole gate. This
   is the assertion that turns §2.4's structural claim into evidence.

The gate must anchor to `world.WindowOrigin()`, not a fixed world position —
gates run in `kOrder` sequence and the streaming gate leaves the origin ~20
chunks out (the documented trap that made the spell gate detonate on tick 1).

**Gate C — the existing suite, unchanged.** 23 gates green on both backends,
`determinism` reporting the pinned `7cfa2420`, both known failures failing the
same way. If any gate's *detail string* changes, the phase changed behaviour.

---

## 5. Pass table and barriers

The phase-6 handoff's warning is exact and applies here verbatim: **a missed
`uses` entry means the generator emits no barrier for that hazard, and it is
invisible under Dawn.** Everything in this section lands in the *same commit*
as the shader change that needs it.

### 5.1 New buffer ids

`pass::Buf` (`src/sim/pass_table.h:29-65`) gains **`PageTable`** and
**`PageFaults`** before `kCount`.

`PageFaults` (16 bytes, an atomic counter, §2.4) is a *diagnostic* buffer, but
it is written by sim kernels and therefore is not exempt from any of this: it
needs its `Buf` id, its binding, and an `A(PageFaults)` use on every row that
can write it — which is every row that calls `voxStore`. Declaring it
unconditionally (rather than only when `PAGE_ASSERT` is on) is what keeps the
bind-group layout, the pass table and the checker all agreeing in both modes,
which is worth 16 bytes.

Each enumerator obliges four edits, all mechanical, all checked:

1. `enum class Buf` in `pass_table.h` — the enumerator itself.
2. `Simulation::PassBuffer`'s switch in `simulation.cpp` — the one place a
   `Buf` id maps to a live `rhi::Buffer`. `check_pass_table.py` re-parses the
   enum out of the header and fails on a `.def` row naming an unknown id.
3. `BUF_TO_WGSL` in `scripts/check_pass_table.py:113-151` —
   `"PageTable": {"pageTable"}`, `"PageFaults": {"pageFaults"}`. Without it the
   checker FAILs at `:470-473` ("uses buffer id 'X', which is not in this
   script's BUF_TO_WGSL map"), which is the checker doing its job.
4. `LAYOUT_BINDINGS` and its constituents `_SIM_GROUP0` (`:166`),
   `_SLIM_GROUP0` (`:171`), `_FAR_GROUP1` (`:174`) — the WGSL name must appear
   in every layout that binds it, or the checker FAILs at `:542-546` ("uses
   binding 'n', which \<layout\> cannot bind").

**Confirmed: the rooted walk handles it with no changes.** The checker maps
`Buf` name → WGSL *identifier* names and walks the call graph rooted at each
entry point. Since `pageTable` is referenced from `voxWordAt`/`voxWordIndex` in
`common.wgsl`, and `common.wgsl` is prepended to every shader, the walk will
see the reference **transitively from every entry point that calls a voxel
accessor** — which is precisely the set of rows that need the `uses` entry.
That is the checker working as designed, and it means the checker will *tell
us* if we miss a row rather than us having to enumerate them by hand.

**One thing to watch:** the walk being transitive means `sim_compact`, which
touches no voxels, must **not** acquire a spurious `R(PageTable)`. It won't, as
long as `common.wgsl`'s accessors are the only referents and `sim_compact`
calls none of them — but this is exactly the "finding 2" over-declaration shape
the checker caught twice before, so verify the checker stays silent on
`sim_compact` and `sim_particle:args1/args2`.

### 5.2 Binding placement

`pageTable` must be bound in **group 0**, alongside `voxels`, in both `simBGL_`
(binding 17 — the first free slot after `genList` at 16) and `simSlimBGL_`
(binding 5, extending 0–4).

**Dawn's limit is 16 storage buffers per stage across ALL bind groups in one
pipeline layout** (barrier_graph §4.10 — it is why `simSlimBGL_` exists at
all). Vulkan does not care
(`maxPerStageDescriptorStorageBuffers` = 1,048,576, phase 3a), but Dawn must
keep building or the oracle is gone. Counted from `simulation.cpp:82-121` and
`:209-219` — **storage entries only**, uniforms do not count:

| layout | groups | storage today | +`pageTable` +`pageFaults` | margin |
|---|---|---|---|---|
| `simPL_` | `simBGL_` | 14 (bindings 0,1,2,3,6,7,8,9,11,12,13,14,15,16) | **16** | **0** |
| `simPL2_` | `simSlimBGL_` (4) + `particleBGL_` (8) | 12 | **14** | 2 |
| `farPL_` | `simSlimBGL_` (4) + `farBGL_` (4) | 8 | **10** | 6 |

**`simPL_` lands at exactly 16 of 16 — the limit, with zero margin.** It fits,
and nothing else ever will. That is too tight to land deliberately, and it
changes a decision:

> **`pageFaults` goes in `simSlimBGL_`, not `simBGL_`.** `simSlimBGL_` is
> bindings 0–4 of the *same* group 0 and is a strict prefix of `simBGL_`'s
> layout — so if `pageFaults` takes `simSlimBGL_` binding 6 (after `pageTable`
> at 5) it must also occupy `simBGL_` binding 6, which is `opsBuf`. It does not
> fit as a prefix.

**[JUDGMENT] Resolution: make `pageFaults` a `PAGE_ASSERT`-only binding after
all, and accept the two-layout cost — but pay it in the ONE place it is
cheap.** The assert build is a developer configuration, not a shipping one, so:
`PAGE_ASSERT` off (the default, and what every normal run and most gates use)
declares no `pageFaults` binding and the layouts are the table above minus one
column — `simPL_` at **15 of 16**, one slot spare. `PAGE_ASSERT` on adds the
binding at `simBGL_` 18 / `simSlimBGL_` 6, putting `simPL_` at 16 of 16, which
is legal and is only ever built in the assert configuration.

The cost is real and must be named: **the bind-group layout now depends on a
flag**, which is a "two things that must agree" shape. Contain it by deriving
both the layout entry and the WGSL declaration from the same `PAGE_ASSERT` C++
constant that generates the prelude, so there is one condition, not two. And
run the `page-roundtrip` gate (§4.4 Gate B) in the assert configuration as its
*normal* mode, so the assert layout is exercised on every suite run rather than
rotting.

**Either way, `simBGL_` has room for at most one more storage buffer after this
phase.** Note it on the board and in a comment above `simBGL_`'s entry list —
see risk 7.

**[JUDGMENT]** If a reviewer would rather not spend the last slot, the
alternative is binding `pageTable` in group 1 of each layout instead — it fits
with more room there (3 and 7 spare) at the cost of a less uniform layout and
an extra `@group(1)` in `common.wgsl`'s accessors, which is awkward precisely
because `common.wgsl` is shared by shaders whose group 1 differs
(`particleBGL_` vs `farBGL_`) — the accessor would have to be declared per
shader rather than once. **That awkwardness is decisive: keep it in group 0.**
It is the reason the shared-accessor design in §2.1 works at all.

### 5.3 The `uses` rows

Every row whose entry point reads a voxel gains `R(PageTable)`. It is a **read
in every sim row without exception** — nothing on the tick path writes it.

| row | add | why |
|---|---|---|
| `mutate` | `R(PageTable)` | `voxWordAt` / `voxWordIndex` |
| `mutateCells` | `R(PageTable)` | `op.cellIdx` is a slot index; §5.5 |
| `explodeMark` | `R(PageTable)` | reads neighbours, may be sentinels |
| `explodeApply` | `R(PageTable)` | reads and writes |
| `ca` (×54) | `R(PageTable)` | the big one |
| `particleIntegrate` | `R(PageTable)` | reads voxels along the flight |
| `particleResolve` | `R(PageTable)` | reads and writes |
| `occupancyFull` | `R(PageTable)` | §4.1's analytic branch |
| `occupancyDirty` | `R(PageTable)` | same |
| `pick_hash`, `pick_dirty` | `R(PageTable)` | `sim_pick` reads voxels |
| `farDown` | `R(PageTable)` | reads voxels to downsample |
| `worldgen`, `worldgenList` | `R(PageTable)` | `genChunk` resolves a base |
| `lr_occupancyFull`, `ho_occupancyFull` | `R(PageTable)` | same as `occupancyFull` |

Rows that must **not** gain it: `compact`, `compactNext` (dirty flags only),
`particleArgs1`/`Args2` (counts only), `farFill` (writes cascades from
worldgen, reads no voxels), and every Fill/Copy row.

**`A(PageFaults)` — only in the `PAGE_ASSERT` configuration, and only on rows
that WRITE voxels**: `mutate`, `mutateCells`, `explodeApply`, `ca`,
`particleResolve`, `worldgen`, `worldgenList`. Reading a sentinel is legal, so
read-only rows can never fault. This makes the `.def` conditional on a build
flag for the first time — **[JUDGMENT]** the cleanest way to express that is a
`C_PAGE_ASSERT` condition on a *separate* set of rows rather than an `#if`
inside a row's `USES(...)`, since a row whose condition is false is skipped
entirely and touches no buffer state (§3.9 of the barrier graph). But
`pageFaults` is written by the *same* dispatch, not a separate one, so that
does not work either. The honest answer is an `#if` around the use in the
`.def` and a matching branch in `check_pass_table.py`'s parser. **This is the
ugliest consequence of the assert design and the reviewer may prefer to drop
the shader-side counter entirely** and rely on hash divergence plus the
CPU-side `pagesInUse_` invariants to catch risk 1. I keep it because "provably
zero page faults" is a much stronger statement than "the hash happened to
match", and risk 1 is the phase's top risk.

**Barrier consequence, and it is benign.** `pageTable` is written only by
uploads and fills that drain at the *head* of a command buffer (§5.4), and read
by ~20 rows thereafter. The §3.3 tracker emits **one** TRANSFER→COMPUTE barrier
at the first reading row and **nothing** for the rest (read-after-read emits
nothing). In the CA loop specifically it emits nothing after iteration 0, for
the same reason `materials` and `reactions` do — and the CA's global barrier
(form B, `SHADER_STORAGE_READ|WRITE` at `COMPUTE_SHADER`) covers `pageTable`'s
access domain anyway.

**One thing the addition genuinely changes**: `ca`'s `uses` count goes from 9
to 10. `kMaxUses = 10` (`pass_table.h:164`), and `AllUsesFit()` is a
`static_assert` at `pass_table.cpp:154`. **`ca` lands exactly on the ceiling.**
Raise `kMaxUses` to 12 in the same commit; do not land at exactly the limit and
leave the next person to discover it.

### 5.4 The new commands: fills and page-table writes

Two new kinds of GPU command, both off-table:

**(1) Page-table entry writes.** Small `queue.WriteBuffer(pageTable, slot*4, …)`
— 4 bytes, or a contiguous run. These are Class A `vkCmdUpdateBuffer` under
barrier_graph §4.1's size rule (≤ 64 KiB; the whole buffer is 128 KiB, so a
*whole-table* rewrite would be Class B — which happens only at `LoadWorld`, and
that path already drains). They join the pending-upload queue and drain at the
head of the next command buffer, in issue order, exactly like the three
existing per-slot streaming writes (`occupancy` @ `slot*4`, `dirty[0/1]` @
`slot*4`, `stream.cpp:270-273`) — same shape, same mechanism, no new machinery.

**(2) Page initialization fills.** `vkCmdFillBuffer(voxels, pageIdx*16 KiB,
16 KiB, pattern)`. These are **not** uploads — they are GPU commands with a
destination offset chosen at runtime, so they cannot be a table row (a
`pass::Row` encodes offsets as literal constants; this is exactly the
`EncodeReadbacks` situation phase 3c ruled on). **They go through
`CommandEncoder::FillTracked(pass::Buf::Voxels, …)`**, the phase-4a entry point
built for precisely this: the hazard is derived from the tracker rather than
hand-placed, and under Dawn it is byte-identical to a plain `ClearBuffer`.

**The ordering requirement, stated as the hazard it is.** For a chunk
materialized this tick, the sequence must be:

```
   fill(page)                                   TRANSFER_WRITE on Voxels
   → pageTable[slot] = page                     TRANSFER_WRITE on PageTable
   → the consuming dispatch                     STORAGE_READ|WRITE on both
```

Both writes must be visible to the dispatch, and the *fill must not be
reordered after* a dispatch that reads the page. Two sub-cases:

- **The `pageTable` write** rides the pending-upload queue and drains at the
  command buffer head, ahead of every row. The tracker marks `PageTable`
  written, and the first row declaring `R(PageTable)` gets a
  TRANSFER→COMPUTE barrier automatically. Nothing new.
- **The fill** must be recorded in the **same command buffer, at the head,
  before the first row** — not in a separate submit, and not interleaved with
  rows. `FillTracked` declares `TransferWrite` on `Voxels`; the first row with
  `RW(Voxels)` then gets a derived TRANSFER→COMPUTE barrier. **The requirement
  on the implementation is that page fills are recorded before
  `Recorder::RecordTable` begins**, i.e. in the same slot the upload flush
  occupies.

  **[JUDGMENT] I recommend routing the fills through the pending-upload queue
  too**, as a third payload class alongside Class A and Class B — call it a
  "queued fill", which is *exactly what phase 4a already built* for
  zero-init (`CreateBuffer` queues a whole-buffer fill that drains at the head
  of the next command buffer, barrier_graph §4.8 as-built). The machinery
  exists; page materialization is the same shape at a smaller granularity.
  This also inherits phase 4a's intra-flush WAW barrier for free — and that
  matters, because a page freed and immediately reallocated within one tick
  would produce two fills to the same range in one flush, which is precisely
  the repeat-destination case that barrier handles.

**The `--shot` far-fill loop and the streaming genList submit** both record
command buffers outside the tick. Each opens with the §3.4 head global barrier
and drains the pending queue, so a page fill enqueued before either of them
lands correctly. No special case.

### 5.5 `mutateCells` and the slot-index assumption

`sim_mutate:cells` takes `op.cellIdx` as a **slot** cell index and stores
`voxels[op.cellIdx]` directly (`:97,99`). Under paging that index is no longer
a physical word index. The fix is mechanical — decompose to a chunk index and a
local offset (the entry point *already does this decomposition* at `:101-106`
to reconstruct the world cell for `markBoth`) and translate:

```wgsl
let ci = op.cellIdx / CHUNK_VOL;        // slot chunk index — table index directly
let lo = op.cellIdx % CHUNK_VOL;
let e  = pageTable[ci];
if ((e & PT_SENTINEL_BIT) != 0u) { /* page fault: no-op + assert */ return; }
voxStore(e * CHUNK_VOL + lo, word);
```

The `CELLOP_IF_AIR` read (`:96`) reads through the same resolve, and reading a
sentinel there is *legal and correct* — a paint-into-air op against an EMPTY
chunk should see air and proceed, which means the CPU must have materialized
that chunk (§3.3 does, from `op.cellIdx / CHUNK_VOL`). Good: the same
decomposition the CPU uses to build the materialization set is the one the
shader uses to index the table.

### 5.6 The eviction fast path removes a tracked copy

§4.2 notes that a sentinel slot needs no eviction copy. That *removes*
`CopyTracked` calls from `Stream::EvictSlots`, which is hazard-reducing and
needs no table change — but it does change the recorded command count, which
`--vk-smoke-loud` prints ("20 rows, 64 dispatches, 38 copies, 5 fills, 104
barrier calls"). Expect those numbers to move and update the as-built record;
do not treat a changed copy count as a regression signal.

---

## 6. Dawn oracle strategy

Dawn stays. Phase 6 is explicit that its job is to *disagree*, and it has
already earned that twice in this port (the phase-4a material-table WAW and the
micro-body pool fill). Removing it while landing the riskiest remaining change
would be exactly backwards.

### 6.1 The equality matrix

Three configurations, one hash sequence:

| # | configuration | role |
|---|---|---|
| 1 | **vulkan-paged** | the new thing |
| 2 | **vulkan-dense** | isolates *paging* from *Vulkan* |
| 3 | **dawn-dense** | the oracle, unchanged, pinned to `7cfa2420` |

All three must produce identical hashes at every probe. The pairwise diffs are
what make a failure attributable, and this is the whole reason for keeping
three rather than two:

- **1 ≠ 2, 2 == 3** → the bug is in paging. The common case, and the one we
  want to be easy.
- **1 == 2, 2 ≠ 3** → the bug is in the Vulkan backend and predates this phase.
- **1 ≠ 2 ≠ 3** → two bugs, or a shader-prelude change that affected both.

### 6.2 The mechanism for the paged/dense switch

Options, and the decision:

- **A compile-time flag.** Rejected: it makes 1 and 2 different binaries, so a
  comparison is between builds, which is the thing `tests/baseline.json`'s
  golden hash exists because we could not trust.
- **A runtime `--residency dense|paged` flag** that selects a `World`
  configuration at init. **Recommended.** Concretely: `kPoolPages` becomes a
  runtime value, `dense` sets it to `kNumChunks` and pre-materializes every
  slot at worldgen (page `i` for slot `i`, so the page table is the identity
  map and translation is a no-op that still executes), `paged` runs the real
  allocator.

**The identity-map property is the design's best gift and §8 leans on it
entirely.** In dense mode the page table is `pageTable[i] = i`, so
`voxWordAt(c) == voxels[cellIndexW(c)]` for every `c` — *the exact same
physical address*. Dense mode therefore exercises the **whole translation
path** (the load, the branch, the multiply-add) while producing bit-identical
addresses to today's code. That means:

1. Commit 1 of §8 can land translation with `dense` as the only mode and prove
   the hash unchanged, before a single sentinel exists.
2. Any hash divergence between paged and dense is *definitionally* about
   sentinels and page assignment, never about the arithmetic.

**Naming.** `--residency` rather than `--paged`/`--dense` as separate flags, so
the two modes are one variable with a total order of values — the phase-6
lesson about `bool backendVulkan` (a flag named for the non-default cannot
express a default flip).

### 6.3 Does dense-Vulkan survive after validation?

**Decision: yes, permanently, as a debugging mode — but it is not a fallback.**

The distinction matters. A *fallback* implies the engine might choose it at
runtime on some condition, which would mean two live residency behaviours in
the field and two memory profiles to reason about. That is a second code path
that will rot. `--residency dense` is a **developer flag**: always available,
never automatic, and its value is that it makes any future "is this a paging
bug?" question a one-flag experiment instead of a bisect.

The cost of keeping it is nearly zero — it is a different `kPoolPages` and a
pre-materialization loop, not a second implementation of anything. Compare to
`--barriers=sledgehammer`, which phase 3b kept for exactly this reason and which
has since been used as an A/B oracle repeatedly.

**Dawn's removal**, per phase 6, is unblocked once phase 7's checkpoints are
green — but it is a *separate cleanup commit*, and I would sequence it after
the phase-7 measurements are recorded, so that the phase's evidence was
produced with the oracle still present.

---

## 7. Risk register

Ranked by (probability × cost of a late discovery).

### Risk 1 — a write reaches an unmaterialized page

**Why dangerous.** It is the determinism nightmare in its purest form. The
write either lands nowhere (a lost voxel) or, in a naive implementation, lands
in *another chunk's* memory (a corrupted bystander). Either way the world hash
diverges at a tick and a location with no causal relationship to the bug, and
under Dawn-dense it cannot reproduce at all.

**How neutralized — structurally, not by care.**

1. **There is no writable accessor that accepts a sentinel.** `voxWordIndex`
   returns `PT_NO_WORD` and `voxStore` tests for it *before* indexing. The
   worst case is a no-op, never a stranger's voxel. This is a property of the
   function signatures, not of anyone remembering a rule (§2.4).
2. **`PT_NO_WORD` is not a valid index**, so even a hypothetical unguarded
   `voxels[PT_NO_WORD]` is an out-of-bounds access that WGSL clamps, rather
   than a plausible-looking in-range address.
3. **`SANDVOX_PAGE_ASSERT` counts every occurrence** into a `pageFaults`
   atomic, converting "impossible" into "measured zero".
4. **§3's materialization set is a proven superset**, with the corner-chunk
   correction (26-neighbourhood, not 6) and the incremental CPU mirror that
   never under-approximates.

**What test.** Gate B step 7 asserts `pageFaults == 0` across the whole gate,
including the sky-boundary explosion. The full 23-gate suite runs with the
assert enabled in a dedicated CI-style invocation. And the loud scenario's
paged-vs-dense hash equality is the end-to-end detector: a lost voxel moves the
hash.

**Residual.** The assert is off in production, so a page fault in a scenario no
gate covers is silent until it moves a hash. Mitigation: leave the assert
cheaply enableable (a prelude define, no rebuild) and name it in the
troubleshooting section of CLAUDE.md.

### Risk 2 — the one-tick-late dirty knowledge is insufficient

**Why dangerous.** This is the risk I spent the most of §3.2 on because the
brief's proposed rule (materialize `dirtyOut(last tick) ∪ neighbours`) is
**refuted**: the readback ring can decline a tick entirely
(`world.cpp:110-113`), the map is async and the frame runs up to 4 ticks, so
the CPU's newest `dirtyFlags` can be many ticks stale. A stale set
under-approximates, and under-approximation is risk 1.

**How neutralized.** The CPU maintains `cpuDirty` **incrementally** rather than
inferring it: one 26-ring dilation per tick plus CPU op targets, reset to
ground truth whenever a snapshot actually arrives (§3.2). It cannot
under-approximate, because the recurrence is the closure lemma applied one tick
at a time; it over-approximates by one ring per missed snapshot and self-heals
on the next.

**What test.** A gate variant that **deliberately starves the readback ring**
— hold all three slots in flight for 10 consecutive ticks while an active fire
spreads — and asserts the hash still matches dense. This is the single most
valuable new test in the phase, because it is the only one that exercises the
conservative path rather than the exact one. Without it, every gate runs with a
healthy ring and the fallback is dead code that has never executed.

**Residual.** The `particleResolve` landing set (§3.4) is bounded by
`TUNE_PART_MAX_VEL`, which is hot-reloadable. Derive the dilation radius from
the live tuning value on every reload, and let the assert catch the rest.

### Risk 3 — hash-tick synthesis diverges from materialized content

**Why dangerous.** A paged world and a dense world would hash differently for
the *same* logical state — the phase's central claim fails, and every save
becomes suspect because a load could change residency.

**How neutralized.** One function. `synthWord(entry)` is used by (a) the read
accessor, (b) the analytic hash branch, and (c) the `vkCmdFillBuffer` pattern
when a sentinel is materialized. Three consumers, one definition — the repo's
standard answer to "two places that must agree". For `EMPTY` the claim is even
stronger: the hash loop skips air entirely, so an EMPTY chunk contributes
literally nothing and there is no arithmetic to get wrong.

**What test.** Gate B step 6, plus a targeted assertion: for a chunk that is
UNIFORM, force materialization, hash, force demotion, hash again, assert equal
(the same world, two representations). That is a much tighter test than the
end-to-end scenario and will localize a divergence to this function.

**Residual.** The state nibble (§2.3). If a promotion rule ever compares
*materials* instead of *whole words*, a chunk with mixed palette variants would
be promoted to a uniform sentinel and every cell's state nibble would silently
change. **Promotion is by whole-word equality, and the test above is what
proves it.**

### Risk 4 — pool exhaustion mid-tick

**Why dangerous.** Any behaviour change on exhaustion that alters voxel state
is hash-relevant. A non-deterministic exhaustion order (e.g. "whichever
allocation happened to be last") makes two machines diverge under load.

**How neutralized.** §3.8: a fixed three-tier priority by provenance, ascending
slot index within a tier — a pure function of the tick's inputs. The clamp
lands on the over-approximation ring, where refusal is *free* by construction.
Tiers 1–2 exhausting is a logged hard error that fails the selftest, not a
silent fallback. Pool sized at 1.65× the measured steady state, with
`pagesHighWater_` reported.

**What test.** A gate that sets `kPoolPages` artificially low (a runtime knob
alongside `--residency`) and asserts: (a) the refusal order is identical across
two runs, (b) the log line fires, (c) `pageFaults` accounts for exactly the
refused-ring writes. Plus `--measure` reporting the high-water mark on the loud
scenario, so the margin is a number rather than a hope.

### Risk 5 — freeing a page that an in-flight readback still refers to

**Why dangerous, and it is subtler than it looks.** The eviction ring
(`stream.cpp:159-195`) holds up to 4 batches of 256 chunks each, mapped and
read *ticks* later — `CompleteOldest` is explicitly asynchronous, and the whole
`pendingChunks_` mechanism exists because a chunk's eviction can still be in
flight when the player doubles back. The readback ring is 3 deep with the same
property. If a page is freed and reallocated to a different chunk while an
eviction copy of the *old* chunk is outstanding, the copy reads the *new*
chunk's data and the store silently records the wrong world.

**Critically: the existing code is safe against this only because slots never
move.** Today a slot's 16 KiB is at a fixed offset forever, so an eviction copy
reads that offset and gets that slot's data, whatever happened to it. Paging
breaks that assumption: a chunk's physical location becomes mutable.

**How neutralized.**

1. **The eviction copy is issued eagerly, before the fill** (`stream.cpp:192`,
   and the corrected comment at `:179-191` says exactly why). The GPU-side read
   is therefore ordered before any reallocation *command*. What is not ordered
   is the CPU-side free.
2. **A page is not returned to the free list while any outstanding operation
   references it.** The free list gains a **retire queue**: a freed page is
   parked with the current submit serial and only becomes reusable once that
   serial has retired. This is precisely the mechanism phase 4a already built
   for released staging buffers ("released staging is freed through a
   serial-stamped graveyard once in-flight submits retire") — reuse it rather
   than inventing a second one.
3. **§4.2's eviction fast path sidesteps most of it entirely**: a sentinel slot
   is not copied at all, so it has no in-flight reference to have.

**What test.** The loud scenario's 8-shift streaming walk with the store-hit
round-trip self-check already exercises evict-then-refill with real latency,
and it reported 100% restoration in phase 3c. Extend the `page-roundtrip`
gate's step 5 to shift the window **while an eviction is deliberately left
outstanding** (do not drain), then assert the store contents.

**Residual.** `Stream::FlushResident` (`stream.cpp:293-298`) evicts all 32,768
slots and drains — a save. With paging, the sentinel fast path makes most of
those free, which is a win, but the mixed ones still queue 4 MiB batches.
Unchanged in shape; worth confirming the save gate's timing does not regress.

### Risk 6 — materialize/demote oscillation that never sleeps

**Why dangerous.** Rule 2. If a chunk at an activity boundary materializes and
demotes every few ticks, the world never settles: page fills are GPU commands,
so an oscillating boundary means a settled-looking world issuing fills forever
— a rule-2 violation that presents as a perf mystery.

**How neutralized.** The free condition is a **conjunction** (§3.6): `occTotal
== 0` for `kPageFreeTicks` consecutive snapshots **AND** the slot is not in
`cpuDirty`. The second conjunct makes oscillation structurally impossible
rather than merely slow: `cpuDirty` is exactly the materialization set, so a
chunk cannot be eligible for freeing and scheduled for materialization on the
same tick. The oscillation requires a chunk to leave `cpuDirty`, stay empty for
8 snapshots, and *then* be written — which is not oscillation, it is an event.

**What test.** The `sleep` gate already asserts a settled world stays under 32
active chunks (currently 4). Extend it to also assert **zero page fills and
zero page frees over the last 50 ticks of the settle**. That is the direct
statement of "costs nothing when idle" for this subsystem, and it is the
assertion that would catch a demotion policy that never converges.

### Risk 7 — the Dawn storage-buffer layout limit (COUNTED: it fits, barely)

**Why dangerous.** A blocked Dawn build removes the oracle at the exact moment
it is most needed.

**Status: measured, not a risk to this phase.** §5.2 counts all three pipeline
layouts: `simPL_` goes 14 → **15** of Dawn's 16, `simPL2_` 12 → 13, `farPL_`
8 → 9. It fits.

**The residual risk is for the phase AFTER this one.** `simBGL_` will have
exactly **one** free storage slot left, and the next feature that wants a group-0
sim buffer — cell-level active masks (ROADMAP §3.1) is the obvious candidate,
and it wants a per-chunk bitmask buffer — takes the last one. After that, Dawn
cannot bind the layout and the oracle dies, or `simSlimBGL_`-style splitting has
to be repeated. Say so in a comment above `simBGL_` and in the ROADMAP §3.1
entry, so it is discovered at design time rather than at link time.

**What test.** The Dawn build itself, at commit 1 of §8 — the earliest possible
discovery point, which is one of the reasons translation lands before sentinels.

### Risk 8 — translation cost on the CA hot path

**Why dangerous.** The CA is 54 dispatches over the dirty list. An extra
dependent load per chunk entered, on the hottest path in the engine.

**How neutralized.** The table is 128 KiB and cache-hot; a workgroup handles
one chunk so the own-chunk entry is one broadcast load for 216 threads. If
measurement says otherwise, hoist it to a workgroup-uniform `let` (legal,
value-preserving).

**What test.** `--measure` before and after, on all three scenarios (settling /
active / settled), reported in §8's final step. This is a perf risk, not a
correctness one, and it is the one thing in this document that could make the
phase not worth landing as designed.

---

## 8. Implementation plan

Ordered commits, each independently verifiable, each ending green. The
sequencing principle: **land the translation machinery while it is provably a
no-op, so that when sentinels arrive the only new variable is sentinels.**

Every checkpoint = `bash scripts/build.sh --selftest` green (pond-freeze and
mob known-failing) **on both backends**, with the actual output in the commit
message. Board discipline per CLAUDE.md: `claim` before editing, `done` with
what landed, `note` for the cross-cutting constants.

---

**Commit 0 — measure what UNIFORM is worth.**
ROADMAP §5.1's ~20-line `--measure` addition: a per-chunk uniformity histogram
(all-air / all-one-word / all-one-material-mixed-state / mixed), from a blocking
whole-buffer read in the measurement path only. Also fix the stale
`kNChunk`/`kNumChunks` comments in `world.h:29-30` and `vulkan_pass_map.md §3a`.
*No behaviour change. Gate: trivially green.*
*Output: the number that decides §3.6's scope. Record it in this document.*

---

**Commit 1 — the table and translation, dense-only.**
`pageTable` buffer; `Buf::PageTable` + `PassBuffer` case +
`check_pass_table.py` registration (`BUF_TO_WGSL`, `LAYOUT_BINDINGS`);
`kMaxUses` 10 → 12; `R(PageTable)` on every voxel-reading row; bindings in
`simBGL_`/`simSlimBGL_`; `voxWordAt`/`voxWordIndex`/`voxStore`/`synthWord` in
`common.wgsl`; every sim shader's voxel access rewritten to use them, including
`mutateCells`'s decomposition (§5.5) and `sim_occupancy`/`worldgen`'s
chunk-base resolve. **The table is initialized to the identity map
(`pageTable[i] = i`) and nothing ever writes it.** No sentinels exist.

*This is the largest commit and it is deliberately the one with no semantic
content.* Every address computed is bit-identical to today's (§6.2).

*Gate: full suite green on both backends; `determinism` reports `7cfa2420`;
`--vk-smoke-loud` 19/19 MATCH with hashes byte-identical to the phase-3c
record; `check_pass_table.py` OK; `check_invariants.py` OK; zero validation
messages.* Any hash movement here is a translation bug and must be fixed, not
absorbed.
*Also: `--measure` on all three scenarios, to isolate risk 8's cost before
sentinels muddy it.*

---

**Commit 2 — `EMPTY` sentinels, read-only, for provably-never-written chunks.**
`--residency dense|paged` flag. In `paged` mode, a chunk is `PT_EMPTY` **only**
if it was all-air at worldgen and has never been in any materialization set —
which at this commit means: worldgen classifies, and *nothing ever demotes or
materializes*. Any chunk that would need a write is materialized permanently at
worldgen and never freed. `SANDVOX_PAGE_ASSERT` + `pageFaults` land here, and
the gate runs with them on.

This is the smallest possible step that makes a sentinel real: it exercises the
read path, the analytic hash branch (§4.1), and the assert, with **no lifecycle
at all**. The pool is still effectively dense in the worst case, so exhaustion
cannot happen.

*Gate: suite green in both residency modes; paged-vs-dense-vs-Dawn hash
equality on the loud scenario; `pageFaults == 0`.*

---

**Commit 3 — materialization.**
`cpuDirty` (§3.2), the CPU-op target sets (§3.3), the particle set (§3.4), the
free-list allocator, queued page fills through the pending-upload queue (§5.4),
worldgen batching (§3.5c), and the streaming/`LoadWorld` classification
(§3.5d,e). Exhaustion policy (§3.8) with the logged hard error. **No
deallocation yet** — pages are allocated and never freed, so `pagesInUse_` is
monotonic and the pool must hold the union of everything ever touched.

Splitting allocation from deallocation is what makes each half debuggable: at
this commit a hash divergence is definitively an *under*-materialization
(risk 1/2), because nothing has been taken away.

*Gate: suite green both modes; loud-scenario equality including the new
sky-boundary explosion; the ring-starvation test (risk 2) passes; `pageFaults
== 0`; `pagesHighWater_` reported.*

---

**Commit 4 — deallocation with hysteresis.**
The consecutive-zero counter in the snapshot callback, the `!cpuDirty`
conjunct, `kPageFreeTicks`, the serial-stamped retire queue (risk 5), and the
eviction fast path for sentinel slots (§4.2). If commit 0 said UNIFORM is
worth it, streaming/load-path demotion lands here too.

*Gate: suite green both modes; the extended `sleep` gate asserting zero fills
and zero frees over the last 50 settled ticks (risk 6); the outstanding-eviction
streaming test (risk 5); loud-scenario equality.*

---

**Commit 5 — the gates.**
`page-roundtrip` in `selftest_sim.cpp` + `kOrder` (all 7 steps, §4.4 Gate B);
the pool-exhaustion determinism test (risk 4); the ring-starvation test
promoted from a manual run to a gate; `--vk-smoke-loud` gaining the residency
axis and the labelled `CompareAndReport`.

*Gate: 24 gates green on both backends, both residency modes.*

---

**Commit 6 — measurement, docs, and the acceptance record.**
`--measure` reports `pagesInUse_`, `pagesHighWater_`, the pool reservation, and
the sentinel-kind histogram. `PLAN_vulkan_port.md` phase 7 gains its
`[AS BUILT]` block. `DESIGN.md` gains the page table in §3 (it is a residency
mechanism, which §3 owns) plus §14's note that pool exhaustion voids the
determinism guarantee (§3.8). This document gains the measured numbers.
`docs/vulkan_barrier_graph.md` gains a §2.4 note for the queued page fills.

### Final acceptance

| criterion | target |
|---|---|
| paged vs dense vs Dawn, loud scenario | **19+/19+ MATCH**, all three, including the sky-boundary probe |
| `determinism` gate, both backends, both modes | `7cfa2420` over 200 ticks |
| full suite | 24 gates, exit 0, both backends, pond-freeze + mob known-failing with **character-identical** detail strings |
| sync validation | **0 messages** |
| `pageFaults` | **0** across the suite with the assert on |
| resident memory, settled | `pagesInUse_ × 16 KiB` reported against **77.7 MiB** measured / **86.9 MiB** estimated; pool reservation reported separately (§3.7) |
| settled tick | reported against **229–236 µs**; a regression beyond ~5% is a finding, not a footnote |
| `check_pass_table.py`, `check_invariants.py` | silent |

---

## 9. Open questions for the reviewer

Marked **[JUDGMENT]** in place; collected here so none is missed.

1. **§3.5c, worldgen's dense transient.** I recommend batching (16 submits of
   2,048 slots) over a dense-for-one-submit peak, because the latter forfeits
   the saving exactly at startup and becomes impossible at a grown window. Is
   the batching complexity worth it at 512³, or is a 512 MiB startup transient
   acceptable for now?
2. **§3.6, UNIFORM scope.** I defer tick-path uniformity discovery pending
   commit 0's measurement, and implement UNIFORM only where the CPU already has
   the words. If the reviewer believes the 2,338 full chunks are mostly
   single-word, that flips.
3. **§3.7, `kPoolPages = 8192` (128 MiB, 1.65×).** Sized against one seed's
   measurement. Too tight, too loose?
4. **§3.8, exhaustion voids the determinism guarantee.** I chose a loud,
   documented hole over a quiet fallback. This is a rule-1 amendment and needs
   an explicit yes.
5. **§3.4(ii), deriving the particle dilation radius from
   `TUNE_PART_MAX_VEL` at load.** Correct, but it makes a *tuning* value
   load-bearing for *memory correctness*, which is a new kind of coupling in
   this codebase. Better alternative?
6. **§6.3, `--residency dense` as a permanent developer flag.** Cheap and
   useful, or a second path that will rot?
7. **§2.3's translation cost (risk 8).** Unmeasured. If commit 1 shows the CA
   materially slower, is the workgroup-uniform hoist sufficient, or does that
   change the phase's cost/benefit?
8. **§5.2/§5.3, the `pageFaults` counter.** It is the only thing that turns
   "a sentinel write is structurally impossible" into a measured zero, and
   risk 1 is the top risk — but it costs a flag-dependent bind-group layout and
   the first conditional `USES(...)` in `pass_table.def`, both of which are
   shapes this repo has a checker to prevent. Keep it, or rely on hash
   divergence alone? **This is the question I am least confident about.**
9. **§5.2, `simBGL_` at 15 of 16.** After this phase there is one storage slot
   left, and ROADMAP §3.1's cell-level active masks want it. Should this phase
   pre-emptively split `simBGL_` (the way `simSlimBGL_` was split) so the next
   feature is not the one that discovers the ceiling — or is that speculative
   generality that should wait for the feature that needs it?
