# PLAN: chunk tickets — the CA outside the residency window

Status: **P0 LANDED 2026-09-08 on branch `tickets-p0`** (slot space + the
addressing audit, `kTicketMax = 0`, bit-identical: the per-site classification
and the three site classes this plan did not anticipate are in
`docs/tickets_p0_audit.md`, and its "Deviations" section is the short list of
what P1 inherits). **P1 IMPLEMENTED 2026-10-02** (`src/sim/tickets.{h,cpp}`,
`kTicketMax = 16`, the ticket table in `pageTable`'s tail, ops in the op
record, gate `ticket-settle`; deviations in §6). **P2 IMPLEMENTED 2026-10-02**
(particles park outside residency on the far cascade and request tickets;
refused ones deposit as far landings; gate `ticket-land`). **P3 IMPLEMENTED
2026-10-02** (only DECAY runs in a ticket; gate `ticket-decay`). **P4
IMPLEMENTED 2026-10-02** (the raymarch draws tickets at full voxel resolution;
boxes re-centre toward their shell; gate `ticket-render`). P5 is not
scheduled. Named "tickets" rather
than "islands" because `docs/PLAN_rigidbody_islands.md` already owns that word
for disconnected solid components. Companion: `docs/PLAN_gas_particles.md`
(independent; phase 2 here can consume its `farVox` blocking).

## 0. Why (read from the code)

- The residency window is a box because a chunk's SLOT is its world coordinate
  modulo the window: `chunkSlotIndex(wc) = wc & NCHUNK_MASK`
  (`common.wgsl:3103`). There are exactly `kNumChunks` slots and arithmetic
  assigns them. A chunk 32 chunks away wants the slot a near chunk holds.
  "Activate a distant chunk" cannot be done by allocating a page: page space
  is not the constraint, slot identity is.
- Out-of-window space is "solid and inert" (`inWindow`, `sim_step.wgsl:66`);
  a particle leaving the window is deleted (`sim_particle.wgsl:135`, `:235`).
  Walk away and everything freezes.
- `Stream::FillSlots(slots, deferWake)` and `EvictSlots(slots, filter)` already
  take ARBITRARY slot lists, not planes (`stream.h:231,257`). `ChunkStore` is
  keyed by world chunk coordinate (`Put(IVec3 wc, rle)` / `Get(IVec3 wc)`,
  `chunkstore.h:31,34`). `FarEdits` keeps cascade cells from healing over an
  edit. The persistence half exists.
- The per-cell RNG keys on `cellIndexW(c)` = world position mod 512
  (`sim_step.wgsl:1931`: `rnd = hash3(T.seed, T.tick*2+substep, slotIdx)`),
  NOT on the storage slot. So a chunk stored in an extra slot rolls the same
  dice it would in the window. (An earlier draft of this idea claimed the
  opposite; it was wrong.)
- Minecraft precedent, for the policy shape: tickets run at FULL fidelity
  (there is no cheap distant tier), are explicit / fixed / time-limited
  (`/forceload`, spawn chunks, portal tickets), and **entities never create
  tickets** — an arrow does not load the chunks it flies into, because that
  would let any player load the world by shooting at it.

## 1. Goals and non-goals

Goals
1. A bounded number of resident chunk GROUPS outside the window ("tickets"),
   simulated by the SAME CA at the SAME tick rate.
2. Movement (powder, CA liquid, gas voxels, particle landing) and DECAY
   reactions run there. A pile that lands out of range settles; an ember that
   drifts out of range burns out; smoke fades. Things finish instead of
   freezing.
3. Rules 1–3 hold. Ticket activation is a MutationQueue op; the twice-run
   gate is the test.
4. Phase 0 moves NO hash: a build with `kTicketMax = 0` is bit-identical.

Non-goals (v1)
- No reduced tick rate. Reaction chances are per-tick probabilities and
  `1-(1-p)^n` is not `n*p`; a slower ticket would burn, spread and settle
  differently from the window. Full rate, fewer chunks.
- No EMIT / PAIR reactions in tickets. Movement and decay TERMINATE — bounded
  by the matter present. Emit/pair PROPAGATE — bounded only by fuel, which
  runs past the ticket's edge, so either the ticket grows without bound (rule
  2) or the fire stops at a straight chunk-aligned edge. Phase 5 makes that a
  design decision with a cap; it is not v1.
- No MPM / fluid seam in tickets. CA liquid only.
- Tickets are not a bigger window. Distant reactions between voxels that both
  live inside one ticket work; anything that needs the window's full
  machinery (mobs, physics bodies, the CPU mirror) does not.

## 2. Design

### 2.1 Slot space

Slots `[0, kNumChunks)` are the window, unchanged. Slots
`[kNumChunks, kNumChunks + kTicketSlots)` are ticket slots.

