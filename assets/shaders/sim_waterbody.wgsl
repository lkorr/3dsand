// sim_waterbody.wgsl — THE DRAIN LEDGER (component 3), THE SURFACE SHAVE
// (component 4), THE DISCHARGE LAW (component 6) and its hole detector, from
// docs/PLAN_water_master.md, plus the per-body adoption reduce component 1
// deferred. Milestones M2 and M3. (Component 7, the local excite at the throat,
// lives in sim_fluid_seam.wgsl's exciteDetect so it inherits the seam's
// existing ceiling and rate — see trigger (e) there.)
//
// ---- THE ONE-PARAGRAPH VERSION --------------------------------------------
//
// A still lake is a NAME plus five integers. Taking water out of it does not
// propagate pressure through 87,000 cells; it adds to one number. Once that
// number reaches `area` — the count of the lake's free-surface cells — every
// surface cell owes one eighth, and a single flat pass takes it. Draining is
// therefore O(surface) per tick and O(1) per unit of water, instead of
// O(volume) per tick, and the CA never runs on the interior at all.
//
// ---- WHY THIS IS ON THE GPU AND NOT ON THE CPU -----------------------------
//
// The master plan's §3.2 is the master rule and it is what forced the split:
//
//   > Debit what was GRANTED, never what was DEMANDED. The shave debits the
//   > ledger by cells it ACTUALLY shaved, not by the area table's prediction.
//
// The only honest source for "what was actually shaved" is an atomic the shave
// pass increments. If the ledger lived on the CPU it would have to READ that
// atomic back, and a readback arrives on a schedule set by fence retirement —
// so "how far the lake has dropped" would depend on when a fence retired, and
// that decides a voxel write. That is rule 1 broken through the back door, and
// no determinism gate that runs twice in one process with the same fence
// cadence would catch it. So the ledger, the level and the adoption verdict all
// live in `waterBodyState`, and the CPU sends only what is a pure function of
// (seed, window, tuning): geometry, a chunk list, thresholds.
//
// This is also the fix for the hazard M1 recorded (DESIGN.md §5b.4,
// waterbody.h's header): the M1 jurisdiction ladder's quiescence term read
// World::Snap(). Quiescence is now measured HERE, from `dirtyIn` and the MPM
// block map — the hashed world's own state, on the tick, every time.
//
// ---- THE SIX PASSES, AND WHY THAT ORDER ------------------------------------
//
//   wbQuiet   one thread per listed chunk. Was this chunk disturbed this tick?
//   wbLedger  one thread per body. The whole state machine and all arithmetic,
//             including the ONE evaluation of the head `h` that component 6's
//             single-evaluation rule turns on.
//   wbDrain   one thread per reserved spawn-op slot. Writes the jet.
//   wbReduce  one workgroup per listed chunk. Sums a candidate's voxel eighths.
//   wbShave   one workgroup per listed chunk. Takes eighths off the free
//             surface and REPORTS what it took.
//   wbHole    one workgroup per listed chunk, and only where something HAPPENED.
//             Finds the water/void interface and reports it for NEXT tick.
//
// The ledger runs BEFORE the shave, so it consumes LAST tick's shave report and
// publishes THIS tick's instruction — plan §3.3's "never read a tally in the
// pass that writes it", at pass granularity, with the recorder's barriers
// making the ordering a fact rather than a hope. The reduce runs after the
// ledger because the ledger is what ARMS it (clearing its accumulators), and it
// runs before the shave because it must measure a body nobody has shaved yet.
//
// ---- THE INVARIANT THIS FILE EXISTS TO KEEP --------------------------------
//
//     voxelEighths(t) + drained(t) - debit(t)  ==  voxelEighths(0)
//
// (From M3 the jet's eighths land somewhere else in the world rather than
// vanishing, so `--gate waterbody` pass H states the same thing over a BOX that
// contains both the lake and where the water went, with the in-flight MPM mass
// as an explicit term. Same discipline, wider bracket.)
//
// `drained` is what left the body forever; `debit` is what has been taken from
// the ledger but is still sitting in the voxels because no shave has removed it
// yet. Their difference is exactly the water the shave has destroyed. That
// divergence is plan §3.3's LEGITIMATE one and it is a STORED field, never
// implied — a conservation gate that forgot it would report a leak that does
// not exist. `--gate waterbody` pass A asserts the line above as integer
// equality, and every mistake in the ledger or the shave breaks it.

@group(0) @binding(0) var<storage, read_write> voxels : array<u32>;
// Read-only here: quiescence asks whether the CA marked this chunk for THIS
// tick's work. The layout entry is Storage (shared with the CA), which is why
// the pass table declares R(DirtyIn) and not something weaker.
@group(0) @binding(1) var<storage, read_write> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// The MPM block map's INDEX half: nonzero means this chunk holds excited fluid.
// The other half of "not settling" — a body with particles in it does not have
// a level surface, and adopting it would freeze that transient in as its height.
// Named with the `S` suffix sim_step.wgsl uses for the same binding, and for
// the reason PLAN_page_table.md §5.2 states: one WGSL identifier cannot carry
// two binding numbers across modules that share common.wgsl, and
// `fluidBlockMap` is already binding 2 of the fluid group.
@group(0) @binding(20) var<storage, read> fluidBlockMapS : array<u32>;
@group(0) @binding(24) var<storage, read_write> waterBodyState : array<atomic<i32>>;
// THE DISCHARGE'S EMISSION SEAM (component 6, M3). The same op stream the mpm
// tool and the lab pours use, and deliberately nothing new: `spawnAppend`
// already charges the pool budget, refuses past FLUID_CAP and clamps to
// FLUID_VMAX. What is new is only WHO fills it — the CPU cannot author these
// ops because the head `h` is derived from a level the GPU owns, and a jet
// emitted by one rule while the lake decrements by another is a mass pump under
// every edge case (plan §6's single-evaluation rule).
//
// The CPU reserves the block (T.waterDrainSpawnBase, T.waterDrainBodies), so
// rule 2's "charge the budget BEFORE emission" is still charged on the CPU and
// still charged before anything is written.
@group(0) @binding(25) var<storage, read_write> waterSpawnOps : array<FluidSpawnOp>;

// ---- human-unit tuning -> per-tick fixed point (the sim.fluid* lane) -------
//
// Q = Cd * A * sqrt(2 g h). Two of those three are physical quantities, so they
// are human-unit floats in tuning.json and they are converted HERE, once, at
// shader compile time: WGSL const-expressions are folded by Tint under IEEE
// rules, so the same tuning.json produces the same integers on every machine
// and the kernel below is integer-only (rule 1). Exactly the discipline
// sim_fluid.wgsl's const block states, for exactly the same reason.
//
// 30 Hz tick; internal units are cells and ticks, so a vox/s^2 gravity divides
// by 900 to become cells/tick^2.
const DRAIN_TWO_G : i32 =            // 2g, Q16.16 cells/tick^2
    i32(round(2.0 * TUNE_DRAIN_GRAVITY * 65536.0 / 900.0));
const DRAIN_CD_Q16 : i32 = i32(round(TUNE_DRAIN_CD * 65536.0));
// THE HEAD CAP, and it is plan §6's FIRST named trap made structural.
// `spawnAppend` clamps velocity to +-FLUID_VMAX (0.45 cell/substep), and a
// Torricelli velocity under real head exceeds it: at pondDepth 26 = 2.6 m the
// exit speed is 7.1 m/s. The clamp is correct and stays. But then the momentum
// asked for is not the momentum granted, and the ledger would be debiting
// against a Q the water never carried. So `h` is capped BEFORE Q is computed —
// v(hMax) is exactly FLUID_VMAX — and the two can no longer disagree.
//
//     v = sqrt(2 g h)  <=  vmax   <=>   h <= vmax^2 / (2 g)
//
// The alternative (accept the clamp and spread the flow over more particles at
// legal speed) is more physical and more work; what is not allowed is letting
// them diverge silently.
const DRAIN_VMAX_C : f32 = f32(FLUID_VMAX) / 65536.0;      // cells/tick
const DRAIN_G_C : f32 = TUNE_DRAIN_GRAVITY / 900.0;        // cells/tick^2
const DRAIN_H_MAX : i32 =
    max(1, i32(floor(DRAIN_VMAX_C * DRAIN_VMAX_C / (2.0 * max(DRAIN_G_C, 1e-6)))));

// Integer square root, Newton, exact over u32 and bit-identical everywhere.
// No sqrt(): this feeds a jet VELOCITY and a flow RATE that a mass ledger is
// compared against, and an f32 that rounds 4489.0 to 4488.9999 is a different
// world on a different driver (rule 1). Bounded to 24 iterations, which is
// several more than the u32 range needs.
fn wbIsqrt(n : u32) -> u32 {
  if (n == 0u) { return 0u; }
  var x = n;
  var y = (x + 1u) / 2u;
  for (var i = 0; i < 24; i++) {
    if (y >= x) { break; }
    x = y;
    y = (x + n / x) / 2u;
  }
  return x;
}

// The ledger's WORD MAP and the hole packing live in common.wgsl now, not
// here: from M3 a SECOND module reads them (sim_fluid_seam.wgsl's drain-shell
// trigger, component 7), and a word map transcribed into two shaders is the
// classic two-places-must-agree bug with no checker behind it. See the
// WBS_*/WB_*/wbHole* block in common.wgsl.

fn wbBase(b : u32) -> u32 { return b * WATERBODY_STATE_WORDS; }
fn wbGet(b : u32, w : u32) -> i32 { return atomicLoad(&waterBodyState[wbBase(b) + w]); }
fn wbSet(b : u32, w : u32, v : i32) { atomicStore(&waterBodyState[wbBase(b) + w], v); }

// ---- M5: the sweep block, past the end of the ledger in the same buffer ----
fn swGet(b : u32, w : u32) -> i32 {
  return atomicLoad(&waterBodyState[wbCurveBase(b) + w]);
}
fn swSet(b : u32, w : u32, v : i32) {
  atomicStore(&waterBodyState[wbCurveBase(b) + w], v);
}

