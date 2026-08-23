// sim_fluid_seam.wgsl — the excite/settle seam between settled voxel liquid
// and excited MLS-MPM particles (docs/PLAN_mpm_fluids.md §7, Phase 2).
//
// Settled liquid is fullness voxels; excited liquid is particles. This module
// is the ONLY code that converts between them, in both directions, and it is
// the only fluid code that writes voxels. Everything here is bit-deterministic
// by the CA's own discipline:
//   * integer-only math, no f32 anywhere;
//   * every accumulation is an integer atomicAdd/atomicMax (associative /
//     idempotent-commutative — scheduling order cannot matter);
//   * every INDEX assignment (compaction slots, excite emission offsets,
//     settle list positions) comes from a slot-order or particle-order scan,
//     never from a first-come atomicAdd;
//   * all randomness is hash3(seed, tick, cellIndex) counter RNG.
//
// THE TICK (recorded from EncodeTick around the solver substeps):
//   PT_FLUID_SEAM (before the substeps):
//     fill scratch -> compactCount/Scan/Scatter (ping-pong, removes corpses)
//     -> spawnAppend (CPU ops) -> exciteDetect (dirty chunks: mark candidate
//     cells in voxel bits 19..23) -> exciteScan (slot-order budget + offsets)
//     -> exciteEmit (particles born, voxels cleared, marks consumed)
//   PT_FLUID x kFluidSubsteps (sim_fluid.wgsl, unchanged shape)
//   PT_FLUID_SETTLE (after the substeps):
//     particleTick (per-slot speed maxima) -> settleJudge (calm counters)
//     -> settleScan (pick <= kFluidSettleMax non-adjacent calm blocks)
//     -> settleBin (per-cell eighths + stain) -> settleCheck (column
//     feasibility, all-or-nothing per block) -> settleCommit (voxels written)
//     -> settleKill (particles die; next tick's compaction removes them)
//
// MASS IS EXACT INTEGER ACCOUNTING. Excite emits exactly `fullness` particles
// per cell (1 eighth each) and clears the cell; settle sums the eighths back
// into columns and refuses a whole block rather than drop or invent a
// remainder. The fluid-settle/fluid-excite gates assert eighths-in ==
// eighths-out end to end.
//
// MATERIALIZATION CONTRACT (PLAN_page_table.md §3). No kernel here tests the
// page table before writing — that state is readback-timing-dependent and a
// behaviour branch on it would be nondeterministic. Instead every chunk this
// module can write is guaranteed a page the same way the CA's targets are:
// excite writes only into this tick's dirty chunks (§3 covers those; the
// cascade advances 1 chunk/tick, exactly the speed cpuDirty propagates), and
// settle writes only into >= 8-tick-calm fluid blocks, which the block-list
// readback has long since fed to PageTable::UpdateFluidChunks. voxStore's
// pageFaults counter is the tripwire on this argument, asserted zero by every
// gate and smoke probe.

@group(0) @binding(0) var<storage, read_write> voxels : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;

// The ping-pong pair: src is LAST tick's particles (read only — the tick's
// working buffer is dst, which every later pass and the solver substeps use).
@group(1) @binding(0) var<storage, read> fluidSrc : array<FluidParticle>;
@group(1) @binding(1) var<storage, read_write> fluidParticles : array<FluidParticle>;
@group(1) @binding(2) var<storage, read> fluidSpawnOps : array<FluidSpawnOp>;
// Solver state from the LAST substep of the PREVIOUS tick: the block map and
// node grid. Read-only here — the wake trigger samples node mass/velocity at
// the settled/active interface, and exciteEmit seeds wake velocities from it.
@group(1) @binding(3) var<storage, read> fluidBlockMapR : array<u32>;
@group(1) @binding(4) var<storage, read> fluidGridR : array<i32>;
// The FA_* word map (common.wgsl). Counters are atomics; the scans store the
// authoritative live count and dispatch args.
@group(1) @binding(5) var<storage, read_write> fluidArgs : array<atomic<u32>>;
// This tick's compacted dirty-chunk list (sim_compact) — exciteDetect's
// domain. read_write to match the shared layout entry; this shader only reads.
@group(1) @binding(6) var<storage, read_write> dirtyList : array<u32>;
// Excite scratch: [0..15] header, then per-slot candidate-particle counts,
// per-slot emission bases (EX_REFUSED = budget refusal), then the candidate
// slot list in slot order. Fill-cleared at the top of every seam tick.
@group(1) @binding(7) var<storage, read_write> exciteScratch : array<atomic<u32>>;
// Per-slot consecutive-calm-tick counters. PERSISTENT (not fill-cleared);
// zeroed by worldgen/reset and whenever a slot has no particles.
@group(1) @binding(8) var<storage, read_write> fluidCalm : array<atomic<u32>>;
// Settle scratch: per-slot speed maxima, per-slot marks, the settle list
// header, then kFluidSettleMax blocks of per-cell (eighths, mat|stain) bins.
// Fill-cleared at the top of every seam tick.
@group(1) @binding(9) var<storage, read_write> settleScratch : array<atomic<u32>>;
// Compaction spans: [0..SPANS) survivor count per 256-particle span,
// [SPANS..2*SPANS) exclusive bases. Rewritten every tick before use.
@group(1) @binding(10) var<storage, read_write> compactScratch : array<u32>;
// Per active-block cell (2 words, world.h layout): [0] the intent word the
// seam writes for the CA — the cell's fluid material + carried stain — and
// [1] the flags the CA writes back (bit0: a reaction consumed this cell's
// excited fluid). Fill-cleared at the head of the settle phase, so the CA
// always reads LAST tick's intents (one tick latent, deterministic).
@group(1) @binding(11) var<storage, read_write> fluidCellScratch : array<atomic<u32>>;
// blockIdx -> chunk slot, from the last substep's alloc. read_write to match
// the solver's shared entry; this shader only reads.
@group(1) @binding(12) var<storage, read_write> fluidBlockList : array<u32>;