A ticket is a **5×5×5 chunk box** (125 slots, 2 MiB of pages) of which the
**inner 3×3×3 is ACTIVE** (dispatched) and the shell is RESIDENT ONLY (read
by neighbours, written to by reach-1 moves, never dispatched). 3×3×3 with only
the centre active is too small to hold a pile; 5³/3³ is the smallest box whose
active region has a full 26-neighbourhood everywhere.

`kTicketMax = 16` → 2,000 slots → `kTicketSlots = 2,048`. `kPoolPages +=
kTicketSlots` (+32 MiB, and the pool proof in `world.h:1032` still holds: one
page per slot plus the retire ceiling).

Every per-slot buffer grows by `kTicketSlots`. Known list (audit for more —
`grep -n "kNumChunks" src/sim/world.h` gives 8 sizing sites; the CPU mirrors
in `Stream`/`PageTable` have their own):
`pageTable`, `dirty[2]`, `dirtyList`, `occupancy` (+ sub-occupancy),
`hasMatter`, `openness`, `opennessGen`, `irradiance` (both planes), `glow`
src + field, `fluidCellScratch`, `supportOut`, the CPU `modified_`,
`shiftEvicted_`, `cpuDirty`, snapshot `dirtyFlags`, and `sim_compact`'s
dispatch `NUM_CHUNKS/64`.

The shadow-cache request record packs a WINDOW-RELATIVE cell in 30 bits
(`world.h:829`); ticket cells are not window-relative. **Ticket chunks do not
participate in the shadow cache** (they render via the cascade in v1, §2.7).

### 2.2 Address resolution

New in `common.wgsl` (one deliberate edit; it is the definition two dozen
shaders must agree on):

```
const SLOT_NONE : u32 = 0xFFFFFFFFu;
fn chunkSlotOf(wc) -> u32   // window: wc & MASK. else: probe ticketMap. else SLOT_NONE
fn chunkResident(wc) -> bool // chunkSlotOf(wc) != SLOT_NONE
fn cellResident(c)   -> bool // chunkResident(worldChunkOf(c))
```

`ticketMap`: open-addressed table, `2 * kTicketSlots` entries of
`(packed wc, slot)`, keyed on a hash of the world chunk coordinate. **CPU-built,
uploaded between ticks, GPU read-only.** Lookups are order-free; the only
writer is `Tickets` on the CPU, and it writes as a consequence of a logged op
(§2.4), so the table is a deterministic function of the op stream.

`slotToWorldChunk(sc, o)` gains the inverse: a slot `>= NUM_CHUNKS` reads its
world chunk from a `ticketSlotWc[]` array (CPU-written beside the map). Every
kernel that reconstructs `wc` from a dirty-list slot (`sim_step` main,
`sim_occupancy`, `sim_openness`, `worldgen` genChunk/fardown, `sim_glow`) goes
through it.

**The call-site audit IS phase 0.** Counts of `NUM_CHUNKS | chunkSlotIndex |
chunkInWindow | inWindow | cellIndexW | slotToWorldChunk | WORLD_MASK` per
shader: common 33, sim_fluid_seam 41, raymarch 21, sim_fluid 15, sim_step 10,
worldgen 7, sim_particle 7, sim_glow 6, sim_mutate 5, sim_waterbody 4,
sim_compact 3, sim_openness 3, sim_explode 2, occupancy/pick 1 each. Each
site is classified as exactly one of:
- (a) **residency test** ("may I read/write here?") → `cellResident` /
  `chunkResident`;
- (b) **storage addressing** (slot for a buffer index) → `chunkSlotOf`;
- (c) **window-box geometry** (the raymarch's `wloI/wloHi` break, streaming
  planes, `SpawnWindowOrigin`, the toroidal wrap in `slotToWorldChunk`) →
  UNCHANGED;
- (d) **identity** (`cellIndexW` as an RNG / hash / claim key) → UNCHANGED
  (position-derived already). A ticket cell shares its RNG stream with the
  window cell 512 voxels away on each axis: deterministic, position-derived,
  statistically correlated. Accepted for v1; keying every roll on the full
  world coordinate fixes it and moves the hash — a later, separate commit.

**The hazard to grep for:** any place a STORAGE slot (`ci`, `slot`, a
`dirtyList[]` entry) is used as a hash / RNG / claim key. `gFilmLicence =
dirtyIn[ci]` is storage (fine); `hash3(..., ci)` anywhere would be the
paged-vs-dense divergence class of bug already paid for once (`sim_step.wgsl`
"TWO BASES" comment).

`voxWordAt` / `voxWordIndex` / `voxStore` are the ONLY voxel accessors
(CLAUDE.md); they take a cell, resolve its slot, then its page. They change
in exactly one place each. `World::PageOffsetOfSlot` on the CPU likewise.

### 2.3 Active vs resident