// The two CPU-sent rows. Read straight off the module-scope uniform rather than
// through a helper that takes TickParams BY VALUE: common.wgsl:522 records what
// the by-value form costs when a function dynamically indexes a uniform array —
// the whole struct spills to scratch, 220 ms frames against 20 ms. The rule is
// "ptr<uniform, T> from the first line written", and the cheapest way to obey
// it is to never pass the struct at all.
fn wbGeom(b : u32) -> vec4<i32> { return T.waterBodies[b * 2u]; }
fn wbSeed(b : u32) -> vec4<i32> { return T.waterBodies[b * 2u + 1u]; }

// A chunk-list entry: (bodyIndex << 16) | chunkSlot, four to a std140 row.
fn wbChunkEntry(i : u32) -> u32 { return T.waterChunks[i >> 2u][i & 3u]; }

// ============================================================================
// M5 — THE FOOTPRINT TEST, and it is the one seam every pass below goes
// through. Before M5 "is this column mine" was a disc test and nothing else.
// After a basin SPLITS it is a disc test AND a component test, because the same
// disc now holds two pools that must descend at their own rates.
//
// THE IDENTITY PROPERTY. A basin with no split map reads component 0 for every
// grid cell (the buffer is zeroed at worldgen, and a body only ever gets a map
// written when the sweep is scheduled for it, which needs a dig). A body's own
// component index is 0 unless the CPU flagged it a child. So on every world
// M2/M3/M4 shipped, `wbOwns` is exactly the disc test it replaced, bit for bit
// — which is why M5 does not move the hash at sim.waterBodyMode 0 or at 1.
// ============================================================================
fn wbSplitAt(b : u32, gi : u32) -> u32 {
  if (gi >= WATER_SPLIT_CELLS) { return WB_COMP_NONE; }
  let w = atomicLoad(&waterBodyState[wbCurveBase(b) + SW_SPLIT0 + (gi >> 4u)]);
  return (u32(w) >> ((gi & 15u) * 2u)) & 3u;
}
// `radius` is carried rather than recomputed per call: every caller already has
// the geometry row, and an isqrt per cell in the shave's inner loop would be a
// real cost for a number that is constant across the dispatch.
// THE MAP LIVES IN THE PARENT'S BLOCK, and a child must be told so. The sweep
// runs once per basin and writes one split map, into the block of the body that
// owns the basin; a child has a curve block of its own that nothing ever fills.
// The first version had `wbOwns` read `wbCurveBase(b)` for whatever body asked,
// so a child read all-zeroes, saw SW_COMPS 0, concluded "no split" and answered
// "I own nothing anywhere" — its adoption reduce measured zero eighths and it
// sat in WB_CANDIDATE forever while the parent went on governing both pools.
// One map, one owner (design guideline #3), named in the signature so a caller
// cannot forget which body it belongs to.
fn wbMapOwner(b : u32, flags : i32) -> u32 {
  if ((flags & WBF_CHILD) == 0) { return b; }
  return u32((flags >> WBF_PARENT_SHIFT) & WBF_PARENT_MASK);
}
fn wbOwns(owner : u32, comp : u32, x : i32, z : i32, g : vec4<i32>,
          radius : i32) -> bool {
  let dx = x - g.x;
  let dz = z - g.y;
  // i32 is ample: kWindPrimMaxExtent-scale radii square to ~2.6e5 and the
  // window is 512 cells across, so the largest legal dx*dx + dz*dz is ~5.2e5.
  if (dx * dx + dz * dz > g.z) { return false; }
  // No split map yet: SW_COMPS is 0 and the whole disc belongs to component 0.
  // A CHILD owns nothing in that state, which is exactly right — it is a body
  // waiting for a pool that does not exist yet, and its reduce measures zero.
  if (swGet(owner, SW_COMPS) <= 1) { return comp == 0u; }
  let cell = wbSplitAt(owner, wbGridIndex(x, z, g.x, g.y, radius));
  // A BLOCKED grid cell falls to component 0. It holds no water at the mapped
  // level by definition, so which body "owns" it decides nothing about mass —
  // but leaving it unowned would drop any water standing ABOVE the split
  // elevation out of every footprint at once, and that water is real.
  if (cell == WB_COMP_NONE) { return comp == 0u; }
  return cell == comp;
}
// The CPU-sent component index and child flag (TickParams row 1, word 3).
fn wbComp(flags : i32) -> u32 {
  return u32((flags >> WBF_COMP_SHIFT) & WBF_COMP_MASK);
}

// The slot's 3D chunk coord, then the WORLD chunk it currently holds. World
// coords for logic, slot index for memory — the invariant this engine states in
// as many words, and the reason a body's chunk list is slots while every test
// below is in world cells.
fn wbSlotWorldChunk(slot : u32) -> vec3<i32> {
  let sc = vec3<i32>(i32(slot % NCHUNK),
                     i32((slot / NCHUNK) % NCHUNK),
                     i32(slot / (NCHUNK * NCHUNK)));
  return slotToWorldChunk(sc, T.origin);
}

// Next-tick dirty mark including boundary neighbours — the seam's convention,
// and for the seam's reason: these passes run after the CA, so their writes are
// next tick's business. The neighbour ring is what lets the CA re-level the
// dithered surface, which plan §4 wants (it is what makes a step read as water
// going down rather than as someone editing the water).
//
// NO SHAVE FIRES WHEN NOTHING DRAINS, so this costs exactly zero on a still
// lake and the <=32-active-chunks-at-rest assertion is untouched. That is
// asserted rather than assumed — `--gate waterbody` pass E.
fn wbMarkDirty(c : vec3<i32>) {
  let lo = c & vec3<i32>(CHUNK_MASK);
  let ch = worldChunkOf(c);
  var xs = array<i32, 2>(0, 0);
  var ys = array<i32, 2>(0, 0);
  var zs = array<i32, 2>(0, 0);
  if (lo.x == 0) { xs[1] = -1; } else if (lo.x == CHUNK_MASK) { xs[1] = 1; }
  if (lo.y == 0) { ys[1] = -1; } else if (lo.y == CHUNK_MASK) { ys[1] = 1; }
  if (lo.z == 0) { zs[1] = -1; } else if (lo.z == CHUNK_MASK) { zs[1] = 1; }
  for (var i = 0; i < 2; i++) {
    for (var j = 0; j < 2; j++) {
      for (var k = 0; k < 2; k++) {
        let n = ch + vec3<i32>(xs[i], ys[j], zs[k]);
        if (chunkInWindow(n, T.origin)) {
          atomicStore(&dirtyOut[chunkSlotIndex(n)], 1u);
        }
      }
    }
  }
}

// ============================================================================
// QUIESCENCE — one thread per listed chunk.
//
// This is the M1 hazard's replacement, and the whole point is WHERE the answer
// comes from: `dirtyIn` is the CA's own dirty set for this tick and
// `fluidBlockMap` is the MPM solver's own block index, both of them state the
// hashed world produced. Nothing here can observe when a fence retired.
//
// It is one storage read per listed chunk — a few hundred threads for a pond,
// every tick, with no writes to the world. That is what makes measuring
// quiescence continuously affordable, and it is why the expensive question
// ("how much water is in there") is a separate pass that runs once.
// ============================================================================
@compute @workgroup_size(64)
fn wbQuiet(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.waterChunkCount) { return; }
  let e = wbChunkEntry(gid.x);
  let b = e >> 16u;
  let slot = e & 0xFFFFu;
  if (b >= T.waterBodyCount) { return; }
  // Only a body still deciding needs this. An adopted body is NOT released by
  // going un-quiet — plan §5's "jurisdiction is LOCAL": a violent drain in an
  // otherwise still pond keeps the pooled descriptor for its bulk and hands
  // only a region around the throat to MPM. Releasing the whole lake because
  // someone poked a hole in it throws away the entire win at exactly the
  // moment it matters.
  let st = wbGet(b, WBS_STATE);
  if (st != WB_CANDIDATE && st != WB_MEASURING) { return; }
  var disturbed = 0;
  if (dirtyIn[slot] != 0u) { disturbed = 1; }
  if (fluidBlockMapS[slot] != 0u) { disturbed = 1; }
  if (disturbed != 0) { atomicAdd(&waterBodyState[wbBase(b) + WBS_RDIRTY], 1); }
}