// ---- layout constants -------------------------------------------------------
const SPANS : u32 = FLUID_CAP / 256u;         // compaction spans
const EX_LIST_COUNT : u32 = 0u;   // candidate slots (accepted AND refused)
const EX_EMITTED : u32 = 1u;      // accepted particle total this tick
const EX_COMPACT_LIVE : u32 = 2u; // survivors after compaction (spawn base)
const EX_ARGS : u32 = 4u;         // [4..6] emit dispatch args (listCount,1,1)
const EX_COUNTS : u32 = 16u;                       // + slot
const EX_BASES : u32 = 16u + NUM_CHUNKS;           // + slot
const EX_LIST : u32 = 16u + 2u * NUM_CHUNKS;       // + list index
const EX_REFUSED : u32 = 0xFFFFFFFFu;
// Settle scratch layout.
const SP_SPEED : u32 = 0u;                         // + slot
const SP_MARK : u32 = NUM_CHUNKS;                  // + slot
const SP_COUNT : u32 = 2u * NUM_CHUNKS;            // settle list count
const SP_LIST : u32 = 2u * NUM_CHUNKS + 1u;        // + list index (16)
const SP_BINS : u32 = 2u * NUM_CHUNKS + 18u;       // + (listIdx*CHUNK_VOL+cell)*2
const SETTLE_MAX : u32 = 16u;                      // kFluidSettleMax
const MARK_SETTLING : u32 = 0x80000000u;
const MARK_REFUSED : u32 = 0x40000000u;
const MARK_LIST_MASK : u32 = 0x1Fu;

// ---- tuning -> per-tick fixed point (same const-eval discipline as the
// solver: WGSL const-expressions fold IEEE-exactly, the kernel stays integer).
// Settle/wake thresholds compare on the splash test's (v >> 8) scale so the
// tuner's vox/s reads identically across all three.
const SEAM_SETTLE8 : i32 =
    i32(round(TUNE_FLUID_SETTLE_EPS * 65536.0 / 30.0)) >> 8u;
const SEAM_SETTLE2 : i32 = SEAM_SETTLE8 * SEAM_SETTLE8;
const SEAM_WAKE8 : i32 =
    i32(round(TUNE_FLUID_WAKE_SPEED * 65536.0 / 30.0)) >> 8u;
const SEAM_WAKE2 : i32 = SEAM_WAKE8 * SEAM_WAKE8;
const SEAM_CALM_TICKS : u32 = u32(clamp(TUNE_FLUID_SETTLE_TICKS, 8, 600));
// Hydrostatic compression per cell of depth, Q16: how much smaller than 1 the
// seeded J gets per submerged cell. g/K with the /900 human-unit conversions
// cancelling; clamped so even a pathological tuning cannot invert J.
const SEAM_HYDRO : i32 = clamp(
    i32(round(TUNE_FLUID_GRAVITY * TUNE_FLUID_REST_DENSITY /
              max(TUNE_FLUID_STIFFNESS, 1.0) * 65536.0)),
    0, 8192);
// The deepest J the 4-bit depth field can express — matches FLUID_JMIN's band.
const SEAM_JFLOOR : i32 = 45875;   // 0.70 in Q16

// Same node addressing as the solver (kept in step by hand — the two modules
// read the same buffers).
fn seamNodeBase(bm : u32, nc : vec3<i32>) -> u32 {
  let lo = vec3<u32>(nc & vec3<i32>(CHUNK_MASK));
  return ((bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x) * FLUID_GW;
}

// Next-tick dirty mark incl. boundary neighbors (the seam runs post-CA, so
// its writes are next tick's business — the sim_particle convention).
fn markDirtyNext(c : vec3<i32>) {
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

// A particle is live if its attr names a material and carries mass.
fn seamLive(p : FluidParticle, idx : u32, bound : u32) -> bool {
  return idx < bound && fpAlive(p.attr);
}

// ============================================================================
// COMPACTION — ping-pong src -> dst, slot order preserved. Three passes:
// count survivors per 256-span, single-workgroup scan of the span counts,
// scatter. Slot assignment is a pure function of particle state and slot
// order — rule 1's "deletion/compaction order likewise" requirement.
// ============================================================================

var<workgroup> wgScan : array<u32, 256>;

@compute @workgroup_size(256)
fn compactCount(@builtin(workgroup_id) wg : vec3<u32>,
                @builtin(local_invocation_index) li : u32) {
  let bound = min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP);
  let idx = wg.x * 256u + li;
  var live = 0u;
  if (idx < bound && fpAlive(fluidSrc[idx].attr)) { live = 1u; }
  wgScan[li] = live;
  workgroupBarrier();
  if (li == 0u) {
    var n = 0u;
    for (var t = 0u; t < 256u; t++) { n += wgScan[t]; }
    compactScratch[wg.x] = n;
  }
}

@compute @workgroup_size(256)
fn compactScan(@builtin(local_invocation_index) li : u32) {
  // 256 threads x (SPANS/256) spans each; thread 0 turns the partials into
  // exclusive bases. Also clears the per-tick FA event counters — this is the
  // first seam pass of the tick, so last tick's values have already ridden
  // the snapshot readback out.
  let per = SPANS / 256u;
  var n = 0u;
  for (var s = li * per; s < (li + 1u) * per; s++) { n += compactScratch[s]; }
  wgScan[li] = n;
  workgroupBarrier();
  if (li == 0u) {
    var sum = 0u;
    for (var t = 0u; t < 256u; t++) {
      let c = wgScan[t];
      wgScan[t] = sum;
      sum += c;
    }
    atomicStore(&exciteScratch[EX_COMPACT_LIVE], sum);
    atomicStore(&fluidArgs[FA_DEAD], 0u);
    atomicStore(&fluidArgs[FA_SETTLED], 0u);
    atomicStore(&fluidArgs[FA_EXCITED], 0u);
    atomicStore(&fluidArgs[FA_REFUSED], 0u);
    atomicStore(&fluidArgs[FA_SETBLOCKS], 0u);
    atomicStore(&fluidArgs[FA_EMITTED], 0u);
    atomicStore(&fluidArgs[FA_BINNED], 0u);
    atomicStore(&fluidArgs[FA_CONSUMED], 0u);
    atomicStore(&fluidArgs[FA_STAINED], 0u);
  }
  workgroupBarrier();
  var base = wgScan[li];
  for (var s = li * per; s < (li + 1u) * per; s++) {
    let c = compactScratch[s];
    compactScratch[SPANS + s] = base;
    base += c;
  }
}

@compute @workgroup_size(256)
fn compactScatter(@builtin(workgroup_id) wg : vec3<u32>,
                  @builtin(local_invocation_index) li : u32) {
  let bound = min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP);
  let idx = wg.x * 256u + li;
  var live = 0u;
  if (idx < bound && fpAlive(fluidSrc[idx].attr)) { live = 1u; }
  wgScan[li] = live;
  workgroupBarrier();
  // Exclusive prefix within the workgroup (serial by thread 0 — 256 adds).
  if (li == 0u) {
    var sum = 0u;
    for (var t = 0u; t < 256u; t++) {
      let c = wgScan[t];
      wgScan[t] = sum;
      sum += c;
    }
  }
  workgroupBarrier();
  if (live == 1u) {
    fluidParticles[compactScratch[SPANS + wg.x] + wgScan[li]] = fluidSrc[idx];
  }
}