`ticketActive[slot]` bit (one u32 per ticket slot, CPU-written). `sim_compact`
skips a dirty ticket slot whose active bit is clear. Shell chunks therefore:
- are readable (neighbourhood is complete for the active interior);
- receive reach-1 writes from the interior (a grain rolling one cell into the
  shell lands there and STAYS — frozen until the ticket is released or
  re-centred, §3 P4);
- are never dispatched, never react, never mark anything.

That gives every active chunk a full 26-neighbourhood, which is the whole
reason for the shell. Matter that reaches the shell is the ticket's
boundary condition, exactly as the window edge is today — but one that is
resident, so nothing is LOST, it is merely paused.

### 2.4 Lifecycle — a MutationQueue op (rule 3)

New `src/sim/tickets.{h,cpp}`, owned beside `Stream`, driven from
`Stream::Update` (between ticks, like a shift).

**Ops.** `TicketActivate{centre wc, reason, tick}` and `TicketRelease{id}`
are MutationQueue ops. That is what makes them deterministic: the sim is
reproducible with respect to the logged op stream, exactly as brush edits
are, and the replay log / save carries them. A request that arrives from the
GPU by async readback (a particle landing, §3 P2) has frame-pacing-dependent
LATENCY in the game — but it becomes an op at a definite tick and the op is
what the sim sees. The selftest drives requests synchronously, so its ops
land at fixed ticks and twice-run compares equal.

**Policy** (all deterministic given the op stream):
- cap `kTicketMax`; a request past the cap is REFUSED (budget charged before
  emission — the existing convention), never queued unboundedly;
- dedupe: a request whose centre falls inside an existing ticket's 5³ box is
  absorbed (no-op);
- a request inside the WINDOW is a no-op (the window already simulates it);
- priority when two requests compete for the last ticket: lower tick, then
  lexicographic `wc`;
- **timeout**: released when its active chunks have all been asleep for
  `kTicketIdleTicks` (from the snapshot `dirtyFlags` the CPU already reads
  back) OR after `kTicketMaxTicks` regardless — the Minecraft portal-ticket
  shape, so a waterfall spraying debris cannot pin tickets forever;
- release: `EvictSlots` → `ChunkStore.Put(wc, rle)` for all 125 chunks →
  `FarEdits` records the sample voxels → map entries removed, slots returned.

**Activation.** `FillSlots(slots)` with an explicit `slot → wc` list (today it
derives `wc` from `origin`; it needs the ticket override). Per chunk:
`ChunkStore.Get(wc)` hit → decode and upload; miss → worldgen `genChunk` for
that slot (the worldgen dispatch takes a slot list and reconstructs `wc`
through §2.2's inverse). Deferred wake as the plane fill does.

**Window interaction.** Before a `ShiftAxis` whose incoming plane overlaps a
ticket's box, RELEASE that ticket first (evict to store), so the plane fill
decodes the ticket's chunks from the store and nothing is simulated twice or
lost. Ordering rule in `Stream::Update`, before `CompleteDueShifts`.

### 2.5 Reactions in tickets

`sim_step.wgsl` main: `let isTicket = ci >= NUM_CHUNKS;` (workgroup-uniform,
one compare). In `doReactions`: `if (isTicket && kind != RK_DECAY) { continue; }`.
Decay is strictly consumptive, so rule 2 needs no budget for it — the same
argument `reactions.json` makes for evaporation. `keepAwake` for skipped
rules is NOT set, so a ticket holding only pair/emit-reactive matter sleeps
and times out.

Movement is untouched. `flagSupportLoss` / island detection: the CPU
rigidbody-island scan reads the mirror, which does not cover tickets; a
ticket's floating voxels stay floating in v1 (they do today outside the
window too). Stated limitation.

### 2.6 Rule 2 accounting

`kTicketMax * 27` active chunks is the ceiling (432 at 16), above the
`sleep` gate's 32-at-rest bound. The bound becomes: **at rest there are zero
tickets** — the timeout guarantees it — and `sleep` asserts both (`awake ≤
32` and `tickets == 0`). `SANDVOX_PT_AUDIT=1` accounting learns the extra slot
range.

### 2.7 Rendering