// ============================================================================
// THE LEDGER — one thread per body. Every arithmetic decision in the design is
// in this function and nowhere else.
//
// The loop, per plan §3:
//   1. debit += what the drain took          (M2: the test source)
//   2. debit -= what LAST tick's shave actually removed        <-- §3.2
//   3. steps = debit / area,  frac = debit % area
//   4. publish (steps, frac) for this tick's shave
//   5. move `level` down when the top layer has actually emptied
//
// Integer against integer throughout. There is no scaling and no rounding
// anywhere in this file, because fullness is already eighths: lowering a body's
// surface by one eighth costs exactly `area` eighths, since each of `area` cells
// loses 1/8 of a voxel.
// ============================================================================
@compute @workgroup_size(64)
fn wbLedger(@builtin(global_invocation_id) gid : vec3<u32>) {
  let b = gid.x;
  if (b >= WATERBODY_CAP) { return; }
  // A body the CPU no longer proposes has left the registry (window moved,
  // straddle discovered, seed changed). Reset it: a descriptor is a description
  // of a world, and a stale one reads as a fresh one.
  if (b >= T.waterBodyCount) {
    if (wbGet(b, WBS_STATE) != WB_CANDIDATE) {
      for (var w = 0u; w < WATERBODY_STATE_WORDS; w++) { wbSet(b, w, 0); }
    }
    return;
  }
  let seed = wbSeed(b);
  let floorY = seed.x;
  let seedLevel = seed.y;
  let seedArea = seed.z;
  let flags = seed.w;

  var st = wbGet(b, WBS_STATE);
  let rdirty = wbGet(b, WBS_RDIRTY);
  wbSet(b, WBS_RDIRTY, 0);

  // ==========================================================================
  // M5 — ARM THIS TICK'S SWEEP, AND THE CYCLE BOUNDARY (components 2 + 10).
  //
  // The sweep ACCUMULATES — atomicAdd per container cell, atomicOr per open
  // grid cell — so something has to zero the slots it is about to fill, and it
  // cannot be the sweep itself: that dispatch is one workgroup per listed chunk
  // and no workgroup may assume it ran first. The ledger is one thread per
  // body, it is recorded before every chunk-shaped pass in the row block, and
  // it already owns every "clear for this tick's writers" line in this file
  // (WBS_SHAVED, WBS_SEEN, WBS_HOLEKEYN). So it clears here, for the same
  // reason and on the same cadence.
  // ==========================================================================
  // RESOLVE THE SWEEP LEVEL, ONCE, HERE. The CPU sends either a cursor level or
  // WB_SWEEP_LIVE ("the level the shave is actually working"), and only the GPU
  // knows the second one. Resolving it in each of the three passes that need it
  // would be three evaluations of one number, and they would DISAGREE on any
  // tick the shave lowers the level between the ledger and the sweep — the
  // clear would zero one area slot while the accumulate filled another, and the
  // count would grow forever. So it is published, exactly as the discharge law
  // publishes WBS_EMIT and WBS_JETV from its single evaluation of h.
  // PROMOTE last tick's arm before anything can re-arm it. 2 means "wbReduce
  // filled RSUM during the tick this was set"; 1 means "the value is ready and
  // the adopted branch below may spend it".
  if (wbGet(b, WBS_REAUDIT) == 2) { wbSet(b, WBS_REAUDIT, 1); }
  var sweepY = T.waterSweepLevel;
  if (sweepY == WB_SWEEP_LIVE) { sweepY = wbGet(b, WBS_LEVEL); }
  let sweepIdx = sweepY - floorY - 1;
  let sweepMine = b == T.waterSweepSlot && (flags & WBF_CHILD) == 0;
  let sweepChild = (flags & WBF_CHILD) != 0 &&
                   u32((flags >> WBF_PARENT_SHIFT) & WBF_PARENT_MASK) ==
                       T.waterSweepSlot;
  wbSet(b, WBS_SWEEPY, select(WB_SWEEP_NONE, sweepY,
                              sweepMine || sweepChild));
  if (sweepMine) {
    if (sweepIdx >= 0 && sweepIdx < i32(WATER_CURVE_MAXY)) {
      swSet(b, SW_AREA0 + u32(sweepIdx), 0);
    }
    for (var sw = 0u; sw < WATER_SPLIT_CELLS / 32u; sw++) {
      atomicStore(&waterBodyState[WATER_SCRATCH_BASE + sw], 0);
    }
  }
  // THE CYCLE BOUNDARY. `sweepIdx == 0` is the first level of a full pass over
  // the basin — a pure function of the tick, like every schedule in this design
  // (plan section 3.4) — and it is where component 10's re-derive actually
  // begins. Two things reset here and one thing is armed:
  //
  //   * SW_SPILLY and SW_SPLITY go back to their atomic identities, so a rim
  //     the player filled in or a partition they knocked down stops being
  //     reported within one cycle rather than forever. A monotone accumulator
  //     that never forgets is how "derived data" becomes stale authority.
  //   * WBS_REAUDIT is armed, which is THE FIX for the leftover M2 named and M3
  //     did not close: a body adopted once carries the volume it had at
  //     adoption, and anyone who digs into it makes that number a lie. It
  //     bounds the discharge through `held = VOLUME - DRAINED`, so a stale one
  //     stops a lake draining that still has plenty in it.
  //
  // Parent and child arm on the SAME boundary, which is why the child carries
  // its parent's slot in its flags: two pools re-measuring on different ticks
  // would briefly report a `held` sum that does not add up to the basin's
  // water, and a conservation gate cannot tell that apart from a leak.
  // Tested against the CURSOR the CPU sent, not against the resolved level: the
  // live-level refresh steps land on arbitrary Y values and would otherwise
  // reset the accumulators every time the surface happened to sit one cell above
  // the floor.
  if ((sweepMine || sweepChild) && T.waterSweepLevel == floorY + 1) {
    if (sweepMine) {
      swSet(b, SW_FLOORY, floorY);
      // PROMOTE, then reset. The *Y words now hold the reduction over the cycle
      // that just finished; the *YN words start the next one. See the SW_SPILLYN
      // note in common.wgsl for the failure this replaced.
      swSet(b, SW_SPILLY, swGet(b, SW_SPILLYN));
      swSet(b, SW_SPLITY, swGet(b, SW_SPLITYN));
      swSet(b, SW_SPILLYN, WB_HOLE_NONE);
      swSet(b, SW_SPLITYN, WB_SPLIT_NONE);
    }
    if (st == WB_ADOPTED) {
      wbSet(b, WBS_RSUM, 0);
      // ARMED == 2, not 1, and the two-step is not ceremony. The consume below
      // is in the SAME invocation of this kernel: written as a single flag it
      // fired on the tick it was armed, read the RSUM it had just zeroed, and
      // set VOLUME = 0 + DRAINED — which makes `held` zero, which REFUSES every
      // drain. The lake stopped draining and the gate reported a level that
      // never moved. Plan section 3.3 is about passes; this is the same rule
      // one level down, inside one pass.
      wbSet(b, WBS_REAUDIT, 2);
    }
  }

  if ((flags & WBF_PROPOSE) == 0) {
    // The CPU's own deterministic refusals (straddle, out of window, over the
    // spill, at the cap). Not a hysteresis exit — those bodies were never
    // adopted, and an adopted one is released through WBF_RELEASE below so the
    // outstanding debit can be paid off first.
    if (st == WB_CANDIDATE || st == WB_MEASURING) {
      for (var w = 0u; w < WATERBODY_STATE_WORDS; w++) { wbSet(b, w, 0); }
      return;
    }
  }

  if (st == WB_CANDIDATE) {
    var quiet = wbGet(b, WBS_QUIET);
    if (rdirty != 0) { quiet = 0; } else { quiet = quiet + 1; }
    wbSet(b, WBS_QUIET, quiet);
    if (quiet >= max(T.waterQuietTicks, 0)) {
      // ARM the reduce: clear its accumulators and let THIS tick's wbReduce
      // (which runs after us) fill them. We read them next tick. One
      // whole-footprint pass per adoption, ever.
      wbSet(b, WBS_RSUM, 0);
      wbSet(b, WBS_LEVEL, -0x40000000);   // atomicMax target for the reduce
      wbSet(b, WBS_STATE, WB_MEASURING);
    }
    return;
  }

  if (st == WB_MEASURING) {
    // The reduce ran last tick. Its sum is the ADOPTION SEED for the ledger and
    // the only moment the descriptor is ever derived from voxels, which is
    // exactly plan §3.1: the voxels are authoritative and this record is a
    // cache of aggregates over them.
    let vol = wbGet(b, WBS_RSUM);
    let lvl = wbGet(b, WBS_LEVEL);
    if (rdirty != 0 || vol < max(T.waterMinVolume, 1) || lvl <= floorY) {
      wbSet(b, WBS_STATE, WB_CANDIDATE);
      wbSet(b, WBS_QUIET, 0);
      wbSet(b, WBS_LEVEL, 0);
      return;
    }
    wbSet(b, WBS_VOLUME, vol);
    wbSet(b, WBS_LEVEL, lvl);
    // The analytic surface area seeds the pace and NOTHING ELSE. The very next
    // shave measures the real count and overwrites it, and the debit is always
    // what was granted — so an inaccurate seed costs one tick of slightly
    // off-pace descent and can never cost a single eighth. That is plan §3.2's
    // "a schedule, not an authority", in one line.
    wbSet(b, WBS_AREA, max(seedArea, 1));
    wbSet(b, WBS_DEBIT, 0);
    wbSet(b, WBS_DRAINED, 0);
    wbSet(b, WBS_SHAVED, 0);
    wbSet(b, WBS_SEEN, 0);
    wbSet(b, WBS_ATLEVEL, 0);
    wbSet(b, WBS_CAPPED, 0);
    wbSet(b, WBS_STEPS, 0);
    wbSet(b, WBS_FRAC, 0);
    wbSet(b, WBS_ADOPTTICK, i32(T.tick));
    // The hole record starts EMPTY, not stale. A body re-adopted after a
    // release must rediscover its holes from the voxels — the descriptor is a
    // cache of aggregates over the world (plan §3.1) and a hole is a fact about
    // the world, so carrying one across an adoption would be the same class of
    // error as carrying a level.
    wbSet(b, WBS_HOLEKEY, WB_HOLE_NONE);
    wbSet(b, WBS_HOLEKEYN, WB_HOLE_NONE);
    wbSet(b, WBS_HOLEAREA, 0);
    wbSet(b, WBS_HOLEAREAN, 0);
    wbSet(b, WBS_HOLETTL, 0);
    wbSet(b, WBS_EMIT, 0);
    wbSet(b, WBS_JETV, 0);
    wbSet(b, WBS_EXSHELL, 0);
    wbSet(b, WBS_STATE, WB_ADOPTED);
    return;
  }

  // ---- adopted (or paying off a release) ----------------------------------
  var debit = wbGet(b, WBS_DEBIT);
  var level = wbGet(b, WBS_LEVEL);
  var area = wbGet(b, WBS_AREA);

  // ---- M5: CONSUME THE RE-AUDIT (component 10) ----------------------------
  //
  // LAST tick's wbReduce refilled WBS_RSUM over this body's CURRENT footprint —
  // which after a split is only its own component's cells. Plan section 3.3
  // again, at pass granularity: the pass that arms the tally is not the pass
  // that spends it.
  //
  // ONLY VOLUME MOVES, and the FORM of the move is the whole correctness
  // argument. The standing invariant between the ledger and the cells is
  //
  //     voxels(this body) == held + debit,   held = VOLUME - DRAINED
  //
  // because a debit is water already accounted gone that no shave has taken off
  // the cells yet (plan section 3.3's legitimate divergence). The reduce
  // measured `voxels` into RSUM, so the volume that preserves the invariant is
  // `RSUM + DRAINED - DEBIT` and not `RSUM + DRAINED`. The simpler form makes
  // `held` equal today's voxels and then lets the outstanding debit be shaved
  // out from under it, so the body permanently over-reports what it holds by
  // one drain's worth — and pass B catches it as a split whose halves do not
  // sum to the basin.
  //
  // WBS_DRAINED and WBS_DEBIT are NOT touched: they are the cumulative terms
  // `--gate waterbody` passes A and H balance their identity on, and a re-audit
  // that reset either would read to both of them as a leak of everything the
  // body had ever drained.
  if (wbGet(b, WBS_REAUDIT) == 1) {
    wbSet(b, WBS_REAUDIT, 0);
    wbSet(b, WBS_VOLUME,
          wbGet(b, WBS_RSUM) + wbGet(b, WBS_DRAINED) - wbGet(b, WBS_DEBIT));
    wbSet(b, WBS_AUDITTICK, i32(T.tick));
  }

  // (2) DEBIT WHAT WAS GRANTED. `shaved` is what the cells actually gave up
  // last tick, which is not what was asked for whenever a cell held fewer
  // eighths than the step wanted. The shortfall simply stays outstanding and is
  // taken next tick — self-pacing, and mass-exact by construction rather than
  // by the area table being right.
  debit = debit - wbGet(b, WBS_SHAVED);

  // The surface the shave actually FOUND replaces the prediction. This is the
  // same rule one level up: measured beats analytic, and the analytic value was
  // only ever a way to start.
  let seen = wbGet(b, WBS_SEEN);
  let atLevel = wbGet(b, WBS_ATLEVEL);
  if (seen > 0) {
    area = seen;
    // The top layer has emptied out from under the surface: everything the
    // shave could still find sits one voxel lower. Moving the level here rather
    // than predicting it from the curve is what lets the drain follow a bowl
    // that narrows without the curve having to be exact.
    if (atLevel == 0) { level = level - 1; }
  } else if (debit > 0 && level > floorY) {
    // Nothing found in the band at all, with water still owed. Either the layer
    // is gone or the shave has not run yet; step down and look again. Bounded
    // by the basin floor, so a body that has genuinely run dry stops rather
    // than descending forever.
    level = level - 1;
  }
  if (area < 1) { area = 1; }

  // (1) THE DRAIN SOURCE. M2 has no discharge law (component 6 is M3), so this
  // is the test-only tap: `sim.waterBodyTestDrain` eighths per tick, on the tick
  // input stream, 0 in every shipped world. `drained` is the running total of
  // what left the body FOREVER and it is the other term of pass A's identity —
  // it is a stored field for the same reason the divergence is: a conservation
  // sum that has to infer one of its terms is a conservation sum that cannot
  // attribute a failure.
  if (st == WB_ADOPTED && T.waterTestDrain > 0) {
    let want = T.waterTestDrain;
    // Never owe more than the body physically holds. Without this a test tap
    // left running past the end of the water would accumulate a debit no shave
    // can ever pay, and the level would march down through the floor.
    let held = wbGet(b, WBS_VOLUME) - wbGet(b, WBS_DRAINED);
    let take = clamp(want, 0, max(held, 0));
    debit = debit + take;
    wbSet(b, WBS_DRAINED, wbGet(b, WBS_DRAINED) + take);
  }

  // ==========================================================================
  // (1b) THE DISCHARGE LAW — component 6. THE drain source from M3 on.
  // ==========================================================================
  //
  // PROMOTE LAST TICK'S SCAN. wbHole ran at the END of last tick's row block
  // and accumulated into the *N words; this is the only reader and the only
  // clearer, so a sighting can never be counted twice (plan §3.3, at pass
  // granularity, exactly as `shaved` above).
  let holeKeyN = wbGet(b, WBS_HOLEKEYN);
  let holeAreaN = wbGet(b, WBS_HOLEAREAN);
  var holeKey = wbGet(b, WBS_HOLEKEY);
  var holeArea = wbGet(b, WBS_HOLEAREA);
  var holeTtl = wbGet(b, WBS_HOLETTL);
  if (holeKeyN != WB_HOLE_NONE) {
    // `holeAreaN` counted cells at the PREVIOUS key's height, so it is only the
    // orifice of the hole we are about to keep. When the deepest escape point
    // moves (the shaft was driven another cell down) the area re-seeds at 1 and
    // the next scan measures it — an under-measured A slows the drain by a tick
    // and cannot lose an eighth, because the debit is what was emitted.
    let sameY = (holeKeyN >> 22) == (holeKey >> 22);
    holeArea = select(1, max(holeAreaN, 1), sameY && holeKey != WB_HOLE_NONE);
    holeKey = holeKeyN;
    holeTtl = WB_HOLE_TTL;
  } else {
    holeTtl = max(holeTtl - 1, 0);
    if (holeTtl == 0) { holeKey = WB_HOLE_NONE; holeArea = 0; }
  }
  wbSet(b, WBS_HOLEKEYN, WB_HOLE_NONE);   // armed for THIS tick's scan
  wbSet(b, WBS_HOLEAREAN, 0);
  wbSet(b, WBS_HOLEKEY, holeKey);
  wbSet(b, WBS_HOLEAREA, holeArea);
  wbSet(b, WBS_HOLETTL, holeTtl);

  var emit = 0;
  var jetv = 0;
  // `b < T.waterDrainBodies` is rule 2 charged BEFORE emission: the CPU
  // reserved this body a block of WATER_DRAIN_OPS spawn slots or it did not,
  // and a body without a block is REFUSED the discharge outright rather than
  // granted eighths no particle can carry. Refusal costs only realism — the
  // water is still there and the CA still moves it.
  if (st == WB_ADOPTED && holeKey != WB_HOLE_NONE && T.waterDrainMax > 0 &&
      b < T.waterDrainBodies) {
    let holeY = wbHoleY(holeKey, floorY);
    // ---- THE SINGLE EVALUATION OF h ---------------------------------------
    // Everything below — the exit speed the particles carry and the eighths the
    // ledger owes — comes from this one line. Capped at DRAIN_H_MAX so the
    // momentum asked for is the momentum spawnAppend grants (see the const
    // block's note on plan §6 trap 1).
    let h = clamp(level - holeY, 0, DRAIN_H_MAX);
    if (h > 0) {
      // v = sqrt(2 g h). DRAIN_TWO_G is Q16.16, so DRAIN_TWO_G * h is v^2 in
      // Q16.16 (cells/tick)^2 and its integer square root is v in Q8 — the
      // half-shift is what keeps the whole computation inside i32 without a
      // 64-bit intermediate. Overflow: DRAIN_TWO_G <= 2*4000/900*65536 = 5.8e5
      // at the tuning clamp, times DRAIN_H_MAX (<= 8 at the shipped substep
      // budget, <= 128 at 32 substeps) = 7.5e7, comfortably inside i32.
      let vQ8 = i32(wbIsqrt(u32(DRAIN_TWO_G * h)));
      jetv = min(vQ8 << 8, FLUID_VMAX);
      // Q = Cd * A * v, in cells^3/tick, x8 to reach eighths.
      //   cdv (Q8) = Cd * v      <= 1.0 * 4.05 cells/tick -> ~1037
      //   q        = cdv * A * 8 / 256 = cdv * A / 32
      // A is a cell count bounded by the basin's surface (14,493 for the
      // harness lake), so the product tops out near 1.5e7.
      let cdv = (DRAIN_CD_Q16 * vQ8) >> 16;
      var q = (cdv * max(holeArea, 1)) / 32;
      // THE PER-HOLE PER-TICK BOUND (rule 2, plan §6 trap 2). Two caps and the
      // op block is the harder of them: granting more than WATER_DRAIN_OPS
      // would be a debit with no particle behind it.
      q = min(q, T.waterDrainMax);
      q = min(q, i32(WATER_DRAIN_OPS));
      // Never owe more than the body physically holds, for the reason the test
      // tap has the same clamp: a debit no shave can pay marches the level down
      // through the basin floor.
      let held = wbGet(b, WBS_VOLUME) - wbGet(b, WBS_DRAINED);
      emit = clamp(q, 0, max(held, 0));
      debit = debit + emit;
      wbSet(b, WBS_DRAINED, wbGet(b, WBS_DRAINED) + emit);
    }
  }
  // Published for wbDrain, which writes exactly `emit` live ops and fills the
  // rest of the block with dead ones. That equality is what makes "debit what
  // was GRANTED" (plan §3.2) true by construction here rather than by audit:
  // the number the ledger owes and the number of particles that exist are the
  // same integer, read from the same word, in the same tick.
  wbSet(b, WBS_EMIT, emit);
  wbSet(b, WBS_JETV, jetv);

  if (debit < 0) { debit = 0; }   // cannot happen; the shave only takes what it was told

  // (3)+(4) PUBLISH. `steps` is whole eighths every surface cell drops; `frac`
  // is the leftover, spent as a per-cell probability by the shave's dither.
  // Expected extra drops = area * (frac/area) = frac exactly — and the ACTUAL
  // number is counted and debited, so the dither is not an approximation, it is
  // just how the remainder is spatially distributed. Without it the whole lake
  // snaps down a step at once and reads as an edit rather than as drainage.
  // ONE VOXEL PER TICK, MAXIMUM (rule 2: bound every emergent process). At the
  // shipped configuration this cap never binds — pass A's tap is sized at
  // exactly one eighth-step so `steps` is 1, and the discharge law's whole
  // emission is bounded by sim.drainMaxEighthsPerTick, three orders below a
  // lake's surface area. It exists for the case M5 introduced: a body whose
  // FOOTPRINT SHRINKS under a split still owes a debit accrued against the
  // whole basin, and `debit / area` over the half it kept is a 20-eighth step —
  // which is not a level model descending, it is a bulldozer, and it strips the
  // pool the parent was left with before the second descriptor can adopt it.
  // Capped, the surplus simply stays outstanding and is taken over the
  // following ticks; refusal is graceful because refused water is still water.
  let steps = min(debit / area, WB_MAX_STEPS);
  let frac = select(0, debit - steps * area, steps < WB_MAX_STEPS);
  wbSet(b, WBS_STEPS, steps);
  wbSet(b, WBS_FRAC, frac);
  wbSet(b, WBS_DEBIT, debit);
  wbSet(b, WBS_LEVEL, level);
  wbSet(b, WBS_AREA, area);
  // Cleared for THIS tick's shave to fill. The ledger is the only reader and
  // the only clearer, so the report can never be double-counted.
  wbSet(b, WBS_SHAVED, 0);
  wbSet(b, WBS_SEEN, 0);
  wbSet(b, WBS_ATLEVEL, 0);

  // ---- the exit, and why it is not immediate -----------------------------
  // Release must be MASS-EXACT in both directions (plan §5). An outstanding
  // debit is water this system has already accounted as gone that is still
  // sitting in the voxels; dropping the descriptor now would hand the CA a lake
  // holding water nobody owns, i.e. it would INVENT water. So a released body
  // keeps shaving — and stops accepting new debit, the `st == WB_ADOPTED` guard
  // above — until the ledger is square.
  if (st == WB_ADOPTED && (flags & WBF_RELEASE) != 0) { st = WB_RELEASING; }
  if (st == WB_RELEASING && debit == 0) {
    for (var w = 0u; w < WATERBODY_STATE_WORDS; w++) { wbSet(b, w, 0); }
    return;
  }
  wbSet(b, WBS_STATE, st);
}

