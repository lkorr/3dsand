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
// read_write ATOMIC rather than read: stainApply lights the settled-liquid
// half of the Y-occupancy mask (common.wgsl fbmYMaskIndex), and 256 threads
// of one block share a chunk's mask word. The INDEX half is still read-only
// in this module — every `bm` below is an atomicLoad of it.
@group(1) @binding(3) var<storage, read_write> fluidBlockMapR : array<atomic<u32>>;
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
// The swimming query's view of the particles: excited-fluid eighths per
// CPU-mirror cell, one byte each packed 4/word, written by mirrorFold and
// read back with the snapshot.
@group(1) @binding(13) var<storage, read_write> fluidMirror : array<u32>;

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
// Per-COLUMN excite-unstable mask: 256 columns per settling block, 8 words of
// bits each. Feasibility refusal stays whole-block (see settleCheck), but
// instability is a property of ONE column and is refused at that granularity.
const SP_COLBAD : u32 = SP_BINS + SETTLE_MAX * CHUNK_VOL * 2u;  // + listIdx*8
const SP_SCRATCH_WORDS : u32 = SP_COLBAD + SETTLE_MAX * 8u;
const MARK_SETTLING : u32 = 0x80000000u;
const MARK_REFUSED : u32 = 0x40000000u;
const MARK_LIST_MASK : u32 = 0x1Fu;

// ---- tuning -> per-tick fixed point (same const-eval discipline as the
// solver: WGSL const-expressions fold IEEE-exactly, the kernel stays integer).
// Settle/wake thresholds compare on the splash test's (v >> 8) scale so the
// tuner's vox/s reads identically across all three.
//
// PRECISION (plan §5's SEAM_SETTLE8 fix): the threshold used to be truncated
// to Q8.8 BEFORE squaring (>>8), which quantized the settleEps slider to
// ~0.117 vox/s steps — non-monotone tuner behaviour right in the interesting
// range. Truncate to Q12.4 instead (>>4), square, and shift the SQUARE down 8
// so the compare stays in the measured side's exact (v >> 8)^2 units; the
// slider now steps at ~0.0073 vox/s.
// Overflow audit: LoadTuning clamps settleEps/wakeSpeed to <= 50 vox/s, i.e.
// 109227 Q16.16, so (109227 >> 4)^2 = 6826^2 = 4.66e7 < 2^31 whatever the
// substep budget does to FLUID_VMAX. (Audited at the absolute VMAX ceiling
// too: 32 substeps -> 943718 Q16.16, (943718 >> 4)^2 = 3.48e9, which would
// NOT fit — hence the audit is on the tuner's 50 vox/s clamp, not on VMAX.)
const SEAM_SETTLE4 : i32 =
    i32(round(TUNE_FLUID_SETTLE_EPS * 65536.0 / 30.0)) >> 4u;
const SEAM_SETTLE2 : i32 = (SEAM_SETTLE4 * SEAM_SETTLE4) >> 8u;
const SEAM_WAKE4 : i32 =
    i32(round(TUNE_FLUID_WAKE_SPEED * 65536.0 / 30.0)) >> 4u;
const SEAM_WAKE2 : i32 = (SEAM_WAKE4 * SEAM_WAKE4) >> 8u;
const SEAM_CALM_TICKS : u32 = u32(clamp(TUNE_FLUID_SETTLE_TICKS, 8, 600));

// ---- the free-surface gravity bias, and why every speed test removes it ----
// A weakly-compressible MPM free surface is NEVER at rest. Pressure comes from
// density >= rest, so the top layer of any pool has none, and gridUpdate adds
// FLUID_GRAVITY / FLUID_SUBSTEPS to it every substep with nothing to cancel it.
// Velocity is overwritten from the grid each substep (pure PIC+APIC), so it
// does not accumulate — it sits at exactly one substep of gravity, forever.
// At the owner's 900 vox/s^2 and 9 substeps that is 3.33 cells/tick = 100
// vox/s, and the calm judgement is a MAX over the chunk, so ONE surface
// particle vetoed every pool in the engine. Measured: the fluid-excite gate's
// drained chamber ended at max 90 vox/s after 320 ticks of 0.9/s damping —
// damping cannot touch it, because the grid regenerates it every substep.
//
// So every speed test here reads the SMALLER of |v| and |v + one substep of
// gravity|. Taking the min rather than always correcting is what makes it safe
// on the other half of the surface: a node the BC has already zeroed (resting
// on a floor) is genuinely at rest at v = 0, and blindly adding gravity back
// would make IT read 100 vox/s instead. Both readings of "at rest" map to 0,
// and a genuinely falling particle keeps all but one substep of its speed.
// The bias was also why settleEps had to be re-scaled with gravity at all;
// with it gone the threshold means the same thing at any g.
const SEAM_GRAV_SUB : i32 =
    i32(round(TUNE_FLUID_GRAVITY * 65536.0 / 900.0)) / FLUID_SUBSTEPS;