// ---- spawn: CPU op stream appended after the survivors ----------------------
@compute @workgroup_size(64)
fn spawnAppend(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.fluidSpawnCount) { return; }
  let slot = atomicLoad(&exciteScratch[EX_COMPACT_LIVE]) + gid.x;
  if (slot >= FLUID_CAP) { return; }  // CPU charges the budget; belt+braces
  let op = fluidSpawnOps[gid.x];
  var p : FluidParticle;
  p.px = op.px; p.py = op.py; p.pz = op.pz;
  p.vx = clamp(op.vx, -176947, 176947);
  p.vy = clamp(op.vy, -176947, 176947);
  p.vz = clamp(op.vz, -176947, 176947);
  p.c00 = 0; p.c01 = 0; p.c02 = 0;
  p.c10 = 0; p.c11 = 0; p.c12 = 0;
  p.c20 = 0; p.c21 = 0; p.c22 = 0;
  p.j = FLUID_ONE;
  p.species = op.species & 3u;
  p.density = 0;
  p.attr = fpPack(op.mat, 1u, 0u, 0u);
  p.birthTick = T.tick;
  p._r0 = 0; p._r1 = 0; p._r2 = 0; p._r3 = 0;
  p._r4 = 0; p._r5 = 0; p._r6 = 0; p._r7 = 0;
  p._r8 = 0; p._r9 = 0; p._r10 = 0; p._r11 = 0;
  fluidParticles[slot] = p;
}

// ============================================================================
// EXCITE — settled cells -> particles.
// ============================================================================

// Is this material a seam-eligible liquid? Non-viscous CLASS_LIQUID only:
// lava/blood keep their authored CA movement (moveEvery > 1) until the fluid
// gains per-material dynamics (plan Phase 7).
fn seamLiquid(mat : u32) -> bool {
  if (mat == MAT_AIR) { return false; }
  let m = materials[mat];
  return m.klass == CLASS_LIQUID && m.moveEvery <= 1u;
}

// detect: one workgroup per dirty chunk (the CA's own indirect args), 16
// cells per thread. A candidate cell gets its mark + depth written into its
// OWN voxel word (bits 19..23 — the sanctioned scratch span) and its
// fullness-count added to its slot's counter.
//
// Concurrency note: the depth scan and the below/wake probes read OTHER
// cells' words while their owning threads may be OR-ing scratch bits in.
// That interleaving cannot change any result: readers here consume only
// bits 0..17 (mat, state), the writes touch only 19..23, and a 32-bit
// aligned word does not tear on any device this engine targets (the same
// word-atomicity the particle system's landing writes already lean on). The
// OUTCOME is therefore a pure function of pre-seam state — rule 1 holds.
@compute @workgroup_size(256)
fn exciteDetect(@builtin(workgroup_id) wg : vec3<u32>,
                @builtin(local_invocation_index) li : u32) {
  let ci = dirtyList[wg.x];
  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let wc = slotToWorldChunk(sc, T.origin);
  let base = wc * i32(CHUNK);
  for (var s = 0u; s < 16u; s++) {
    let localIdx = li * 16u + s;
    let lo = vec3<i32>(i32(localIdx & 15u), i32((localIdx >> 4u) & 15u),
                       i32(localIdx >> 8u));
    let c = base + lo;
    let idx = voxWordIndex(c);
    if (idx == PT_NO_WORD) { continue; }  // sentinel chunk: nothing to excite
                                          // in a chunk with no page — a
                                          // uniform interior has no exposed
                                          // face by construction
    let w = voxels[idx];
    let mat = voxMat(w);
    if (!seamLiquid(mat)) { continue; }

    // Trigger (a), gated by the tick input stream: the cell would FALL — air
    // below. This is the disturbance trigger (carve, explosion, mutation);
    // while the CA still owns liquid movement it stays off by default.
    var excite = false;
    var byFall = false;
    if (T.fluidExciteEnable != 0u) {
      let below = c + vec3<i32>(0, -1, 0);
      if (inWindow(below, T.origin) && voxMat(voxWordAt(below)) == MAT_AIR) {
        excite = true;
        byFall = true;
      }
    }
    // Trigger (b), always on: progressive wake. A face neighbor's grid node
    // (last substep of last tick) carries real mass moving above the wake
    // threshold — the disturbance in the active region has reached this
    // settled cell. Fires only where MPM already exists, so a world that
    // never spawned fluid never runs this branch's body.
    if (!excite) {
      for (var f = 0u; f < 6u; f++) {
        var d = vec3<i32>(0, 0, 0);
        if (f == 0u) { d.x = 1; } else if (f == 1u) { d.x = -1; }
        else if (f == 2u) { d.y = 1; } else if (f == 3u) { d.y = -1; }
        else if (f == 4u) { d.z = 1; } else { d.z = -1; }
        let n = c + d;
        let nwc = worldChunkOf(n);
        if (!chunkInWindow(nwc, T.origin)) { continue; }
        let bm = fluidBlockMapR[chunkSlotIndex(nwc)];
        if (bm == 0u) { continue; }
        let nb = seamNodeBase(bm, n);
        if (fluidGridR[nb] < 16) { continue; }  // FLUID_MASS_MIN
        let vx = fluidGridR[nb + 1u] >> 8u;
        let vy = fluidGridR[nb + 2u] >> 8u;
        let vz = fluidGridR[nb + 3u] >> 8u;
        if (vx * vx + vy * vy + vz * vz >= SEAM_WAKE2) {
          excite = true;
          break;
        }
      }
    }
    if (!excite) { continue; }

    // Depth to the free surface: contiguous same-liquid cells above, capped
    // at the 4-bit field. Computed HERE, against pre-write state — emit runs
    // while neighbouring chunks are being cleared and could not scan safely.
    //
    // FALL-trigger cells only. A wake-excited cell seeds J = 1 (and rest
    // velocity, see emit): the settle/wake pair must be strictly DISSIPATIVE
    // — settle discards kinetic energy, so excite must not hand any back, or
    // a churning pool that wakes its own freshly-settled bank becomes a
    // perpetual-motion loop (measured: a sealed drained pool held ~14 vox/s
    // of churn indefinitely when wake re-seeded velocity + pre-compression;
    // the surrounding flow re-accelerates a rest-seeded particle through P2G
    // within a substep anyway).
    var depth = 0u;
    if (byFall) {
      for (var d = 1; d <= 15; d++) {
        let a = c + vec3<i32>(0, d, 0);
        if (!inWindow(a, T.origin) || voxMat(voxWordAt(a)) != mat) { break; }
        depth += 1u;
      }
    }
    voxels[idx] = (w & ~EXCITE_SCRATCH_BITS) | EXCITE_PEND_BIT |
                  (depth << EXCITE_DEPTH_SHIFT);
    atomicAdd(&exciteScratch[EX_COUNTS + ci], voxState(w) + 1u);
  }
}