// ============================================================================
// THE ADOPTION REDUCE — one workgroup per listed chunk, one thread per column.
//
// Component 1 specified this and M1 deferred it: adoption reads the voxel sum
// into the ledger. It is the ONE whole-footprint pass in the design and it runs
// once per adoption, on the single tick a body spends in WB_MEASURING.
//
// It also finds the level, by atomicMax over the highest cell of the body's
// material. Measured rather than taken from the analytic fill height, because
// the fill height is what worldgen INTENDED and the voxels are what is there —
// pond life, a ruin intruding, a player's bucket.
// ============================================================================
@compute @workgroup_size(16, 1, 16)
fn wbReduce(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_id) li : vec3<u32>) {
  if (wg.x >= T.waterChunkCount) { return; }
  let e = wbChunkEntry(wg.x);
  let b = e >> 16u;
  let slot = e & 0xFFFFu;
  if (b >= T.waterBodyCount) { return; }
  // M5: the reduce now serves TWO callers. Adoption is the original one and
  // runs exactly once, on the single tick a body spends in WB_MEASURING. The
  // RE-AUDIT is the second: an adopted body whose basin someone dug into
  // re-measures on its sweep cycle's first level, which is once per
  // kWaterSweepPeriod * span ticks and only while the basin is dirty. A still
  // lake nobody has touched runs neither, which is what keeps this the ONE
  // whole-footprint pass in the design.
  let rState = wbGet(b, WBS_STATE);
  let reaudit = wbGet(b, WBS_REAUDIT) != 0;
  if (rState != WB_MEASURING && !(reaudit && rState == WB_ADOPTED)) { return; }

  let g = wbGeom(b);
  let seed = wbSeed(b);
  let wc = wbSlotWorldChunk(slot);
  let x = wc.x * i32(CHUNK) + i32(li.x);
  let z = wc.z * i32(CHUNK) + i32(li.z);
  // M5: the disc test AND the split component test, in one seam (wbOwns). On an
  // unsplit basin this is bit-identical to the bare disc test it replaced.
  if (!wbOwns(wbMapOwner(b, seed.w), wbComp(seed.w), x, z, g,
              i32(wbIsqrt(u32(max(g.z, 0)))))) {
    return;
  }

  var sum = 0;
  var top = -0x40000000;
  for (var ly = 0; ly < i32(CHUNK); ly++) {
    let y = wc.y * i32(CHUNK) + ly;
    if (y <= seed.x || y > seed.y) { continue; }   // (floorY, seedLevel]
    let w = voxWordAt(vec3<i32>(x, y, z));
    if (i32(voxMat(w)) != g.w) { continue; }
    sum = sum + i32(((w >> 12u) & 0xFu) + 1u);     // fullness, 1..8 eighths
    top = max(top, y);
  }
  if (sum > 0) { atomicAdd(&waterBodyState[wbBase(b) + WBS_RSUM], sum); }
  // THE LEVEL IS AN ADOPTION-ONLY OUTPUT. A re-audit must not touch it: an
  // adopted body's level is the ledger's, moved down by the shave as layers
  // empty, and an atomicMax against a straggler cell left standing above the
  // free surface would jack it back up and un-drain the lake on paper.
  if (rState == WB_MEASURING && top > -0x40000000) {
    atomicMax(&waterBodyState[wbBase(b) + WBS_LEVEL], top);
  }
}

