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
  let steps = debit / area;
  let frac = debit - steps * area;
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
  if (wbGet(b, WBS_STATE) != WB_MEASURING) { return; }

  let g = wbGeom(b);
  let seed = wbSeed(b);
  let wc = wbSlotWorldChunk(slot);
  let x = wc.x * i32(CHUNK) + i32(li.x);
  let z = wc.z * i32(CHUNK) + i32(li.z);
  // i32 is ample: kWindPrimMaxExtent-scale radii square to ~2.6e5 and the
  // window is 512 cells across, so the largest legal dx*dx + dz*dz is ~5.2e5.
  let dx = x - g.x;
  let dz = z - g.y;
  if (dx * dx + dz * dz > g.z) { return; }

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
  if (top > -0x40000000) {
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
  // i32 is ample: kWindPrimMaxExtent-scale radii square to ~2.6e5 and the
  // window is 512 cells across, so the largest legal dx*dx + dz*dz is ~5.2e5.
  let dx = x - g.x;
  let dz = z - g.y;
  if (dx * dx + dz * dz > g.z) { return; }

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
  if (dx * dx + dz * dz > g.z) { return; }

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