// scan: slot-order budget + emission offsets, alloc's single-workgroup shape.
// Acceptance is a MONOTONE cutoff in slot order: slots are accepted until the
// first that does not fit the remaining pool, and every slot after it is
// refused this tick (their water stays settled and retries). Refused slots
// still enter the list — emit must consume their scratch bits.
var<workgroup> exPart : array<u32, 256>;    // candidate slots per span
var<workgroup> exPartP : array<u32, 256>;   // candidate particles per span
var<workgroup> exListBase : array<u32, 256>;
var<workgroup> exAccept : array<u32, 256>;  // spans wholly accepted
var<workgroup> exBaseAt : array<u32, 256>;  // particle base at span start
var<workgroup> exCrossSerial : u32;         // span containing the cutoff
var<workgroup> exBudget : u32;

@compute @workgroup_size(256)
fn exciteScan(@builtin(local_invocation_index) li : u32) {
  let span = NUM_CHUNKS / 256u;
  var slots = 0u;
  var parts = 0u;
  for (var s = li * span; s < (li + 1u) * span; s++) {
    let c = atomicLoad(&exciteScratch[EX_COUNTS + s]);
    if (c != 0u) { slots += 1u; parts += c; }
  }
  exPart[li] = slots;
  exPartP[li] = parts;
  workgroupBarrier();
  if (li == 0u) {
    let liveNow = atomicLoad(&exciteScratch[EX_COMPACT_LIVE]) +
                  min(T.fluidSpawnCount, FLUID_CAP);
    exBudget = FLUID_CAP - min(liveNow, FLUID_CAP);
    var listSum = 0u;
    var partSum = 0u;
    exCrossSerial = 256u;
    for (var t = 0u; t < 256u; t++) {
      exListBase[t] = listSum;
      exBaseAt[t] = partSum;
      listSum += exPart[t];
      // A span is wholly accepted while everything so far, plus it, fits.
      if (exCrossSerial == 256u) {
        if (partSum + exPartP[t] <= exBudget) {
          exAccept[t] = 1u;
          partSum += exPartP[t];
        } else {
          exAccept[t] = 0u;
          exCrossSerial = t;  // first span that does not wholly fit
        }
      } else {
        exAccept[t] = 0u;
      }
    }
    // Serial walk of the single crossing span: accept slot by slot until the
    // cutoff, refuse the rest. Everything after this span is refused whole.
    if (exCrossSerial < 256u) {
      let t = exCrossSerial;
      var base = exBaseAt[t];
      for (var s = t * span; s < (t + 1u) * span; s++) {
        let c = atomicLoad(&exciteScratch[EX_COUNTS + s]);
        if (c == 0u) { continue; }
        if (base + c <= exBudget) {
          atomicStore(&exciteScratch[EX_BASES + s], base);
          base += c;
        } else {
          atomicStore(&exciteScratch[EX_BASES + s], EX_REFUSED);
          atomicAdd(&fluidArgs[FA_REFUSED], 1u);
        }
      }
      partSum = base;
    }
    atomicStore(&exciteScratch[EX_LIST_COUNT], listSum);
    atomicStore(&exciteScratch[EX_EMITTED], partSum);
    // Emit dispatch args + the tick's final live count + per-particle args.
    atomicStore(&exciteScratch[EX_ARGS + 0u], listSum);
    atomicStore(&exciteScratch[EX_ARGS + 1u], 1u);
    atomicStore(&exciteScratch[EX_ARGS + 2u], 1u);
    let live = min(liveNow + partSum, FLUID_CAP);
    atomicStore(&fluidArgs[FA_LIVE], live);
    atomicStore(&fluidArgs[4u], (live + 63u) / 64u);
    atomicStore(&fluidArgs[5u], 1u);
    atomicStore(&fluidArgs[6u], 1u);
  }
  workgroupBarrier();
  // Per-span assignment for the fully-accepted / fully-refused spans (the
  // crossing span was finished serially above).
  var listIdx = exListBase[li];
  var base = exBaseAt[li];
  let wholly = exAccept[li] == 1u;
  for (var s = li * span; s < (li + 1u) * span; s++) {
    let c = atomicLoad(&exciteScratch[EX_COUNTS + s]);
    if (c == 0u) { continue; }
    atomicStore(&exciteScratch[EX_LIST + listIdx], s);
    listIdx += 1u;
    if (li == exCrossSerial) { continue; }  // bases already assigned serially
    if (wholly) {
      atomicStore(&exciteScratch[EX_BASES + s], base);
      base += c;
    } else {
      atomicStore(&exciteScratch[EX_BASES + s], EX_REFUSED);
      atomicAdd(&fluidArgs[FA_REFUSED], 1u);
    }
  }
}

// Q16 multiply for the emit lattice (a * b >> 16). Operands are positive and
// bounded (offset < 2^16, J <= 2^16), so the staged shifts stay exact enough
// (1/16-cell granularity on a 1/4-cell lattice) and inside u32.
fn seamMulQ16(a : i32, b : i32) -> i32 {
  return i32(((u32(a) >> 4u) * (u32(b) >> 4u)) >> 8u);
}

// emit: one workgroup per candidate slot. Threads scan the chunk's cells in
// index order, build a workgroup-exclusive prefix of per-cell particle
// counts, then either convert (particles born, voxel cleared, dirty marked)
// or — refused slot — restore the word's scratch bits and leave the water.
var<workgroup> emitPart : array<u32, 256>;
var<workgroup> emitBaseW : u32;
var<workgroup> emitRefused : u32;