// ============================================================================
// THE SURFACE SHAVE — one workgroup per listed chunk, one thread per column.
//
// THE PREDICATE, per cell (plan §4):
//     is this body's liquid
//     AND my Y is in the band [level-1, level]
//     AND the cell directly above is not this liquid   (i.e. I am free surface)
//     AND ( steps > 0  OR  hash3(seed, tick, cellIndex) % area < frac )
//
// Reach 0 for the write, reach 1 for the read directly above. Lattice-safe by
// construction and no mark/apply needed: a thread owns one (x,z) COLUMN and
// writes at most one cell in it, so no two threads can ever write the same cell
// and the only read-after-write in the design is a thread reading the cell it
// is about to write. The `break` is what keeps even that from happening — one
// shave per column per tick, always.
//
// THE BAND IS TWO Y VALUES, and that is what makes the pass O(surface) instead
// of O(volume). A listed chunk whose Y span misses the band returns after three
// scalar loads, so a body's full footprint can be listed once and the dispatch
// still only does work where the water actually is.
// ============================================================================
@compute @workgroup_size(16, 1, 16)
fn wbShave(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_id) li : vec3<u32>) {
  if (wg.x >= T.waterChunkCount) { return; }
  let e = wbChunkEntry(wg.x);
  let b = e >> 16u;
  let slot = e & 0xFFFFu;
  if (b >= T.waterBodyCount) { return; }
  let st = wbGet(b, WBS_STATE);
  if (st != WB_ADOPTED && st != WB_RELEASING) { return; }
  let steps = wbGet(b, WBS_STEPS);
  let frac = wbGet(b, WBS_FRAC);
  if (steps == 0 && frac == 0) { return; }   // nothing owed: idle cost is zero
  let level = wbGet(b, WBS_LEVEL);
  let area = max(wbGet(b, WBS_AREA), 1);

  let g = wbGeom(b);
  let seed = wbSeed(b);
  let wc = wbSlotWorldChunk(slot);
  let cyLo = wc.y * i32(CHUNK);
  let cyHi = cyLo + i32(CHUNK) - 1;
  let y1 = min(level, cyHi);
  let y0 = max(level - 1, cyLo);
  if (y1 < y0) { return; }   // this chunk is not in the band

  let x = wc.x * i32(CHUNK) + i32(li.x);
  let z = wc.z * i32(CHUNK) + i32(li.z);
  // M5: disc AND split component (wbOwns). After a basin splits this is what
  // keeps the parent shaving its own pool and stops it taking eighths off the
  // puddle on the other side of the partition — a pool with no hole in it must
  // stop descending, and before M5 it did not.
  if (!wbOwns(wbMapOwner(b, seed.w), wbComp(seed.w), x, z, g,
              i32(wbIsqrt(u32(max(g.z, 0)))))) {
    return;
  }

  for (var y = y1; y >= y0; y--) {
    if (y <= seed.x) { break; }              // at or below the basin floor
    let c = vec3<i32>(x, y, z);
    let w = voxWordAt(c);
    if (i32(voxMat(w)) != g.w) { continue; }
    // FREE SURFACE: nothing of ours directly above. This read may cross a chunk
    // boundary, which voxWordAt resolves through the page table; it is a READ,
    // so a sentinel chunk synthesizes rather than faulting.
    if (i32(voxMat(voxWordAt(c + vec3<i32>(0, 1, 0)))) == g.w) { continue; }

    // COUNT WHAT WAS SEEN, always — even when the cell turns out to have
    // nothing left to give. `seen` becomes next tick's `area` and `atLevel` is
    // what tells the ledger the top layer has emptied, so both have to describe
    // the surface rather than the successful writes.
    atomicAdd(&waterBodyState[wbBase(b) + WBS_SEEN], 1);
    if (y == level) { atomicAdd(&waterBodyState[wbBase(b) + WBS_ATLEVEL], 1); }

    var want = steps;
    if (frac > 0 &&
        i32(hash3(T.seed, T.tick, cellIndexW(c)) % u32(area)) < frac) {
      want = want + 1;
    }
    let full = i32(((w >> 12u) & 0xFu) + 1u);   // 1..8 eighths
    let take = min(want, full);
    if (take > 0) {
      let left = full - take;
      // A liquid's state nibble is fullness-1, so an emptied cell is not
      // "fullness 0" — it is AIR, written as a clean zero word the way every
      // other emptier in the engine writes it (stain and stamp go with it).
      var nw = 0u;
      if (left > 0) { nw = (w & 0xFFFF0FFFu) | (u32(left - 1) << 12u); }
      voxStore(voxWordIndex(c), nw);
      atomicAdd(&waterBodyState[wbBase(b) + WBS_SHAVED], take);
      wbMarkDirty(c);
    }
    // ATTRIBUTION, not statistics. A drain that stalls is either "the ledger is
    // wrong" or "the cells had less than the ledger thought", and those need
    // different fixes. Recording the shortfall here is the difference between
    // one run and CLAUDE.md rule 6's fourteen.
    if (want > take) {
      atomicAdd(&waterBodyState[wbBase(b) + WBS_CAPPED], want - take);
    }
    break;   // one surface cell per column per tick — see the header
  }
}

