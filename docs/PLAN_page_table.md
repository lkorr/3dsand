# PLAN: the software page table for the voxel buffer

Phase 7 of `docs/PLAN_vulkan_port.md`, rewritten per `docs/ROADMAP_scale.md` §1
(user-reviewed): a **flat u32 software page table with `EMPTY` and
`UNIFORM(material)` sentinels**, not `VK_KHR_sparse_binding`, not an octree.

Status: design, 2026-08-22, **revision 2 — post adversarial review**. No code
has been written. Companion docs: `docs/vulkan_barrier_graph.md` (the barrier
design this must extend), `docs/vulkan_pass_map.md` (the buffer inventory),
`src/sim/pass_table.def` (the table this must add `uses` entries to).

**Revision 2 changelog.** Review returned REWORK on 4 critical + 4 major + 5
minor findings, all concentrated in **coverage** — the first draft's mechanisms
were sound where they applied and the failures were things they did not apply
to. Fixed in place, each tagged `[REVIEW …]` at the point of change:

| # | finding | where |
|---|---|---|
| C1 | the snapshot reset **under-approximated** — it assigned where it must intersect | §3.2 |
| C2 | `EncodeWakeAll` dirties all 32,768 chunks and was **entirely unmodelled**; no gate can catch it because the suite freezes the day phase | §3.2a |
| C3 | the **render** bind groups were omitted; `raymarch.wgsl` has 17 raw voxel reads | §5.2a |
| C4 | a whole missed class: **five CPU-side raw slot-offset** accesses to `voxels` | §2.1a |
| M1 | `particleResolve` has **two** write targets; the micro-stain one parks on the contact cell | §3.4 |
| M2 | the `∩ hasMatter` materialization rule, stated as its own rule | §3.2a |
| M3 | the hash must key on the **slot** base while the load uses the **page** base | §4.1 |
| M4 | commit 2 was not independently green — merged with commit 3 | §8 |
| m4 | the LIFO page-reproducibility claim was false; withdrawn | §3.7 |
| m5 | `--shot` inherits C3; `--measure` runs dense-only | §5.4 |