@compute @workgroup_size(256)
fn exciteEmit(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let ci = atomicLoad(&exciteScratch[EX_LIST + wg.x]);
  if (li == 0u) {
    let b = atomicLoad(&exciteScratch[EX_BASES + ci]);
    emitRefused = select(0u, 1u, b == EX_REFUSED);
    emitBaseW = atomicLoad(&exciteScratch[EX_COMPACT_LIVE]) +
                min(T.fluidSpawnCount, FLUID_CAP) + select(b, 0u, b == EX_REFUSED);
    if (b != EX_REFUSED) {
      atomicStore(&fluidArgs[FA_LASTSLOT], ci);
    }
  }
  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let wc = slotToWorldChunk(sc, T.origin);
  let base = wc * i32(CHUNK);
  // Per-thread candidate-particle count over its 16 cells, for the prefix.
  var mine = 0u;
  for (var s = 0u; s < 16u; s++) {
    let localIdx = li * 16u + s;
    let lo = vec3<i32>(i32(localIdx & 15u), i32((localIdx >> 4u) & 15u),
                       i32(localIdx >> 8u));
    let idx = voxWordIndex(base + lo);
    if (idx == PT_NO_WORD) { continue; }
    let w = voxels[idx];
    if ((w & EXCITE_PEND_BIT) != 0u) { mine += voxState(w) + 1u; }
  }
  emitPart[li] = mine;
  workgroupBarrier();
  if (li == 0u) {
    var sum = 0u;
    for (var t = 0u; t < 256u; t++) {
      let c = emitPart[t];
      emitPart[t] = sum;
      sum += c;
    }
  }
  workgroupBarrier();
  let refused = emitRefused == 1u;
  var off = emitPart[li];
  for (var s = 0u; s < 16u; s++) {
    let localIdx = li * 16u + s;
    let lo = vec3<i32>(i32(localIdx & 15u), i32((localIdx >> 4u) & 15u),
                       i32(localIdx >> 8u));
    let c = base + lo;
    let idx = voxWordIndex(c);
    if (idx == PT_NO_WORD) { continue; }
    let w = voxels[idx];
    if ((w & EXCITE_PEND_BIT) == 0u) { continue; }
    if (refused) {
      // Budget refusal: the water stays settled. The scratch bits MUST go —
      // they are per-tick state and the CA would carry them on the next move.
      voxels[idx] = w & ~EXCITE_SCRATCH_BITS;
      continue;
    }
    let mat = voxMat(w);
    let fullness = voxState(w) + 1u;
    let depth = (w >> EXCITE_DEPTH_SHIFT) & 0xFu;
    let stainT = voxStainType(w);
    let stainA = voxStainAmt(w);
    // Hydrostatic pre-compression: J shrinks with depth so the reawakened
    // column starts holding its own weight (the EOS blend in sim_fluid.wgsl
    // p2g2 turns (1 - J) into pressure). Without this every drained lake
    // bounces like jelly — plan §7 O-4.
    let j0 = max(FLUID_ONE - i32(depth) * SEAM_HYDRO, SEAM_JFLOOR);
    // Every excite seeds at REST. The task-book option of seeding wake cells
    // with the local grid velocity is an energy injection when paired with
    // settle (see the dissipation note in detect); P2G hands the new
    // particles the neighbourhood's momentum on the very next substep, which
    // is both gentler and free.
    let cellSlotIdx = cellIndexW(c);
    let h = hash3(T.seed, T.tick, cellSlotIdx);
    for (var k = 0u; k < fullness; k++) {
      let hk = pcg(h ^ (k * 0x9E3779B9u));
      // 2x2x2 sub-cell lattice (quarter/three-quarter points) + 1/16-cell
      // jitter; the y offsets compress by J toward the cell floor so the
      // seeded density carries the hydrostatic profile geometrically too.
      var ox = select(16384, 49152, (k & 1u) != 0u);
      var oy = select(16384, 49152, (k & 2u) != 0u);
      var oz = select(16384, 49152, (k & 4u) != 0u);
      ox += i32(hk & 0x1FFFu) - 4096;
      oy += i32((hk >> 13u) & 0x1FFFu) - 4096;
      oz += i32((hk >> 21u) & 0x7FFu) * 4 - 4096;
      oy = seamMulQ16(oy, j0);
      var p : FluidParticle;
      p.px = (c.x << 16u) + clamp(ox, 2048, 63488);
      p.py = (c.y << 16u) + clamp(oy, 2048, 63488);
      p.pz = (c.z << 16u) + clamp(oz, 2048, 63488);
      p.vx = 0; p.vy = 0; p.vz = 0;
      p.c00 = 0; p.c01 = 0; p.c02 = 0;
      p.c10 = 0; p.c11 = 0; p.c12 = 0;
      p.c20 = 0; p.c21 = 0; p.c22 = 0;
      p.j = j0;
      p.species = (mat - 1u) & 3u;
      p.density = 0;
      p.attr = fpPack(mat, 1u, stainT, stainA);
      p.birthTick = T.tick;
      p._r0 = 0; p._r1 = 0; p._r2 = 0; p._r3 = 0;
      p._r4 = 0; p._r5 = 0; p._r6 = 0; p._r7 = 0;
      p._r8 = 0; p._r9 = 0; p._r10 = 0; p._r11 = 0;
      let slot = emitBaseW + off;
      if (slot < FLUID_CAP) { fluidParticles[slot] = p; }
      off += 1u;
    }
    // The cell is now particles. Air, no stain, no stamp — and dirty, so the
    // CA re-evaluates everything that was resting on this water.
    voxels[idx] = 0u;
    markDirtyNext(c);
    atomicAdd(&fluidArgs[FA_EXCITED], fullness);
    atomicAdd(&fluidArgs[FA_EMITTED], fullness);
  }
}

// ============================================================================
// CA COUPLING — consumption, occupancy intents, contact staining.
// ============================================================================

// consumeApply (seam FRONT half, after the compaction): a CA reaction that
// matched excited fluid as its neighbour this tick set bit0 of the cell's
// flags word (sim_step doReactions). The WHOLE cell bin dies — consumption
// granularity is the voxel-eighth, and a reaction that takes the neighbour
// takes all of it, exactly as it would a fullness voxel. Order-free: every
// particle tests its own cell's flag; which particles die is a pure function
// of position. The block map here is still the LAST substep's (this runs
// before the substeps rebuild it), which is the same addressing the CA used
// to set the flag.
@compute @workgroup_size(64)
fn consumeApply(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= min(atomicLoad(&exciteScratch[EX_COMPACT_LIVE]), FLUID_CAP)) {
    return;
  }
  var p = fluidParticles[gid.x];
  if (!fpAlive(p.attr)) { return; }
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let bm = fluidBlockMapR[chunkSlotIndex(worldChunkOf(cell))];
  if (bm == 0u) { return; }
  let lo = vec3<u32>(cell & vec3<i32>(CHUNK_MASK));
  let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  if ((atomicLoad(&fluidCellScratch[ci * 2u + 1u]) & 1u) == 0u) { return; }
  atomicAdd(&fluidArgs[FA_CONSUMED], fpFullness(p.attr));
  atomicAdd(&fluidArgs[FA_DEAD], 1u);
  p.attr = 0u;
  fluidParticles[gid.x] = p;
}