// ============================================================================
// THE DISCHARGE, EMITTED — component 6's other half. One thread per RESERVED
// spawn-op slot.
//
// WHY A THREAD PER SLOT rather than a loop in the ledger: every slot in the
// block must be written every tick it exists. A slot the pass skipped keeps
// whatever a previous tick left in it and `spawnAppend` would hand that stale
// particle back to the pool as live — the same class of bug as a carried
// descriptor, and invisible until a lake spat two-tick-old water. So the block
// is exhaustively filled: `emit` live ops, then dead ones (mat 0, which
// spawnAppend writes as a dead particle for compaction to drop next tick).
//
// THE SINGLE-EVALUATION RULE IS KEPT BY CONSTRUCTION. This pass does not
// recompute h, Q or v. It reads WBS_EMIT and WBS_JETV, which the ledger wrote
// from one evaluation, in the pass before this one. There is no second rule to
// disagree with the first.
// ============================================================================
@compute @workgroup_size(64)
fn wbDrain(@builtin(global_invocation_id) gid : vec3<u32>) {
  let total = T.waterDrainBodies * WATER_DRAIN_OPS;
  if (gid.x >= total) { return; }
  let b = gid.x / WATER_DRAIN_OPS;
  let k = i32(gid.x % WATER_DRAIN_OPS);
  let slot = T.waterDrainSpawnBase + gid.x;

  var op : FluidSpawnOp;
  op.px = 0; op.py = 0; op.pz = 0;
  op.vx = 0; op.vy = 0; op.vz = 0;
  op.species = 0u;
  op.mat = 0u;                       // DEAD: spawnAppend writes a dead particle

  if (b < T.waterBodyCount && b < WATERBODY_CAP) {
    let emit = wbGet(b, WBS_EMIT);
    let st = wbGet(b, WBS_STATE);
    if (st == WB_ADOPTED && k < emit) {
      let g = wbGeom(b);
      let seed = wbSeed(b);
      let key = wbGet(b, WBS_HOLEKEY);
      let area = max(wbGet(b, WBS_HOLEAREA), 1);
      let hx = wbHoleX(key, g.x);
      let hz = wbHoleZ(key, g.y);
      let hy = wbHoleY(key, seed.x);
      // SPREAD OVER THE ORIFICE. All `emit` particles born in one cell would be
      // an over-packed node the EOS ejects rather than a jet; the hole is `area`
      // cells wide, so scatter across a square of that side. hash3 keyed on
      // (seed, tick, slot) — the slot is this thread's own index, a pure
      // function of the dispatch shape and not of arrival order (rule 1).
      let hr = i32(wbIsqrt(u32(area))) / 2;
      let h0 = hash3(T.seed, T.tick, u32(0x5EA1u) ^ gid.x);
      let h1 = pcg(h0);
      var ox = 0;
      var oz = 0;
      if (hr > 0) {
        let span = u32(2 * hr + 1);
        ox = i32(h0 % span) - hr;
        oz = i32((h0 >> 8u) % span) - hr;
      }
      let c = vec3<i32>(hx + ox, hy, hz + oz);
      // Q16.16 world cells, jittered inside the cell so the P2G scatter sees a
      // spread of positions rather than `emit` coincident particles.
      op.px = (c.x << 16) + 8192 + i32(h1 & 0xBFFFu);
      op.py = (c.y << 16) + 8192 + i32((h1 >> 14u) & 0xBFFFu);
      op.pz = (c.z << 16) + 8192 + i32(pcg(h1) & 0xBFFFu);
      // The exit velocity, DOWNWARD. Bounded by DRAIN_H_MAX to be inside
      // FLUID_VMAX, so spawnAppend's clamp is a belt-and-braces no-op here and
      // the momentum granted is the momentum the head paid for.
      op.vy = -wbGet(b, WBS_JETV);
      op.mat = u32(g.w);
      op.species = (u32(g.w) - 1u) & 3u;   // the exciteEmit convention
    }
  }
  waterSpawnOps[slot] = op;
}

// ============================================================================
// HOLE DETECTION — component 6's trigger. One workgroup per listed chunk, one
// thread per (x,z) COLUMN.
//
// A HOLE IS AN AIR CELL UNDER THE WATERLINE WITH AIR UNDER IT: inside the
// body's footprint, at or below the body's level, and with nothing beneath it
// to hold water up. In an intact basin that set is EMPTY by construction — a
// tarn's bowl replaces the ground, so every column inside the disc is stone up
// to the floor and water from there to the level. It becomes non-empty exactly
// when someone digs, explodes or bores through, which is what plan §6 means by
// "detect on the chunk-dirty path".
//
// AND THAT IS LITERALLY THE GATE: this pass does nothing at all on a chunk the
// CA did not mark and the MPM has no block in. A still lake with no hole in it
// runs `dirtyIn[slot] == 0` for every listed chunk and returns, so component 6
// costs the same zero at rest that components 3 and 4 do (pass E).
//
// THE LOWEST CANDIDATE WINS, by atomicMin over a key that packs (y, x, z) — the
// deepest escape point is the greatest head, and integer min is order-free so
// two threads racing cannot change which cell that is.
// ============================================================================
@compute @workgroup_size(16, 1, 16)
fn wbHole(@builtin(workgroup_id) wg : vec3<u32>,
          @builtin(local_invocation_id) li : vec3<u32>) {
  if (wg.x >= T.waterChunkCount) { return; }
  let e = wbChunkEntry(wg.x);
  let b = e >> 16u;
  let slot = e & 0xFFFFu;
  if (b >= T.waterBodyCount) { return; }
  if (T.waterDrainMax <= 0) { return; }
  let st = wbGet(b, WBS_STATE);
  if (st != WB_ADOPTED) { return; }
  // THE CHUNK-DIRTY PATH. `dirtyIn` is the CA's own answer to "was this chunk
  // disturbed this tick" and the block map is the MPM's; between them they
  // cover the dig that opens a hole and the jet that keeps one open. A chunk
  // that is neither is a chunk nothing has happened in, and rescanning it would
  // be the O(volume)-per-tick cost this whole design exists to delete.
  if (dirtyIn[slot] == 0u && fluidBlockMapS[slot] == 0u) { return; }

  let g = wbGeom(b);
  let seed = wbSeed(b);
  let level = wbGet(b, WBS_LEVEL);
  let curKey = wbGet(b, WBS_HOLEKEY);
  let wc = wbSlotWorldChunk(slot);
  let x = wc.x * i32(CHUNK) + i32(li.x);
  let z = wc.z * i32(CHUNK) + i32(li.z);
  let dx = x - g.x;
  let dz = z - g.y;
  // M5: disc AND split component. A hole belongs to the pool standing over it,
  // so after a split each body finds its OWN holes — which is what makes the
  // puddle with the shaft in it the one that keeps draining.
  if (!wbOwns(wbMapOwner(b, seed.w), wbComp(seed.w), x, z, g,
              i32(wbIsqrt(u32(max(g.z, 0)))))) {
    return;
  }

  let cyLo = wc.y * i32(CHUNK);
  let y0 = max(cyLo, seed.x - WB_HOLE_YBIAS);
  let y1 = min(cyLo + i32(CHUNK) - 1, level);
  if (y1 < y0) { return; }

  // The current hole's height, for the ORIFICE AREA count. Measuring A at the
  // height the ledger is actually using is what makes `Q = Cd*A*sqrt(2gh)` an
  // orifice equation rather than an arbitrary rate: a 5x5 shaft twenty cells
  // deep has an area of 25, not 500.
  var curY = -0x40000000;
  if (curKey != WB_HOLE_NONE) { curY = wbHoleY(curKey, seed.x); }

  var found = false;
  for (var y = y0; y <= y1; y++) {
    // THE PREDICATE IS THE WATER/VOID INTERFACE, not "a void under the lake",
    // and the difference is the whole orifice equation. The first version of
    // this pass took any air cell with air below it, which finds the FLOOR OF
    // THE CAVERN the shaft opens into — measured on `--gate waterbody` pass H
    // as A = 473 for a 5x5 shaft, because the chamber under it is 25 cells
    // across. `A` is then not an orifice at all and Q is an arbitrary rate.
    //
    // What water escapes THROUGH is the cell where the body's own liquid has
    // nothing under it. That set is EMPTY in an intact basin by construction
    // (every column inside the disc is stone to the floor and water above it),
    // it is exactly the 5x5 shaft mouth when someone bores through, and it
    // TRACKS the mouth down as the shaft empties — which is the head growing,
    // which is what Torricelli is about.
    let c = vec3<i32>(x, y, z);
    if (i32(voxMat(voxWordAt(c))) != g.w) { continue; }
    if (voxMat(voxWordAt(c + vec3<i32>(0, -1, 0))) != MAT_AIR) { continue; }
    // The HOLE is the void the water is standing over, one cell down.
    let hy = y - 1;
    if (hy == curY) {
      atomicAdd(&waterBodyState[wbBase(b) + WBS_HOLEAREAN], 1);
    }
    if (!found) {
      // Lowest in this column only: the cells above it are the same shaft, and
      // one atomicMin per column per tick is the bound on this pass.
      atomicMin(&waterBodyState[wbBase(b) + WBS_HOLEKEYN],
                wbHoleKey(hy - seed.x, dx, dz));
      found = true;
    }
  }
}