v1: ticket chunks are visible ONLY through the far-field cascade. `fardown`
runs over the dirty list, so ticket slots downsample into the cascade like
window chunks do (they need §2.2's inverse to find their `wc`). Coarse, and
enough to see "the pile is there".

P4: the raymarch, after `tExit`, tests at most `kTicketMax` AABBs (a
uniform array; 16 slab tests) and runs the ordinary DDA inside a hit box,
with `cellResident` in place of the `wloI/wloHi` break. Ticket chunks then
render at full voxel resolution from any distance.

### 2.8 Determinism summary

Op-stream-defined activation; CPU-built map uploaded between ticks; GPU
reads only; position-keyed RNG unchanged; `dirtyList` order still irrelevant
(colour lattice is world-coordinate). `--gate determinism` twice-run, and
`--residency dense` must produce the same hash as paged with tickets active
(dense has the same extra slots; the map is the same).

## 3. Phases

**P0 — slot space + `chunkSlotOf` + the audit.** Grow every per-slot buffer,
add the map (empty), route every (a)/(b) site through the new functions,
`kTicketMax = 0`. **Acceptance is bit-identity**: `determinismHash`, both
`--vk-smoke` probe tables and `--shader-stats` register counts within noise
(the accessors gained a compare + a rarely-taken probe; the raymarch DDA must
not regress — `gotcha-raymarch-register-cliff`). This is the phase with the
risk in it, and it ships alone.

**P1 — lifecycle + a manual op + gate.** `Tickets` class, the two ops, cap /
dedupe / timeout / release, `FillSlots` override, window-overlap rule,
`--ticket x,y,z` debug flag (or a brush op) to activate one. Gate
`ticket-settle` (§4).

**P2 — particle landing requests.** Relax the two kills in `sim_particle.wgsl`
for particles that are outside the window: block against `farVox` (from
PLAN_gas_particles §2.2, or standalone), and on landing append to a
`ticketReq` buffer (cell, material, tick) read back async; `Tickets` turns
requests into ops, subject to §2.4's policy. The particle itself is HELD
(not resolved) until its cell is resident — it parks, exactly as a claim
loser does today, and lands the tick after activation. If refused (cap), it
lands as a `FarEdit` only: visible in the cascade, materialised when the
window arrives.

**P3 — decay in tickets** (§2.5) + gate: an ember placed by op inside a
ticket becomes ash within its authored lifetime; a `wood` voxel beside it
does NOT ignite.

**P4 — direct render + re-centring.** §2.7's AABB path. A ticket whose
activity has moved into its shell re-centres (release + activate one chunk
over, through the op stream) at most once per `kTicketRecentreTicks`.

**P5 — propagating reactions (design decision, not scheduled).** Allow
emit/pair in tickets; a burning active chunk requests neighbouring tickets;
growth stops at the cap, and the fire stops at a straight edge. Or: coarse
fire spread on the cascade feeding the gas field, converted to voxel fire
when the window arrives. Decide when P3 has been lived with.

## 4. Gates

- `ticket-settle` (P1): activate a ticket 40 chunks outside the window over
  real terrain; pour 2,048 sand by op at its centre; 200 ticks. Assert: the
  sand's column count is conserved; the active chunks are asleep by tick
  150; the ticket releases by `kTicketIdleTicks` later; `ChunkStore.Get`
  returns the pile; shifting the window there and reading the mirror finds
  the pile; twice-run hash equal; paged == dense.
- `ticket-land` (P2): drop a stone particle off the window edge at 20 m/s;
  assert a ticket activates within `L` ticks of the landing (L fixed in the
  harness), the stone is in the store afterwards, and a second stone landing
  inside the same box does NOT open a second ticket.
- `ticket-decay` (P3): as §3.
- `sleep`: `awake ≤ 32 AND tickets == 0` at rest.
- `determinism`, `page-roundtrip`, `SANDVOX_PT_AUDIT` clean with tickets
  active.

## 5. Risks and open decisions

- **Audit breadth** (~160 sites, 15 shaders; `sim_fluid_seam.wgsl` alone is
  41). Misclassifying one (b) site as (c) reads another chunk's memory —
  which is exactly the failure the page table's own plan spent two review
  rounds on. P0 is one commit, reviewed against the classification list, and
  its acceptance is bit-identity.
- **`FillSlots` / `genChunk` assume slot→wc is `origin`-derived.** Both need
  the override; `genChunk` must not touch the window's `origin`-dependent
  state (stamps, openness gen words) for a ticket slot.
- **Shadow cache** excludes tickets (packing). Fine in v1 because they
  render via cascade; P4 must either widen the record or keep tickets
  unshadowed.
- **Memory**: +32 MiB pool, +~4% on every per-slot table.
- **Frozen shell.** Matter that rolls into the shell pauses. Re-centring (P4)
  is the fix; v1 states it.
- **Selftest fixture.** The gate needs terrain 40 chunks out; `--voxserve` /
  `TerrainHeight` give the column height without a readback.
- **Not a bigger window.** A user who wants a distant forest to burn needs
  P5 or a bigger window (see the `kWorldN` discussion: 1024×512×1024 is 4x
  memory, fits both `static_assert`s). Tickets are for things that LEAVE the
  window, not for simulating a region continuously.

## 6. Deviations from this plan (P1–P4, as implemented)

Each is a choice the plan left open or made differently, taken the way that
keeps rules 1–3 provable. The code comments beside each say the same thing.

### P1

1. **The ticket map rides `pageTable`'s tail, not a new binding** (§2.2 asked
   for a bound `ticketMap`). Every kernel that resolves a cell already binds
   `pageTable`, which is GPU-read-only everywhere; appending
   `kTicketTableWords` (132) after the `kNumSlots` entries costs no binding, no
   layout and no pass-table row beyond the ten rows that now genuinely read it
   through `ticketSlotOf` (`check_pass_table.py` found them).
