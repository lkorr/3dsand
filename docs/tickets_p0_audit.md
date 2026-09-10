# Chunk tickets P0 — the addressing audit

Companion to `docs/PLAN_chunk_tickets.md` §2.2, which says "the call-site audit
IS phase 0". This is that audit: every site that turns a world coordinate into a
storage slot, or asks whether a coordinate may be touched, classified and then
either rewritten or deliberately left alone.

**Acceptance for the commit this file lands in is BIT-IDENTITY**, because
`kTicketMax = 0` makes `kNumSlots == kNumChunks` and every ticket branch a dead
const-expression. That is also the audit's one blind spot and it is worth saying
plainly up front: *bit-identity cannot check the ticket path, because at P0
there is no ticket path.* What bit-identity proves is that the rewrite did not
change the WINDOW's behaviour — which is the failure mode §5 of the plan is
actually afraid of ("misclassifying one (b) site as (c) reads another chunk's
memory"). The ticket half is proved by P1's `ticket-settle` gate.

## The four classes

| | class | question the site is asking | action |
|---|---|---|---|
| (a) | RESIDENCY TEST | "may I read/write here?" | → `cellResident` / `chunkResident` |
| (b) | STORAGE ADDRESSING | "which buffer index holds this?" | → `chunkSlotOf` / `slotWorldChunk` / `NUM_SLOTS` |
| (c) | WINDOW-BOX GEOMETRY | "where is the window box?" | UNCHANGED |
| (d) | IDENTITY | RNG / hash / claim / memo key | UNCHANGED |

The split that made the audit tractable is that **(c) and (d) both keep saying
`NUM_CHUNKS`, and they say it for opposite reasons**: (c) because the box really
is the window, (d) because the key really is position-mod-the-window. Only (a)
and (b) moved.

## The new vocabulary (`common.wgsl`, one batched edit)

```
const SLOT_NONE : u32 = 0xFFFFFFFFu;
fn ticketSlotOf(wc) -> u32                 // P0 stub: SLOT_NONE
fn ticketSlotWorldChunk(slot) -> vec3<i32> // P0 stub: 0
fn chunkSlotOf(wc, o) -> u32               // window mask, else ticket, else SLOT_NONE
fn chunkResident(wc, o) -> bool
fn cellResident(c, o) -> bool
fn slotWorldChunk(slot, o) -> vec3<i32>    // the inverse, ticket-aware
fn voxSlotOfCell(c) -> u32                 // the cell-level resolver the 3 accessors share
```

plus two prelude constants (`ShaderConstantPrelude()` **and**
`scripts/check_shaders.sh`, per CLAUDE.md's two-places rule):

```
const TICKET_SLOTS : u32 = 0u;      // kTicketSlots
const NUM_SLOTS    : u32 = 32768u;  // kNumSlots = kNumChunks + kTicketSlots
```

`TICKET_SLOTS != 0u` is a const-expression, so every ticket arm is removed
before SPIR-V. That is what keeps the raymarch DDA off the register cliff
(`gotcha-raymarch-register-cliff`): the window mask stays the first, branch-free
term of `voxSlotOfCell`, and the probe is a cold tail that does not exist yet.

## Per-shader classification

Counts are call sites, not token occurrences (the plan's §2.2 numbers were token
counts, which is why they are larger).

### `common.wgsl` — the definitions themselves

| site | class | action |
|---|---|---|
| `cellIndexW` / `chunkIndexW` (defs) | (b)+(d) | UNCHANGED — see "the two-hat problem" below |
| `inWindow` / `chunkInWindow` (defs) | (a) or (c) by caller | UNCHANGED; now also the fast arm of the new fns |
| `chunkSlotIndex` (def) | (b) | UNCHANGED — it is the WINDOW arm of `chunkSlotOf` |
| `slotToWorldChunk` (def) | (c) | UNCHANGED — the toroidal wrap is window geometry |
| `voxWordAt` | (b) | → `voxSlotOfCell` |
| `voxWordIndex` | (b) | → `voxSlotOfCell` |
| `voxStore` fault decode | (b)→(c) | → `slotWorldChunk(gPtSlot, ...)` |
| `worldCellOfSlotLocal` | (b)→(c) | → `slotWorldChunk(chunkSlot, ...)` |
| `voxWordAtEntry`, `voxWordInChunk`, `voxWordInChunkAt`, `pageEntryOf` | (b) | UNCHANGED — they take a SLOT, already resolved |
| `fbmYMaskIndex` = `NUM_CHUNKS + slot` | (b) plane stride | → `NUM_SLOTS` |
| `GI_CACHE_BASE`, `OPEN_WALKED_BASE`, `OPEN_TOUCH_BASE`, `GLOW_FIELD_BASE` | (b) plane strides | → `NUM_SLOTS` |
| `farOccIndex`, `farSlotToChunk`, all `FAR_*` | — | UNCHANGED, different grid |
| `shadowPackCell` / `shadowUnwrapCell` | (d)/(c) | UNCHANGED — and see "what tickets do NOT get" |
| `flagSupportLoss`'s `inWindow` | (a) | → via `inBounds` rewrite in the consumer |

### `sim_step.wgsl`

| site | class | action |
|---|---|---|
| `inBounds` wrapper | (a) | → `cellResident` (covers every `inBounds` caller in the file) |
| `markDirtyR`: `chunkInWindow(n)` + `dirtyOut[chunkSlotIndex(n)]` | (a)+(b) | → one `chunkSlotOf` + `SLOT_NONE` test |
| `fluidOccMat`, `flagFluidConsume`: same shape on `fluidBlockMapS` | (a)+(b) | → same |
| `main`: `ci = dirtyList[wg.x]`, `dirtyIn[ci]` | (b) | UNCHANGED — already a slot |
| `main`: the `sc = (ci % NCHUNK, ...)` + `slotToWorldChunk` decode | (b)→(c) | → `slotWorldChunk(ci, T.origin)` |
| `main`: `slotIdx = cellIndexW(c)`, `hash3(..., slotIdx)` | (d) | UNCHANGED |
| `doReactions` `hash3(rnd, ri, slotIdx)`, `doStaining` `niSlot`, `windRndS` | (d) | UNCHANGED |
| colour lattice from `base` (world coords) | (c) | UNCHANGED |

### `sim_fluid_seam.wgsl` (the file the plan flagged as the largest)

| site | class | action |
|---|---|---|
| `EX_BASES`, `EX_LIST`, `SP_MARK`, `SP_COUNT`, `SP_LIST`, `SP_BINS` | (b) plane strides | → `NUM_SLOTS` |
| `exciteScan` / `settleScan` `span = NUM_CHUNKS / 256u` | (b) slot-space walk | → `NUM_SLOTS` |
| `settleJudge` `slot >= NUM_CHUNKS` | (b) slot bound | → `NUM_SLOTS` |
| 4 × `sc = (ci % NCHUNK, ...)` + `slotToWorldChunk` decodes | (b)→(c) | → `slotWorldChunk` |
| `markDirtyNext`, `seamExcitedEighths`, `exciteDetect` neighbour, `mirrorFold` | (a)+(b) | → `chunkSlotOf` + `SLOT_NONE` |
| `consumeApply`, `particleTick`, `settleBin`, `settleKill` (`inWindow` then `chunkSlotIndex(worldChunkOf(cell))`) | (a)+(b) | → one `chunkSlotOf`, guard replaces the `inWindow` |
| `seamNeighbourState`, `seamSupport`, `exciteDetect`'s above/below/`bw`/`a` probes | (a) | → `cellResident` |
| ~60 × `settleScratch[SP_* + slot]`, `fluidCalm[slot]`, `exciteScratch[EX_* + s]` | (b) | UNCHANGED — the index is already a slot |
| `exciteEmit` `hash3(T.seed, T.tick, cellIndexW(c))`; `stainApply` likewise | (d) | UNCHANGED |
| `settleScan`'s `dw = min(d, NCHUNK - d)` wrapped slot distance | (c) | UNCHANGED |

### `sim_fluid.wgsl`

| site | class | action |
|---|---|---|
| `alloc` `span = NUM_CHUNKS / 256u` | (b) | → `NUM_SLOTS` |
| `nodeBlock`, `mark` corner | (a)+(b) | → `chunkSlotOf` + `SLOT_NONE` |
| `fluidSolid`, `mark`, `clearGrid`, `p2g1`, `p2g2`, `g2p` `inWindow` | (a) | → `cellResident` |
| `clearGrid` / `gridUpdate` slot decodes | (b)→(c) | → `slotWorldChunk` |
| `fluidBlockMap[s]`, `fluidBlockList[idx] = s` | (b) | UNCHANGED |
| `g2p` splash/foam `hash3(..., gid.x)` | (d) | UNCHANGED — particle-pool identity, not a chunk slot |

### `raymarch.wgsl`

| site | class | action |
|---|---|---|
| `inBounds` wrapper (≈20 callers: `traceMicro`, `tracePlant`, `aoSolidAt`, `waterAbove`, `godRays`, `mistLiquidAt`, `bloodPooling`, `moltenPooling`, …) | (a) | → `cellResident` |
| **`trace`'s `wloI` / `wloHi` hoist and the per-step `any(cell < wloI)` break** | **(c)** | **UNCHANGED — this is the hot DDA and it is a BOX, not a residency question** |
| `trace`'s per-ray float slab clip (`tEnter`/`tExit`) | (c) | UNCHANGED |
| `chunkOcc` `occupancy[chunkIndexW(cell)]`, `cchPt = pageEntryOf(chIdx)` | (b) | UNCHANGED — inside the window box the clip already proved |
| `fluidNodeBase`, `fluidCellAt`, `fluidChunkWater`, `fluidYMaskOf`, `fluidChunkClass` | (a)+(b) | → `chunkSlotOf` / `chunkResident` + `SLOT_NONE` |
| `fluidMarch` / `fluidMarchBlocky` `slot != heldSlot` | **(d)** | UNCHANGED — a change-detection memo key, never a buffer index |
| `giGatherRays`, `giCacheWordAt`, `giBounceAt` `chunkIndexW` + `opennessStamp` | (b)+(d) | UNCHANGED — the stamp is the residency test and it keys on the WORLD chunk |
| `microCellHash`, `fs`'s `paletteJitter(cellIndexW)` | (d) | UNCHANGED |
| `traceFar`, `farShadowDist` | — | UNCHANGED, far grid |

### `worldgen.wgsl`

| site | class | action |
|---|---|---|
| `genChunk` slot decode; `pagefill`; `fardown` | (b)→(c) | → `slotWorldChunk` |
| `main` dispatched over `NUM_CHUNKS` workgroups | (b) | comment → `NUM_SLOTS` (the C++ `D_CHUNKS` extent moved) |
| `genChunk`'s `occupancy[slot]`, `dirtyIn/Out[slot]`, `voxWordInChunk(slot, …)` | (b) | UNCHANGED — already a slot |
| `synthWordAt(entry, base + l, seed)` positional palette | (d) | UNCHANGED |
| `far` / `farpatch` (`FAR_NCHUNK`, `farSlotToChunk`) | — | UNCHANGED, far grid |

### The rest

| shader | (a) → | (b) → | unchanged |
|---|---|---|---|
| `sim_particle.wgsl` | `inBounds` wrapper (both window-exit kills) | `markDirtyNext` pair | `claimSlot(cellIndexW(...))` ×5 (d) |
| `sim_mutate.wgsl` | `inBounds` wrapper | `markBoth` pair; `windWake` bound → `NUM_SLOTS` | `slotIdx` RNG (d); `op.cellIdx` decode → `slotWorldChunk` |
| `sim_explode.wgsl` | `inBounds` wrapper | `chunkIndexW(c)` (already post-guard) | `slotIdx` RNG (d) |
| `sim_compact.wgsl` | — | **both `i >= NUM_CHUNKS` bounds → `NUM_SLOTS`** | list position `slot` (b, unrelated) |
| `sim_occupancy.wgsl` | — | dispatch comment → `NUM_SLOTS` | `hashBase = wg.x * CHUNK_VOL` (d) — the single most bit-identity-fragile line in the set, see below |
| `sim_openness.wgsl` | — | `openWorldChunk` → `slotWorldChunk` | stamp-as-residency (a-via-d); `% NUM_CHUNKS` refresh cursor (c) |
| `sim_glow.wgsl` | — | `glowWorldChunk` → `slotWorldChunk`; `glowSrcOf` / `glowWriteField` → `chunkSlotOf` **+ an explicit `SLOT_NONE` arm** | stamp-as-residency; `% NUM_CHUNKS` refresh cursor (c) |
| `sim_waterbody.wgsl` | — | `wbMarkDirty` pair; `wbSlotWorldChunk` → `slotWorldChunk` | `wbShave` `cellIndexW` roll (d); `wbDrain` `gid.x` roll (d) |
| `sim_pick.wgsl` | `inBounds` wrapper | — | the directional `lo`/`hi` exit test (c) |

## The hazard grep

The plan asks for one specific thing: *any place a STORAGE slot (`ci`, `slot`, a
`dirtyList[]` entry) is used as a hash / RNG / claim key.*

**Result: none, in any of the 22 shaders.** Every RNG key traces to
`cellIndexW(<world cell>)` or to a dispatch/particle-pool index, never to a
chunk storage slot and never to a page index. The five `TWO BASES` comments
(`sim_step:main`, `sim_step:doReactions`, `sim_step:doStaining`,
`sim_particle:resolve`, `sim_mutate`, `sim_explode`) are the record of the one
time this was violated — a page index fed to `hash3` made every reaction roll a
function of allocation history — and the split they describe is intact.

Two sites are worth naming even though they are correct:

- `sim_occupancy.wgsl:210,244` — `hashBase = wg.x * CHUNK_VOL` (SLOT) beside
  `loadBase = e * CHUNK_VOL` (PAGE). Correct, and the most fragile line here:
  merging them, or re-deriving `hashBase` through `pageTable`, moves the world
  hash for every non-identity page assignment.
- `cellIndexW` is `c & WORLD_MASK`, so **a ticket cell will share its RNG stream
  with the window cell 512 voxels away on each axis.** Deterministic and
  position-derived, so rule 1 holds; statistically correlated, so it is a
  quality question, not a correctness one. The plan accepts this for v1 (§2.2d).
  Keying every roll on the full world coordinate is a later, separate commit
  because it moves the hash.

## The two-hat problem (`cellIndexW`), and what it costs

`cellIndexW(c)` is simultaneously **(b)** — the linear cell index into a
window-slot's page — and **(d)** — the counter-RNG key. Under P0 both hats fit
because the only cells that reach it are window cells. Under P1 they come apart:
a ticket cell's storage index is not `cellIndexW`, but its RNG key still should
be something position-derived.

That is why `voxWordIndex` now goes through `voxSlotOfCell` (the (b) hat, made
ticket-aware) while every `hash3(..., cellIndexW(c))` was left exactly alone
(the (d) hat, which the plan says stays). **Anyone extending this must not
"tidy" the two into one function.** The `TWO BASES` comments exist for the
symmetric mistake one level down.

## Sites the plan did not anticipate

Three classes came out of the audit that §2.2's list does not mention, and all
three are places where a P1 author would have introduced silent corruption:

1. **Plane strides inside multi-plane per-slot buffers.** `SP_MARK = NUM_CHUNKS`,
   `EX_BASES = 16u + NUM_CHUNKS`, `OPEN_WALKED_BASE`, `OPEN_TOUCH_BASE`,
   `GLOW_FIELD_BASE`, `GI_CACHE_BASE`, `fbmYMaskIndex`, and the C++ mirrors
   `SUBOCC_BASE` (prelude), `stream.cpp`'s sub-occupancy write offset and
   `measure.cpp`'s read offset. These are neither "a residency test" nor "a slot
   for a buffer index" — they are *where plane k begins*, and they are the one
   thing bit-identity provably cannot catch, since at `kTicketSlots = 0` a
   forgotten one is still numerically right. They are (b) and they all moved to
   `NUM_SLOTS`.

2. **`.size() == kNumChunks` guards.** A dozen CPU sites gate a feature on a
   vector being exactly window-sized (`snap.supportFlags`, `snap.occupancy`,
   `snap.occStain`, `zeroStreak_`, `chunkBody_`, the waterbody label vector, the
   debug-overlay `dirtyFlags`). Growing the vector without moving the guard does
   not crash — it silently turns the feature **off**. Every one moved.

3. **Slot decomposition into WINDOW coordinates on the CPU.** `DilateN26` and
   the page census's column binning turn a linear slot into `(s % kNChunk, …)`.
   That is meaningless for a ticket slot. Both now skip non-window slots via a
   new `World::IsWindowSlot`, and `World::SlotToWorldChunk` gained the same
   ticket branch its WGSL twin has.

Two smaller ones: `windprim.cpp`'s wake-dedup bitset is `kNumChunks / 64` with
integer division (hence `static_assert(kNumSlots % 64 == 0)` and the rounding in
`kTicketSlots`), and `vk_record.cpp`'s `D_CHUNKS64` has the same divisor.

## What tickets deliberately do NOT get (all class (c), all UNCHANGED)

- **The raymarch's window clip.** `wloI` / `wloHi` and the per-step break. v1
  renders tickets through the far cascade only (plan §2.7); P4 adds the AABB
  path. Touching the DDA here buys nothing and risks the register cliff.
- **The shadow cache.** Its request record packs a WINDOW-RELATIVE cell in 30
  bits (`world.h` `kWorldShift` `static_assert`); a ticket cell does not fit.
- **The openness / glow rolling refresh cursors** (`% NUM_CHUNKS`) and their
  `render.opennessChunksPerFrame` / `glowChunksPerFrame` clamps: window
  maintenance for render-only data.
- **`opennessGen`'s third plane** (`kNChunk²` per-column touch ticks): a ticket
  has no place in the window's (x, z) column grid.
- **`Stream::FlushResident` / `ReloadWindow`** enumerate the window's slots by
  construction.
- **The whole far cascade** (`FAR_*`, `farSlotToChunk`, `farOcc`): its own
  address space, decoupled from `kWorldN` on purpose.

## Deviations from the plan's P0

1. **No `ticketMap` GPU binding.** The plan's §2.2 has the map as a bound,
   GPU-read-only storage buffer. At `kTicketMax = 0` it is provably empty and
   every read of it is dead code, so binding it would mean adding a binding to
   ~13 shaders and ~40 `pass_table.def` rows, plus new bind-group-layout risk,
   to make a table nothing can read. That trade is backwards for a commit whose
   entire acceptance is bit-identity. The map's *seam* ships instead:
   `ticketSlotOf` / `ticketSlotWorldChunk` in WGSL and
   `World::TicketSlotWorldChunk` in C++ are one-line stubs that name exactly
   what P1 replaces, and every caller is already routed through them. P1 adds
   the binding, the rows, and the CPU builder together, where they can be tested.
2. **No `let isTicket = ci >= NUM_CHUNKS;` in `sim_step`.** The plan allows it
   "only if it is free". A `let` with no consumer is dead code the compiler
   drops, so it is free and worthless in equal measure; P3 adds it with the
   `doReactions` gate that reads it.
3. **`slotWorldChunk` replaced ten copy-pasted decodes**, which the plan lists
   as the `slotToWorldChunk` inverse but describes as an addition. Collapsing
   them was necessary rather than tidy: each copy is arithmetic that *cannot*
   produce a ticket slot's world chunk, so leaving even one is a P1 bug.
4. **`raymarch`'s two `heldSlot` memo keys stay `chunkSlotIndex`.** The plan's
   class list would push them to (b); they are (d).