fn seamRestVy(vy : i32) -> i32 { return min(abs(vy + SEAM_GRAV_SUB), abs(vy)); }
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

// Spans that can hold a live particle. compactCount/compactScatter dispatch
// exactly this many workgroups (fluidArgs[FA_ARGS_COMPACT], staged into
// fluidPDispatchArgs), so compactScratch beyond it is STALE and this scan must
// not read it — that is the whole contract of making those two rows indirect.
fn liveSpans() -> u32 {
  return (min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP) + 255u) / 256u;
}

@compute @workgroup_size(256)
fn compactScan(@builtin(local_invocation_index) li : u32) {
  // 256 threads x (SPANS/256) spans each; thread 0 turns the partials into
  // exclusive bases. Also clears the per-tick FA event counters — this is the
  // first seam pass of the tick, so last tick's values have already ridden
  // the snapshot readback out.
  let per = SPANS / 256u;
  let spans = liveSpans();
  var n = 0u;
  for (var s = li * per; s < (li + 1u) * per; s++) {
    if (s < spans) { n += compactScratch[s]; }
  }
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
    // consumeApply's dispatch: one thread per SURVIVOR, not per pool slot.
    atomicStore(&fluidArgs[FA_ARGS_CONSUME + 0u], (min(sum, FLUID_CAP) + 63u) / 64u);
    atomicStore(&fluidArgs[FA_ARGS_CONSUME + 1u], 1u);
    atomicStore(&fluidArgs[FA_ARGS_CONSUME + 2u], 1u);
    atomicStore(&fluidArgs[FA_DEAD], 0u);
    atomicStore(&fluidArgs[FA_SETTLED], 0u);
    atomicStore(&fluidArgs[FA_EXCITED], 0u);
    atomicStore(&fluidArgs[FA_REFUSED], 0u);
    atomicStore(&fluidArgs[FA_SETBLOCKS], 0u);
    atomicStore(&fluidArgs[FA_EMITTED], 0u);
    atomicStore(&fluidArgs[FA_BINNED], 0u);
    atomicStore(&fluidArgs[FA_CONSUMED], 0u);
    atomicStore(&fluidArgs[FA_STAINED], 0u);
    atomicStore(&fluidArgs[FA_CLAMPED], 0u);
    atomicStore(&fluidArgs[FA_SETREFUSED], 0u);
    atomicStore(&fluidArgs[FA_SETUNSTABLE], 0u);
  }
  workgroupBarrier();
  var base = wgScan[li];
  for (var s = li * per; s < (li + 1u) * per; s++) {
    if (s >= spans) { break; }
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
  // The CFL cap is derived from the substep knob (common.wgsl), so a spawn op
  // authored against one substep budget cannot smuggle a super-CFL velocity
  // into a world running another.
  p.vx = clamp(op.vx, -FLUID_VMAX, FLUID_VMAX);
  p.vy = clamp(op.vy, -FLUID_VMAX, FLUID_VMAX);
  p.vz = clamp(op.vz, -FLUID_VMAX, FLUID_VMAX);
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

// ---- the excite GEOMETRY, shared by detect and by settle's stability test ---
//
// WP3 item 2's "hysteresis by construction": settleCheck must refuse to settle
// any column whose resulting cells would immediately satisfy an excite
// trigger, so a settled configuration is excite-STABLE and the seam cannot
// oscillate. That only holds if the two sides evaluate the SAME predicate, so
// the predicate lives here, once, and both call it.
//
// The stability test runs regardless of T.fluidExciteEnable (the plan's
// critical detail): the mid-slope freeze the user reported happens at
// exciteMode 0, which is how `mpm`-tool water is placed. Refusing to freeze it
// needs no excite path at all — the water simply stays particles until it
// reaches a configuration that would not immediately want to move again.

// How much liquid a cell holds, for the geometric tests: settled eighths from
// the voxel word, or — for an air cell — the excited eighths the MPM node grid
// carries there. That second arm is load-bearing. Without it a pool spanning
// two chunks could never settle: the chunk that goes calm first sees its
// neighbour's water as AIR (it is still particles, the voxel really is empty),
// reads a full 8-eighth lateral gradient, and refuses forever. Same Q10-mass
// -> whole-particles conversion mirrorFold uses.
fn seamExcitedEighths(c : vec3<i32>) -> u32 {
  let wc = worldChunkOf(c);
  if (!chunkInWindow(wc, T.origin)) { return 0u; }
  let bm = atomicLoad(&fluidBlockMapR[chunkSlotIndex(wc)]);
  if (bm == 0u) { return 0u; }
  return u32(clamp(fluidGridR[seamNodeBase(bm, c)] >> 10u, 0, 8));
}

// A neighbour cell as the geometry sees it: .x = 1 if the cell BLOCKS flow
// (solid, powder, an out-of-window cell, or a liquid this seam does not own),
// .y = its liquid content in eighths (settled + excited). Blockers report 0
// content and are simply skipped by every test below — water resting against
// stone is stable, which is the whole point of a basin.
fn seamNeighbourState(c : vec3<i32>) -> vec2<u32> {
  if (!inWindow(c, T.origin)) { return vec2<u32>(1u, 0u); }
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return vec2<u32>(0u, seamExcitedEighths(c)); }
  if (seamLiquid(mat)) { return vec2<u32>(0u, voxState(w) + 1u); }
  return vec2<u32>(1u, 0u);
}