**Revision 3** (verification round; C1's dispute was adjudicated in this
document's favour and the reviewer withdrew their alternative form):

| # | finding | where |
|---|---|---|
| NEW-1 | `EncodeWakeAll` set `cpuDirty` but was never added to `C(N)` — a same-tick tightening could intersect its chunks away | §3.1a (c) |
| NEW-2 | **`Stream::FillSlots` writes `dirty[0]`/`dirty[1]` per refilled slot** (`stream.cpp:271-273`) — a third CPU dirty-writer, entirely missed | §3.1a (d) |
| NEW-3 | `materialize(N)` was enumerated in two places and composed in neither; op targets and particle chunks never composed with the `∩ hasMatter` rule | §3.2 step (4) |
| NEW-4 | the 2-ring write-reach bound and the 1-ring recurrence were both stated, neither reconciled | §3.2 |
| NEW-5 | risk 2 still said "reset to ground truth", contradicting the §3.2 it cited | risk 2 |

Three user decisions are folded in as settled, superseding the first draft:
**exhaustion is a fatal error** (§3.8, Q4 closed), **`pageFaults` is
unconditional and always bound** (§5.1, Q8 closed), and **Dawn is being removed
entirely** (§6; Q9 and risk 7 moot, analysis retained rather than deleted).

Revision 3 fixed a fifth finding of the same shape (NEW-1..5), and its lesson
is the caveat below.

> ## Read this before adding anything to this design
>
> **The reviewer's summary of every hole found across two rounds:** *"`C(N)`
> and the materialization set are each enumerated in more than one place and
> composed in none — every hole found in review so far has been a missing
> contributor, not a wrong mechanism."*
>
> That is the failure mode of this document, and it is worth more than any
> individual fix. The mechanisms — the intersection recurrence, the sentinel
> encoding, the write-path no-op, the analytic hash — have all survived review.
> What kept breaking was **coverage**: `EncodeWakeAll`, the five CPU byte-offset
> sites, the render bind groups, `particleResolve`'s second write target,
> `Stream::FillSlots`' dirty writes. Every one was a contributor nobody had
> written into a list.
>
> So there are now exactly **two** normative lists, and they live in exactly
> one place each:
>
> - **§3.1a** — the CPU dirty-writers, contributors (a)–(d) to `C(N)`.
> - **§3.2's "THE NORMATIVE DEFINITIONS"** — the four composed formulas,
>   including `materialize(N)`.
>
> **Adding a writer, a dirty-marker, or a materialization source means editing
> those two places.** Any other section that appears to state a rule is
> referencing them and says so. If you find yourself writing a set expression
> anywhere else in this document, that is the bug this caveat exists to
> prevent.

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
5. Gates: paged-vs-dense hash equality on the `--vk-smoke-loud` scenario (and
   both against its pinned constants), a new `page-roundtrip` selftest gate,
   and a **daylight-boundary** gate the suite has never had (§3.2a).
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
- **Removing Dawn.** Already decided and **running in parallel as its own
  project** (user decision). This phase does not do it and does not wait for
  it, but it does inherit the consequences — §6 works through them, and the two
  projects share `vk_smoke.cpp` and `simulation.cpp`, so they must coordinate
  on the board.

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

ROADMAP §1's "the CA is unaware of it" is not aspiration — it is a property
that holds because `sim_step.wgsl` never names a buffer offset it did not get
from `cellIndexW`.

**Standing obligation:** a sim kernel that computes a `voxels[]` subscript by
any means other than these two helpers bypasses the page table and reads
physical memory that may belong to another chunk. §5 gives the checker rule
that catches it.

**This is the shader seam. It is not the only seam** — see §2.1a, which the
first draft of this document missed entirely.

### 2.1a The SECOND seam: CPU byte offsets into `voxels`

**[REVIEW C4 — FIXED. A whole missed class, and the miss was methodological:
I audited shaders and never audited the CPU.]** The survey above is complete
*for shaders*. It says nothing about C++ that computes a byte offset into
`voxels` from a slot index — and every such site assumes **slot `s` lives at
`s * 16 KiB`**, which is exactly the assumption paging deletes.

Five sites, all verified at source:

| site | what it does | what breaks under paging |
|---|---|---|
| `world.cpp:135-137` | chunk-fetch copy, `SlotChunkIndex(fetchIds[i]) * kChunkBytes` | the CPU chunk cache gets **another chunk's voxels** → island detection and terrain collision meshing operate on wrong data |
| `world.cpp:153-156` | the 3×3×3 **mirror** copy, `ci * kChunkBytes` | `World::KindAt` returns wrong materials → **the player falls through the floor**. **Outside the hashed domain**, so no determinism gate catches it |
| `stream.cpp:170-172` | eviction `CopyTracked(Voxels, s * kChunkBytes → staging)` | **saves the wrong chunk to disk**, silently and permanently |
| `stream.cpp:260-261` | store-hit refill `WriteBuffer(voxels, s * kChunkBytes)` | writes decoded RLE into **another chunk's page** |
| `selftest_sim.cpp:179-181`, `:290` | gate voxel dumps: `awake[k] * kChunkVol * 4`, and a whole-buffer copy from offset 0 indexed by `World::SlotCellIndex` | gates read the pool as if dense → assertions about the wrong voxels |

The mirror case is the worst of the five, because its failure is invisible to
everything else this document relies on: the mirror is CPU-only collision data,
so a corrupted mirror is a gameplay bug with a **correct world hash**.

> **Second seam statement, normative: any CPU path that computes a byte offset
> into `voxels` from a slot index must resolve through
> `World::PageOffsetOfSlot(slot)`**, returning either a byte offset into the
> pool or a "no page" marker. There is no other way to address `voxels` from
> C++.

Per-site resolution:

- **Mirror and chunk-fetch** (`world.cpp`): skip sentinel slots in the copy
  loop; **synthesize** their 4,096 words CPU-side from the table entry when the
  snapshot is consumed. Strictly cheaper than today — a sentinel chunk costs a
  4-byte table read instead of a 16 KiB GPU→CPU copy — and it reduces the
  readback traffic `kFetchPerTick = 64` exists to bound. The synthesis must use
  the same rule as the shader, so a C++ `SynthWord` goes in `world.h` beside
  `PackVoxNew` and the §4.4 gate asserts the two agree.
- **Eviction** (`stream.cpp:170-172`): §4.2's fast path stops being an
  optimization and becomes **mandatory** — a sentinel slot is not copied at
  all; the CPU synthesizes its RLE (`{4096, synthWord}`) directly.
- **Store-hit refill** (`stream.cpp:260-261`): translate through
  `PageOffsetOfSlot`, allocating first per §3.5(d) — the same branch already
  classifies the decoded chunk, so allocation and offset come from one place.
- **Selftest dumps** (`selftest_sim.cpp`): **decided per gate, listed in §8
  commit 5.** The `pond-freeze` whole-buffer copy at `:290` is a dense-layout
  read by construction; gates that dump raw voxels run under
  `--residency dense` (they test sim behaviour, not residency), while
  `page-roundtrip` is the gate that reads *through* the translation.

**Why this class was missed, recorded so the next audit is not shaped the same
way:** the shader survey was a `grep` for `voxels[`, which by construction
cannot see C++. The correct sweep is "every reader of `World::voxels`, in both
languages". §8 commit 1 carries it as a checklist item.

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

// Every sim write goes through this. A sentinel write is a NO-OP and is
// ALWAYS counted — the counter is unconditional (§5.1).
fn voxStore(idx : u32, w : u32) {
  if (idx == PT_NO_WORD) {
    atomicAdd(&pageFaults[0], 1u);
    return;
  }
  voxels[idx] = w;
}
```

**`pageFaults` is permanently bound and the increment is unconditional** — no
prelude flag, no conditional binding, no conditional `USES(...)` (§5.1, user
decision). The cost in a correct build is a branch that never fires, since
§3 guarantees every writable chunk is materialized; the benefit is that "zero
page faults" is a claim the suite can make on *every* run rather than in a
special configuration, and that there is exactly one bind-group layout and one
`pass_table.def`. A conditionally-declared binding would mean two layouts that
must agree — the shape this repo has a checker to prevent.

Three properties, in the order that matters:

1. **A sentinel write is a no-op, not an out-of-bounds store.** `PT_NO_WORD` is
   not an index into `voxels`; it is a value `voxStore` tests before indexing.
   The failure mode is a *lost voxel* (bad, visible in the hash) rather than a
   *corrupted stranger* (worse, invisible until it isn't). §7 risk 1 argues
   why this is the right choice of failure.
2. **It is detectable, always.** A permanently-bound `pageFaults` atomic
   counter (§5.1) is incremented on the no-op path, unconditionally. Every gate
   asserts it is **zero**. This converts "structurally impossible" from a claim
   into a measurement made on every run, not in a special configuration.
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

Plus two classes that are not rows in that table and were missed by the first
draft's enumeration — recorded here so the list is complete where it claims to
be:

| not a row | why it matters |
|---|---|
| the five CPU byte-offset sites | `world.cpp` mirror + fetch, `stream.cpp` evict + refill, selftest dumps — they read and write `voxels` from C++ at `slot * 16 KiB`. §2.1a |
| `raymarch.wgsl`'s 17 reads | render-only, but they index `voxels` from a slot-derived address and would sample the wrong chunk. §5.2a |

And one non-writer that still matters: `occupancyFull` **reads every slot**
(`D_CHUNKS` = 32,768 workgroups), which §4.1 addresses.

### 3.1a CPU DIRTY-WRITERS — the class, enumerated once

**[REVIEW NEW-1/NEW-2 — FIXED. This is the enumeration whose absence produced
three of the four critical findings.]** Separately from "who writes voxels"
there is a second class: **who marks chunks dirty from the CPU**. Every member
feeds `C(N)` in §3.2's recurrence, and every hole found in review so far has
been a missing member of *this* list rather than a wrong mechanism. It is
therefore enumerated here, once, and nothing else in this document may
re-enumerate it — other sections reference `C(N)` and this table.

| # | CPU dirty-writer | mechanism | found by |
|---|---|---|---|
| a | **brush / cell / explosion op targets** | the kernels' `markBoth(c)` on CPU-authored op streams — the CPU knows every target before the ops are uploaded (§3.3). **Materializes DILATED BY ONE RING** — `N26(opTargets(N))`, the induction base case for matter created in empty sky (§3.2 step (4), §3.4). Bounded by the op caps (`kMaxOpsPerTick`, `kMaxExplosionsPerTick`) and recomputed each tick, never carried | first draft; ring added phase 7 |
| b | **`particleSpawnChunks(N)`** | `resolve`'s `markDirtyNext` (`sim_particle.wgsl:242,:265`) dirties the 26-neighbourhood of a **GPU-decided** location; step (1)'s `N26` of `C(N)` covers that. The set itself is THIS tick's spawn sites plus one ring, recomputed from scratch — bounded by `kMaxParticleSpawnsPerTick` + `kMaxExplosionsPerTick`, **independent of flight duration** (§3.4, amended) | review M1; bound amended |
| c | **`EncodeWakeAll`** (`simulation.cpp:821-828`) | a bare `queue.WriteBuffer(dirty[page_], ones)` — **all 32,768 flags**, no table row, fired from `SubmitTick` (`support.cpp:155`) on a daylight crossing (§3.2a) | review C2 |
| d | **`Stream::FillSlots`** (`stream.cpp:271-273`) | per store-hit slot, `WriteBuffer(dirty[0], s*4, 1)` **and** `dirty[1]` — the "wake once: neighbors may have changed since this chunk was saved" write. Mid-frame, between ticks, from its own path (§3.5d) | review NEW-2 |
| e | **the PARTICLE FLIGHT SHELL** (`PageTable::ApplyParticleShell`) | while a particle may be in flight, `occMatter(S)` — every chunk whose latest-snapshot occupancy is non-zero — is unioned into `cpuDirty`, applied at step (3) strictly after the tightening, **after** step (1)'s propagate, and one application PAST the off condition. This is what closes the GPU-ORIGINATED-WAKE hole: `resolve`'s landing `markDirtyNext` is the one dirty-writer with no CPU-known target at its own tick, and the intersection in step (2) can never ADD it back once a mid-flight snapshot has (correctly) tightened the mirror to empty (§3.4, phase-7 close) | phase-7 close |

(d) is the same failure shape as (c) and is worth stating plainly: a refilled
slot is dirty on the **next tick**, in **both** pages, decided by streaming
rather than by the tick loop. Its target chunk was just written by streaming, so
it is materialized by §3.5(d) anyway — but it must still enter `C(N)`, or a
snapshot tightening (§3.2 (2)) in the same tick would intersect the refilled
chunk's *neighbours* away, and the CA frontier that a stream-in creates would be
invisible to the mirror.

**Ordering rule for (c) and (d) within a tick — the one this needs to pin
down.** Both write dirty flags at points that can straddle the snapshot
tightening. **Decision: (c) and (d) are applied to `cpuDirty` STRICTLY AFTER
the tightening intersection**, in the same order they issue their
`WriteBuffer`s. Equivalently they are unioned into `cpuDirty(M)` after step (2)
rather than contributed to the `C(j)` history that step (2) rolls forward.

Both of the reviewer's options are sound; this one is chosen because it needs
no ordering reasoning at all — a union applied after an intersection cannot be
undone by it, so the wake's 32,768 chunks and the refill's slots survive
regardless of when the snapshot happened to land. Putting them in `C(j)` would
also work but requires the retention ring to carry a 32,768-entry all-ones set,
which is both wasteful and a trap (`C(j)` sets are otherwise tiny and bounded
by the op caps).

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
is a 2-ring, not a 1-ring. ∎ (`mutate`, `mutateCells`, `explodeApply` and
`particleSpawn` are all in `C(N)`, being CPU-op-driven; `particleResolve`'s
landing cell is bounded by the particle's CPU-unknown flight, which §3.4
handles separately.)

#### Which ring the recurrence uses, and why it is 1 and not 2

**[REVIEW NEW-4 — FIXED. Stated explicitly, because an implementer reading the
2-ring above and the `N26^1` below would otherwise have to guess.]**

The two bounds describe **different compositions** and both are correct:

- **The 2-ring above** composes *write reach* with *mark reach*: a cell in
  chunk `X` writes ≤1 cell away (possibly into a neighbour chunk), and
  `markDirty` on that written cell marks ≤1 chunk beyond *it*. That is the
  bound on **"which chunks can a tick whose dirty set is `D` mark dirty,
  measured from `D`"** — and it composes two hops because it passes through an
  intermediate written cell.
- **The recurrence below uses `N26^1` per tick**, and that is sufficient
  because it propagates **`dirtyIn` → `dirtyIn`**, not `dirtyIn` → *written
  cells* → `dirtyIn`. `markDirty(c)` (`sim_step.wgsl:66-88`) marks the 2×2×2
  combination of `worldChunkOf(c)` with its ±1 boundary offsets — i.e. every
  chunk it marks is in `N26({chunkOf(c)})`. And `chunkOf(c)` is a chunk the
  kernel *acted in*, which is a member of `dirtyIn(N)` itself, because the CA
  dispatches one workgroup per chunk **in the dirty list** and a thread only
  acts on cells of its own chunk. So every chunk marked during tick `N` lies in
  `N26(dirtyIn(N))` — one ring — and `dirtyOut(N) = dirtyIn(N+1)` follows.

The distinction is exactly "where is the marking cell?": the 2-ring measures
from the *source* chunk of a write that has already crossed a boundary; the
1-ring measures from the *acting* chunk, which is in `dirtyIn` by construction.
**The recurrence and the roll-forward both use `N26^1` per tick.** The 2-ring
is retained above because it is the correct bound for the write-reach argument
that §3.2a's materialization rule rests on.

Iterating the 2-ring form `k` times would give a `2k`-ring dilation from a
stale snapshot. **[JUDGMENT] I reject that as the mechanism**, for two reasons
worth stating because they are what motivate the incremental design:

- `k` is unbounded in principle (a long GPU stall saturates the ring
  indefinitely), so the dilation radius is unbounded, so the "conservative
  set" degenerates toward the whole window. A rule whose worst case is "all
  32,768 chunks" has no rule-2 story.
- Computing a `2k`-ring dilation over 32,768 chunks on the CPU every tick is
  itself a full-world scan — the exact rule-2 violation the phase is supposed
  not to add.

**The mechanism instead: maintain a CPU-side conservative mirror of `dirtyOut`
incrementally, and use an arriving snapshot only to TIGHTEN it — never to
replace it.**

> ## THE NORMATIVE DEFINITIONS
>
> **[REVIEW NEW-3 — these four are the single source. Every other section of
> this document REFERENCES them and none restates them.]** The CPU keeps
> `cpuDirty[kNumChunks]` (a bitset, 4 KiB).
>
> ```
> // ---- (0) the CPU dirty-writer contributions, enumerated ONCE in §3.1a ----
> //     opTargets(N)      = chunks touched by brush / cell / explosion ops   (a)
> //     particleSpawnChunks(N) = THIS tick's spawn sites + 1 ring, §3.4      (b)
> //                              recomputed each tick, never carried; bounded
> //                              by the op caps, NOT by flight duration
> //     C(N)              = opTargets(N) ∪ particleSpawnChunks(N)
> //     (c) EncodeWakeAll and (d) Stream::FillSlots are applied in step (3),
> //     strictly AFTER (2) — see §3.1a's ordering rule.
>
> // ---- (1) propagate — every tick, no GPU dependency ----------------------
> //     N26^1 per tick: dirtyIn -> dirtyIn is a ONE-ring, see above.
> cpuDirty(N+1)  =  N26( cpuDirty(N) )  ∪  C(N)
>
> // ---- (2) tighten — ONLY when a snapshot arrives, ONLY by intersection ---
> //     A snapshot stamped tick S is consumed while encoding tick M, M > S.
> //     dirtyFlags(S) == dirtyIn(S+1) exactly, so rolling it forward the
> //     (M-S-1) ticks since encoded gives a SECOND superset of dirtyIn(M) —
> //     usually much tighter than (1)'s, occasionally not. NEVER an assignment.
> cpuDirty(M)   ←  cpuDirty(M)  ∩  [ N26^(M-S-1)( dirtyFlags(S) )
>                                    ∪  ⋃_{j=S+1}^{M-1} C(j) ]
>
> // ---- (3) union the unconditional CPU dirty-writers, AFTER (2) -----------
> cpuDirty(M)   ∪=  allOnes            if EncodeWakeAll fired this tick   (c)
> cpuDirty(M)   ∪=  refilledSlots(M)   from Stream::FillSlots             (d)
> cpuDirty(M)   ∪=  occMatter(S)       while particles may be in flight   (e)
>     // (e) is applied AFTER step (1)'s propagate rather than before it, so
>     // it is not dilated in the tick it is computed; its 26-ring is then
>     // materialized by step (4)'s bracketed half (the seed is in
>     // cpuDirty ∩ hasMatter), which is what keeps the flight shell at
>     // matter + ONE ring. §3.4's amendment carries the full argument.
>
> // ---- (4) THE MATERIALIZATION SET — the one normative formula -----------
> materialize(N) = [ (cpuDirty ∩ hasMatter) ∪ N26(cpuDirty ∩ hasMatter) ]
>                  ∪ opTargets(N)  ∪  N26( opTargets(N) )
>                  ∪ particleSpawnChunks(N)
> ```
>
> **`N26(opTargets(N))` is the INDUCTION BASE CASE, added in phase 7 after it
> cost a real page fault** — see §3.4's soundness note. The bracketed half is
> evaluated against `hasMatter` *at encode time*, so an op that creates matter
> in previously-`PT_EMPTY` sky is invisible to it, and the CA moves that matter
> one cell in the SAME tick. One ring around the op targets covers every such
> write; two are not needed, because sim write reach is 1. It is bounded by the
> op caps and recomputed each tick, exactly like `particleSpawnChunks(N)`.
>
> **Both operands of the `∩` in (2) are supersets** of the true `dirtyIn(M)`,
> so their intersection is also a superset and is at least as tight as either.
> That is the whole correctness argument, and it is why (2) is an intersection
> and never an assignment.
>
> **Read (4) carefully — the two halves have different sentinel rules, and
> conflating them was the first draft's error:**
>
> - The **bracketed half** is filtered by `∩ hasMatter`, because a dirty
>   EMPTY chunk can only *receive* matter from a non-empty neighbour, and the
>   ring around the non-empty set covers every such neighbour (§3.2a). This is
>   what keeps a wake-all from demanding 32,768 pages.
> - **`opTargets(N)` and `particleSpawnChunks(N)` are NOT filtered.** They
>   materialize **regardless of sentinel state**, because they are writes into
>   cells the CPU chose, and a CPU op genuinely writes into isolated empty sky:
>   `sim_mutate.wgsl:79` paints wherever `voxMat(voxels[idx]) == MAT_AIR` — an
>   op-mode-0 brush into a `PT_EMPTY` chunk with no non-empty chunk anywhere
>   near it is an ordinary, intended operation. Filtering these through
>   `∩ hasMatter` would make the brush silently no-op in open sky, which is
>   the single most visible thing a player can do.
>
> `particleSpawnChunks` is unfiltered for a narrower and truer reason: on the
> FIRST TICK OF FLIGHT a particle has not yet encountered anything, so its
> spawn ring is not yet pinned to existing matter. Every LATER particle write
> is adjacent to matter that already exists and is covered by the bracketed
> half — see §3.4. (An earlier draft said a particle can come to rest in
> isolated sky having "run out of life". That is FALSE: ordinary particles have
> no life counter and come to rest against matter.)

**Ordering, and it is what closes the induction.** The bracketed half is
evaluated against `hasMatter` **at encode time for tick N**. A tick-N
reinsertion places matter that is present at N+1, and `markDirtyNext` marks its
chunk, so N+1's `(cpuDirty ∩ hasMatter)` includes it. The set is therefore
correct at every tick without ever tracking a particle in flight.

**[REVIEW C1 — FIXED. The earlier draft of this section was wrong and the
reviewer is right about why.** It wrote `cpuDirty ← dirtyFlags(snapshot)` — an
*assignment*, described as "EXACT reset ... ground truth". It is not ground
truth at consumption time. `dirtyFlags(S)` is exact for `dirtyIn(S+1)`, but the
snapshot is consumed while encoding some later tick `M`: the ring declines a
tick entirely when all three slots are in flight (`world.cpp:110-113`, and
`EncodeDirtyCopy` is guarded by `if (lastSlot_ < 0) return;`), and the frame
loop runs up to 4 ticks per `ProcessEvents` (`main.cpp:1795`,
`ticksThisFrame < 4`). Assigning therefore *discards* `M−S−1` ticks of
legitimate dilation and installs a frontier stale by exactly the latency the
mechanism exists to survive — an under-approximation, which is risk 1. The
intersection form has no such failure: it can only ever remove chunks that
*both* estimates agree are clean.**]**

`snap.tick` is already carried (`world.h:488`, set at
`world.cpp:118`), so `M−S` is known at consumption with no new plumbing. The
per-tick `C(j)` sets must be retained in a small ring for the roll-forward —
they are bounded by `kMaxOpsPerTick = 64`, `kMaxExplosionsPerTick = 8` and the
cell-op count, and only the last few ticks are ever needed.

**[JUDGMENT] The simpler variant, and why I am not taking it.** The reviewer
offers `cpuDirty ← cpuDirty ∩ N26^(M-S-1)(dirtyFlags(S))` without the `C(j)`
union. That is *not* a superset — a CPU op issued at tick `S+1` marks chunks
the snapshot never saw, and intersecting them away loses them. The `C(j)` union
is mandatory, and it is cheap because those sets are exactly the ones the CPU
already computes for §3.3. (If the roll-forward's bookkeeping is judged not
worth it, the safe degradation is to **skip the tightening entirely** when
`M−S > 1` and let (1) carry — never to tighten with an incomplete superset.)

Why this is the right shape:

- **It is tight in the common case.** With a healthy ring `M = S+1`, the
  roll-forward is the identity, and the intersection reduces `cpuDirty` to
  exactly `dirtyIn(M)`. The common case is exact — it just is not exact *by
  assignment*, it is exact because the intersection happens to be.
- **It degrades gracefully and never unsoundly.** Missed snapshots widen the
  ring; the next intersection narrows it. A settled world's `cpuDirty` is empty
  and stays empty (both operands empty).
- **It is a set operation over members, not a scan.** The `N26` dilation
  iterates the *members* of the set, never all 32,768 slots. A settled world
  iterates zero elements. This is the rule-2 story.
- **It never under-approximates**, which is the only property correctness
  needs. Over-approximating costs a materialized page that turns out empty and
  is freed by §3.6's hysteresis a few ticks later.

**Headline, stated precisely so it is not misremembered:** *the snapshot is a
second superset, usually tighter — never ground truth about the tick being
encoded.*

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

### 3.2a `EncodeWakeAll` — the writer that is not an op and not a kernel

**[REVIEW C2 — FIXED. This was a genuine hole and it was the most dangerous one
in the document.]** The earlier draft's `C(N)` enumerated CPU *ops*.
`Simulation::EncodeWakeAll` (`simulation.cpp:821-828`) is neither an op nor a
table row — it is a bare `queue.WriteBuffer(world_->dirty[page_], ones)` that
sets **all 32,768 dirty flags to 1**, called from `SubmitTick`
(`support.cpp:155`) on any tick where `DaylightStrengthCpu` crosses zero.

Verified at source, and the consequences are exactly as the reviewer states:

- Every chunk in the window becomes `dirtyIn` on the next tick, so every chunk
  dispatches and may write — while `cpuDirty` is near-empty because the world
  was settled. **Silent voxel loss at every dawn and every dusk.**
- **No gate catches it.** The selftest pins the day phase in both directions —
  `night.dayNight.freeze = 1; freezePhase = 0` (`selftest_sim.cpp:242-243`) and
  `noon.dayNight.freeze = 1` (`:389`) — so `wasDay != isDay` is never true in
  the suite and `EncodeWakeAll` is never called. The bug would ship.

**Fix, part 1 — the wake sets the mirror**, as contributor (c) in §3.1a, unioned
in at step (3) of the normative definitions (strictly after the tightening).
`EncodeWakeAll` sets `cpuDirty` to all-ones in the same call: the two must be
one operation, not two that must agree — the wake *is* a dirty-set mutation and
the CPU mirror is a mirror of the dirty set. Give it a signature that makes the
pairing structural (the wake takes the mirror, or the mirror lives beside
`page_` and the wake updates both), so a future caller cannot get one without
the other.

**Fix, part 2 — the bracketed half of the materialization formula is what stops
this from becoming a crash. [REVIEW M2 — FIXED.]** With `cpuDirty` all-ones, a
naive "materialize everything dirty" would demand 32,768 pages from an
8,192-page pool: guaranteed exhaustion, which under §3.8's settled policy is a
guaranteed **abort**, twice per in-game day.

The rule is **step (4) of the normative definitions above** — not restated
here, per NEW-3. What that section does not have room to argue is *why the
`∩ hasMatter` filter is sound*, which is this:

> **`hasMatter` excludes only `PT_EMPTY`.** A `UNIFORM(mat)` sentinel with
> `mat != MAT_AIR` HOLDS MATTER and is included, because a particle or a CA
> write can land against it — `blocksParticle` reads through `voxWordAt`, and a
> UNIFORM sentinel BLOCKS, so a particle can legitimately come to rest against
> a chunk of uniform water. This is a strict WIDENING of the earlier
> `∩ nonSentinel`: the ~4,974 non-empty chunks ARE the `hasMatter` set, so the
> wake-all argument below is untouched.
>
> **Dirty ≠ has matter.** A chunk that is dirty but is `PT_EMPTY` holds no
> matter, so nothing in it can move; the only way it can *receive* matter is
> from a neighbouring chunk that has matter — and every such neighbour is in
> `cpuDirty ∩ hasMatter`, whose 26-ring is materialized. A dirty EMPTY chunk
> with no non-empty chunk in its 26-neighbourhood is therefore provably
> unwritable **by the CA** this tick and needs no page.

Note the qualifier **"by the CA"**: it is doing real work. The claim is only
about matter *moving* under the automaton, which is why `opTargets` and
`particleSpawnChunks` are unioned in *outside* the filter (step 4) — those are
writes the CPU commanded into cells it chose, and they reach isolated sky where
no CA write ever could.

This is sound for the same reason the write-reach argument is: writes reach ≤1
cell, so matter crosses at most one chunk boundary per substep, so a chunk can
only be written by a CA source within its 26-neighbourhood. Under a wake-all
the bracketed half collapses from 32,768 to *the non-empty chunks plus their
ring* — the same ~4,974 + ring the world already needs, which is why the pool
sizing in §3.7 survives a wake-all at all.

**Fix, part 3 — the hysteresis interaction.** §3.6's free condition includes
`AND the slot is not in cpuDirty`. With `cpuDirty` all-ones after a wake, **no
page is eligible to be freed until the mirror shrinks**, which it does over the
following ticks as the intersection in §3.2 (2) tightens it against arriving
snapshots — a settled world's next snapshot reports almost nothing dirty and
collapses the mirror in one step. So deallocation stalls for a few ticks and
then resumes; it does not deadlock. Stated explicitly because "all deallocation
freezes" is alarming if discovered rather than predicted, and because it is a
real (bounded) resident-memory spike at dawn.

**The gate this needs, because the suite structurally cannot catch it**
(§8 commit 4): settle a world, then run across a daylight boundary **with
`dayNight.freeze` OFF**, and assert (a) the hash matches a dense run,
(b) `pagesInUse_ < kPoolPages` throughout, (c) `pageFaults == 0`. This is the
only gate in the suite that exercises `EncodeWakeAll` at all — which is worth
noting independently of paging.

### 3.3 (b) sim_mutate / sim_explode / particleSpawn

All three take CPU-authored op streams, so their targets are CPU-known **before
the ops are written to their buffers** — which is the same place `SubmitTick`
already computes `opsCount`/`expCount`/`cellCount` (`support.cpp:113-136`).

**These chunks form `opTargets(N)`**, which enters the normative definitions in
two places: as contributor (a) to `C(N)` (step 0), and — critically — as an
**unfiltered** term of `materialize(N)` (step 4). They are *not* subject to
`∩ hasMatter`: `sim_mutate.wgsl:79` paints into any cell that reads as air,
so a mode-0 brush into an isolated `PT_EMPTY` chunk is an ordinary operation
and must have a page. Filtering it would make the brush no-op in open sky.

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

`sim_particle:resolve` writes `voxels` at a GPU-decided location. The CPU does
not know it. This is the writer the brief flags as "can land anywhere along a
flight path", and it is real.

**[REVIEW M1 — FIXED. There are TWO write targets, not one, and the earlier
draft described only the first.]**

1. **Reinsertion** — an ordinary particle backs off to the **last air cell**
   before the blockage (`sim_particle.wgsl:178-180`, `p.px = lastAir.x` …) and
   `resolve` writes a whole voxel there (`:248-250`).
2. **Micro-stain** — a MICRO particle does **not** back off. It parks at the
   **contact cell**, i.e. the first *blocked* sample (`:170-176`; the comment
   there says so outright: "the droplet is parked at the CONTACT point (first
   blocked sample), not backed off to the last air cell"), and `resolve` writes
   a stain into that occupied cell (`:241`,
   `voxels[tgt] = (w & ~STAIN_BITS) | packStain(...)`).

That difference matters here for two reasons. The stain target is a **solid,
non-air** cell — a cell reinsertion logic would never choose — so a
materialization set derived from "where can a particle come to rest in air"
misses it. And stain is **hashed state** (bits 24..30, `sim_occupancy.wgsl:83`),
so losing one moves the world hash.

**Both targets are within the swept path**, so one conservative set covers
both — but the set must be built from the swept path, not from a notion of
"landing spot".

**Second consequence, and this one feeds back into §3.2:** `resolve` calls
`markDirtyNext(cell)` on the stain path (`:242`) and on the reinsertion path
(`:265`), which dirties the **26-neighbourhood of a location the CPU never
chose**. So particle activity seeds next tick's CA reach through a channel
`cpuDirty`'s own recurrence does not model. **`particleSpawnChunks(N)` must be
UNIONed into `cpuDirty(N+1)`**, not merely materialized alongside it —
otherwise the CA frontier that particles create is invisible to the mirror
until a snapshot happens to report it.

**THE ADJACENCY ARGUMENT — [AMENDED, and this REPLACES the swept set
entirely].** The first draft tracked a carried, dilated set of chunks a
particle might BE in. That is a correct superset but it is unbounded: dilating
the previous dilation one ring per tick makes it a k-ring after k ticks —
(2k+1)³ chunks — and measured on the loud scenario one explosion's debris drove
it 1, 27, 125, 343, 729, 1331, 2197, 3375 over eight ticks, past an 8,192-page
pool on its own.

**The set is deleted.** Every particle write lands within one cell of matter
that already exists, so the bracketed half of `materialize(N)` already covers
all of them after the first tick of flight:

- **A STAIN write targets a non-air cell DIRECTLY.** `resolve` guards on
  `hit == MAT_AIR` and then on class (`sim_particle.wgsl:229`, `:234`), and the
  buried branch sits behind `blocksParticle(startCell)` (`:130-139`). A stained
  cell therefore HAS MATTER by construction, so its own chunk is in
  `cpuDirty ∩ hasMatter`.
- **A REINSERTION targets `lastAir`**, which is ≤ 1 cell from a blocking sample
  because the sweep subdivides to ≤ half a voxel
  (`n = max(1, (maxc + 127) / 128)`, `:153-154`). The blocking sample has
  matter, so `lastAir`'s chunk is within `N26` of a `hasMatter` chunk.
- **The only gap is the FIRST TICK OF FLIGHT**, before a particle has
  encountered anything — and that is exactly what the spawn ring covers.

> **The old formula tracked where a particle might BE, which grows with flight
> time. This one tracks where a particle might WRITE, which is pinned to matter
> that already exists.**

**What remains is `particleSpawnChunks(N)`:**

```
particleSpawnChunks(N) = chunks(spawnOps(N)) ∪ chunks(explosionCenters(N)),
                         dilated ceil(PART_MAX_VEL / CHUNK) + 1 = 1 ring
```

**RECOMPUTED FROM SCRATCH each tick from CPU-known inputs, never carried.** It
is bounded by `kMaxParticleSpawnsPerTick` and `kMaxExplosionsPerTick` and is
**independent of flight duration** — which is the whole point.

It stays contributor **(b)** to `C(N)` in §3.1a: `resolve`'s `markDirtyNext`
still dirties the 26-neighbourhood of a GPU-decided location, and step (1)'s
`N26` of `C(N)` covers that.

**Fallback (ii) is kept:** the ring radius is derived from
`TUNE_PART_MAX_VEL` at load rather than hardcoded, so raising the tuning value
automatically widens it — and since `TUNE_*` is hot-reloadable (F5), it is
recomputed on reload.

**`pageFaults == 0` on the loud scenario is the EVIDENCE for this argument, not
a formality.** If it is ever non-zero, a write escaped the adjacency claim and
the path must be found — do not widen the ring to make it go away.

**A simpler option exists and I am not taking it:** materialize the whole window
whenever `particlesActive`. That is correct and trivially safe, but a single
explosion would then un-sparse the entire world for as long as its debris
flies — turning the most common "something is happening" case into the
worst-case memory footprint. That defeats the phase.

#### The induction base case the adjacency argument does not cover — [AS BUILT, phase 7]

The argument above, and §3.2's recurrence generally, is an induction whose step
is sound and whose **base case was missing**. The step: a write at tick `N`
places matter that exists at `N+1`, `markDirtyNext` marks its chunk, so `N+1`'s
`(cpuDirty ∩ hasMatter)` and its ring see it. That closes for every tick after
matter exists.

It does **not** close for the tick in which an op FIRST creates matter in
previously-empty sky. The bracketed half of `materialize(N)` is evaluated
against `hasMatter` **at encode time**, when those chunks are still `PT_EMPTY`
and contribute nothing — and then the CA runs in that same tick and moves the
new matter one cell, off the op chunk and into a neighbour that is in neither
half of the set. Measured: the loud scenario's WATER brush at `(176,150,176)`
r5 spans the `y=9` chunks only; water fell into a `y=8` chunk within tick 8 and
that write was lost — one page fault, slot 11531, `pageTable` entry
`0x80000000`. Sand does not expose it, because it is painted adjacent to
existing matter and the bracketed half already covers its neighbourhood; water
spreads and falls on the first tick.

**Fix: `materialize(N)` dilates op targets by one ring** — the
`∪ N26(opTargets(N))` term now in §3.2 step (4).

**Soundness, and why ONE ring is exactly right.** The CA this tick acts only on
chunks dirty at compaction time, which is `previously-dirty ∪ op-marked`.
Previously-dirty chunks have matter, so they and their ring are already in the
bracketed half. Op-marked acting cells write at reach ≤ 1 cell — rule 1's
lattice bound, which the whole race-freedom argument already rests on — so
every write they can make lands within `N26(opChunks)`. One ring is therefore
sufficient, and a second is not needed: the reach is 1, not 2.

**Bounded, per rule 2.** Op counts are capped (`kMaxOpsPerTick = 64`,
`kMaxExplosionsPerTick = 8`) and the set is recomputed from CPU-known inputs
every tick and never carried, so it cannot grow with time. It mirrors
`particleSpawnChunks`'s reviewed 1-ring treatment exactly — same shape, same
reason, same bound.

**How it stayed hidden:** `pageFaults` recorded this fault all along and nobody
could read the register. `vk_smoke` printed a hardcoded `0` (`RunResult::pageFaults`
was never assigned), and the counter buffer was never zeroed, so it started at
driver garbage (measured 2²⁷ on a 3060 Ti). Both are fixed — the counter is a
real blocking readback after `WaitIdle`, and it is zeroed in `ResetAllEmpty` /
`ResetIdentity` and once after paged worldgen (see the comment there for why
worldgen's own sentinel stores legitimately count). §3.4's standing rule — *if
`pageFaults` is ever non-zero, find the path; do not widen the ring to make it
go away* — is only enforceable now that the number is real.

#### The GPU-originated-wake hole — the second missing case, and the flight shell — [AS BUILT, phase-7 close]

The adjacency argument above says every particle write after the first tick of
flight is covered by the bracketed half. **That claim silently assumed the
landing chunk would be in `cpuDirty`, and the recurrence cannot put it there.**
Measured, gate `flung-liquid` in paged mode: blood particles fly ~6 ticks over
a slab; while airborne they dirty nothing, so a fresh snapshot correctly
tightens `cpuDirty` to **0** (the snapshot genuinely contains no dirty gate
chunks). The chunks under the slab then demote to `PT_EMPTY` under §3.6. The
landing itself succeeds — the slab is resident — but the landed blood then
flows as CA liquid off the slab edge into the demoted chunks and is lost: 62
page faults, `cpuDirty == 0` for 37 straight ticks, 21 landed voxels at max
fullness 1 against dense's 76 at 7.

**The formula gap, stated exactly.** `cpuDirty`'s only additive contributors
are the CPU-known events (a)–(d) plus `N26` of itself, and the tightening is
an intersection — it can only ever REMOVE. `resolve`'s landing `markDirtyNext`
is a dirty-writer at a tick the CPU never chose (§3.4's own "second
consequence" said so), so once a mid-flight snapshot has tightened the mirror
to empty, **0 stays 0**: the closure lemma in §3.2 is missing a term for
landings, and both operands of the intersection stop being supersets the
moment a particle lands inside the gap. The spawn ring covers only the birth
tick; nothing covers the death tick.

**The fix: contributor (e), the flight shell.** While a particle may be in
flight, union `occMatter(S)` — every chunk whose latest-snapshot occupancy is
non-zero — into `cpuDirty`, at step (3), strictly after the tightening. The
bracketed half then materializes `N26(occMatter)` by its own rule. Soundness
rides on the same adjacency argument as the collapse above: every particle
write is ≤1 cell from a blocking cell, blocking cells need matter, and
occupancy sees ALL matter (it reads through `voxWordAt`, so `UNIFORM`
sentinels report their synthesized counts). Matter created SINCE the snapshot
was stamped is not in `occMatter` and does not need to be: op-created matter
is covered by `N26(opTargets)` (the induction base case), and landed or
CA-moved matter is genuinely dirty, hence in `cpuDirty` by the marks the
tightening keeps, resident, and ringed by the bracketed half.

Four design points that took a wrong build each to learn:

1. **Seed from OCCUPANCY, not the page table.** `hasMatter` (any non-`EMPTY`
   entry) cannot tell a resident-but-all-air chunk from one holding matter, so
   a residency-seeded shell FEEDS BACK: materializing the ring makes it
   resident, the next shell rings the ring, and the set dilates one ring per
   tick for as long as a particle flies — the exact unbounded growth this
   section's amendment deleted. Occupancy counts actual non-air cells, which
   materializing an empty page cannot change; the fixed point is pinned to
   real matter.
2. **The seed enters the MIRROR, not just the materialization set.** The
   landed chunk must end up in `cpuDirty`, or the CA flow the landing seeds
   faults the moment the last particle dies: materialization alone covers the
   landing write but the intersection can never ADD the landed chunk later.
   In the mirror it survives every later tightening on its own merits — it is
   genuinely dirty, so it is in BOTH operands — and step (1)'s `N26` tracks
   the flow frontier at the 1-chunk/tick it can move.
3. **Applied after step (1)'s propagate, and the ring stays OUT of the
   mirror.** Union the ringed shell before the propagate and three dilations
   compound (shell ring, propagate ring, bracket ring): fixed point matter +
   3 rings, measured past an 8,192-page pool on `flung-liquid` alone. Seed
   only, post-propagate: matter + 1 ring. The ring chunks are instead kept as
   an explicit **demotion guard** (`shell_`, consulted by §3.6's free
   decision), preserving the structural property that nothing in the
   materialization set is eligible to free — without it, hysteresis frees the
   very chunks a particle is about to land in, which is the measured trace.
4. **The shell LINGERS one application past its off condition.** The last
   landing (tick Z) first shows occupancy in the snapshot stamped Z — the
   same snapshot whose `particleCount == 0` lowers the shell. One more
   application seeded from THAT snapshot is what carries the landed chunk
   into the mirror. The off condition itself is conservative: a zero count
   only counts if the snapshot POSTDATES the last particle-spawning
   submission (`lastSpawnTick_`), because a count proves nothing about spawns
   after its stamp.

**The rejected candidates, for the record.** *(a) Carry the spawn ring while
particles fly* — the birth ring does not track a ballistic arc; making it
track one means dilating per tick, which is the deleted unbounded set.
*(b) Suppress demotion while `particleCount > 0`* — fixes the measured trace
(the faulted chunks were resident and demoted mid-flight) but not a landing
in never-resident airspace, and not the post-landing flow; it also falls out
of (e) for free via the existing `!cpuDirty` conjunct plus the ring guard.

**Bounded, per rule 2.** The shell exists only while `particlesActive` gates
a live particle pipeline AND the latest snapshot cannot prove every particle
has landed; a settled world pays one branch. Its size is `occMatter` + one
ring ≈ matter + surface, recomputed per tick, never carried, independent of
flight duration. Measured on `flung-liquid`: `occMatter` 4,978, shell 8,070,
flat across the flight — and the gate now lands **76 voxels at max fullness
7/7, bit-identical to dense, `pageFaults == 0`**.

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

> ### [MEASURED — commit 0, 2026-08-22, default seed, 300 ticks settled]
>
> `--measure`'s uniformity histogram (MEASUREMENT 1b, dense-only) over the
> 32,768-chunk window:
>
> | bucket | chunks | % | note |
> |---|---|---|---|
> | all-air (`EMPTY`) | 27,793 | 84.82% | sentinel, 4 B |
> | **all-one-WORD (`UNIFORM`)** | **41** | **0.13%** | sentinel, 4 B |
> | all-one-MATERIAL, mixed state/stain | 2,115 | 6.45% | needs a page |
> | mixed material | 2,819 | 8.60% | needs a page |
>
> Resident pages with `EMPTY` only: **4,975 = 77.7 MiB**. With `EMPTY` +
> `UNIFORM`: **4,934 = 77.1 MiB**. **UNIFORM's marginal saving is 41 chunks =
> 0.6 MiB, or 0.8% of the resident set.** All 41 are material 5 (water) — flat
> pond interiors, whose fullness nibble is a uniform 8.
>
> **The 2,115 all-one-MATERIAL chunks are the interesting number, and they are
> exactly what §2.3 predicted:** one material, differing state nibbles, so a
> 12-bit-material sentinel cannot represent them. That is 6.45% of the window —
> 33 MiB — sitting one bit-layout decision away, and it is unreachable without
> widening the page-table entry to carry a state nibble, which is a different
> design. Recorded as the finding, not acted on in phase 7.
>
> **Decision rule applied: the recommendation below stands unchanged.** 0.6 MiB
> does not justify a GPU uniformity scan on the tick path. UNIFORM stays
> representable and constructible on the paths that already hold the words
> (streaming store-hit, `LoadWorld`), which costs nothing extra and keeps the
> sentinel live and tested; discovery stays out.

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

**Two corrections this condition inherits from §3.2 and §3.2a:**

1. **`occTotal == 0` is itself stale** — it comes from the same snapshot whose
   staleness C1 is about. That is *safe here and only here*, because the free
   condition is a conjunction with `!cpuDirty`: a chunk that became non-empty
   since the snapshot was stamped was written by something, and anything that
   writes it puts it in `cpuDirty` (that is §3.2's whole guarantee), so the
   second conjunct rejects it. **The staleness of `occTotal` is covered by the
   freshness of `cpuDirty`, not by luck** — and this is why `cpuDirty` must be
   the *conservative* mirror rather than the snapshot's own dirty flags. Using
   `snap.dirtyFlags` here instead would make both conjuncts stale in the same
   direction and the argument would collapse.
2. **After a wake-all, `cpuDirty` is all-ones, so nothing is freeable** until
   it shrinks (§3.2a fix 3). Bounded, self-clearing, and called out as open
   question 7 because it is a real resident-memory spike at dawn and dusk.

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

#### Demotion in practice — four amendments — [AS BUILT, phase-7 close]

The first full paged suite (which only became runnable at the close) showed
~14,400 pages still resident after the streaming and spells gates and pages
that could never free at all. Four amendments, each with its measured cause:

1. **The free predicate is "every cell is STAINLESS AIR", not `Classify`'s
   exact-word rule.** Two passenger-bit classes survive on air cells after a
   chunk empties: the tick stamp (the CA deliberately stamps vacated cells,
   `sim_step.wgsl:114` — words like `0x00030000`) and the state nibble
   (`sim_mutate` paints EVERY voxel with a palette jitter, air included,
   `sim_mutate.wgsl:80` — an erase brush leaves `0x00002000`). `Classify`
   refuses both forever, so every chunk the CA or a brush ever touched leaked
   its page permanently. Ignoring bits 12..23 on an AIR word is sound on
   grounds verifiable at source: the hash skips `MAT_AIR` cells entirely
   (`sim_occupancy.wgsl:161`); the only stamp reader is the acting cell's own
   early-out (`sim_step.wgsl:802`) and an air cell's act is a no-op; every
   sim read of a NEIGHBOUR's state nibble is behind a same-material guard
   (`sim_step.wgsl:682,:705,:724,:753`) and the flow-into-air branch passes
   an explicit 0 for air's fullness; `tryMove` copies both fields through the
   swap without branching. STAIN stays load-bearing — it is hashed — and a
   stained all-air chunk keeps its page, which is the erode-then-demote hash
   bug the words-probe exists to prevent. `Classify` keeps exact-word
   strictness where it is needed: `UNIFORM` promotion must reproduce resident
   words bit-exactly.
2. **The hysteresis counter re-arms on materialization.** `zeroStreak_`
   saturates at 255 and the free triggers on the EXACT pass through
   `kPageFreeTicks`; a chunk materialized while all-air (every ring chunk is)
   whose counter had saturated during its long sentinel life could never pass
   through the trigger again — occupancy stays 0, nothing resets it — and
   its page leaked for the run. The counter now means "consecutive empty
   snapshots since residency began" and is zeroed when the page is allocated.
3. **Free probes are capped per tick** (`kMaxFreeProbesPerTick = 64`, the
   overflow held at `kPageFreeTicks - 1` so it re-arms rather than sailing
   past the exact-== trigger). Each probe is a blocking `WaitIdle` + 16 KiB
   readback, and a mass-demotion event — a whole-window load, a wake-all,
   the streaming gate's window churn — can mint thousands of candidates on
   one tick; uncapped, that measured as a minutes-long stall that presented
   as a hang. The backlog drains at the cap rate.
4. **The flight shell's ring guards demotion** (§3.4's amendment, point 3):
   while particles may be in flight, `shell_ = occMatter ∪ N26(occMatter)` is
   a third conjunct on the free condition, because the ring chunks are
   deliberately NOT in `cpuDirty` and hysteresis would otherwise free the
   chunks a particle is about to land in — the exact measured `flung-liquid`
   failure.

One stale-occupancy quirk, recorded so nobody chases it: streaming refills
write voxels directly and occupancy refreshes with the chunk's next dirty
tick, so a freshly refilled slot can sit a few snapshots with `occ == 0`
while holding matter. The words-probe refuses those correctly (`0x00000001`-
class refusals in the debug trace); the cost is a wasted probe, not a wrong
free.

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

**Allocation is a pop; free is a push.** LIFO for cache locality: a
recently-freed page is the one most likely still resident in whatever cache
hierarchy cares.

**[REVIEW m4 — FIXED.]** The first draft additionally claimed LIFO makes page
assignment "reproducible run to run" and therefore aids debugging. **That is
false and the claim is withdrawn.** Free order depends on the *snapshot
cadence* — which slots' occupancy readbacks arrive on which tick, and whether
the ring declined (§3.2) — and that cadence is a function of GPU timing and
frame pacing, not of tick inputs. So two runs of the same seed can assign
different page indices to the same chunk.

This is **harmless to correctness**: the page table is not hashed and not
saved (§4.2), so page assignment is not part of the world. It only means the
stated debugging benefit does not exist, and a debugger comparing two runs
should compare *table entries by slot*, never *page indices*.

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

#### Pool re-sized from measurement: `kPoolPages = 16384` — [AS BUILT, phase-7 close]

The 8,192 recommendation above did not survive the first full paged suite.
Measured with a dense-size pool so true demand was observable (and with
`ResetIdentity` no longer latching `pagesHighWater_` by construction — the bug
that invalidated the first sizing attempt; the high-water is real demand only
if its sole writer is `Alloc()`):

| number | value |
|---|---|
| full paged suite high-water | **14,934 pages = 233.3 MiB**, stable across four builds (14,934 / 14,934 / 15,185 / 15,185) |
| the driver | the STREAMING gate's flight-speed churn: each shift puts the refilled plane into `cpuDirty` (contributor (d)) and its matter chunks' rings materialize, behind an 8-tick hysteresis + capped drain; sawtooth band 12.4k–14.5k, ±1.7% run to run |
| flight-shell contribution | `flung-liquid` gate high-water 8,406 (of which 4,975 is the settled world) |
| resident settled | unchanged: **4,975 pages = 77.7 MiB** |
| **`kPoolPages = 16384`** | **256 MiB reserved, 2× under dense, 1.10× over the measured worst case** |

The sizing rule "high-water × 1.25 rounded up to a power of two" lands on
32,768 — the dense size, i.e. no reservation win at all — so the rounding half
of the rule is deliberately not honoured. The judgment: the measured worst
case is the suite's deliberately-hostile flight-speed streaming, far above
walking-pace play; the number is stable across runs; §3.8's abort stays loud;
and the suite re-measures the margin on every paged run (the high-water line
at the end of `--selftest`, `selftest.cpp`). If the margin erodes, the number
moves — that is what a tracked number is for.

### 3.8 Exhaustion is a FATAL ERROR

> **Decision (user, settled): if `freePages_` cannot satisfy the
> materialization set, the engine aborts with a clear
> `page pool exhausted: N needed, M free` error. In every mode — game,
> selftest, `--shot`, `--measure`. There is no fallback, no refusal, no
> degradation.**

The reasoning is short and it is better than the first draft's: **if the pool
can exhaust in normal play, the pool is mis-sized.** That is a bug in
`kPoolPages`, and the correct response to a bug is to fail loudly at the
moment of detection, not to invent a graceful behaviour that hides it and
mutates the world while doing so.

#### Pool sized by DERIVATION, not measurement — [AS BUILT, 2026-08-30]

The pool had been at `kNumChunks` (32,768 = 512 MiB = exactly dense) on the
argument that dense IS the worst case, so nothing below it can be safe and
nothing above it can be needed. The engine aborted twice in ordinary play
anyway (`crash.log` 19:22:50 and 19:42:07, `Alloc` ← `Materialize`).

**Every step of that argument is true and the conclusion is false**, which is
what makes it worth recording. A slot holds at most one page, there are
`kNumChunks` slots, so demand really cannot exceed `kNumChunks`. But **pages
leave the free list by two doors and the argument counts one.** The retire
queue parks freed pages for `kRetireTicks` against in-flight eviction copies,
at up to `kMaxFreeProbesPerTick` per tick — up to **2,048 pages, 6.25% of the
pool, out of circulation**. `Alloc()` fails when `resident + retired ==
kPoolPages`, so the abort band opens at **93.75% residency**, which is exactly
where every observed abort landed.

So `kPoolPages` is now **derived**: `kNumChunks + kPageRetireCeiling` (34,816
pages, 544 MiB at the 512 window, +33 MiB with `actVoxViz`). The guarantee is
arithmetic rather than measured — `Alloc()` only runs for a *sentinel* slot, so
`resident ≤ kNumChunks - 1`, hence `free ≥ 1` always. Both inputs live in
`world.h` beside `kPoolPages`; `pagetable.h` aliases them and `static_assert`s
the relationship, so the three cannot drift.

Three things landed with it, in order of how much diagnosis time they save:

- **`SANDVOX_PT_AUDIT=1`** — recounts resident/free/retired from the
  authoritative structures every tick and aborts on the first imbalance, so a
  leak is caught at the tick it happens rather than thousands of ticks later at
  the bottom of the pool. It found nothing over a full `--selftest` and 1,200
  `--autofly-hard` frames, which is what *excluded* the leak hypotheses and
  left the retire queue as the only remaining mechanism.
- **A verdict line at `Alloc()` failure** naming which of genuine-demand /
  retire-starvation / bookkeeping-leak / lost-or-aliased-pages fired. The old
  report printed `pagesInUse_` and nothing else, which is a bare count and buys
  one hypothesis per reproduction (CLAUDE.md rule 6).
- **A last-chance drain in `Alloc()`** before aborting, so the guarantee does
  not also depend on call ORDER (`RetirePages` runs *after* `Materialize`, so a
  page that aged out this tick is reusable but undrained).

**Sizing input, corrected.** §3.7 sized the pool against `--autofly-hard`, and
that harness peaks at **17,769 / 34,816 (54.2%)**; a full `--selftest` reaches
**23,631 (67.9%)**. Neither comes near the abort band, because both stress
**streaming**. What fills the pool is **activity** — `Materialize`'s set is
`cpuDirty ∪ hasMatter ∪ opTargets ∪ particleChunks ∪ fluidChunks`, and smoke,
fire and particles rising into open sky convert the `PT_EMPTY` sky sentinels
(~half the window, and free) into real pages. **Size against a world-wide wake,
not against flight.**

**`ResetIdentity` had to change too**, and it is the reason a bare constant
bump would have been a silent no-op: it seeds `t[i] = i` for `i < kNumChunks`
and nothing else ever invents a page index, so pages `[kNumChunks, kPoolPages)`
had to be pushed onto the free list explicitly or the headroom would be
allocated in VRAM, counted by `kPoolPages`, and permanently unreachable on
every path that starts from the identity map — worldgen and `LoadWorld`, i.e.
all of them. The spot previously held a comment *asserting* there were no such
pages, true only while the pool was capped at `kNumChunks`.

**The window-size guard rail.** Because the pool now tracks `kWorldN`, a
`static_assert` in `world.h` checks the derived buffer against Vulkan's
`maxStorageBufferRange`, whose spec ceiling is `uint32_t` — 4 GiB − 1, not a
property of any particular card. `kWorldN = 1024` yields 264,192 pages =
**4.03 GiB in one binding and fails to compile**, which is the correct outcome:
doubling the window needs the voxel buffer SPLIT across bindings first, and the
compile error says so at the moment the constant changes rather than after a
build and a crash.

This replaces the first draft's three-tier priority-refusal scheme entirely.
**That scheme is dead** — and it deserves a sentence on why, because it looked
reasonable: it made exhaustion *survivable* at the cost of making it *silent*,
so a mis-sized pool would present as occasional lost matter and hash divergence
in the field rather than as a crash in testing. It also required
`voxStore`'s no-op path to become an expected outcome, which would have
undermined §2.4's guarantee that a page fault is always a bug.

Consequences of the settled policy:

- **The determinism guarantee is not qualified.** The first draft proposed a
  DESIGN.md amendment saying exhaustion "voids the hash guarantee". That is no
  longer needed and would be wrong: an aborted process produces no hash to
  diverge. The DESIGN.md note says **fatal error**, in the same register as an
  out-of-memory allocation failure — a condition the engine detects and refuses
  to continue past, not a caveat on rule 1.
- **`pageFaults` stays a pure bug detector.** Nothing in normal operation can
  make it fire, because the only path that could (a refused materialization) no
  longer exists. That is what makes "assert it is zero" meaningful (§4.4).
- **Pool sizing becomes load-bearing rather than advisory**, which is the
  honest position. §3.7's `kPoolPages = 8192` (1.65× the measured steady state)
  must be validated against the worst case the gates can produce, and
  `pagesHighWater_` must be reported on every `--measure` run so the margin is
  a tracked number. §8 commit 6 does this.
- **The wake-all case is where sizing is actually tested** (§3.2a): without the
  `∩ hasMatter` materialization rule a daylight boundary would demand 32,768
  pages and, under this policy, **crash twice per in-game day**. That rule is
  therefore not an optimization — it is what keeps the abort unreachable.

**What the gates owe this policy.** An abort is only acceptable if the
condition is genuinely unreachable in normal play, so the suite must probe the
margin rather than merely avoid it: the daylight-boundary gate (§3.2a), the
sky-boundary explosion (§4.4), and a deliberate low-`kPoolPages` run asserting
the abort fires cleanly with the right message (§8 commit 5) — testing that the
failure mode *works*, since it is now the only one.

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
  let hashBase = wg.x * CHUNK_VOL;          // SLOT index — the hash key
  var h = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    h += pcg((hashBase + i) ^ (v * 0x9E3779B9u));
  }
  if (T.hashEnable != 0u) { atomicAdd(&wgHash, h); }
  workgroupBarrier();
  if (li == 0u) {
    occupancy[wg.x] = packOcc(CHUNK_VOL,
        select(0u, CHUNK_VOL, isRayBlocker(materials[mat])));
    if (T.hashEnable != 0u) { atomicAdd(&worldHash[0], atomicLoad(&wgHash)); }
  }
  return;
}

// ---- resident: TWO DIFFERENT BASES, and conflating them is a silent desync --
let loadBase = e * CHUNK_VOL;               // PAGE index  — where the words are
let hashBase = wg.x * CHUNK_VOL;            // SLOT index  — what the hash keys on
for (var i = li; i < CHUNK_VOL; i += 64u) {
  let w = voxels[loadBase + i];             // <-- page
  let v = (w & 0xFFFFu) | ((w & STAIN_BITS) >> 8u);
  let m = v & 0xFFFu;
  if (m != MAT_AIR) {
    count += 1u;
    if (isRayBlocker(materials[m])) { block += 1u; }
    if (T.hashEnable != 0u) { h += pcg((hashBase + i) ^ (v * 0x9E3779B9u)); }  // <-- slot
  }
}
```

**Stated once, in the strongest form, because phase 7 found a seventh site that
this section's narrower wording did not cover: an index used as an IDENTITY —
an RNG key, a hash key, a claim-lattice key — is the SLOT index, NEVER the
page index; only a memory address may be the page.** See §4.1a for the
enumeration and for `sim_step:doReactions`, the violation that `main()`'s own
comment predicted verbatim.

**[REVIEW M3 — FIXED.]** The first draft's trailing comment said the dense path
runs "with `base` resolved from the page index", which would feed the *page*
base into `pcg`. That is wrong and it is the kind of wrong that produces a
paged-vs-dense hash mismatch with no other symptom: today's code uses one `base`
for both purposes (`sim_occupancy.wgsl:64`) because under a dense layout page
index **is** slot index. Paging splits them, and **only the load follows the
page; the hash must keep keying on the slot** or every non-identity page
assignment changes the world hash. This is on commit 1's checklist as an
explicit item, and it is a good argument for landing commit 1 as the identity
map: under `pageTable[i] == i` the two bases are equal, so the split can be
introduced and proven hash-neutral before it can possibly differ.

#### 4.1a The two-base rule is NOT only about the hash — [AS BUILT, commit 1]

**[FOUND IN IMPLEMENTATION. The mechanism above is right; its scope was too
narrow, which is this document's documented failure mode — a missing
contributor, not a wrong rule.]**

M3 states the rule for the world hash: *the load follows the page; the key
keys on the slot.* Implementing commit 1 turned up **five more sites with the
same shape and none of them a hash** — and phase 7's paged bring-up turned up a
**sixth, `doReactions`, which is the last row below**. Every one of them would
make a paged run diverge from a dense one while staying perfectly
self-consistent:

| site | what the index keys | what breaks under a page index |
|---|---|---|
| `sim_step:main` | `hash3(T.seed, tick*2+substep, idx)` — the per-cell RNG for every reaction, stain and movement roll | every cell's random stream becomes a function of allocation history |
| `sim_step:doStaining` | `hash3(rnd, 0x51A17u, ni)` — the stain-consumption roll | same, per neighbour |
| `sim_explode:apply` | `hash3(T.seed^0xB0011u, tick, idx)` — ejecta velocity/jitter | same, per destroyed cell |
| `sim_explode:apply` | `hash3(rnd, 0x6217u, idx)` — the micro-grit roll | same |
| `sim_mutate:main` | `hash3(T.seed^0x5EEDu, tick, idx)` — the palette-variant roll | a brush paints different state nibbles depending on which page it landed in |
| `sim_particle:resolve` | `claimSlot(tgt)` — the reinsertion claim lattice | two particles targeting one world cell could hash to different claim slots after a reallocation and **both win** |
| `sim_step:doReactions` | `hash3(rnd, ri, idx)` — the per-rule reaction roll | **[FOUND IN PHASE 7, the seventh site and the one this list missed.]** `doReactions` took only `idx` (the page-resolved address) and keyed the RNG on it, so every reaction roll became a function of allocation history. Invisible under the identity map — dense and paged agree until a page is assigned non-identically. Reproduced as a deterministic lava/stone swap at slot 9450, words 893/1149, in `--vk-smoke-loud --residency paged`. Now takes `slotIdx` alongside `idx`, exactly as `main` does |

**The generalized rule, which is what §4.1 should have said from the start:**

> **A voxel index used as an IDENTITY — an RNG key, a hash key, a claim-lattice
> key, anything whose value must be a property of *where a cell is in the
> world* — must be the SLOT index. Only a memory address may be the PAGE
> index.** The two are equal under the identity map, which is exactly why the
> split is introduced in commit 1 and provable there.

As built, each site carries both: a `slotIdx` / `tgtSlot` from `cellIndexW` for
keying, and an `idx` from `voxWordIndex` for addressing. The comment at each
site says which is which and why.

**Standing obligation, in the same register as §2.1's:** a kernel that derives
a value from a voxel index must say whether that value is an address or an
identity. `voxWordIndex` returns an address; `cellIndexW` returns an identity.
Nothing else may be used for either.

**Phase 7 note on how the sixth site escaped, because the lesson is about the
review method rather than the rule.** The five sites above were found by
auditing *call sites of `hash3`*, and `doReactions`' `hash3` call is inside a
helper whose caller passed `idx` correctly for its addressing use. `main()`'s
own comment predicted this failure in words — the split is documented right
above the call that got it right — so the rule was never in doubt; only its
propagation across a function boundary was. A helper that takes a voxel index
must take **both** bases, or take the one it needs and be named for it. All
`hash3` sites in the sim shaders were re-audited at that commit and this was
the only remaining offender.

The same split applies to `mainDirty` (`sim_occupancy.wgsl:35`,
`base = dirtyList[wg.x] * CHUNK_VOL`) — but `mainDirty` computes no hash, so it
needs only the load base. Stated so nobody "fixes" it symmetrically.

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

**`mainDirty` needs the same treatment, and its sentinel branch is
MANDATORY — not a belt.** **[REVIEW M3, second half — FIXED.]** The first draft
claimed `mainDirty` "can never see a sentinel" because every chunk in
`dirtyList` is in the materialization set. **That was true only under the first
draft's materialization rule, and §3.2a deleted it.** Under the corrected rule
(§3.2a fix 2) the set is `(cpuDirty ∩ hasMatter) ∪ N26(...)`, which
deliberately does **not** materialize a dirty EMPTY chunk with no non-empty
neighbour — and a wake-all makes *every* chunk dirty, so after any daylight
boundary `dirtyList` is full of sentinel chunks. `mainDirty` sees them
routinely, by design.

It takes the same analytic branch (occupancy only — no hash). It must **not**
increment `pageFaults`: a sentinel here is now expected, not a fault. That
distinction matters, because a counter that fires in normal operation is a
counter nobody looks at.

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

**Gate A — paged-vs-dense hash equality on the loud scenario, plus the pinned
sequence.** Reuse `--vk-smoke-loud`'s 19-probe driver, which is being
repurposed as a **Vulkan-only pinned-hash-sequence regression gate** (its 19
known values become expected constants — §6.1). The scenario is already exactly
what this phase needs to stress: brush + melt ops, three explosions at
t45/t52/t75 (the mark/apply split, the whole particle chain), exact-cell stamps
anchored to `world.WindowOrigin()`, the readback ring live every tick, and an
**8-shift streaming walk with eviction and procgen refill** at t85–t100. That
last leg is the page alloc/free/realloc path under streaming, for free.

So Gate A asserts two things, not one: paged **==** dense (the live
differential), and both **==** the pinned constants (the historical oracle).

The comparison machinery in `src/gpu/vk_smoke.cpp` needs the axis changed from
backend to residency: `RunScenario(kind, loud, ...)` at `:128` gains a
residency parameter and loses its `kind` variation; `CompareAndReport(name, a,
b, validation)` at `:214` needs label parameters, since its column header is the
hardcoded `"  stage              Dawn         Vulkan\n"` (`:228`) and `KindName`
(`:123`) hardcodes the two backend names. **Coordinate with the Dawn-removal
agent** — that project is touching the same function for the same reason, and
two sessions rewriting `CompareAndReport` independently is the collision
CLAUDE.md's board exists to prevent.

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
7. **`pageFaults == 0`** for the whole gate — always available, since the
   counter is unconditional (§5.1). This is the assertion that turns §2.4's
   structural claim into evidence.
8. **CPU/GPU synthesis agreement** (§2.1a): assert the C++ `SynthWord` and the
   shader's `synthWord` produce identical words for every sentinel kind in
   play, by comparing a mirror-synthesized chunk against a GPU readback of the
   same chunk after forcing materialization.

The gate must anchor to `world.WindowOrigin()`, not a fixed world position —
gates run in `kOrder` sequence and the streaming gate leaves the origin ~20
chunks out (the documented trap that made the spell gate detonate on tick 1).

**Gate D — the daylight-boundary gate (§3.2a), and the suite structurally
cannot do without it.** `EncodeWakeAll` is never called anywhere in the current
suite, because every day/night gate pins the phase
(`selftest_sim.cpp:242-243` freezes at midnight, `:389` at noon). So the
wake-all path — which sets all 32,768 dirty flags — has **zero** test coverage
today, paging or no paging. This gate: settle a world, run across a daylight
transition with `dayNight.freeze` **off**, and assert
(a) the hash matches a dense run, (b) `pagesInUse_ < kPoolPages` at every tick
(the abort in §3.8 must stay unreachable), (c) `pageFaults == 0`, (d) chunks
return to sleep afterwards.

**Gate E — the existing suite.** All gates green in **both residency modes**
(dense is now the only oracle — §6.3), `determinism` reporting the pinned
`7cfa2420`, both known failures failing the same way. If any gate's *detail
string* changes, the phase changed behaviour.

---

## 5. Pass table and barriers

The phase-6 handoff's warning is exact and applies here verbatim: **a missed
`uses` entry means the generator emits no barrier for that hazard.** (The
warning's original form said "invisible under Dawn"; with Dawn removed it is
invisible full stop, which makes the checker more important, not less.)
Everything in this section lands in the *same commit* as the shader change that
needs it.

### 5.1 New buffer ids

`pass::Buf` (`src/sim/pass_table.h:29-65`) gains **`PageTable`** and
**`PageFaults`** before `kCount`.

**`PageFaults` is permanent, always-bound and unconditional** (16 bytes, an
atomic counter, §2.4). No `PAGE_ASSERT` prelude flag, no conditional binding,
no `#if` in `pass_table.def`. It gets its `Buf` id, a permanent binding, and an
`A(PageFaults)` use on every row that calls `voxStore`. The atomic increment
sits on a branch that is never taken in a correct build, so its production cost
is a branch that never fires — and in exchange there is **one** bind-group
layout, **one** `.def`, and no configuration under which the pass table and the
shaders disagree. *(This supersedes the first draft's conditional design, which
created a flag-dependent layout — the exact "two things that must agree" shape
this repo has a checker to prevent. See §9 Q8, closed.)*

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

`pageTable` and `pageFaults` bind in **group 0**, alongside `voxels`:
`simBGL_` bindings 17 and 18 (the first free slots after `genList` at 16), and
`simSlimBGL_` bindings 5 and 6 (extending 0–4).

**Group 0 is not negotiable, and that is what makes §2.1's shared accessor
work.** `common.wgsl` is prepended to every shader, so the accessors must name
a group whose meaning is identical everywhere. Group 1 differs by pipeline
(`particleBGL_` vs `farBGL_` vs `renderPartBGL_`), so a group-1 `pageTable`
would have to be declared per shader rather than once — which dissolves the
single-seam property the whole design rests on.

#### 5.2a The render layouts — **[REVIEW C3 — FIXED]**

The first draft counted `simPL_`, `simPL2_` and `farPL_` and **omitted the
render pipelines entirely**. That is a correctness hole, not an accounting one:
`raymarch.wgsl` performs **17 raw `voxels[cellIndexW(...)]` reads** — verified
by grep, at lines 722, 730, 935, 943, 1181, 1829, 2028, 2282, 2437, 3182, 3262,
3304, 3816, 3855, 4241, 4381, 4464. Under paging every one of those indexes the
*pool* with a *slot-derived* index and samples **the wrong chunk** wherever the
target is a sentinel. The world would render as garbage while hashing
perfectly — the render path is outside the hashed domain, so no determinism
gate could ever catch it.

So the seam covers the renderer too, and `renderBGL_` gains `pageTable`.

**Layout counts, storage entries only** (uniforms do not count), from
`simulation.cpp:82-121`, `:139-155`, `:209-219`:

| layout | groups | storage today | + page bufs | note |
|---|---|---|---|---|
| `simPL_` | `simBGL_` | 14 | **16** | sim |
| `simPL2_` | `simSlimBGL_` (4) + `particleBGL_` (8) | 12 | **14** | sim |
| `farPL_` | `simSlimBGL_` (4) + `farBGL_` (4) | 8 | **10** | see below |
| `renderPL_` | `renderBGL_` (7) + `renderPartBGL_` | 7+ | **8+** | **was missing** |
| `microBodyPL_` | `renderBGL_` (7) + `microBodyBGL_` | 7+ | **8+** | **was missing** |

`renderBGL_` has ample headroom — 7 fragment storage entries today (bindings 0,
1, 2, 4, 5, 7, 8; bindings 3 and 6 are uniforms), and the comment at
`simulation.cpp:145-149` already reasons about this ceiling. `pageTable` takes
binding 9. It is `ReadOnlyStorage, S::Fragment`, matching `voxels` at binding 0.

`microBodyBGL_` does **not** need `pageTable` itself — `microbody.wgsl` reads
its own brick pool, not `voxels` — but it shares `renderBGL_` as group 0, so it
inherits the binding for free. Confirmed: the `cellIndexW` grep reports 0 hits
in `microbody.wgsl`, `debris.wgsl` and `debug_lines.wgsl`.

**`fardown` is covered, and the coverage is now deliberate rather than lucky.**
`worldgen.wgsl:fardown` reads `voxels` and runs on `farPL_` = `simSlimBGL_` +
`farBGL_`. Because `pageTable` goes into `simSlimBGL_` at binding 5 — the *slim
prefix*, not just the full `simBGL_` — `fardown` gets it automatically. That is
a consequence of putting the page buffers in the slim prefix and it should be
stated, not discovered: **any future pipeline built on `simSlimBGL_` inherits
translation, and any built on a group 0 that is not `simBGL_`/`simSlimBGL_`/
`renderBGL_` does not.** Those three are the complete list of group-0 layouts
that can address `voxels`.

#### 5.2b Slot pressure — **RESOLVED BY DAWN REMOVAL**

The first draft spent considerable space on `simPL_` landing at 15–16 of Dawn's
**16 storage buffers per stage across all bind groups in one pipeline layout**
(barrier_graph §4.10 — the limit that `simSlimBGL_` exists to work around), and
derived a flag-conditional `pageFaults` binding to stay under it.

**That constraint no longer exists.** Dawn is being removed entirely (user
decision, in progress in parallel). Vulkan's
`maxPerStageDescriptorStorageBuffers` on this hardware is **1,048,576**
(phase 3a capability record) — the limit was always a *Dawn* limit, and the
phase-3a record said so at the time.

Consequences, recorded rather than deleted because the analysis is what
justifies the conclusion:

- `simPL_` at 16 storage buffers is fine. So is 30.
- `pageFaults` is unconditional and permanently bound (§5.1). The
  `PAGE_ASSERT`-conditional layout is dead.
- **§9 Q9 (should this phase pre-emptively split `simBGL_` to leave room for
  ROADMAP §3.1's cell masks?) is moot** — there is no ceiling to leave room
  under. Cell masks can simply take a binding.
- Risk 7 is closed for the same reason.

One thing *not* moot: `simSlimBGL_` still exists and is still worth keeping, but
its justification changes from "Dawn's layout limit forces it" to "the
particle/explosion/far pipelines genuinely do not need 17 bindings". Worth a
comment correction in `simulation.cpp:104-105`, whose current text cites the
Dawn limit — **but that comment belongs to the Dawn-removal commit, not this
phase.** Flag it to that agent rather than editing it here.

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

**`A(PageFaults)` on every row that WRITES voxels**: `mutate`, `mutateCells`,
`explodeApply`, `ca`, `particleResolve`, `worldgen`, `worldgenList`.
Unconditional — no `#if`, no flag-dependent row (§5.1). Reading a sentinel is
legal, so read-only rows can never fault and must not declare it.

**Barrier consequence, and it is benign.** `pageTable` is written only by
uploads and fills that drain at the *head* of a command buffer (§5.4), and read
by ~20 rows thereafter. The §3.3 tracker emits **one** TRANSFER→COMPUTE barrier
at the first reading row and **nothing** for the rest (read-after-read emits
nothing). In the CA loop specifically it emits nothing after iteration 0, for
the same reason `materials` and `reactions` do — and the CA's global barrier
(form B, `SHADER_STORAGE_READ|WRITE` at `COMPUTE_SHADER`) covers both new
buffers' access domain anyway, including `pageFaults`' atomic.

**One thing the addition genuinely changes**: `ca`'s `uses` count goes 9 → 10
(`R(PageTable)`) → **11** (`A(PageFaults)`). `kMaxUses = 10`
(`pass_table.h:164`) with `AllUsesFit()` a `static_assert` at
`pass_table.cpp:154`, so **the current ceiling is exceeded and the build stops
until it is raised.** Raise `kMaxUses` to 12 in commit 1 — not to 11, so the
next row addition does not repeat this.

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

**(3) `pageFaults` rides the readback ring.** Risk 1's residual mitigation folds
the 16-byte counter into `World::EncodeReadbacks`' existing copies (alongside
`hash`, `pick`, `particleCounts` — all the same shape). That is one more
`CopyTracked` per readback tick and one more `pass::Buf` id in the slot layout
(`kSlotBytes`), plus a 256-padded region. It is not a table row, for the same
reason none of the readback copies are (phase 3c's ruling). No clear-after-copy
is needed — unlike `support`, the counter is monotonic and a non-zero value is
a permanent "this build has a bug" latch, which is the desired semantics.

**The `--shot` far-fill loop and the streaming genList submit** both record
command buffers outside the tick. Each opens with the §3.4 head global barrier
and drains the pending queue, so a page fill enqueued before either of them
lands correctly. No special case.

**[REVIEW m5 — the headless harnesses.]** `--shot` and `--shot-mob` **render**,
so they inherit C3 entirely: they need `pageTable` bound in `renderBGL_` and
they exercise the raymarch translation path. Nothing extra is required beyond
§5.2a — noted so the render-path fix is not scoped to "the game" and quietly
skipped for the shot harnesses, which are exactly where a sentinel-sampling bug
would be most visible (they photograph sky boundaries by design).

`--measure` is the other headless path, and commit 0's uniformity histogram
does a **blocking whole-buffer read of `voxels`** — the C4 shape. **Decision:
`--measure`'s histogram runs `--residency dense` only**, and says so in a
`printf` at the top of the measurement so a reader cannot mistake a dense
number for a paged one. The measurement exists to *size* the pool, so it wants
the dense ground truth anyway; making it page-aware would mean synthesizing
sentinel chunks to count them, which is circular. The separate
`pagesInUse_`/`pagesHighWater_` reporting (commit 5) is what covers the paged
side.

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

## 6. The oracle strategy — **DAWN IS GONE**

**Decision (user, settled): Dawn is being removed entirely, in parallel with
this work.** The first draft of this section assumed Dawn survived through
phase 7 as the hash oracle (phase 6's stated plan) and built a three-way
equality matrix on it. That plan is superseded.

This *removes* a safety net at the moment of the port's riskiest change, so
what replaces it has to be named precisely rather than waved at.

### 6.1 The equality matrix — two configurations, plus a pinned sequence

| # | configuration | role |
|---|---|---|
| 1 | **vulkan-paged** | the new thing |
| 2 | **vulkan-dense** | the oracle: identity map, address-identical to pre-phase code |
| — | `tests/baseline.json` `7cfa2420` + `--vk-smoke-loud`'s 19 pinned values | the *historical* oracle, frozen in the repo |

Both configurations must produce identical hashes at every probe, and both must
reproduce the pinned constants.

**What is lost, stated honestly:** the first draft's `1 == 2, 2 ≠ 3` diagnosis —
"the bug is in the Vulkan backend and predates this phase" — is no longer
available as a *live* experiment. Nothing can re-derive the expected hash
independently any more.

**What replaces it, and why it is adequate here:**

1. **The pinned values are the oracle, and they are already Dawn-derived.**
   `7cfa2420` and the 19 `--vk-smoke-loud` probes (`f97ba745` … `cb036bd1`)
   were produced and cross-validated while Dawn existed. They are frozen
   constants in the repo now. A cross-backend oracle answers "do two
   implementations agree today"; a pinned constant answers "does this build
   agree with the world we shipped" — which for *this* phase is the stronger
   question, because phase 7's whole claim is that nothing changes.
2. **`--vk-smoke-loud` is being repurposed as exactly that**: a Vulkan-only
   pinned-hash-sequence regression gate, where the 19 known values become
   expected constants rather than a live Dawn-vs-Vulkan comparison. This phase
   consumes it in that form (§4.4).
3. **The identity map is a *within-backend* oracle for the part that matters.**
   `--residency dense` produces bit-identical *addresses* to pre-phase code
   (§6.2), so `1 ≠ 2` still isolates paging from everything else — which is the
   diagnosis this phase actually needs. Losing the Dawn leg costs the ability
   to blame the Vulkan backend, not the ability to blame paging.

**A note for whoever sequences these two projects:** if Dawn removal and phase 7
land close together and a pinned hash moves, the first question is which
project moved it. Landing commit 1 (a provable no-op) *before* Dawn removal
completes would settle that cheaply — but this is a scheduling preference, not
a dependency, and the pinned constants make it recoverable either way.

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

**With Dawn gone, `--residency dense` is no longer merely convenient — it is
the only live oracle the engine has.** That raises its status from "developer
flag worth keeping" to "load-bearing test infrastructure", and it means the
dense path must stay exercised: **every gate runs in both residency modes** in
the suite (§8 commit 5), not just the page gate. A dense mode that quietly
stops working would remove the phase's only differential test.

---

## 7. Risk register

Ranked by (probability × cost of a late discovery).

### Risk 1 — a write reaches an unmaterialized page

**Why dangerous.** It is the determinism nightmare in its purest form. The
write either lands nowhere (a lost voxel) or, in a naive implementation, lands
in *another chunk's* memory (a corrupted bystander). Either way the world hash
diverges at a tick and a location with no causal relationship to the bug — and
with Dawn gone there is no second implementation to reproduce it against.

**How neutralized — structurally, not by care.**

1. **There is no writable accessor that accepts a sentinel.** `voxWordIndex`
   returns `PT_NO_WORD` and `voxStore` tests for it *before* indexing. The
   worst case is a no-op, never a stranger's voxel. This is a property of the
   function signatures, not of anyone remembering a rule (§2.4).
2. **`PT_NO_WORD` is not a valid index**, so even a hypothetical unguarded
   `voxels[PT_NO_WORD]` is an out-of-bounds access that WGSL clamps, rather
   than a plausible-looking in-range address.
3. **`pageFaults` counts every occurrence**, unconditionally and on every run,
   converting "impossible" into "measured zero".
4. **§3's materialization set is a proven superset**, with the corner-chunk
   correction (26-neighbourhood, not 6) and the incremental CPU mirror that
   never under-approximates.

**What test.** Gate B step 7 asserts `pageFaults == 0` across the whole gate,
including the sky-boundary explosion. The full 23-gate suite runs with the
assert enabled in a dedicated CI-style invocation. And the loud scenario's
paged-vs-dense hash equality is the end-to-end detector: a lost voxel moves the
hash.

**Residual — much smaller than the first draft's.** The counter is always live,
so a page fault is *recorded* even in a scenario no gate covers; what remains
is only that nothing in the game loop *reads* it every frame. Mitigation: the
readback ring already carries a slot's worth of small counters, so fold
`pageFaults` into the existing snapshot copy and log once if it is ever
non-zero. Cheap, and it makes the detector work in ordinary play rather than
only under test.

### Risk 2 — the one-tick-late dirty knowledge is insufficient

**Why dangerous.** This is the risk I spent the most of §3.2 on because the
brief's proposed rule (materialize `dirtyOut(last tick) ∪ neighbours`) is
**refuted**: the readback ring can decline a tick entirely
(`world.cpp:110-113`), the map is async and the frame runs up to 4 ticks, so
the CPU's newest `dirtyFlags` can be many ticks stale. A stale set
under-approximates, and under-approximation is risk 1.

**How neutralized.** **[REVIEW NEW-5 — FIXED; this paragraph previously said
"reset to ground truth whenever a snapshot actually arrives", the exact framing
§3.2 exists to refute, while citing §3.2.]** The CPU maintains `cpuDirty`
**incrementally**: a one-ring dilation per tick plus the enumerated CPU
dirty-writers (§3.1a), per step (1) of the normative definitions. An arriving
snapshot is **a second superset — usually tighter, never ground truth about the
tick being encoded** — and it is composed by **intersection** (step 2), never
by assignment. Both operands are supersets, so the result is a superset that is
at least as tight as either. It therefore cannot under-approximate; it
over-approximates by one ring per missed snapshot and self-heals on the next
one that lands.

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

### Risk 4 — pool exhaustion — the risk changes SHAPE under the fatal policy

**Why dangerous, restated.** Under §3.8's settled policy exhaustion is a
**fatal error**, so the risk is no longer "the world diverges under load" — it
is "**the game crashes**". That is a better failure (loud, immediate,
attributable) but a more consequential one, and it moves the entire weight of
this risk onto **pool sizing** and onto the materialization set staying tight.

**How neutralized.**

1. **The `∩ hasMatter` rule (§3.2a / M2) is what keeps the abort
   unreachable.** Without it a daylight wake-all demands 32,768 pages from an
   8,192-page pool and crashes twice per in-game day. With it, a wake-all
   demands the non-empty set plus a ring — the same order as steady state.
2. **`kPoolPages = 8192` is 1.65× the measured steady state** (§3.7), and
   `pagesHighWater_` is reported on every `--measure` run so the margin is a
   tracked number rather than an assumption.
3. **Over-approximation is bounded per tick** (a 1-ring per missed snapshot,
   §3.2), so the materialization set cannot drift upward without bound.

**What test.** Three, and they are the phase's real safety net now:
(a) the **daylight-boundary gate** (§3.2a / Gate D) asserting
`pagesInUse_ < kPoolPages` throughout — the worst realistic spike;
(b) a **deliberate low-`kPoolPages` run** asserting the abort fires cleanly
with the right message, i.e. testing that the failure mode *works*, since it is
now the only one; (c) `pagesHighWater_` on the loud scenario and on
`--measure`, so the margin is measured rather than hoped for.

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

### Risk 7 — the Dawn storage-buffer layout limit — **CLOSED (Dawn removed)**

**Why it was dangerous.** A blocked Dawn build would have removed the hash
oracle at the moment it was most needed. `simBGL_` at 14 storage buffers plus
two new ones lands at 16 — exactly Dawn's per-stage layout ceiling.

**Status: moot.** Dawn is being removed entirely (§6). Vulkan reports
`maxPerStageDescriptorStorageBuffers` = **1,048,576** (phase 3a) — the ceiling
was always a Dawn limit, as the phase-3a record noted at the time. The analysis
is retained in §5.2b rather than deleted, because it is what justifies calling
this closed rather than merely unlikely.

**Consequence for the risk that replaces it:** with no cross-implementation
oracle, `--residency dense` is the only live differential test the phase has
(§6.3), so the real risk moves to "dense mode silently rots". That is why every
gate runs in both residency modes rather than just the page gate.

### Risk 7b — no cross-implementation oracle for the CA rewrite

**Why dangerous.** Commit 1 rewrites every voxel access in every sim *and*
render shader. Previously such a change was checked by two independent
implementations agreeing; now it is checked against pinned constants only.

**How neutralized.** The identity map (§6.2) makes commit 1 address-identical
to pre-phase code, so the pinned `7cfa2420` and the 19 `--vk-smoke-loud` values
are a *complete* check of it — they were produced by the code being replaced,
and any arithmetic error moves them. This is precisely why commit 1 is
structured as a provable no-op rather than landing translation and sentinels
together.

**What test.** Commit 1's gate, which is the strongest checkpoint in the plan
and the one not to weaken: *every* pinned value, unchanged, plus
`check_pass_table.py` and `check_invariants.py` silent.

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

Every checkpoint = `bash scripts/build.sh --selftest` green (known failures
carried), with the actual output in the commit message. From commit 2 onward
that means **both residency modes** — dense is the oracle now (§6.3). Board
discipline per CLAUDE.md: `claim` before editing, `done` with what landed,
`note` for the cross-cutting constants — and in particular **coordinate with the
Dawn-removal agent**, which is touching `vk_smoke.cpp` and `simulation.cpp`
concurrently.

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

*Gate: full suite green; `determinism` reports `7cfa2420`; `--vk-smoke-loud`
19/19 against the pinned constants; `check_pass_table.py` OK;
`check_invariants.py` OK; zero validation messages.* Any hash movement here is
a translation bug and must be fixed, not absorbed.
*Also: `--measure` on all three scenarios, to isolate risk 8's cost before
sentinels muddy it.*

**Commit 1's explicit checklist** (the items most likely to be skipped):

- [ ] **Render shaders too** (§5.2a, C3): `raymarch.wgsl`'s 17 raw reads;
      `pageTable` in `renderBGL_` binding 9.
- [ ] **The CPU seam** (§2.1a, C4): `World::PageOffsetOfSlot` and all five
      sites — mirror, chunk-fetch, eviction, store-hit refill, selftest dumps.
      At the identity map every one is a no-op, which is exactly why they can
      be converted here and proven harmless.
- [ ] **Two bases in `sim_occupancy:main`** (§4.1, M3): load from the page,
      hash on the slot. Equal under the identity map — introduce the split
      while it cannot differ.
- [ ] `kMaxUses` 10 → 12 (`ca` goes to 11 uses).
- [ ] Stale comments: `world.h:29-30` (`kNChunk`/`kNumChunks`) and
      `vulkan_pass_map.md §3a`.
- [ ] `--measure` runs dense-only or iterates the table (m5) — decide and say
      so in the code.

---

**Commit 2 — `EMPTY` sentinels + materialization, together.**

**[REVIEW M4 — FIXED, by merging what were commits 2 and 3.]** The first draft
made commit 2 "sentinels exist, no materialization machinery", claiming the
pool stayed effectively dense so exhaustion could not happen. **That commit was
not independently green**: the moment a sentinel exists, any gate that paints
into sky (the brush gates, `player-plants`, the spell gate) writes into a chunk
with no page, `voxStore` no-ops it, and matter is lost — a red suite, not a
checkpoint. There is no coherent halfway state where sentinels exist and
nothing can create a page, because *the gates create pages*.

The reviewer's alternative (b) — mark `EMPTY` only for chunks that are all-air
**and** have no non-air chunk in their 26-neighbourhood **and** are outside
CPU-op reach — would be independently green, but it is a throwaway predicate
that exists for one commit and is then deleted, and it duplicates the
materialization rule it is standing in for. **Merging is the better trade.**

So this commit lands: `--residency dense|paged`; worldgen classification into
`PT_EMPTY`; `cpuDirty` (§3.2) with the intersection-tightening rule;
`EncodeWakeAll` setting the mirror (§3.2a); the `∩ hasMatter` materialization
rule (§3.2a / M2); CPU-op target sets (§3.3); the particle set (§3.4) unioned
into `cpuDirty`; the free-list allocator; queued page fills (§5.4); worldgen
batching (§3.5c); streaming/`LoadWorld` classification (§3.5d,e); the fatal
exhaustion check (§3.8). **No deallocation** — pages are allocated and never
freed, so `pagesInUse_` is monotonic and a hash divergence is definitively an
*under*-materialization (risk 1/2) rather than a premature free.

*Gate: suite green in both residency modes; paged == dense on the loud
scenario including the new sky-boundary explosion; both == the pinned
constants; the ring-starvation test (risk 2); the daylight-boundary test
(§3.2a / C2) — which at this commit is the one that proves the wake-all does
not exhaust the pool; `pageFaults == 0`; `pagesHighWater_` reported.*

---

**Commit 3 — deallocation with hysteresis.**
The consecutive-zero counter in the snapshot callback, the `!cpuDirty`
conjunct, `kPageFreeTicks`, the serial-stamped retire queue (risk 5), and the
eviction fast path for sentinel slots (§4.2 — mandatory per C4, not optional).
If commit 0 said UNIFORM is worth it, streaming/load-path demotion lands here.

*Gate: suite green both modes; the extended `sleep` gate asserting zero page
fills and zero frees over the last 50 settled ticks (risk 6); the
outstanding-eviction streaming test (risk 5); loud-scenario equality; the
daylight gate re-run to confirm deallocation resumes after a wake-all
(§3.2a fix 3).*

---

**Commit 4 — the gates.**
`page-roundtrip` in `selftest_sim.cpp` + `kOrder` (all 8 steps, §4.4 Gate B);
the daylight-boundary gate (Gate D) promoted from a manual run; the
low-`kPoolPages` abort test (§3.8); the ring-starvation test promoted to a
gate; `--vk-smoke-loud` gaining the residency axis (coordinate with the
Dawn-removal agent, §4.4); **the per-gate residency decision list** (§2.1a):
which gates run dense-only because they dump raw voxels, and which run both.

*Gate: 25 gates green, both residency modes.*

---

**Commit 5 — measurement, docs, and the acceptance record.**
`--measure` reports `pagesInUse_`, `pagesHighWater_`, the pool reservation, and
the sentinel-kind histogram. `PLAN_vulkan_port.md` phase 7 gains its
`[AS BUILT]` block. `DESIGN.md` gains the page table in §3 (a residency
mechanism, which §3 owns) plus a §14 note that **pool exhaustion is a fatal
error** (§3.8 — not a caveat on rule 1). This document gains the measured
numbers. `docs/vulkan_barrier_graph.md` gains a §2.4 note for the queued page
fills, and a `pass_table.def` comment records the two new buffers.

### Final acceptance

| criterion | target |
|---|---|
| paged vs dense, loud scenario | **19+/19+ MATCH**, including the sky-boundary probe |
| both modes vs the pinned constants | `f97ba745` … `cb036bd1`, unchanged |
| `determinism` gate, both modes | `7cfa2420` over 200 ticks |
| full suite | 25 gates, exit 0, both residency modes, known failures with **character-identical** detail strings |
| daylight-boundary gate | passes with `dayNight.freeze` OFF; `pagesInUse_ < kPoolPages` throughout |
| sync validation | **0 messages** |
| `pageFaults` | **0** across the suite |
| resident memory, settled | `pagesInUse_ × 16 KiB` reported against **77.7 MiB** measured / **86.9 MiB** estimated; pool reservation reported separately (§3.7) |
| settled tick | reported against **229–236 µs**; a regression beyond ~5% is a finding, not a footnote |
| `check_pass_table.py`, `check_invariants.py` | silent |

---

## 9. Open questions

### Closed by user decision

- **Q4 — exhaustion policy. CLOSED: fatal error, all modes.** Not a documented
  hole in rule 1, not tiered refusal. If the pool can exhaust in normal play it
  is mis-sized. §3.8 rewritten; the three-tier priority scheme is deleted.
- **Q8 — the `pageFaults` counter. CLOSED: keep, unconditional.** Permanently
  bound in `simSlimBGL_`/`simBGL_`, no `PAGE_ASSERT` prelude flag, no
  conditional `USES(...)`. The increment lives on a branch never taken in a
  correct build. One layout, one `.def`. §5.1/§5.3 rewritten.
- **Q9 — `simBGL_` slot pressure. MOOT: Dawn removed.** The 16-storage-buffer
  ceiling was a Dawn limit; Vulkan reports 1,048,576. §5.2b records the
  analysis and marks it resolved. Risk 7 closed on the same grounds.
- **Q6 — `--residency dense` as a permanent mode. CLOSED: yes, and it is now
  load-bearing rather than convenient** — with Dawn gone it is the only live
  differential oracle, so every gate runs in both modes (§6.3).

### Still open

1. **§3.5c, worldgen's dense transient.** I recommend batching (16 submits of
   2,048 slots) over a dense-for-one-submit peak, because the latter forfeits
   the saving exactly at startup and becomes impossible at a grown window.
   **Now sharper under the fatal-exhaustion policy: a dense transient at
   worldgen means `kPoolPages` must be ≥ 32,768 or startup aborts.** That
   arguably settles it — batching is not a nicety, it is what lets the pool be
   smaller than dense at all. Confirm.
2. **§3.6, UNIFORM scope. ANSWERED by commit 0 — see the [MEASURED] block in
   §3.6. 41 chunks of 32,768 are whole-word uniform (0.6 MiB). Tick-path
   discovery is not worth building; the recommendation stands unchanged.**
   Tick-path uniformity discovery deferred pending
   commit 0's measurement; UNIFORM implemented only where the CPU already has
   the words. If the 2,338 full chunks turn out mostly single-word, that flips.
3. **§3.7, `kPoolPages = 8192` (128 MiB, 1.65×).** Sized against one seed. Under
   the fatal-abort policy this number is now safety-critical rather than
   advisory: too tight is a crash, too loose is wasted VRAM. The
   daylight-boundary and low-pool gates probe it, but the honest answer needs
   commit 5's high-water measurements across real scenarios.
4. **§3.4, deriving the particle dilation radius from `TUNE_PART_MAX_VEL` at
   load.** It makes a *tuning* value load-bearing for *memory correctness* — a
   new kind of coupling here, and hot-reloadable (F5), so the radius must be
   recomputed on reload. Better alternative?
5. **§2.3's translation cost (risk 8).** Unmeasured. If commit 1 shows the CA
   materially slower, is the workgroup-uniform hoist sufficient?
6. **NEW — §3.2's `C(j)` retention ring.** The corrected recurrence needs the
   per-tick CPU-op sets kept for `M−S−1` ticks to roll a snapshot forward. That
   is a small bounded ring, but it is new state. My proposal: size it to the
   readback ring depth × max ticks/frame = 3 × 4 = 12, and **skip the tightening
   entirely** when the snapshot is older than the ring can cover — never tighten
   with an incomplete superset.

   **Sizing is a performance knob, not a correctness one** (reviewer's note,
   and it is the right framing): 12 bounds *frame-loop* staleness only, and a
   long GPU stall can exceed it. When it does, the skip-tightening path handles
   it correctly — `cpuDirty` simply stays at step (1)'s wider estimate until a
   coverable snapshot arrives. An undersized ring costs extra materialized
   pages, never a missed one. Confirm the degradation is acceptable; there is
   no soundness question here.
7. **NEW — §3.2a, the wake-all deallocation stall.** After a daylight boundary
   `cpuDirty` is all-ones, so *no* page is eligible to be freed until it
   shrinks. Bounded (the next snapshot collapses it) but it is a real resident
   spike at dawn and dusk. Acceptable, or should the free condition use a
   different dirtiness test that a wake-all does not saturate?

---

# §9. The JITTER sentinel [AS BUILT 2026-08-23]

The follow-up `world.h`'s `kPoolPages` note called for: *"~all of [the
underground working set] is single-material-with-state chunks a widened sentinel
could represent (§3.6's 2,115-chunk finding, which the game window multiplies)."*
This is that widened sentinel.

## 9.1 What it is

A third sentinel form, `JITTER(mat)` — bit 30 of a sentinel entry — meaning
**"every cell is `mat`, stainless, `kStampNever`, and its state nibble is the
worldgen palette variant for that cell's world position."**

It exists because `UNIFORM`'s whole-word rule is exact but nearly useless
underground. `genCell` gives every solid cell a variant
`hash3(seed ^ 0xC0FFEE, x ^ (z<<12), y) % 3`, so a chunk of plain stone holds
three distinct words and cannot be `UNIFORM`. Commit 0 measured the gap: **41 of
32,768 chunks are whole-word uniform against 2,115 that are one material with
mixed state.**

The variant is not random — it is a pure function of position and seed — so the
chunk's 4,096 words are describable by 4 bytes plus a formula.

## 9.2 The two consequences that shaped the implementation

1. **Synthesis is POSITIONAL.** `SynthWord(entry)` cannot serve a JITTER
   sentinel; every synthesis site needs the cell's world coordinate. The
   chunk-linear sites hold a SLOT index, and a slot is not a position — the
   window is toroidal — so they recover the world chunk through the window
   origin. New: `SynthWordAt` / `JitterStateFor` (C++), `synthWordAt` /
   `synthJitterState` / `worldCellOfSlotLocal` (WGSL).
2. **Materialization is no longer a `vkCmdFillBuffer`.** A 32-bit pattern cannot
   express per-cell variation, so a JITTER page is filled by a dispatch —
   `worldgen.wgsl:pagefill`, table `PT_PAGEFILL`, over its own
   `pageFillList` buffer of (slot, entry) pairs. EMPTY and UNIFORM keep the
   one-command fill.

## 9.3 Measured

| | pages | resident | determinism hash |
|---|---|---|---|
| dense | — | 512 MiB | `7cfa2420` |
| paged, JITTER off | 4,975 | 77.7 MiB | `7cfa2420` |
| paged, JITTER on | **2,861** | **44.7 MiB** | `7cfa2420` |

Suite high-water 14,934 → 11,284 of 24,576.

**The game window gains far less: 16,420 → 15,545 pages (5%), against the
harness's 42%.** This is the §3.7 "synthetic numbers lie" lesson again and it is
recorded here rather than buried: the selftest window is sky-heavy with a large
pristine stone bulk, while the real player-centred window is full of disturbed,
mixed-material and cave-boundary terrain that no single-material sentinel can
represent. `kPoolPages` is therefore left at 24,576 — this change does not
license a smaller pool.

## 9.4 The four bugs, because each was a class

Every one produced a world-hash divergence with `pageFaults == 0`, and none was
in the synthesis formula (which was correct from the first build — verified by
recomputing slot 0's words by hand against the resident page).

1. **`T.genCount` in the fill kernel.** `pagefill` guarded on `wg.x >=
   T.genCount`, but the tick UBO's `genCount` is written only by
   `Stream::FillSlots`. `EncodePageFill` sets the recorder's dispatch *extent*
   and never touches the uniform, so the guard read the previous tick's value —
   0 — and every workgroup returned immediately. 2,114 chunks of stone
   materialized as all-zero pages, i.e. silently became air. **The dispatch
   extent IS the bound; the guard was removed.**
2. **Deferred-write ordering.** `rhi::Queue::WriteBuffer` is a *deferred* host
   write that drains at the head of the NEXT command buffer. The upload was
   written as an argument to `EncodePageFill(enc, UploadJitterFills(queue))`,
   i.e. after `CreateCommandEncoder`, so the list drained one command buffer too
   late and the dispatch read stale pairs. The upload now happens **before the
   encoder exists**.
3. **`genList` aliasing.** Sharing `genList` was documented as safe because "page
   fills and worldgen list-fills are always separate submits" — false, because
   `Stream::FillSlots` writes it mid-frame while a page fill drains at the head
   of the next buffer, and the two deferred writes interleave. Page fills got
   their own buffer (`pageFillList`, binding 19).
4. **`HashWorldNow` left `origin` at `{0,0,0}`.** The standalone `PT_HASHONLY`
   rehash builds a fresh `TickParams` and never set `origin` — harmless while
   nothing in the hash path used it. The analytic sentinel branch now resolves a
   JITTER chunk's world position from (slot, origin), so after a window shift it
   hashed every jittered chunk at the wrong coordinates. Symptom:
   `--vk-smoke-loud` diverged at ticks 86/88 (the first two shifts) with chunk
   CONTENTS provably identical — a per-chunk digest diff showed **zero**
   differing slots, which is what localised it to the hash rather than the world.

The generalisable lesson, and it is the same one §3.2's caveat states: **every
one of these was a contributor nobody had written into a list** — a uniform
field, an ordering, a buffer, an origin. None was a wrong mechanism.

## 9.5 The promotion test is EXACT, including the stamp

`Classify` compares each word against `SynthWordAt` bit-for-bit. The tick stamp
is deliberately **not** masked, which is the opposite of the EMPTY rule's
`kAirDemoteMask`, and the asymmetry is load-bearing: that mask ignores the stamp
because it only applies to AIR cells, whose act is a no-op. JITTER cells are
SOLID and act, and `sim_step`'s "already acted this substep" gate reads the
stamp — so a chunk promoted while any cell carried a live stamp would come back
with it erased and act twice. In practice this costs nothing: settled buried
terrain carries `kStampNever` everywhere, and an actively-simulating chunk is not
one worth compressing.

`SANDVOX_NO_JITTER=1` disables promotion, which is the differential oracle:
paged-with must hash identically to paged-without.

## 9.6 Not done

- **Coverage is single-material only.** Cave walls, ore seams and biome
  boundaries still cost a full page. A "pristine" sentinel (§9's flavour 2 —
  "this chunk is exactly what worldgen produces here") would cover those, but it
  must run the whole terrain function — trees, ivy, grass — on every read, and is
  a much deeper change than this one.
- **No eviction to the chunk store** for disturbed-then-abandoned buried chunks.

## 9.7 The adversarial-descent number [MEASURED post-merge, 2026-08-23]

Merging onto `2debd8a` brought `--autofly-hard`, the adversarial traversal that
CLAUDE.md's page-pool invariant was written against: a diagonal flight plus a
descent into solid rock, which is the residency worst case (the harness window
and a standing player both under-report by ~2x).

**That scenario is where this change actually pays.**

| scenario | JITTER off | JITTER on | |
|---|---|---|---|
| selftest window | 4,975 | 2,861 | −42% |
| real game, standing | 16,420 | 15,545 | −5% |
| **`--autofly-hard`** | **32,395** | **14,697** | **−55%** |

The invariant previously read: *"UNSOLVED: sustained fast flight still exhausts
the pool and aborts, even at dense size — it is a MEMORY-CONSUMPTION problem,
not a sizing or speed one, and the fix is reducing the resident working set
(sparse/buried-chunk compression, a widened sentinel that can represent
single-material-with-state chunks), not a bigger pool."*

That is precisely this sentinel, and the descent now settles at 14,697 of 32,768
with no exhaustion, no page faults and a clean exit. The invariant is updated
rather than deleted, because the reasoning behind it is still correct: `kPoolPages`
stays dense-sized, since **a chunk the player has DUG is not representable by any
sentinel** and a sufficiently destructive session still trends toward dense.

Note the ordering lesson for whoever measures next: the 5% standing-player figure
and the 55% descent figure are the SAME build. Residency wins are scenario-shaped,
so quote the scenario with the number or the number means nothing.