// ============================================================================
// M5 — THE CONTAINER SWEEP (plan component 2, case 2; component 10).
// One workgroup per listed chunk, one thread per (x,z) COLUMN.
//
// ---- WHY THIS RUNS ON THE GPU AND NOT ON THE CPU --------------------------
//
// Plan section 2 offers three homes for the sweep, in preference order: a GPU
// compute pass over the basin's chunk AABB writing the table to a small buffer;
// an ASYNC readback through voxregion; a CPU walk. The first is the only one
// that is rule-1 clean, and the reason is one M1 already paid for.
//
// The table decides which pool a cell belongs to and therefore which cells the
// shave takes an eighth off. That is a voxel write. A table derived from a
// READBACK would have its CONTENT fixed by the tick it was requested on (fine)
// and its ARRIVAL fixed by fence retirement (not fine) — so "when did the lake
// start draining as two puddles" would depend on when a fence retired, on a
// machine, on a driver. `--gate waterbody` pass F would not catch it either,
// because two runs in one process share a fence cadence. That is exactly the
// hazard section 1.1 correction 2 records, arriving through a different door.
//
// So the sweep is a kernel, its output is consumed by kernels, and the CPU's
// entire contribution is a SCHEDULE: which body, which level, both pure
// functions of the tick (plan section 3.4).
//
// ---- THE FOUR OUTPUTS, AND WHICH PASS PRODUCES EACH -----------------------
//
//   1. area(y)          this pass, atomicAdd per container cell   -> SW_AREA0
//   2. spill elevation  this pass, atomicMin over the ring out    -> SW_SPILLY
//   3. split elevations wbSplit, atomicMax over split levels      -> SW_SPLITY
//   4. split children   wbSplit, the 2-bit component map          -> SW_SPLIT0
//
// One level per scheduled tick. A 26-deep bowl is a 26-scheduled-tick =
// 104-world-tick re-derive, i.e. 3.5 s of staleness in the worst case, and that
// costs pace rather than mass — the table is a schedule, not an authority (plan
// section 3.2), so the shave still debits what it actually removed either way.
//
// ---- COUNT CELLS, NOT COLUMNS ---------------------------------------------
//
// Plan section 2 is emphatic and this pass obeys it: the inner test counts a
// CELL at the swept level, so a cave, an overhang, a ledge or a flooded tunnel
// under the lake is counted once each. Counting columns would silently
// reimplement the single-span-per-column assumption that got heightfields
// rejected in RESEARCH_water_architecture.md section 4.1.1 — and it would do it
// invisibly, because a flat-floored test basin gives the same answer either
// way.
// ============================================================================
@compute @workgroup_size(16, 1, 16)
fn wbSweep(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_id) li : vec3<u32>) {
  if (wg.x >= T.waterChunkCount) { return; }
  let e = wbChunkEntry(wg.x);
  let b = e >> 16u;
  let slot = e & 0xFFFFu;
  // ONE BODY PER TICK. The schedule is the CPU's only say in this, and it is
  // `slot % kWaterSweepPeriod == tick % kWaterSweepPeriod` — see
  // WaterBodySystem::BuildGpu. Every other body returns here.
  if (b != T.waterSweepSlot || b >= T.waterBodyCount) { return; }
  let g = wbGeom(b);
  let seed = wbSeed(b);
  // Only the PARENT sweeps. A split child shares its parent's disc and reads
  // its parent's map; sweeping twice would double every area count.
  if ((seed.w & WBF_CHILD) != 0) { return; }

  let floorY = seed.x;
  // THE LEDGER RESOLVED THIS, and this pass does not second-guess it. See the
  // note over WBS_SWEEPY in the ledger: the CPU may ask for "the live level",
  // only the GPU knows what that is, and the shave may have moved it between
  // the ledger's clear and this accumulate.
  let y = wbGet(b, WBS_SWEEPY);
  let idx = y - floorY - 1;
  if (idx < 0 || idx >= i32(WATER_CURVE_MAXY)) { return; }
  let radius = i32(wbIsqrt(u32(max(g.z, 0))));

  let wc = wbSlotWorldChunk(slot);
  // The swept level has to be inside THIS chunk or the chunk has nothing to say
  // about it. That is what makes the pass O(footprint area) rather than
  // O(footprint volume): a body's whole column of chunks is listed and all but
  // one Y layer of them return after three scalar loads — the same shape the
  // shave's two-Y band has.
  let cyLo = wc.y * i32(CHUNK);
  if (y < cyLo || y >= cyLo + i32(CHUNK)) { return; }

  let x = wc.x * i32(CHUNK) + i32(li.x);
  let z = wc.z * i32(CHUNK) + i32(li.z);
  let dx = x - g.x;
  let dz = z - g.y;
  let d2 = dx * dx + dz * dz;

  // A CONTAINER CELL is one that could hold this body's water: air, or the
  // body's own liquid. NOT "any non-solid" and not "water" — air is what a dug
  // basin is full of before it fills and the body's liquid is what it holds
  // after, and the curve has to give the same answer either way or a drain
  // would re-pace itself as it descends.
  let w = voxWordAt(vec3<i32>(x, y, z));
  let m = i32(voxMat(w));
  let container = m == i32(MAT_AIR) || m == g.w;

  if (d2 <= g.z) {
    if (container) {
      atomicAdd(&waterBodyState[wbCurveBase(b) + SW_AREA0 + u32(idx)], 1);
      // OUTPUT 4's input: the openness bitmap the label propagation reads. One
      // bit per grid cell, set if ANY column in it is open — the LIBERAL
      // direction, which can only ever UNDER-split. See world.h's
      // kWaterSplitGrid note on why over-splitting is the unsafe one.
      let gi = wbGridIndex(x, z, g.x, g.y, radius);
      if (gi < WATER_SPLIT_CELLS) {
        atomicOr(&waterBodyState[WATER_SCRATCH_BASE + (gi >> 5u)],
                 i32(1u << (gi & 31u)));
      }
    }
  } else if (d2 <= g.z + 2 * radius + 1) {
    // OUTPUT 2, THE SPILL ELEVATION: the ring one cell OUTSIDE the disc. Water
    // leaves a basin over its rim and the rim is not in the basin — probing
    // inside the disc would find the pool floor and report that the lake spills
    // at its own bottom. For an intact tarn or authored pool this ring is berm
    // or rim lift, solid through the whole scanned span, so SW_SPILLY stays at
    // WB_HOLE_NONE and says "this basin does not leak". Notch the rim and it
    // reports the notch, which is component 5's first enter test made real for
    // terrain the player shaped.
    if (container) {
      atomicMin(&waterBodyState[wbCurveBase(b) + SW_SPILLYN], y);
    }
  }
}

// The label-propagation scratch. 2,304 u32 = 9,216 B, comfortably inside the
// 16 KiB workgroup-storage floor. A second array for double buffering would
// have doubled that and gone over; the read-phase / barrier / write-phase split
// below buys the same race freedom for nothing, because a thread only ever
// WRITES cells it owns.
var<workgroup> gLabel : array<u32, 2304>;
var<workgroup> gRoots : array<u32, 8>;
var<workgroup> gSize : array<u32, 8>;
var<workgroup> gPick : array<u32, 4>;
var<workgroup> gRootCount : atomic<u32>;
var<workgroup> gChanged : atomic<u32>;
var<workgroup> gMapDirty : atomic<u32>;