2. **Box tests, not an open-addressed hash.** Tickets are 16 boxes of 5³, not
   2,000 arbitrary chunks: `ticketSlotOf` scans the COMPACT live list (n box
   tests; a world with no ticket pays one load) and the slot inside a box is
   arithmetic. Ticket `i` owns slots `[kNumChunks + 125 i, +125)`, and a box
   chunk sits at the slot its coordinate takes **mod 5** — the window's own
   toroidal rule at the box's size — so P4's re-centre refills one plane.
3. **The active bit is derived, not a `ticketActive[]` array**: inner 3³ = box
   offset 1..3 on every axis, from the table. `sim_compact.wgsl`'s `main` skips
   a dirty shell slot; `mainNext` keeps it (occupancy and digest update).
4. **The renderer keeps the P0 stub.** The probe is compiled only into shaders
   that declare `pageTable` and are not render shaders (`uniform> R :`):
   common.wgsl's `TICKET_BOUND`/`TICKET_UNBOUND` blocks, selected by
   `BodyResolvesTickets` (resources.cpp) and mirrored in `check_shaders.sh` /
   `check_pass_table.py`. `TICKET_PROBE` is a const, so the raymarch DDA is
   unchanged by P1; P4 draws tickets through its own box march.
5. **A ticket miss falls back to the masked window slot** in `voxSlotOfCell`
   (the pre-ticket aliased read), never `SLOT_NONE` as an index.
6. **Every ticket slot holds a real page for its whole life** (paged). Nothing
   materializes a ticket chunk on demand — the mirror's N26 ring is window
   arithmetic — so a sentinel in a ticket would be a page fault on the first
   write. The free path skips live ticket slots; release frees all 125 at once.
   2 MiB per ticket, exactly the `kTicketSlots` the pool grew by.
7. **The release is two-phase.** At tick T the box leaves the table and all
   125 slots are copied out; which chunks the store KEEPS is decided when the
   snapshot of T-1 is published (a chunk is kept iff some published snapshot
   since activation showed it dirty). Deciding at T would miss a particle that
   landed in the last `kSnapshotLatency` ticks. A fill that needs one of those
   chunks first forces the batch and keeps all 125 — conservative, never lossy.
8. **Idle = the 27 active chunks clean in `kTicketIdleTicks` (30) consecutive
   published snapshots**; `kTicketMaxTicks` = 1,800 (60 s). Both read
   `World::Snap()`, the fixed-latency view, so a release is tick-deterministic.
9. **Placement slides the box** rather than refusing: up to 125 offsets that
   keep the requested chunk inside the box are tried (interior first, nearest
   the centre, then lexicographic), skipping any that touch the window or a
   live box. Refused only when all 125 collide (counted as `refusedPlacement`).
10. **The ops are recorded, not replayed from the record** — `Frame::tickets`
    (record version 7) carries every decision; a replay re-derives them (they
    are a pure function of the recorded inputs and the fixed-latency snapshot)
    and COMPARES (`ReplayTicketMismatches`, `ReplayTicketOps`). *Audit
    2026-10-02: as first landed, no replay loop ran the ticket step at all, so
    the comparison was dead code and a recording with a ticket in it could
    not reproduce.* Both replay loops (`--replay-ops`, the `ops-replay` gate)
    now call `Stream::TicketTick` before each recorded tick's submit, after
    `InjectTicketRequestsIfReplaying` re-queues the recording's EXTERNAL
    requests (manual / gate activations — no replayed input carries them) at
    the recorded box centre. `ops-replay` records a ticket, pours into it and
    fails on any differing or missing decision. Like `genList`, this is a
    replay of a FIXED window: window shifts are still not replayed.
11. **Regen and load DROP tickets** (`OnRegen` / `SubmitWorldgen`, and
    `ReloadWindow`, whose only callers are `LoadWorld` and the net late-join
    re-pull — there is no teleport path that reloads the window). Both replace
    the world, so the dropped ticket is the replaced world's, exactly as the
    window's own unsaved state is. A SAVE includes live ticket slots
    (`FlushResident`), and a releasing ticket's pending batch is drained by the
    save keep-all. Not saved: the far landings held on the CPU (below, P2 3) —
    in-flight particles are not saved either, but a landing can be held far
    longer than a flight, which is why it now asks for a ticket itself.
12. **`ticket-settle` proves "the window arriving finds the pile" through the
    store-hit door a second ticket takes** (FillSlots' store branch, the one a
    window shift uses) plus a direct `ChunkStore` decode, instead of shifting
    the window 40 chunks. paged == dense is the `determinism` gate run with
    `--residency dense` — that gate now opens a ticket and pours into it.