// THE TRIGGER, over a cell that holds water and one of its lateral
// neighbours: DIAGONAL FALL — the lateral neighbour is empty (no settled
// water, no excited mass, not a blocker) and the cell BELOW that neighbour is
// empty too. The water could fall diagonally, so it is not at rest.
//
// This is plan §6 item 1's trigger (b). It is the steep-slope case trigger (a)
// structurally cannot see: on a slope there is terrain directly below, so
// nothing "falls", but the cell diagonally down-slope is air even for a
// ONE-VOXEL step (the neighbour's floor is one lower, so its cell at MY water
// level and the cell under it are both air). That is the hill's stepped ramp,
// and it is the user's reported mid-slope clump.
//
// DELIBERATE DEVIATION from plan §6 item 1, measured twice. The plan also
// wanted trigger (c), "a lateral neighbour >= 2 eighths lower (air counts as
// 0)". As an excite trigger that is defensible; as the settle-STABILITY test
// it is fatal, and the two have to be one predicate or they oscillate against
// each other. Two reasons it cannot be the stability test:
//
//   * A column's fullness is a count of PARTICLES, 8 to a full cell. A pool
//     four eighths deep carries a couple of eighths of shot noise column to
//     column, so a 2-eighth threshold sits UNDER the discretization's own
//     noise floor. Measured on the fluid-settle gate at the owner's defaults:
//     36 blocks picked, 36 refused as unstable, 0 settled, 1,280 eighths
//     still live at tick 400.
//   * Worse, it is not even a noise problem. settleColumn bottom-packs, so a
//     deeper column's TOP cell is always beside a shallower column's empty
//     cell at the same level. "Cell >= 2 with empty beside it" is therefore
//     true at the surface of every pool that is not perfectly level to within
//     one eighth — a condition no particle method reaches.
//
// AND IT IS ASKED ONLY AT THE BASE OF A COLUMN — the cell whose own below is
// not this liquid. That restriction is the second measured lesson: asked at
// every level, the test still refused 40 of 40 picks, because a deeper column's
// TOP cell overhangs a shallower neighbour's empty one and the cell under THAT
// is empty too. But an overhanging free surface is not perched water, it is
// pool curvature, and the CA spreads it perfectly well (fullness 8 flows
// sideways into air). What the CA cannot do is get water OFF A SLOPE, and that
// is exactly a base cell with a diagonal void: the column is standing on a
// ledge. Testing bases only makes the predicate mean "this body of water is
// perched", which is the question WP3 is actually asking.
//
// It also restores the hysteresis-by-construction guarantee exactly: the
// excite side applies the same base restriction, so
// {cells excite would take} == {cells settle refuses to create}, and the seam
// cannot oscillate. Draining a perched column still works, progressively — the
// base converts to particles, the cells above lose their support, and trigger
// (a) (air below) takes them on the following ticks, which is the mechanism
// the seam already uses everywhere else.
//
// `nb`/`bel` are seamNeighbourState of the lateral cell and of the cell below
// it. Pure function of the two states: no ordering, no randomness.
fn seamLateralExcite(nb : vec2<u32>, bel : vec2<u32>) -> bool {
  return nb.x == 0u && nb.y == 0u && bel.x == 0u && bel.y == 0u;
}