// ============================================================================
// SETTLE — calm particles -> fullness voxels.
// ============================================================================

// particleTick: once per tick per particle (after the substeps). Three jobs
// on one pass over the pool:
//   * per-slot speed maxima for the calm judgement (atomicMax, order-free);
//   * the cell's OCCUPANCY INTENT for the CA (material + carried stain,
//     atomicMax — deterministic dominant pick), read one tick later by
//     doReactions' excited-fluid synthesis;
//   * CONTACT STAIN intents on solid/powder face neighbours, the MPM
//     counterpart of CA liquid staining (stainApply rolls and writes them).
@compute @workgroup_size(64)
fn particleTick(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP)) { return; }
  let p = fluidParticles[gid.x];
  if (!fpAlive(p.attr)) { return; }
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let slot = chunkSlotIndex(worldChunkOf(cell));
  let sx = p.vx >> 8u; let sy = p.vy >> 8u; let sz = p.vz >> 8u;
  let s2 = u32(sx * sx + sy * sy + sz * sz);
  atomicMax(&settleScratch[SP_SPEED + slot], s2 + 1u);

  // The stain this particle applies: what it CARRIES (excited out of a
  // stained voxel) wins over its material's authored stain — blood-stained
  // water marks walls with blood, plain water wets them.
  let mat = fpMat(p.attr);
  let m = materials[mat];
  var sType = fpStainType(p.attr);
  var sAmt = fpStainAmt(p.attr);
  if (sType == 0u && matStains(m)) {
    sType = matStainType(m);
    sAmt = matStainAmount(m);
  }
  let intent = (mat << 16u) | (sAmt << 3u) | sType;

  // Own cell: occupancy intent for the CA.
  let bm = fluidBlockMapR[slot];
  if (bm != 0u) {
    let lo = vec3<u32>(cell & vec3<i32>(CHUNK_MASK));
    let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
    atomicMax(&fluidCellScratch[ci * 2u], intent);
  }
  if (sType == 0u) { return; }

  // Solid/powder face neighbours: stain intents (applied by stainApply).
  for (var f = 0u; f < 6u; f++) {
    var d = vec3<i32>(0, 0, 0);
    if (f == 0u) { d.x = 1; } else if (f == 1u) { d.x = -1; }
    else if (f == 2u) { d.y = 1; } else if (f == 3u) { d.y = -1; }
    else if (f == 4u) { d.z = 1; } else { d.z = -1; }
    let n = cell + d;
    if (!inWindow(n, T.origin)) { continue; }
    let nmat = voxMat(voxWordAt(n));
    if (nmat == MAT_AIR) { continue; }
    let nk = materials[nmat].klass;
    if (nk != CLASS_SOLID && nk != CLASS_POWDER) { continue; }
    let nwc = worldChunkOf(n);
    let nbm = fluidBlockMapR[chunkSlotIndex(nwc)];
    if (nbm == 0u) { continue; }  // outside particle support: no block, and
                                  // no contact that matters
    let nlo = vec3<u32>(n & vec3<i32>(CHUNK_MASK));
    let nci = (nbm - 1u) * CHUNK_VOL + (nlo.z * CHUNK + nlo.y) * CHUNK + nlo.x;
    atomicMax(&fluidCellScratch[nci * 2u], intent);
  }
}

// stainApply: one thread per active-block cell (the node-pass indirect args
// from the last substep). A SOLID cell with a stain intent rolls the seam's
// stain chance and takes the stain, following the CA's merge rules: same
// type climbs toward the substrate's ceiling, a foreign type starts over.
// One thread owns one cell — no write races, and the roll keys on
// hash3(seed, tick, cellSlot): state, never scheduling (rule 1).
const SEAM_STAIN_CHANCE : u32 =
    u32(round(clamp(TUNE_FLUID_STAIN_RATE, 0.0, 30.0) * 65536.0 / 30.0));

@compute @workgroup_size(256)
fn stainApply(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let block = wg.x >> 4u;
  let localIdx = (wg.x & 15u) * 256u + li;
  let intent = atomicLoad(&fluidCellScratch[(block * CHUNK_VOL + localIdx) * 2u]);
  let sType = intent & 0x7u;
  let sAmt = (intent >> 3u) & 0xFu;
  if (sType == 0u || sAmt == 0u) { return; }
  let slot = fluidBlockList[block];
  let sc = vec3<i32>(i32(slot % NCHUNK), i32((slot / NCHUNK) % NCHUNK),
                     i32(slot / (NCHUNK * NCHUNK)));
  let wc = slotToWorldChunk(sc, T.origin);
  let lo = vec3<i32>(i32(localIdx & 15u), i32((localIdx >> 4u) & 15u),
                     i32(localIdx >> 8u));
  let c = wc * i32(CHUNK) + lo;
  let idx = voxWordIndex(c);
  if (idx == PT_NO_WORD) { return; }
  let w = voxels[idx];
  let nmat = voxMat(w);
  if (nmat == MAT_AIR) { return; }
  let nk = materials[nmat].klass;
  if (nk != CLASS_SOLID && nk != CLASS_POWDER) { return; }
  let h = hash3(T.seed ^ 0x5741u, T.tick, cellIndexW(c));
  if ((h & 0xFFFFu) >= SEAM_STAIN_CHANCE) { return; }
  // The CA's merge rules (doStaining): the substrate's absorb capacity caps
  // how deep a stain it takes; same type climbs one level per contact on
  // absorbent ground and jumps to the ceiling on plain stone; foreign types
  // restart.
  let ceiling = min(sAmt, max(matAbsorbCapacity(materials[nmat]), 1u));
  let cur = voxStainAmt(w);
  let curType = voxStainType(w);
  var amt = ceiling;
  if (curType == sType && cur >= ceiling) { return; }  // saturated: sleep
  if (curType == sType) { amt = min(cur + 1u, ceiling); }
  voxels[idx] = (w & ~STAIN_BITS) | packStain(sType, amt);
  atomicAdd(&fluidArgs[FA_STAINED], 1u);
  markDirtyNext(c);
}