13. **P0 missed a literal**: `SOL_POOL_PAGES` (the solute pool, derived from
    `kNumSlots` in C++) was `4096u` in four WGSL mirror blocks;
    `check_invariants.py` caught it the moment `kNumSlots` moved.

### P2

1. **`farVox` blocking is standalone in `sim_particle.wgsl`** (`farBlocked`,
   the gas kernel's `gasFarBlocked` verbatim; `particleBGL_` gained bindings 8
   `farVox` and 9 `FarParams`). Outside the cascade reads OPEN, as for gas.
   **Stated limitation, shared with gas stage 1:** the cascade is render-derived
   data whose fill is budgeted per tick, and in the GAME its first fill waits on
   a background pipeline compile — so where a particle comes down outside the
   window before the horizon has filled is frame-timing dependent there. Every
   selftest fills the cascade synchronously (`DrainFullRefill`) or not at all,
   so the twice-run gates are unaffected; a replay of a game recording made
   during the first seconds of a cold shader cache may differ in where far
   debris parked. Making the far terrain a pure function of the tick needs a
   sim-owned far blocker, which is out of scope here.
2. **The `ticketReq` buffer is 32 atomicMax BUCKETS in the pageFaults record**,
   not an append buffer (`world.h kTicketReq*`): an append cursor that
   overflowed would refuse a scheduling-dependent subset. A parked particle
   re-requests every tick, so a chunk that shares a bucket is served a tick
   later. The record rides the fixed-latency snapshot, so a request becomes a
   TicketOp exactly `kSnapshotLatency` ticks after the landing in every run;
   the gate allows 8.
3. **The FarEdit fallback is a FAR LANDING, not a cascade patch.** A parked
   particle not resident within `PARK_DEPOSIT_TICKS` (16) — cap refused, or its
   bucket lost every tick — DEPOSITS: it bids its `particlePriority` into one of
   six deposit slots (atomicMax), the winner writes its state there in
   `resolve` and dies, and the CPU (`Tickets` far landings, fed through
   `World::TakeTicketDeposits` exactly once per published snapshot) re-throws it
   with zero velocity the tick its chunk is resident again — a ticket, or the
   window arriving. A single voxel is below every cascade level's resolution
   (`faredits.h`: an edit shows at level k only if it changes a cell CENTRE), so
   a cascade patch would draw nothing; the landing is invisible until resident.
   Bounded: 65,536 held, oldest dropped and counted (`landingsDropped`).
   *Audit 2026-10-02:* a deposited particle stopped requesting, so a landing
   refused at the cap waited for the WINDOW — possibly forever, and a save
   does not write it. `Tickets::AskForLandings` now has every waiting landing
   ask for a ticket over its own chunk whenever an index is free (oldest
   first, one request per distinct chunk, at most one per free index, at most
   once per `kTicketIdleTicks`; never into a full cap, so no refusal op every
   tick). `ticket-land` arm B asserts it is re-thrown with no request from the
   gate.
4. **Spray still ends at the window.** A micro particle leaving residency dies
   as before — it is an effect with a lifetime, not conserved matter.