// ============================================================================
// M5 — THE SPLIT (plan component 2, outputs 3 and 4; component 10).
// ONE workgroup, 256 threads, nine grid cells each.
//
// ---- WHY A SINGLE WORKGROUP, AND WHY THAT IS THE POINT --------------------
//
// This is connected-component labelling, which classically wants union-find
// with path compression — and path compression wants atomicCAS, which
// CLAUDE.md rule 1 bans outright because a CAS loop's outcome depends on which
// thread arrived first. Min-label propagation reaches the same answer with no
// CAS at all (integer min is associative and commutative, so the fixpoint is
// unique), but naive propagation across a whole dispatch races on the
// neighbour read.
//
// Inside ONE workgroup that race has a standard deterministic answer: read
// phase into registers, `workgroupBarrier()`, write phase into cells this
// thread alone owns. The fixpoint is then a pure function of the input bitmap.
// It costs one under-occupied workgroup, which is the right trade for a pass
// that runs once per scheduled tick on one basin.
//
// ---- THE MERGE TREE, WHICH IS WHY THE PLAN WANTED A SWEEP AT ALL ----------
//
// Plan section 2: "every height where two components MERGE going up is a height
// where one basin SPLITS going down. This is the merge tree of the terrain."
// The sweep visits every level of the basin in turn, so the merge tree falls
// out as the set of levels whose component count exceeds one — and the SPLIT
// ELEVATION is the highest of them (atomicMax, order-free). That is output 3.
// Output 4 is the map itself, which is what makes a split an exact partition of
// the parent's cells rather than a search for where to cut.
//
// ---- WHAT A SPLIT DOES, AND WHY NO NEW ARITHMETIC IS INVOLVED -------------
//
// Nothing here divides anybody's water. The map changes which cells each body
// OWNS; the existing ladder does the rest. The child's adoption reduce measures
// its own voxels, the parent's re-audit re-measures what is left, and both are
// voxel sums — so held(parent) + held(child) equals the parent's pre-split
// voxel content BY MEASUREMENT, not by a division that could round. That is
// plan section 5's "both directions must be mass-exact" reused rather than
// re-derived, and it is what `--gate waterbody` pass B asserts.
// ============================================================================
@compute @workgroup_size(256)fn wbSplit(@builtin(local_invocation_id) li : vec3<u32>) {
  // ---- THE UNIFORMITY CONTRACT, and it shapes this whole function ---------
  //
  // `workgroupBarrier` may only be called from UNIFORM control flow, and WGSL's
  // analysis treats anything loaded from a storage buffer as possibly
  // non-uniform. This pass has to read two such values — the resolved sweep
  // level the ledger published (WBS_SWEEPY) and the body's live level
  // (WBS_LEVEL) — so neither may gate a `return` or an `if` that contains a
  // barrier. Every early-out below is therefore either UNIFORM (derived only
  // from the uniform buffer) or a FLAG that guards work rather than control
  // flow. The barriers all sit at function top level, so every invocation
  // executes exactly the same sequence of them.
  //
  // The first version returned on the level test and Tint rejected it in as
  // many words: "control flow depends on possibly non-uniform value". Writing
  // it the other way costs a few hundred wasted workgroup ops on the rare tick
  // where the level is out of range, and buys a kernel that provably cannot
  // deadlock.
  let b = T.waterSweepSlot;
  if (b >= T.waterBodyCount || b >= WATERBODY_CAP) { return; }   // uniform
  let seed = wbSeed(b);
  if ((seed.w & WBF_CHILD) != 0) { return; }                     // uniform

  // Same published level wbSweep used, for the same reason: the openness bitmap
  // this pass labels was written at THAT level and at no other.
  let y = wbGet(b, WBS_SWEEPY);
  let idx = y - seed.x - 1;
  // `inRange` and not `active`: `active` is a RESERVED KEYWORD in WGSL, the
  // same trap `target` already is (CLAUDE.md's build-gotchas list).
  let inRange = idx >= 0 && idx < i32(WATER_CURVE_MAXY);

  let t = li.x;
  let per = WATER_SPLIT_CELLS / 256u;   // 9

  // ---- seed: open cells label themselves, blocked cells take the sentinel --
  for (var k = 0u; k < per; k++) {
    let i = t * per + k;
    let bit = (u32(atomicLoad(&waterBodyState[WATER_SCRATCH_BASE + (i >> 5u)]))
               >> (i & 31u)) & 1u;
    gLabel[i] = select(0xFFFFFFFFu, i, bit != 0u && inRange);
  }
  if (t == 0u) { atomicStore(&gRootCount, 0u); atomicStore(&gMapDirty, 0u); }
  workgroupBarrier();

  // ---- propagate to a fixpoint --------------------------------------------
  // 96 iterations, ALWAYS, with no early break. The bound is rule 2 (bound
  // every emergent process) and it is comfortably past a 48-wide grid's
  // straight-line diameter; running it to the bound rather than breaking on a
  // workgroup flag is the same uniformity argument as above. `converged` ends
  // up meaning "the last iteration changed nothing", which is the property the
  // split decision actually needs — a basin shaped like a spiral that has not
  // settled by 96 REFUSES to split, which is the safe direction, because an
  // unsplit basin is one the CA already knows how to handle.
  for (var iter = 0; iter < 96; iter++) {
    if (t == 0u) { atomicStore(&gChanged, 0u); }
    workgroupBarrier();
    // READ PHASE. Every load below is of a cell some other thread may write in
    // the write phase, which is why the barrier between them is not optional.
    var want = array<u32, 9>(0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u);
    for (var k = 0u; k < per; k++) {
      let i = t * per + k;
      var mn = gLabel[i];
      if (mn != 0xFFFFFFFFu) {
        let gx = i % WATER_SPLIT_GRID;
        let gz = i / WATER_SPLIT_GRID;
        if (gx > 0u) { mn = min(mn, gLabel[i - 1u]); }
        if (gx + 1u < WATER_SPLIT_GRID) { mn = min(mn, gLabel[i + 1u]); }
        if (gz > 0u) { mn = min(mn, gLabel[i - WATER_SPLIT_GRID]); }
        if (gz + 1u < WATER_SPLIT_GRID) { mn = min(mn, gLabel[i + WATER_SPLIT_GRID]); }
      }
      want[k] = mn;
    }
    workgroupBarrier();
    // WRITE PHASE. Only cells this thread owns, so no two invocations can
    // disagree about a cell and the fixpoint is a pure function of the bitmap.
    for (var k = 0u; k < per; k++) {
      let i = t * per + k;
      if (want[k] != gLabel[i]) {
        gLabel[i] = want[k];
        atomicAdd(&gChanged, 1u);
      }
    }
    workgroupBarrier();
  }
  let converged = atomicLoad(&gChanged) == 0u;

  // ---- roots, in ascending index order ------------------------------------
  // A cell is a ROOT when it labelled itself, which after the fixpoint means it
  // is the smallest index in its component. Collected by ONE thread in a single
  // ascending scan, so the ranking is a property of the grid rather than of
  // which invocation got there first — the same reason the ledger's hole key is
  // an atomicMin over a packed position rather than a first-writer-wins.
  //
  // SIZED, and then the small ones DROPPED, which the first version did not do
  // and paid for. Ranking the first four roots by grid index alone made
  // component 0 whatever component happened to touch the lowest-numbered grid
  // cell — and at the top of a disc that is a SPECK: two or three open cells
  // where the circle's edge clips a grid cell. The parent inherited the speck,
  // the child inherited the entire lake, and the parent was left holding a
  // debit against a pool with no water in it. So: collect up to eight roots,
  // count what each actually owns, keep the ones over a floor, and take the
  // three largest. Ties break by grid index, and the kept roots are numbered in
  // ASCENDING index order, so the whole assignment is a pure function of the
  // bitmap.
  if (t == 0u) {
    for (var r = 0u; r < 8u; r++) { gRoots[r] = 0xFFFFFFFFu; gSize[r] = 0u; }
    var n = 0u;
    for (var i = 0u; i < WATER_SPLIT_CELLS; i++) {
      if (gLabel[i] == i) {
        if (n < 8u) { gRoots[n] = i; }
        n = n + 1u;
      }
    }
    for (var i = 0u; i < WATER_SPLIT_CELLS; i++) {
      let lab = gLabel[i];
      if (lab == 0xFFFFFFFFu) { continue; }
      for (var r = 0u; r < 8u; r++) {
        if (gRoots[r] == lab) { gSize[r] = gSize[r] + 1u; }
      }
    }
    // A component under WB_SPLIT_MIN_CELLS grid cells is not a pool, it is an
    // artefact of the downsample. Folding it back into the parent is the same
    // safe degradation every refusal here takes — the CA handles a puddle.
    for (var q = 0u; q < 4u; q++) { gPick[q] = 0xFFFFFFFFu; }
    var kept = 0u;
    for (var q = 0u; q < 3u; q++) {
      var best = 0xFFFFFFFFu;
      var bestSize = 0u;
      for (var r = 0u; r < 8u; r++) {
        if (gRoots[r] == 0xFFFFFFFFu) { continue; }
        if (gSize[r] < WB_SPLIT_MIN_CELLS) { continue; }
        var taken = false;
        for (var q2 = 0u; q2 < q; q2++) {
          if (gPick[q2] == gRoots[r]) { taken = true; }
        }
        if (taken) { continue; }
        if (gSize[r] > bestSize) { bestSize = gSize[r]; best = gRoots[r]; }
      }
      if (best != 0xFFFFFFFFu) { gPick[q] = best; kept = kept + 1u; }
    }
    // Number the kept roots by ascending grid index, so "component 0" is a
    // property of the terrain and not of which one happened to be largest this
    // tick — a component that swapped index with its sibling would swap the two
    // pools' ledgers.
    for (var a = 0u; a + 1u < 3u; a++) {
      for (var bq = 0u; bq + 1u < 3u - a; bq++) {
        if (gPick[bq] > gPick[bq + 1u]) {
          let tmp = gPick[bq];
          gPick[bq] = gPick[bq + 1u];
          gPick[bq + 1u] = tmp;
        }
      }
    }
    atomicStore(&gRootCount, kept);
  }
  workgroupBarrier();
  let roots = atomicLoad(&gRootCount);

  // OUTPUT 3: THE SPLIT ELEVATION. atomicMax over every level this cycle has
  // found disconnected — the merge tree read downward, which is plan section
  // 2's fourth output and the reason it wanted a height-ordered sweep rather
  // than a flood fill per level. Reset to WB_SPLIT_NONE at the cycle's first
  // level by the ledger, so a partition the player knocks down stops being
  // reported within one cycle instead of forever.
  if (t == 0u && inRange && converged && roots > 1u) {
    atomicMax(&waterBodyState[wbCurveBase(b) + SW_SPLITYN], y);
  }

  // ---- OUTPUT 4: THE ACTIVE MAP -------------------------------------------
  // Written ONLY when the swept level is the body's LIVE level, because that is
  // the only level a footprint test ever asks about. A map from some other
  // level would tell the shave to split a surface nowhere near the partition.
  // A FLAG rather than a return, per the uniformity contract at the top.
  let writeMap = inRange && converged && y == wbGet(b, WBS_LEVEL);
  // More components than the 2-bit map can name: publish ONE, i.e. refuse to
  // split. Safe degradation, the same answer every refusal in this subsystem
  // gives — "simulated the way it is today".
  var comps = 1u;
  if (roots >= 1u && roots <= 3u) { comps = roots; }

  // 144 words, one thread each. No atomics: a word is 16 grid cells and one
  // thread owns all 16 of them.
  if (t < WATER_SPLIT_WORDS && writeMap) {
    var packed = 0u;
    for (var k = 0u; k < 16u; k++) {
      let i = t * 16u + k;
      var comp = WB_COMP_NONE;
      let lab = gLabel[i];
      if (lab != 0xFFFFFFFFu) {
        if (comps > 1u) {
          for (var r = 0u; r < 3u; r++) {
            if (gPick[r] == lab) { comp = r; }
          }
          // A dropped speck, or a component past the third, keeps
          // WB_COMP_NONE, which wbOwns hands back to the parent — so it is
          // simulated the way it is today, which is the same answer as not
          // splitting at all.
        } else {
          comp = 0u;
        }
      }
      packed = packed | (comp << (k * 2u));
    }
    let addr = wbCurveBase(b) + SW_SPLIT0 + t;
    if (u32(atomicLoad(&waterBodyState[addr])) != packed) {
      atomicStore(&waterBodyState[addr], i32(packed));
      atomicAdd(&gMapDirty, 1u);
    }
  }
  workgroupBarrier();
  if (t == 0u && writeMap) {
    swSet(b, SW_COMPS, i32(comps));
    swSet(b, SW_MAPY, y);
    if (atomicLoad(&gMapDirty) != 0u) {
      swSet(b, SW_MAPGEN, swGet(b, SW_MAPGEN) + 1);
    }
  }
}
