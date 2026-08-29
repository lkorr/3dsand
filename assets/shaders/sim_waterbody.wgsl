// sim_waterbody.wgsl — THE DRAIN LEDGER (component 3) and THE SURFACE SHAVE
// (component 4) of docs/PLAN_water_master.md, plus the per-body adoption reduce
// component 1 deferred. Milestone M2.
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
// ---- THE FOUR PASSES, AND WHY THAT ORDER -----------------------------------
//
//   wbQuiet   one thread per listed chunk. Was this chunk disturbed this tick?
//   wbLedger  one thread per body. The whole state machine and all arithmetic.
//   wbReduce  one workgroup per listed chunk. Sums a candidate's voxel eighths.
//   wbShave   one workgroup per listed chunk. Takes eighths off the free
//             surface and REPORTS what it took.
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

// ---- the GPU-owned ledger, one record per body -----------------------------
// Must match kWaterBodyStateWords in src/sim/world.h and the WBS_* reader in
// src/test/selftest_water.cpp. Sixteen words is four more than the state needs,
// and that is deliberate: plan §7 asks for attribution words BEFORE they are
// needed, because "conservation failed by 37" with nothing attached is the bare
// count CLAUDE.md rule 6 says costs a dozen elimination runs to un-ask.
const WBS_STATE     : u32 = 0u;   // WB_* below
const WBS_LEVEL     : u32 = 1u;   // world Y of the free surface
const WBS_AREA      : u32 = 2u;   // surface cells the ledger paces against
const WBS_DEBIT     : u32 = 3u;   // eighths owed but not yet off the voxels
const WBS_SHAVED    : u32 = 4u;   // what LAST tick's shave actually removed
const WBS_SEEN      : u32 = 5u;   // free-surface cells that shave saw
const WBS_ATLEVEL   : u32 = 6u;   // of those, how many sat at exactly LEVEL
const WBS_STEPS     : u32 = 7u;   // published: whole eighths per surface cell
const WBS_FRAC      : u32 = 8u;   // published: dither numerator, in [0, area)
const WBS_DRAINED   : u32 = 9u;   // cumulative eighths that left forever
const WBS_VOLUME    : u32 = 10u;  // the reduce's voxel-eighth sum at adoption
const WBS_QUIET     : u32 = 11u;  // consecutive undisturbed ticks
const WBS_RSUM      : u32 = 12u;  // reduce scratch: running eighth sum
const WBS_RDIRTY    : u32 = 13u;  // quiescence scratch: disturbed chunks
const WBS_CAPPED    : u32 = 14u;  // attribution: eighths the cells did not have
const WBS_ADOPTTICK : u32 = 15u;  // attribution: the tick adoption happened on

// The ladder, GPU side. Candidate -> Measuring -> Adopted, and Releasing is the
// way out. Measuring is its own state rather than a flag because the reduce is
// a WHOLE-FOOTPRINT pass and must run exactly once per adoption: a body that
// re-measured every tick would be the O(volume)-per-tick cost this design
// exists to delete.
const WB_CANDIDATE : i32 = 0;
const WB_MEASURING : i32 = 1;
const WB_ADOPTED   : i32 = 2;
const WB_RELEASING : i32 = 3;

// CPU-sent per-body flags (TickParams.waterBodies row 1, word 3).
const WBF_PROPOSE : i32 = 1;   // the CPU's deterministic tests all passed
const WBF_RELEASE : i32 = 2;   // an EXIT test failed; hand this body back

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