5. **Rule 2 for far flight**: a particle leaving the far box (two window edges
   centred on the window, the gas outer box's extent) dies and is counted
   (`pageFaults[126]`); a parked particle lives at most 16 ticks before it
   deposits.
6. Two flag bits: `PFLAG_PARKED` (bit 17), `PFLAG_DEPOSIT` (bit 18); the park
   clock reuses the float-patience bits (5..12), which a non-micro particle
   outside residency cannot be using. `check_invariants.py` `ticket record`
   pins the bits and the record layout.

### P3

1. **`gInTicket` is a per-cell private set at the top of `main`** (`ci >=
   NUM_CHUNKS`, one compare), not a `let` threaded through: `doReactions`
   skips every non-DECAY rule before its roll and before `keepAwake`, and
   `coatReact` (a coat's rules are all PAIR rules) returns "nothing covered,
   nothing spent" at its head. The same applies to `excitedReact`'s call,
   which only exists where MPM fluid does — never in a ticket.
2. **The gate places 32 embers on a wood slab**, not one ember beside one
   wood voxel: a single ember becomes ash only 1 time in 8 (its authored decay
   splits ash / smoke / air 1 : 4 : 3), so "the ember becomes ash" is asserted
   as "every ember is gone within 9 authored mean lives and some ash is left",
   with the bound computed from the compiled table.
3. **Smoke an ember decays to rises into the shell and freezes there** — the
   frozen-shell limitation (§5) applied to gas. P4's re-centre follows activity
   into the shell, but a plume rising out of the top of the box is exactly the
   case tickets do not cover (gas that LEAVES belongs to the gas particles).

### Found by the gates (P1-P4)

1. **The TICKET_BOUND selection read the binding declaration too literally.**
   `BodyResolvesTickets` (resources.cpp; the same rule in `check_shaders.sh`
   and `check_pass_table.py`) looked for `> pageTable`; worldgen, sim_step,
   sim_particle, sim_mutate, sim_explode and the rest column-align their
   bindings (`>   pageTable`), so they compiled the STUB and every ticket
   chunk was generated as its window alias (solid stone 50 cells above the
   real ground). Now: `>` followed by any run of spaces/tabs.
2. **`sim_mutate`'s cell ops and `sim_solute`'s scoop bounded the cell index
   by `WORLD_N^3`**, so every op into a ticket slot was dropped (a P0 audit
   miss). Bound is `NUM_SLOTS * CHUNK_VOL`.
3. **Far-landing re-throws are drained before the `particlesActive` latch** in
   `PhaseL`, so a re-thrown particle runs the tick it is spawned.

### Found by the audit (2026-10-02)

An adversarial review of P1-P4 after the gates above were green. Each is fixed
on the branch. In a world with NO ticket every fix is an identity (a window
cell's path is bit-identical, and the pinned `determinismHash` did not move);
with a ticket live, fix 1 also stops ticket rings from stamping window slots,
so window behaviour beside a live ticket changes (deterministically).

1. **The repose snapshot decoded ticket slots as window coordinates.**
   `sim_step.wgsl reposesnap` runs over the dirty list and split each centre
   into `(cx, cy, cz)` by `% NCHUNK`; a ticket centre (>= `NUM_CHUNKS`) wrapped
   onto arbitrary WINDOW slots, filling and stamping them, while the ticket's
   own chunks were never snapshotted — and `reposeSnapOpen` read a ticket cell
   at `cellIndexW`, the window cell 512 away. Angle of repose in a ticket read
   another place in the world whenever that slot happened to be stamped (no
   matter lost: the move still goes through `tryMove`). A third instance of the
   P0 audit's "slot decomposition into window coordinates" class, this time in
   WGSL. Fixed: a ticket centre resolves its ring in world chunks
   (`reposeRingSlotT` / `reposeRingOwnedT`, owners must be ACTIVE dirty
   chunks), the probe resolves a ticket cell at its ticket slot, and the
   buffer covers `kNumSlots` (`world.h kReposeSnapBitWords`, +1 MiB).
2. **`sim_explode.wgsl markBoth` used `chunkIndexW`** after an `inBounds` that
   admits ticket cells: the shockwave woke the WINDOW chunk a ticket cell
   aliases, and left the blasted ticket chunk asleep (its release could skip
   keeping it). Now `chunkSlotOf`.
3. **Particle claims keyed ticket cells on `cellIndexW`**, the window cell 512
   away: a ticket landing and a window landing at the aliased cell shared a
   claim, and a grain group's uniform key would merge both groups' mass into
   whichever cell won. `sim_particle.wgsl claimCellId` is `cellIndexW` for a
   window cell (unchanged) and a world-block-salted id for a ticket cell.
   `flagLandedUnsupported` likewise flagged the aliased window chunk for an
   island scan; it is window-only now, as `flagSupportLoss` always was.
4. **`sim_openness.wgsl dirty` walked ticket slots as window slots.** It is a
   render shader, so it keeps the ticket STUB, whose `slotWorldChunk` decodes
   a ticket slot as a window slot: each dirty ticket chunk cost a walk of the
   wrong chunk and stamped the touch plane round it. It skips ticket slots now
   (a ticket draws through the far path, which reads no openness).
5. **The probe selection had no second opinion.** Besides accepting zero
   blanks (`read>pageTable`) in all three copies of the rule,
   `AssembleShaderSource` now finds the declaration a second, independent way
   (a non-comment line with `var` then the `pageTable` token) and REFUSES to
   build a shader on which the two disagree — the silent-stub failure the gates
   found once can no longer happen silently.
6. **Replay never ran the ticket step** (P1 10 above), and **held far landings
   never asked for a ticket** (P2 3 above).
7. A dead no-op hook (`Tickets::OnChunkResident`, called per filled slot) is
   gone; a stale comment on what triggers a re-centre is corrected.

New gate `ticket-look`: what a player SEES, with the far cascade FILLED (which
`ticket-render` leaves empty). A ticket on real terrain 4 chunks past the face,
looked at from ~15 m and ~47 m: activation changes the box's screen region by
a mean 2.0 / 1.2 RGB (no pop: fine voxels with far-path lighting against the
cascade's cells of the same terrain), a pillar built in it is drawn, and after
the release and a cascade refill it is still there. `SANDVOX_TICKET_SHOTS=
<prefix>` writes the eight frames.

**Measured idle cost against main fc8e451** (`--perf --scenario idle`,
`SANDVOX_RUN_EXCLUSIVE=1`, RTX 3060 Ti, same world hash `4720e963` on both):
frame p50 6.01 vs 6.02 ms; the sim-side GPU rows +11 us/frame (+3.5%), all of
it the slot space being 6% larger — `readbackCopy` +11 us (the snapshot's
per-slot arrays), `occupancyFull` +7 us on hash ticks — with `compact` flat
(3.7 vs 3.5 us). The raymarch's `ticketCount > 0` test is one compare per sky
ray and is inside noise.

**Stated limitations that remain** (follow-ups, not fixed here):
- A deposit TIE (two parked particles with bit-identical position and payload
  in one deposit slot on one tick) hands over ONE record and kills both. Same
  class as the claim system's equal-priority rule; needs a count word in the
  record.
- Far flight before the cascade first fills: `farBlocked` reads OPEN outside
  the cascade, so a particle can fall until the far box kills it (counted) —
  matter lost, and frame-timing dependent in the game (P2 1).
- Window-edge cells now see an ADJACENT ticket's shell as resident (a box may
  touch the window), so matter can move from the window into a ticket shell,
  where it freezes until the box re-centres, releases, or the window covers it.
  Deterministic, and nothing is lost, but the window edge stops being "solid
  and inert" there.
- Multiplayer: each peer's ticket set follows its own window and particles
  (per-client residency, DESIGN.md §10); divergence between peers is the
  existing per-chunk hash / authority resync's to repair, as for any chunk one
  peer simulates and another does not.
- Decay reactions with a reaction EFFECT (`reactFxNote`) are window-only; one
  firing in a ticket has no effect.
- `ticket-decay` has no in-window control arm proving the same fixture DOES
  ignite in the window (re-breaking `gInTicket` makes it fail: 55 of 64 wood
  cells burned, 33 fire cells — so it is sensitive today).

### P4

1. **A ticket hit is shaded by the FAR path.** `traceTickets` (raymarch.wgsl)
   returns a level-1 `FarHit` with `FAR_HIT_TICKET`: geometry and depth at full
   voxel resolution, lighting the cascade's (palette at the fine voxel, cascade
   shadow march and AO at level-1 granularity, no shadow cache — §5's packing
   limit stands). A second near-quality shade would be a second copy of the
   window's shading in the fragment shader with no register headroom; the
   march's state is all local to the call and dies before the shade.
2. **The ticket boxes ride `RenderParams`** (`ticketCount` + 16 `vec4<i32>`),
   filled from the same World table the sim resolves through. The slot inside
   a hit box is arithmetic (base + chunk mod 5), so the render path needs no
   probe and keeps P1's stub everywhere else.
3. **The march starts at the WINDOW exit (`windowExitT`), not `h.tExit`**,
   which the LOD handoff may have clamped well short of the window; and it is
   not gated on the ray-start map, which summarizes the window and cascade.
4. **Re-centring is a box shift**, not "release + activate": the leaving and
   entering planes share their 25 slots (mod 5), so the leaving plane is copied
   out as a two-phase keep batch (exactly as a release) and the same slots are
   refilled — 25 chunks instead of 125, recorded as one `kRecentre` op.
   **What counts as "activity in the shell" is matter crossing into it**,
   measured as a change of the shell chunk's non-air count (snapshot
   `occupancy`) and accumulated per chunk between re-centres (`shellHit`).
   Not a dirty flag: an interior write on a face cell marks the neighbouring
   shell dirty through the fan-out without moving anything into it, and an
   arrival leaves the shell dirty for one tick, so "dirty in the latest
   snapshot" missed every arrival that fell inside the cooldown (measured:
   `ticket-render`'s sand reached the bottom shell at ~+20 and the box never
   moved). The face is the one with the most hit shell chunks (ties: lower
   axis, then the negative side), at most once per `kTicketRecentreTicks`
   (60). Three refusals, each counted in `recentreRefused`: a step that would
   touch the window, one that would touch another live box, and **one that
   would turn an ACTIVE interior plane into shell** (the plane at offset 1
   stepping +, n-2 stepping -, dirty in the latest snapshot) — without that
   rule a decaying ember slab was chased out of its own box by a ripple at
   the far face (`ticket-decay`, 4 re-centres, slab frozen in the shell). A
   ticket with a pending shell hit does not idle out until Recentre has acted
   on it or refused it (at most `kTicketRecentreTicks`), so the release lag
   after a re-centre is idle + latency from the re-centre, not from the
   moment the interior slept; `ticket-settle` measures it that way.
5. **A fifth gate, `ticket-render`**, carries both P4 claims: a one-voxel
   column in a ticket changes the pixel it projects to and not the one three
   column-widths beside it (the cascade left empty, so only `traceTickets` can
   draw it), and poured sand that falls into the bottom shell moves the box
   with its mass conserved.