// settleJudge: one thread per chunk slot. Speed 0 means "no particles here" —
// the calm counter resets so a re-flooded chunk starts its window over.
@compute @workgroup_size(256)
fn settleJudge(@builtin(global_invocation_id) gid : vec3<u32>) {
  let slot = gid.x;
  if (slot >= NUM_CHUNKS) { return; }
  let sp = atomicLoad(&settleScratch[SP_SPEED + slot]);
  if (sp == 0u) {
    atomicStore(&fluidCalm[slot], 0u);
    return;
  }
  if (i32(sp - 1u) <= SEAM_SETTLE2) {
    atomicAdd(&fluidCalm[slot], 1u);
  } else {
    atomicStore(&fluidCalm[slot], 0u);
  }
}

// settleScan: pick up to SETTLE_MAX calm blocks, in slot order, with a greedy
// adjacency exclusion — no two picked blocks within one chunk of each other
// (wrapped slot distance, conservative), so concurrently-committing blocks
// can never read cells another block is writing. One workgroup; the serial
// part touches only spans that contain candidates.
var<workgroup> ssPart : array<u32, 256>;

@compute @workgroup_size(256)
fn settleScan(@builtin(local_invocation_index) li : u32) {
  let span = NUM_CHUNKS / 256u;
  var n = 0u;
  for (var s = li * span; s < (li + 1u) * span; s++) {
    if (atomicLoad(&fluidCalm[s]) >= SEAM_CALM_TICKS) { n += 1u; }
  }
  ssPart[li] = n;
  workgroupBarrier();
  if (li != 0u) { return; }
  var picked = array<vec3<i32>, 16>();
  var count = 0u;
  for (var t = 0u; t < 256u && count < SETTLE_MAX; t++) {
    if (ssPart[t] == 0u) { continue; }
    for (var s = t * span; s < (t + 1u) * span && count < SETTLE_MAX; s++) {
      if (atomicLoad(&fluidCalm[s]) < SEAM_CALM_TICKS) { continue; }
      let sc = vec3<i32>(i32(s % NCHUNK), i32((s / NCHUNK) % NCHUNK),
                         i32(s / (NCHUNK * NCHUNK)));
      var clash = false;
      for (var q = 0u; q < count; q++) {
        let d = abs(sc - picked[q]);
        let m = i32(NCHUNK);
        let dw = min(d, vec3<i32>(m) - d);   // wrapped slot distance
        if (dw.x <= 1 && dw.y <= 1 && dw.z <= 1) { clash = true; break; }
      }
      if (clash) { continue; }
      picked[count] = sc;
      atomicStore(&settleScratch[SP_LIST + count], s);
      atomicStore(&settleScratch[SP_MARK + s], MARK_SETTLING | count);
      count += 1u;
    }
  }
  atomicStore(&settleScratch[SP_COUNT], count);
  atomicStore(&fluidArgs[FA_SETBLOCKS], count);
}

// settleBin: per particle. Particles inside a settling slot bin their eighths
// (atomicAdd) and their identity (atomicMax of mat<<16 | stainAmt<<3 |
// stainType — deterministic dominant-mat pick for mixed cells).
@compute @workgroup_size(64)
fn settleBin(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP)) { return; }
  let p = fluidParticles[gid.x];
  if (!fpAlive(p.attr)) { return; }
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let slot = chunkSlotIndex(worldChunkOf(cell));
  let mark = atomicLoad(&settleScratch[SP_MARK + slot]);
  if ((mark & MARK_SETTLING) == 0u) { return; }
  let listIdx = mark & MARK_LIST_MASK;
  let lo = vec3<u32>(cell & vec3<i32>(CHUNK_MASK));
  let cellIdx = (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  let b = SP_BINS + (listIdx * CHUNK_VOL + cellIdx) * 2u;
  atomicAdd(&settleScratch[b], fpFullness(p.attr));
  atomicAdd(&fluidArgs[FA_BINNED], fpFullness(p.attr));  // mass audit
  atomicMax(&settleScratch[b + 1u],
            (fpMat(p.attr) << 16u) | (fpStainAmt(p.attr) << 3u) |
            fpStainType(p.attr));
}

// Support under a settling column: what water can rest on. Out-of-window is
// solid and inert (the residency rule).
fn seamSupport(c : vec3<i32>) -> bool {
  if (!inWindow(c, T.origin)) { return true; }
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return false; }
  let k = materials[mat].klass;
  if (k == CLASS_SOLID || k == CLASS_POWDER) { return true; }
  if (k == CLASS_LIQUID) { return voxState(w) + 1u >= 8u; }
  return false;
}