// The 4 lateral offsets, in a fixed order (no scheduling dependence).
fn seamLateral(d : u32) -> vec3<i32> {
  if (d == 0u) { return vec3<i32>(1, 0, 0); }
  if (d == 1u) { return vec3<i32>(-1, 0, 0); }
  if (d == 2u) { return vec3<i32>(0, 0, 1); }
  return vec3<i32>(0, 0, -1);
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
      // Triggers (b) diagonal fall and (c) lateral pressure gradient (WP3
      // item 1), the same predicate settleCheck refuses to settle INTO. These
      // are what unstick water the CA has parked on a slope: trigger (a) is
      // false there (the tread is solid) and the CA's own liquidEqualize
      // makes the 1-eighth staircase a stable resting state.
      // Base cells only — see the seamLateralExcite block. `below` is air here
      // whenever trigger (a) already fired, so this only ever adds the case
      // where the cell rests on TERRAIN with a lateral void beside it.
      // Out-of-window below is solid and inert (the residency rule), so it is
      // a base.
      var onBase = true;
      let bw = c + vec3<i32>(0, -1, 0);
      if (inWindow(bw, T.origin)) {
        onBase = !seamLiquid(voxMat(voxWordAt(bw)));
      }
      if (!excite && onBase) {
        for (var d = 0u; d < 4u; d++) {
          let n = c + seamLateral(d);
          if (seamLateralExcite(seamNeighbourState(n),
                                seamNeighbourState(n + vec3<i32>(0, -1, 0)))) {
            excite = true;
            break;
          }
        }
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
        let bm = atomicLoad(&fluidBlockMapR[chunkSlotIndex(nwc)]);
        if (bm == 0u) { continue; }
        let nb = seamNodeBase(bm, n);
        if (fluidGridR[nb] < 16) { continue; }  // FLUID_MASS_MIN
        // Same free-surface gravity strip as the calm measure. Without it the
        // node just above any settled pool reads a full substep of gravity —
        // 100 vox/s at the owner's defaults, four times wakeSpeed — so the
        // wake trigger fired on every settled cell touching any fluid node,
        // permanently. That is the other half of the settle<->wake thrash.
        let vx = fluidGridR[nb + 1u] >> 8u;
        let vy = seamRestVy(fluidGridR[nb + 2u]) >> 8u;
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
    // NEXT tick's compaction dispatch: one workgroup per 256-slot span that
    // can hold a survivor. This is the tick's authoritative population, so it
    // is the one place that can size the compaction — which is why the args
    // are written here rather than by the compaction itself (plan §7 item 1).
    atomicStore(&fluidArgs[FA_ARGS_COMPACT + 0u], (live + 255u) / 256u);
    atomicStore(&fluidArgs[FA_ARGS_COMPACT + 1u], 1u);
    atomicStore(&fluidArgs[FA_ARGS_COMPACT + 2u], 1u);
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
  let bm = atomicLoad(&fluidBlockMapR[chunkSlotIndex(worldChunkOf(cell))]);
  if (bm == 0u) { return; }
  let lo = vec3<u32>(cell & vec3<i32>(CHUNK_MASK));
  let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  if ((atomicLoad(&fluidCellScratch[ci * 2u + 1u]) & 1u) == 0u) { return; }
  atomicAdd(&fluidArgs[FA_CONSUMED], fpFullness(p.attr));
  atomicAdd(&fluidArgs[FA_DEAD], 1u);
  p.attr = 0u;
  fluidParticles[gid.x] = p;
}

// cellClear: zero the per-cell intent/flags scratch for the ACTIVE BLOCKS only.
//
// This replaces an 8 MiB vkCmdFillBuffer that ran every single tick, forever,
// for a buffer sized by the kFluidBlocks CEILING (256) while the measured lab
// scenes allocate 8-22 blocks (plan §7 item 1; the fill was 8 MiB, the real
// working set is 250-700 KB). Dispatched off the node-pass indirect args of the
// LAST substep — the same blocks:16-workgroups shape stainApply uses — so it is
// zero work when the solver has no blocks, which is what "sleep" means here.
//
// Block indices past the current count keep whatever they last held: nothing
// can address them, because every access resolves through the block map, whose
// entries only ever name indices below the count.
@compute @workgroup_size(256)
fn cellClear(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let cell = (wg.x >> 4u) * CHUNK_VOL + (wg.x & 15u) * 256u + li;
  atomicStore(&fluidCellScratch[cell * 2u], 0u);
  atomicStore(&fluidCellScratch[cell * 2u + 1u], 0u);
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
  // seamRestVy: strip the free-surface gravity bias (see the const block).
  let sx = p.vx >> 8u; let sy = seamRestVy(p.vy) >> 8u; let sz = p.vz >> 8u;
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
  let bm = atomicLoad(&fluidBlockMapR[slot]);
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
    let nbm = atomicLoad(&fluidBlockMapR[chunkSlotIndex(nwc)]);
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
  // ---- Y-occupancy mask, settled-liquid half (common.wgsl) ----------------
  // The renderer's isosurface takes VIRTUAL MASS from settled liquid voxels so
  // the MPM surface meets voxel water instead of ending at a cliff. Those cells
  // carry no node mass, so gridUpdate cannot see them, and without this the
  // march would skip the y levels a half-converted pool still holds as voxels —
  // a seam straight through the middle of it. This pass is the one that already
  // walks every active-block cell with the voxel word in hand.
  if (nmat != MAT_AIR && materials[nmat].klass == CLASS_LIQUID) {
    atomicOr(&fluidBlockMapR[fbmYMaskIndex(slot)], fbmYBits(lo.y));
  }
  if (sType == 0u || sAmt == 0u) { return; }
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
//
// A NOTE ON THE THRASH THIS DOES NOT NEED TO HANDLE. Settle is a per-CHUNK
// decision and wake is a per-CELL one, so an earlier WP3 revision also
// required the six face-neighbour chunks to be quiet before a block could
// bank a calm tick — a chunk on the edge of churning water was settling and
// being woken again the next tick, measured at 3,833 eighths settled against
// 3,721 re-excited in one 400-tick gate run.
//
// That neighbourhood gate is GONE, because the thrash was not what it looked
// like: the wake trigger was reading the free-surface gravity bias (see
// SEAM_GRAV_SUB) as 100 vox/s of motion on every node above every settled
// pool, so it fired permanently and unconditionally. Stripping the bias
// removes the cause; the gate only removed the symptom, and it removed a great
// deal of legitimate settling with it (0 blocks picked in four of five bench
// scenes, and the sealed fluid-excite chamber never converting at all).
// Re-excitation is measured, not assumed — FA_EXCITED against FA_SETTLED, the
// `re-excited` column of --fluid-bench's seam-flow line.
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
  // TRUE SLEEP (plan §7 item 2). This scan and the solver's `alloc` are the
  // only fluid passes whose cost does NOT come from an indirect arg — both are
  // single-workgroup walks of all NUM_CHUNKS slots, so a world that poured once
  // and settled kept paying them forever (the CPU-side fluidCount is monotone
  // by design, so the TABLE keeps being recorded — that is the determinism
  // contract, and the sanctioned way to make it free is exactly this).
  // With no particles alive, no slot can have a calm counter to find: settleJudge
  // resets a slot the moment its speed reads zero.
  // (The early-out cannot `return` before the workgroupBarrier below — a
  // storage read is non-uniform to the compiler, and a barrier in non-uniform
  // control flow is a WGSL validation error. Skipping the WORK is enough.)
  let asleep = atomicLoad(&fluidArgs[FA_LIVE]) == 0u;
  let span = NUM_CHUNKS / 256u;
  var n = 0u;
  if (!asleep) {
    for (var s = li * span; s < (li + 1u) * span; s++) {
      if (atomicLoad(&fluidCalm[s]) >= SEAM_CALM_TICKS) { n += 1u; }
    }
  }
  ssPart[li] = n;
  workgroupBarrier();
  if (li != 0u) { return; }
  if (asleep) {
    atomicStore(&settleScratch[SP_COUNT], 0u);
    atomicStore(&fluidArgs[FA_SETBLOCKS], 0u);
    return;
  }
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
      // Belt-and-braces: `seam_fill_settle` already zeroes the whole scratch
      // at the head of the seam, but settleCheck only ever ORs into this mask
      // and a stale bit here silently strands a column's water as particles.
      // Cheap (8 stores per pick, one thread) and it makes the OR-only
      // contract locally sound rather than dependent on a distant pass row.
      for (var q = 0u; q < 8u; q++) {
        atomicStore(&settleScratch[SP_COLBAD + count * 8u + q], 0u);
      }
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

// Did the stability test refuse THIS column of this settling block? Shared by
// settleCommit (do not write it) and settleKill (do not eat its particles) —
// the two must read the identical bit or mass is dropped or invented.
fn seamColumnRefused(listIdx : u32, col : u32) -> bool {
  return (atomicLoad(&settleScratch[SP_COLBAD + listIdx * 8u + col / 32u]) &
          (1u << (col % 32u))) != 0u;
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
//
// RECORDING (WP3 item 2). settleCheck is one workgroup of 256 threads over a
// 16x16 block — exactly one thread per column — so the whole block's resulting
// shape is computable inside one workgroup. `rec` makes the feasibility walk
// also publish its per-level answer into workgroup memory, which is what lets
// the stability test below compare a column against its IN-BLOCK lateral
// neighbours' POST-settle fill rather than their pre-settle voxels (which are
// air: the water is still particles). 24 levels x 4 bits + a blocked bitmask =
// 4 words per column, 4 KiB per workgroup.
const SETTLE_SPILL : i32 = 8;
const SETTLE_LEVELS : i32 = 16 + SETTLE_SPILL;   // 24
var<workgroup> wgFill : array<u32, 768>;   // [col*3 + y/8], nibble (y%8)*4
var<workgroup> wgBlk : array<u32, 256>;    // bit y = level y blocks flow
// bit y = level y is BYTE-IDENTICAL to the voxel already there: this settle
// neither creates nor moves that water. See the stability test in settleCheck
// for why that distinction decides whether the level may veto the block.
var<workgroup> wgSame : array<u32, 256>;

fn wgFillAt(col : u32, y : i32) -> u32 {
  return (wgFill[col * 3u + u32(y) / 8u] >> ((u32(y) % 8u) * 4u)) & 0xFu;
}
fn wgBlockedAt(col : u32, y : i32) -> bool {
  return (wgBlk[col] & (1u << u32(y))) != 0u;
}
fn wgSameAt(col : u32, y : i32) -> bool {
  return (wgSame[col] & (1u << u32(y))) != 0u;
}

fn settleColumn(listIdx : u32, base : vec3<i32>, cx : i32, cz : i32,
                write : bool, rec : bool, col : u32) -> bool {
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
      if (rec && y < SETTLE_LEVELS) { wgBlk[col] |= 1u << u32(y); }
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
    // An UNTOUCHED cell: nothing arrived from below (the pool was empty when
    // the walk reached this level), nothing binned here, and the cell already
    // holds water. The placement below is then min(existing, 8) == existing
    // with poolMatStain == its own identity, i.e. `nw == w` and the walk
    // rewrites the cell with itself. Recorded because the stability test must
    // not veto a block for water this settle is not creating.
    let untouched = pool == 0u && binned == 0u && existing > 0u;
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
    if (rec && y < SETTLE_LEVELS) {
      wgFill[col * 3u + u32(y) / 8u] |= place << ((u32(y) % 8u) * 4u);
      if (untouched) { wgSame[col] |= 1u << u32(y); }
    }
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
//
// TWO tests now, and the second is WP3's headline:
//
//   1. FEASIBILITY — the column walk fits (existing behaviour).
//   2. EXCITE STABILITY — every cell the walk WOULD write must fail every
//      geometric excite trigger (seamLateralExcite). A settled configuration
//      that immediately satisfies a trigger is a one-tick oscillation at best
//      and, at exciteMode 0 where nothing can re-excite it, is the user's
//      reported mid-slope FREEZE: water converted to CA voxels exactly where
//      it stalled on a hillside, in a staircase the CA's liquidEqualize=2 then
//      holds as a stable equilibrium forever. Refusing it costs nothing —
//      the water stays particles and keeps flowing — and it is bounded,
//      because water that reaches flat ground does satisfy the test.
//      This runs REGARDLESS of T.fluidExciteEnable, on purpose (plan §6 item
//      2): the freeze it prevents happens at exciteMode 0.
//
// The stability test needs the POST-settle fill of the four lateral
// neighbours. In-block neighbours are still particles, so their voxels read
// air — the answer has to come from the walk, which is why every thread
// publishes its column into workgroup memory and the test runs after a
// barrier. Out-of-block neighbours are never concurrently settling (the scan's
// adjacency exclusion), so their voxels ARE their post-settle state, and
// seamNeighbourState adds the excited-particle arm for the water next door.
// Any thread may find the block unstable, and the refusal has to be applied
// once, by one thread, after everybody has voted. Workgroup atomicOr: 0/1,
// associative, no CAS (rule 1).
var<workgroup> wgBad : atomic<u32>;      // infeasible column
var<workgroup> wgUnstable : atomic<u32>; // excite-unstable resulting cell

@compute @workgroup_size(256)
fn settleCheck(@builtin(workgroup_id) wg : vec3<u32>,
               @builtin(local_invocation_index) li : u32) {
  // Workgroup memory is not zero-initialised, and a column that refuses on
  // feasibility returns early with its record half-written. Clear first.
  // No early `return` anywhere in this function: every barrier below has to
  // sit in control flow the compiler can see is uniform, and `wg.x < count`
  // comes from a storage read (settleScan's note on the same hazard).
  wgFill[li * 3u] = 0u;
  wgFill[li * 3u + 1u] = 0u;
  wgFill[li * 3u + 2u] = 0u;
  wgBlk[li] = 0u;
  wgSame[li] = 0u;
  if (li == 0u) { atomicStore(&wgBad, 0u); atomicStore(&wgUnstable, 0u); }
  workgroupBarrier();

  let live = wg.x < atomicLoad(&settleScratch[SP_COUNT]);
  var ci = 0u;
  var base = vec3<i32>(0, 0, 0);
  if (live) {
    ci = atomicLoad(&settleScratch[SP_LIST + wg.x]);
    let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                                 ci / (NCHUNK * NCHUNK)));
    base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  }
  let cx = i32(li & 15u);
  let cz = i32(li >> 4u);
  // ---- test 1: feasibility (and publish the resulting column) -------------
  if (live && !settleColumn(wg.x, base, cx, cz, false, true, li)) {
    atomicOr(&wgBad, 1u);
  }
  workgroupBarrier();   // every column's record is now readable

  // ---- test 2: excite stability -------------------------------------------
  // Skipped once the block is already doomed: the answer cannot change and
  // the walk is the expensive part.
  if (live && atomicLoad(&wgBad) == 0u) {
    var unstable = false;
    for (var y = 0; y < SETTLE_LEVELS && !unstable; y++) {
      let full = wgFillAt(li, y);
      if (full == 0u) { continue; }
      // SCOPE: only cells this settle CREATES may veto it. Settle is
      // all-or-nothing per 16x16x16 block, and the column walk rewrites every
      // cell it passes — including standing water it merely re-places
      // unchanged. Letting those vote made the veto over-reach badly: a block
      // holding one perched pre-existing film refused to convert the NEW water
      // everywhere else in it, forever, and refusing could never fix the film
      // because the settle was not what put it there. Measured on the sealed
      // fluid-excite chamber, whose drained floor is exactly this shape: 14 of
      // 18 picks refused unstable, 3,683 of 4,056 eighths stranded as
      // particles. The hysteresis guarantee is unaffected — it is a statement
      // about the cells settle CREATES, and an untouched cell is not one.
      if (wgSameAt(li, y)) { continue; }
      // WHO OWNS SETTLED LIQUID decides how strict this is, and both arms are
      // the same question — "will this configuration still move?".
      //   exciteMode 1: the MPM owns it. Refuse anything excite would take
      //     straight back, which is the full hysteresis guarantee.
      //   exciteMode 0 (stock, and how the mpm dev tool places water): the CA
      //     owns settled liquid, and the CA CAN move a perched cell — it
      //     spreads laterally into air and takes down-diagonals — as long as
      //     the cell holds at least 2 eighths (sim_step's `if (f >= 2u)` gate,
      //     the CA's own lateral-spread threshold). Refusing those does not
      //     prevent a freeze, it CAUSES one: a film thinner than the solver's
      //     3-cell B-spline support gathers rho << rest, so its EOS pressure
      //     is zero and the MPM cannot move it either. Measured on the hill
      //     scene: a puddle on every single tread of the ramp, 49% of the pour,
      //     inert as particles for the 440 ticks after the pour stopped.
      //     Below 2 eighths the CA cannot spread it either, so that IS a
      //     freeze and stays refused in both modes.
      if (T.fluidExciteEnable == 0u && full >= 2u) { continue; }
      // BASE cells only: the bottom of each contiguous body of water in this
      // column (see seamLateralExcite). At y = 0 the cell below is outside the
      // block, so it comes from the voxels/node grid like any other
      // out-of-block probe.
      if (y > 0) {
        if (wgFillAt(li, y - 1) != 0u) { continue; }
      } else if (seamNeighbourState(base + vec3<i32>(cx, -1, cz)).y != 0u) {
        continue;
      }
      for (var d = 0u; d < 4u; d++) {
        let off = seamLateral(d);
        let nx = cx + off.x;
        let nz = cz + off.z;
        var nb : vec2<u32>;
        var bel : vec2<u32>;
        if (nx >= 0 && nx < 16 && nz >= 0 && nz < 16) {
          // In-block: the neighbour column's POST-settle answer.
          let ncol = u32(nz * 16 + nx);
          nb = vec2<u32>(select(0u, 1u, wgBlockedAt(ncol, y)),
                         wgFillAt(ncol, y));
          if (y > 0) {
            bel = vec2<u32>(select(0u, 1u, wgBlockedAt(ncol, y - 1)),
                            wgFillAt(ncol, y - 1));
          } else {
            bel = seamNeighbourState(base + vec3<i32>(nx, -1, nz));
          }
        } else {
          // Out of block: voxels + excited node mass, both already final.
          nb = seamNeighbourState(base + vec3<i32>(nx, y, nz));
          bel = seamNeighbourState(base + vec3<i32>(nx, y - 1, nz));
        }
        if (seamLateralExcite(nb, bel)) {
          unstable = true;
          break;
        }
      }
    }
    // PER-COLUMN refusal. Publishing the vote as a bit rather than folding it
    // into the block verdict is what stops one perched column from vetoing
    // the other 255: settleCommit skips exactly these columns and settleKill
    // spares exactly their particles, so the refused water stays particles
    // and the mass ledger still balances column by column.
    if (unstable) {
      atomicOr(&settleScratch[SP_COLBAD + wg.x * 8u + li / 32u],
               1u << (li % 32u));
      atomicOr(&wgUnstable, 1u);
    }
  }
  workgroupBarrier();

  let infeasible = atomicLoad(&wgBad) != 0u;
  let unstableBlk = atomicLoad(&wgUnstable) != 0u;
  // Only INFEASIBILITY refuses the whole block. Instability is now per-column
  // (above), so an unstable block still commits every stable column it has;
  // the counter is kept because "how much did the veto take" is the number
  // WP3 diagnoses with.
  if (live && li == 0u && unstableBlk && !infeasible) {
    atomicAdd(&fluidArgs[FA_SETUNSTABLE], 1u);
    // A block that lost columns to the veto has NOT finished converting, so
    // it must not bank a fresh calm window against unchanged geometry
    // (WP3 item 3's cooldown, same halving as a full refusal).
    atomicStore(&fluidCalm[ci], SEAM_CALM_TICKS / 2u);
  }
  if (live && li == 0u && infeasible) {
    atomicOr(&settleScratch[SP_MARK + ci], MARK_REFUSED);
    // Refused blocks HALVE their calm window rather than zeroing it (WP3 item
    // 3). Zeroing made a geometrically-awkward pool re-run the full 45-tick
    // bin/check/refuse cycle forever against unchanging geometry; halving
    // keeps a real cooldown (rule 2) while still letting a block that only
    // just missed retry soon.
    atomicStore(&fluidCalm[ci], SEAM_CALM_TICKS / 2u);
    // Two counters, because they are opposite diagnoses: INFEASIBLE means the
    // column arithmetic did not fit (geometry/coverage), UNSTABLE means it fit
    // and would have wanted to move again immediately (WP3's whole point).
    atomicAdd(&fluidArgs[FA_SETREFUSED], 1u);
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
  // Columns the stability test refused keep their water as particles. Their
  // bins are simply never drained and their voxels never written, which is
  // why the ledger stays exact without any special case: settleKill spares
  // the same particles by the same bit.
  if (!seamColumnRefused(wg.x, li)) {
    settleColumn(wg.x, base, cx, cz, true, false, li);
  }
  if (li == 0u) { atomicStore(&fluidCalm[ci], 0u); }
}

// mirrorFold: pack excited-fluid occupancy (eighths per cell, from the last
// substep's node mass) for the 27 CPU-mirror chunks, so the player's
// swimming query sees particles the way it sees fullness voxels (plan §6.5
// — `inLiquid` is a fraction, and node mass gives one naturally). One
// workgroup per mirror chunk; 4 cells pack into each written word.
@compute @workgroup_size(256)
fn mirrorFold(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let m = wg.x;
  let wc = T.mirrorBase + vec3<i32>(i32(m % 3u), i32((m / 3u) % 3u),
                                    i32(m / 9u));
  var bm = 0u;
  if (chunkInWindow(wc, T.origin)) {
    bm = atomicLoad(&fluidBlockMapR[chunkSlotIndex(wc)]);
  }
  for (var wpos = li * 4u; wpos < li * 4u + 4u; wpos++) {
    var packed = 0u;
    if (bm != 0u) {
      for (var b = 0u; b < 4u; b++) {
        let cellIdx = wpos * 4u + b;
        let mass = fluidGridR[((bm - 1u) * CHUNK_VOL + cellIdx) * FLUID_GW];
        let e = clamp(mass >> 10u, 0, 8);   // Q10 mass -> whole particles
        packed |= u32(e) << (b * 8u);
      }
    }
    fluidMirror[m * (CHUNK_VOL / 4u) + wpos] = packed;
  }
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
  // Per-column veto: this particle's own column may have been refused while
  // the rest of the block committed. Same bit settleCommit consulted.
  let lo = vec3<u32>(cell & vec3<i32>(CHUNK_MASK));
  if (seamColumnRefused(mark & MARK_LIST_MASK, lo.z * CHUNK + lo.x)) { return; }
  p.attr = 0u;
  fluidParticles[gid.x] = p;
  atomicAdd(&fluidArgs[FA_DEAD], 1u);
}