// The shared column walk of settleCheck and settleCommit: segment the column
// at blockers, pool each segment's content (existing same-liquid eighths +
// binned eighths), refill the segment bottom-up. `write` distinguishes the
// feasibility pass from the commit pass — both run the identical arithmetic,
// which is what makes all-or-nothing refusal sound.
//
// SPILL: the walk continues up to SETTLE_SPILL cells into the CHUNK ABOVE.
// A gravity-compressed pool holds MORE than 8 eighths per cell (the weakly
// compressible EOS packs the bottom ~5-15% over rest), so the bottom block
// of any real pool carries 1-2 eighths per column past its own top — without
// the spill every pick refused and the pool deadlocked as particles forever
// (measured: 66/66 refusals on a fully calm pool). Writing above is safe:
// the adjacency exclusion means no concurrently-settling block can touch
// those cells, and the fluid-chunk N26 ring materialized them.
// Returns false if the column is infeasible (content with no floor, content
// past the spill ceiling, or trapped under a blocker).
const SETTLE_SPILL : i32 = 8;
fn settleColumn(listIdx : u32, base : vec3<i32>, cx : i32, cz : i32,
                write : bool) -> bool {
  var floorOk = seamSupport(base + vec3<i32>(cx, -1, cz));
  var pool = 0u;         // eighths waiting to be placed in this segment
  var poolMatStain = 0u; // packed identity of the pooled content
  for (var y = 0; y <= 16 + SETTLE_SPILL; y++) {
    var blocked = y == 16 + SETTLE_SPILL;  // the spill ceiling ends the walk
    var w = 0u;
    var idx = PT_NO_WORD;
    if (y < 16 + SETTLE_SPILL) {
      let c = base + vec3<i32>(cx, y, cz);
      idx = voxWordIndex(c);
      if (idx != PT_NO_WORD) { w = voxels[idx]; }
      let mat = voxMat(w);
      if (mat != MAT_AIR && !seamLiquid(mat)) { blocked = true; }
      // An unmapped word can only mean the coverage guarantee failed (the
      // walk stays inside the settling chunk + its materialized N26 ring);
      // treating it as a blocker refuses the block instead of dropping mass
      // — voxStore's belt-and-braces, applied to the one non-voxStore write.
      if (idx == PT_NO_WORD) { blocked = true; }
    }
    if (blocked) {
      // Close the segment: everything pooled must have been placed.
      if (pool > 0u) { return false; }
      floorOk = true;  // content above rests on this blocker
      // A particle can end its calm life FRACTIONALLY inside a blocker (the
      // node BC stops momentum at the face, not the cell centre), which bins
      // its eighth at the blocked cell. That mass pops UP: it seeds the pool
      // of the segment ABOVE the blocker. Dropping it instead was the
      // 2-in-1280 leak the first sealed settle-gate run caught.
      if (y < 16) {
        let blo = vec3<u32>((base + vec3<i32>(cx, y, cz)) &
                            vec3<i32>(CHUNK_MASK));
        let bIdx = (blo.z * CHUNK + blo.y) * CHUNK + blo.x;
        let bb = SP_BINS + (listIdx * CHUNK_VOL + bIdx) * 2u;
        pool = atomicLoad(&settleScratch[bb]);
        poolMatStain = atomicLoad(&settleScratch[bb + 1u]);
      }
      continue;
    }
    // Open cell: pool its content, then place bottom-first as we walk. A
    // cell's own binned + existing eighths join the pool at its own height,
    // and the pool drains into the LOWEST open cells — that is the
    // bottom-packing an equilibrium column must have. Bins exist only for
    // the settling block's own 16 cells; the spill cells above contribute
    // existing content only (the CHUNK_MASK wrap would otherwise alias a
    // spill cell onto the bottom rows' bins).
    var binned = 0u;
    var packed = 0u;
    if (y < 16) {
      let lo = vec3<u32>((base + vec3<i32>(cx, y, cz)) &
                         vec3<i32>(CHUNK_MASK));
      let cellIdx = (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
      let b = SP_BINS + (listIdx * CHUNK_VOL + cellIdx) * 2u;
      binned = atomicLoad(&settleScratch[b]);
      packed = atomicLoad(&settleScratch[b + 1u]);
    }
    let mat = voxMat(w);
    var existing = 0u;
    if (mat != MAT_AIR) { existing = voxState(w) + 1u; }
    // Existing settled water keeps its identity; binned content brings its
    // own. Max keeps the pick deterministic when the two differ.
    var idHere = packed;
    if (existing > 0u) {
      idHere = max(idHere, (mat << 16u) | (voxStainAmt(w) << 3u) |
                            voxStainType(w));
    }
    pool += binned + existing;
    poolMatStain = max(poolMatStain, idHere);
    // FA_SETTLED counts NET NEW eighths: re-placed existing water cancels
    // out (atomicSub here against the atomicAdd of `place` below), so the
    // counter equals the particle mass CONVERTED — the number the mass
    // audits and the splash sound cue actually want.
    if (write && existing > 0u) {
      atomicSub(&fluidArgs[FA_SETTLED], existing);
    }
    if (pool > 0u && !floorOk) { return false; }
    let place = min(pool, 8u);
    pool -= place;
    if (write && idx != PT_NO_WORD) {
      var nw = 0u;
      if (place > 0u) {
        let pm = poolMatStain >> 16u;
        nw = packVox(pm, place - 1u, STAMP_NEVER) |
             packStain(poolMatStain & 0x7u, (poolMatStain >> 3u) & 0xFu);
      }
      if (nw != w) {
        voxels[idx] = nw;
        markDirtyNext(base + vec3<i32>(cx, y, cz));
      }
      if (place > 0u) { atomicAdd(&fluidArgs[FA_SETTLED], place); }
    }
    if (place > 0u) {
      floorOk = true;   // water stacks on water
    } else {
      // An open cell that stays empty breaks the support chain: content
      // higher up must not be placed above an air gap (it would be a written
      // floating voxel), it must refuse the block instead.
      floorOk = false;
    }
  }
  return true;
}

// settleCheck: one workgroup per settling block, one thread per column. Any
// infeasible column refuses the WHOLE block (atomicOr) — mass is never
// partially converted, so it can never be dropped or invented.
@compute @workgroup_size(256)
fn settleCheck(@builtin(workgroup_id) wg : vec3<u32>,
               @builtin(local_invocation_index) li : u32) {
  if (wg.x >= atomicLoad(&settleScratch[SP_COUNT])) { return; }
  let ci = atomicLoad(&settleScratch[SP_LIST + wg.x]);
  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  let cx = i32(li & 15u);
  let cz = i32(li >> 4u);
  if (!settleColumn(wg.x, base, cx, cz, false)) {
    atomicOr(&settleScratch[SP_MARK + ci], MARK_REFUSED);
    // Refused blocks start their calm window over rather than re-running the
    // full bin/check every tick against unchanging geometry (rule 2).
    atomicStore(&fluidCalm[ci], 0u);
  }
}

// settleCommit: the identical walk, writing. Refused blocks skip.
@compute @workgroup_size(256)
fn settleCommit(@builtin(workgroup_id) wg : vec3<u32>,
                @builtin(local_invocation_index) li : u32) {
  if (wg.x >= atomicLoad(&settleScratch[SP_COUNT])) { return; }
  let ci = atomicLoad(&settleScratch[SP_LIST + wg.x]);
  if ((atomicLoad(&settleScratch[SP_MARK + ci]) & MARK_REFUSED) != 0u) { return; }
  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  let cx = i32(li & 15u);
  let cz = i32(li >> 4u);
  settleColumn(wg.x, base, cx, cz, true);
  if (li == 0u) { atomicStore(&fluidCalm[ci], 0u); }
}

// settleKill: particles of committed blocks die. Their eighths are voxels
// now; next tick's compaction reclaims the slots and shrinks the live count.
@compute @workgroup_size(64)
fn settleKill(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP)) { return; }
  var p = fluidParticles[gid.x];
  if (!fpAlive(p.attr)) { return; }
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let slot = chunkSlotIndex(worldChunkOf(cell));
  let mark = atomicLoad(&settleScratch[SP_MARK + slot]);
  if ((mark & MARK_SETTLING) == 0u || (mark & MARK_REFUSED) != 0u) { return; }
  p.attr = 0u;
  fluidParticles[gid.x] = p;
  atomicAdd(&fluidArgs[FA_DEAD], 1u);
}
