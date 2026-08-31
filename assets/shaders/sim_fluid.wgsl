// sim_fluid.wgsl — fixed-point MLS-MPM liquid (docs/PLAN_mpm_fluids.md).
//
// An EXPERIMENTAL second liquid representation running alongside the CA liquid
// so the two can be compared in-world before either replaces the other. The
// core is the MLS-MPM of Hu et al. 2018, restructured the way grantkot's
// WebGPU liquid demos are: P2G is split into a mass/momentum scatter (p2g1)
// and a stress scatter (p2g2) so that pressure can come from the REAL local
// density sampled off the grid — pressure = stiffness * ((rho/rest)^power - 1),
// floored at -cohesion — instead of from a per-particle volume ratio J. The
// density EOS is what stops a small cavity swallowing unbounded particles:
// over-packing now builds real ejecting pressure rather than saturating a
// clamped J. p2g2 also applies dynamic viscosity (via the APIC C matrix) and
// the per-species attraction terms (attract same / attract different).
//
// DETERMINISM. The fluid never writes a voxel and no CA kernel reads a fluid
// buffer, so the WORLD hash cannot move (verified by the pinned determinism
// gate staying at its baseline). The fluid's own state is bit-deterministic by
// the same discipline the CA uses, and the fluid_det gate verifies it twice-run:
//   * integer-only math end to end — no f32 anywhere in this file;
//   * every grid scatter accumulates via i32 atomicAdd: integer addition is
//     associative, so the sum cannot depend on workgroup scheduling (this is
//     the plan's §5.1 fixed-point-atomics bet, and the whole point of Phase 0);
//   * p2g2's density gather reads only words p2g1 finished writing (the pass
//     barrier orders them) and adds only to momentum words — different words,
//     both associative, so the interleaving cannot matter;
//   * block indices are assigned by a single-workgroup scan in slot order
//     (deterministic), never by first-come atomics;
//   * per-particle work is keyed on particle state only, never on dispatch or
//     buffer order.
//
// THE SUBSTEP (recorded kFluidSubsteps times per tick from EncodeTick):
//   fill blockMap=0 -> mark -> alloc(scan) -> copy args -> clear ->
//   p2g1 (mass+momentum+species mass) -> p2g2 (density -> stress scatter) ->
//   grid update (gravity + terrain BCs) -> G2P/advect
// Terrain collision is a boundary condition on grid nodes read straight from
// the voxel buffer through voxWordAt — dig under a pool and it drains with no
// coupling code (plan §6.1). Out-of-window space is solid and inert, exactly
// like the CA. A particle whose chunk lost its block (budget overflow) or left
// the window simply freezes: it gathers no velocity and advects nowhere until
// coverage returns — deterministic and mass-preserving.
//
// FIXED-POINT FORMATS (see the FluidParticle block in common.wgsl):
//   position Q16.16 world cells | velocity Q16.16 cells/tick | C Q16.16 /tick
//   J Q16 (diagnostic only) | node mass Q10 (particle mass 1.0 = 1024) |
//   node momentum Q16.16 | particle density Q16.16 masses/cell
// Grid nodes sit at CELL CENTERS: node n's collision cell is cell n, and a
// particle at x uses base = floor(x - 1.0) in cell coords with quadratic
// B-spline weights of fx = (x - 0.5) - base in [0.5, 1.5].
//
// GRID LAYOUT: 8 i32 words per node —
//   [0] mass (Q10)   [1..3] momentum -> velocity (Q16.16)
//   [4..6] mass of species 1..3 (Q10; species 0 mass = [0] - [4] - [5] - [6])
//   [7] FOAM FIELD (Q16, saturated at 1.0) — the ONLY word that is NOT
//       cleared per substep. It accumulates the Ihmsen diffuse-material
//       potentials in g2p and decays geometrically in gridUpdate, so foam
//       persists in the wake of a splash instead of tracking the instantaneous
//       velocity. The renderer samples it trilinearly as the surface's white.
// world.cpp sizes fluidGrid by FLUID_GW; keep them in step.
//
// Overflow audit (worst cases, all < 2^31): node mass 2000 crowded particles
// x 432 = 8.6e5 (Q10); rho gathered from that as Q16.16 tops out < 2^26 and is
// clamped to 4*rest (rest <= 32.0, so 2^23) before the ratio staging
// (rho << 7 <= 2^30). The powered ratio is capped at 64.0, the raw pressure at
// [-2^20, 2^25], and the volume-folded stress matrix entries at FLUID_VEFF_MAX
// — every mq() operand stays under 2^21 and every staged product under 2^31.
// Velocities are CFL-clamped to FLUID_VMAX (0.45 cell/substep).

@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;

@group(1) @binding(0) var<storage, read_write> fluidParticles : array<FluidParticle>;
@group(1) @binding(2) var<storage, read_write> fluidBlockMap : array<atomic<u32>>;
@group(1) @binding(3) var<storage, read_write> fluidBlockList : array<u32>;
@group(1) @binding(4) var<storage, read_write> fluidGrid : array<atomic<i32>>;
// Atomic like the seam's view of the same buffer: gridUpdate counts VMAX clamp
// engagements into FA_CLAMPED (plan §5 item 1's CFL-honesty probe), and an
// atomicAdd needs the atomic type. alloc's scans store through it instead of
// plain-assigning; same words, same values.
@group(1) @binding(5) var<storage, read_write> fluidArgs : array<atomic<u32>>;
// Splash coupling: g2p appends micro droplets into the ballistic particle
// system's WRITE page (this tick's, which is next tick's read page — the fluid
// substeps run after particleResolve). Same append idiom sim_particle.wgsl
// uses; behaviour keys on particle state, never on the slot the atomicAdd
// hands out, so the grid stays deterministic (DESIGN.md §2/§4).
@group(1) @binding(6) var<storage, read_write> pWrite : array<Particle>;
@group(1) @binding(7) var<storage, read_write> counts : array<atomic<u32>>;

// ---- human-unit tuning -> per-tick fixed point ------------------------------
// The fluid tuning values arrive from tuning.json in HUMAN units (voxels/s²,
// voxels/s, vox²/s, seconds — see the MPM Fluid tuner section). They are
// converted here, ONCE, at shader compile time: WGSL const-expressions are
// folded by Tint under IEEE-exact rules, so the same tuning.json produces the
// same integer constants on every machine and the kernel below stays
// integer-only (rule 1 discipline — no runtime f32 ever touches fluid state).
// 30 Hz tick, FLUID_SUBSTEPS substeps; internal units are cells and ticks.
const FLUID_GRAVITY : i32 =                    // vox/s² -> Q16.16 cells/tick²
    i32(round(TUNE_FLUID_GRAVITY * 65536.0 / 900.0));
const FLUID_STIFFNESS : i32 =                  // (vox/s)² -> Q16.16 cells²/tick²
    i32(round(TUNE_FLUID_STIFFNESS * 65536.0 / 900.0));
const FLUID_REST_DENSITY : i32 =               // particles/voxel -> Q16.16
    i32(round(TUNE_FLUID_REST_DENSITY * 65536.0));
const FLUID_COHESION : i32 =                   // (vox/s)² -> Q16.16 cells²/tick²
    i32(round(TUNE_FLUID_COHESION * 65536.0 / 900.0));
const FLUID_ATTRACT_SAME : i32 =               // (vox/s)² -> Q16.16 cells²/tick²
    i32(round(TUNE_FLUID_ATTRACT_SAME * 65536.0 / 900.0));
const FLUID_ATTRACT_DIFF : i32 =               // (vox/s)² -> Q16.16 cells²/tick²
    i32(round(TUNE_FLUID_ATTRACT_DIFF * 65536.0 / 900.0));
const FLUID_VISCOSITY : i32 =                  // vox²/s -> Q16.16 cells²/tick
    i32(round(TUNE_FLUID_VISCOSITY * 65536.0 / 30.0));
const FLUID_DAMPING : i32 =                    // fraction/s -> Q16.16 /tick
    i32(round(TUNE_FLUID_DAMPING * 65536.0 / 30.0));
// Wall friction: fraction/s of TANGENTIAL velocity shed per second of solid
// contact, applied per SUBSTEP in gridUpdate's separate BC (so it compounds to
// the per-second rate over a tick of continuous contact). 0 = free-slip, the
// water default; the knob exists for mud/goo authoring (per-material JSON is
// plan §6.1's job). The kernel branch is compiled out entirely at 0 — mq() by
// 1.0 is NOT the identity (it truncates 6 low bits), so a guard, not a blend.
const FLUID_FRICTION : i32 =                   // fraction/s -> Q16 /substep
    i32(round(TUNE_FLUID_FRICTION * 65536.0 / (30.0 * f32(FLUID_SUBSTEPS))));
// Splash droplets (see the g2p emission block): rate is droplets/second per
// eligible particle spread over the substeps, speed the eligibility threshold.
const FLUID_SPLASH_CHANCE : i32 =              // per-substep probability, Q16
    i32(round(TUNE_FLUID_SPLASH_RATE * 65536.0 / (30.0 * f32(FLUID_SUBSTEPS))));
const FLUID_SPLASH_SPEED : i32 =               // vox/s -> Q16.16 cells/tick
    i32(round(TUNE_FLUID_SPLASH_SPEED * 65536.0 / 30.0));
const FLUID_SPLASH_MAX_RHO : i32 =             // fraction of rest -> Q16.16
    i32(round(TUNE_FLUID_SPLASH_MAXDENS * TUNE_FLUID_REST_DENSITY * 65536.0));
const FLUID_SPLASH_LIFE : u32 =                // seconds -> ticks (life field)
    u32(clamp(TUNE_FLUID_SPLASH_LIFE * 30.0, 1.0, 255.0));

// ---- diffuse material: spray / foam / bubbles (Ihmsen et al. 2012) ---------
// Same const-eval discipline as the rows above: every human-unit knob becomes
// an integer here at SHADER COMPILE TIME, so the kernel is integer-only and
// bit-deterministic (rule 1). LoadTuning has already ordered every min/max
// pair, so none of these divisions can be by zero.
//
// Trapped-air thresholds: vox/s -> Q16.16 cells/tick.
const FLUID_TA_MIN : i32 = i32(round(TUNE_FLUID_TRAPPED_MIN * 65536.0 / 30.0));
const FLUID_TA_MAX : i32 = i32(round(TUNE_FLUID_TRAPPED_MAX * 65536.0 / 30.0));
// Wave-crest thresholds are dimensionless; keep them in Q16.16 too.
const FLUID_WC_MIN : i32 = i32(round(TUNE_FLUID_CREST_MIN * 65536.0));
const FLUID_WC_MAX : i32 = i32(round(TUNE_FLUID_CREST_MAX * 65536.0));
// Kinetic-energy thresholds: (vox/s)^2 -> (Q16.16 cells/tick)^2 measured the
// same way g2p measures speed², i.e. on (v >> 8) so the square fits i32.
// (vox/s)² -> (cells/tick)² is /900; then (Q16.16 >> 8) scales by (256)².
const FLUID_EK_MIN : i32 =
    i32(round(TUNE_FLUID_FOAM_EMIN * 65536.0 * 65536.0 / (900.0 * 65536.0)));
const FLUID_EK_MAX : i32 =
    i32(round(TUNE_FLUID_FOAM_EMAX * 65536.0 * 65536.0 / (900.0 * 65536.0)));
// Generation rates (Eq. 8): particles/second -> per-substep probability in
// Q16, i.e. the chance THIS substep that this particle sheds one foam particle
// at full potential. Bounded by construction (rule 2): expected foam per tick
// = (kta + kwc) / 30 per ELIGIBLE particle, and eligibility needs all three
// potentials non-zero, which a settled pool never has.
const FLUID_FOAM_KTA : i32 =
    i32(round(TUNE_FLUID_FOAM_RATE * 65536.0 / (30.0 * f32(FLUID_SUBSTEPS))));
const FLUID_FOAM_KWC : i32 =
    i32(round(TUNE_FLUID_FOAM_CREST_RATE * 65536.0 / (30.0 * f32(FLUID_SUBSTEPS))));
// Foam lifetime band, ticks. The paper sets lifetime in proportion to the
// generation potential so dense clusters outlive isolated specks.
const FLUID_FOAM_LIFE : u32 =
    u32(clamp(TUNE_FLUID_FOAM_LIFE * 30.0, 1.0, 255.0));
const FLUID_FOAM_LIFE_MIN : u32 =
    u32(clamp(TUNE_FLUID_FOAM_LIFE_MIN * 30.0, 1.0, 255.0));
// Classification thresholds on the density the solver already gathered. The
// paper classifies by NEIGHBOUR COUNT; density is the same measurement with
// the kernel weights already applied, and it costs nothing extra here.
const FLUID_BUBBLE_RHO : i32 =
    i32(round(TUNE_FLUID_BUBBLE_RHO * TUNE_FLUID_REST_DENSITY * 65536.0));
const FLUID_SPRAY_RHO : i32 =
    i32(round(TUNE_FLUID_SPRAY_RHO * TUNE_FLUID_REST_DENSITY * 65536.0));
// Foam field: what a full-potential emission adds to grid word 7, Q16. The
// field saturates at FLUID_FOAM_FULL and decays by FOAM_DECAY per tick, so a
// churning region builds a persistent white patch that then fades — this is
// what makes foam TRAIL a splash instead of blinking with the velocity.
const FLUID_FOAM_FULL : i32 = 65536;
const FLUID_FOAM_GAIN : i32 = 26214;   // 0.4 per full-potential substep
// Per-SUBSTEP survival, Q16. Applied once per substep in gridUpdate, it
// compounds over FLUID_SUBSTEPS to the per-tick survival implied by the foam
// lifetime — so the tuner's "seconds" really are seconds. Expressed as the
// substep-th root via exp/log at const-eval time (the shader compiler folds
// it; no float ever reaches the kernel).
const FLUID_FOAM_DECAY : i32 = i32(round(65536.0 *
    exp(log(max(1.0 - 1.0 / max(TUNE_FLUID_FOAM_LIFE * 30.0, 2.0), 0.001)) /
        f32(FLUID_SUBSTEPS))));
// ---- wind on the grid (docs/RESEARCH_wind.md §4.6, phase 3) ----------------
// Same const-eval discipline as every row above: human units in tuning.json,
// integers here, nothing but i32 in the kernel.
//
// Fraction of the gap between a node's velocity and the local wind closed per
// SUBSTEP. Divided by the substep count for the same reason the gravity add is
// — the knob is a per-second rate and must mean that at any substep budget.
const FLUID_WIND_DRAG : i32 = i32(round(clamp(TUNE_WIND_DRAG, 0.0, 30.0) *
    65536.0 / (30.0 * f32(FLUID_SUBSTEPS))));
// How much of the field a fully exposed node feels, Q16.
const FLUID_WIND_GAIN : i32 =
    i32(round(clamp(TUNE_WIND_FLUID_GAIN, 0.0, 4.0) * 65536.0));
// The exposure band, in the Q10 particle-masses word 0 is accumulated in
// (p2g1 scatters `w >> 6`). A node buried in fluid at rest density carries
// REST_DENSITY particle masses, which in Q10 is FLUID_REST_DENSITY >> 6; the
// knob is a fraction of that. Wind fades to nothing as a node approaches it.
//
// THIS IS §8's OPEN QUESTION, ANSWERED: low-mass nodes only, not all of them.
// Wind on every node of a pond is a CURRENT — the whole body translates, the
// surface stays flat, and it looks like the lake is being poured sideways.
// What wind actually does to water is act on the interface: it drags the skin,
// it carries spray, and the body below follows only through the fluid's own
// viscosity. Gating on node mass gets that for free, because "how much fluid
// is around this node" is a number the solver has already computed.
const FLUID_WIND_MASS : i32 = max(i32(round(
    clamp(TUNE_WIND_FLUID_MASS, 0.02, 4.0) * f32(FLUID_REST_DENSITY >> 6u))), 1);
// Q16.16 cells/s (windAtQ's unit) -> Q16.16 cells/tick.
const FLUID_WIND_HZ : i32 = 30;
// THE CURRENT FIELD's one sim coupling (docs/PLAN_water_master.md component 8).
// Same shape and same units as FLUID_WIND_DRAG above — a per-SECOND rate, so
// divided by the substep count for the reason the gravity add is. There is no
// exposure gate and no ramp: air touches the skin of a body of water, a current
// runs through it, and that difference is why this is a second term rather than
// a second weather vector.
const FLUID_CURRENT_DRAG : i32 = i32(round(clamp(TUNE_CURRENT_DRAG, 0.0, 30.0) *
    65536.0 / (30.0 * f32(FLUID_SUBSTEPS))));

// 0.5 cell in Q16.16 — the cell-center offset of the node lattice.
const FLUID_HALF : i32 = 32768;
// FLUID_VMAX (the CFL cap, 0.45 cell/substep expressed in cells/TICK) and
// FLUID_MARK_PAD now live in common.wgsl next to FLUID_SUBSTEPS, because all
// three are the same decision and the substep count is a tuning knob.
// Bound on the fused velocity+stress+affine term before the staged multiply.
// 6.0 cells/tick at the historical 6 substeps; scales with the CFL cap so a
// higher substep budget does not clip terms the solver is now allowed to
// produce (2*VMAX is the worst fused magnitude). The `max` keeps 6 substeps
// bit-identical to before the substep knob existed. Overflow audit: at the
// 32-substep ceiling this is 1,887,436 < 2^21, so every mq() operand still
// obeys the file-header bound.
const FLUID_VEFF_MAX : i32 = max(393216, FLUID_VMAX * 2);
// Affine matrix clamp, Q16.16 per tick. A velocity difference of VMAX across
// one cell IS a C of VMAX, so the clamp must not sit below it.
const FLUID_CMAX : i32 = max(262144, FLUID_VMAX);
// J clamps, Q16: [0.6, 1.4]. J is diagnostic now (the EOS reads density), but
// the clamp keeps the fluid-det sanity band meaningful.
const FLUID_JMIN : i32 = 39322;
const FLUID_JMAX : i32 = 91750;
// Node mass floor, Q10 (~0.016 particle masses): below this the node's
// momentum-to-velocity division would amplify noise on a node that cannot
// influence any particle meaningfully anyway.
const FLUID_MASS_MIN : i32 = 16;
// Settled-liquid static mass, as a Q8 fraction of rest density (sim.
// fluidSettledMass; see the seedSettledMass block in clearGrid). 0 compiles the
// whole mechanism out and restores WP4's pass-through behaviour exactly.
const FLUID_SETTLED_Q8 : i32 =
    i32(round(clamp(TUNE_FLUID_SETTLED_MASS, 0.0, 2.0) * 256.0));
// Gravity, EOS (stiffness / rest density / power / cohesion), the species
// attraction pair, viscosity and damping all come from tuning (sim.* —
// integer, F5-reloadable, the "fluid" rows of tuning_params.def). LoadTuning
// clamps every one of them into the ranges the overflow audit above assumes.

// Q16.16 multiply, exact to ~2^-4 fixed-point units, valid for |a|,|b| < 2^21.
// The staging (>>6, >>6, >>4) keeps the product inside i32. Computed on
// MAGNITUDES with the sign restored afterwards: an arithmetic-shift truncation
// rounds toward -inf, which biased every product slightly negative and showed
// up as the whole fluid creeping along the -x/-y/-z diagonal on a flat floor.
// Truncating the magnitude rounds toward zero — symmetric, and identical on
// every device (rule 1 cares about sameness, not the rounding mode).
fn mq(a : i32, b : i32) -> i32 {
  let m = ((abs(a) >> 6u) * (abs(b) >> 6u)) >> 4u;
  return select(m, -m, (a ^ b) < 0);
}

// The paper's clamping function (Eq. 1), in Q16.16 throughout:
//   Phi(I, tmin, tmax) = (min(I,tmax) - min(I,tmin)) / (tmax - tmin)
// Result is Q16 in [0, 65536]. LoadTuning guarantees tmax > tmin, so the
// divisor is never zero; the shift keeps the numerator inside i32 for the
// widest threshold pair the tuner allows.
// Signed wrapper over common.wgsl's exact integer isqrt. The foam potentials
// need a magnitude from a squared sum, and rule 1 forbids f32 in the CA: a
// hardware sqrt is free to differ in the last ulp between vendors, which is
// precisely the class of divergence the pinned world hash exists to catch.
fn isqrtI(x : i32) -> i32 {
  if (x <= 0) { return 0; }
  return i32(isqrt(u32(x)));
}

fn phiQ(v : i32, tmin : i32, tmax : i32) -> i32 {
  let num = min(v, tmax) - min(v, tmin);
  let den = max(tmax - tmin, 1);
  if (num <= 0) { return 0; }
  return clamp((num >> 4u) * 65536 / max(den >> 4u, 1), 0, 65536);
}

// Quadratic B-spline weights of fx in [0.5, 1.5] (Q16.16 in, Q16.16 out).
//   w0 = 0.5*(1.5-fx)^2   w1 = 0.75-(fx-1)^2   w2 = 0.5*(fx-0.5)^2
// Squared on the magnitude for the same symmetry reason as mq: (fx-1) crosses
// zero, and flooring a negative operand made w1 lopsided across the node.
fn wsqHalf(a : i32) -> i32 { let u = abs(a) >> 8u; return (u * u) >> 1u; }
fn wsq(a : i32) -> i32 { let u = abs(a) >> 8u; return u * u; }

struct Axis {
  base : i32,          // first node (cell coord) of the 3-node support
  w : vec3<i32>,       // Q16.16 weights for nodes base+0..2
  fx : i32,            // Q16.16 offset of the particle from `base`, in [0.5,1.5]
};

fn axisOf(p : i32) -> Axis {
  let xs = p - FLUID_HALF;              // node-index space (node n at coord n)
  let base = (xs - FLUID_HALF) >> 16u;  // arithmetic shift = floor
  let fx = xs - (base << 16u);          // Q16.16 in [0.5, 1.5]
  var a : Axis;
  a.base = base;
  a.fx = fx;
  a.w = vec3<i32>(wsqHalf(98304 - fx), 49152 - wsq(fx - 65536), wsqHalf(fx - 32768));
  return a;
}

// blockMap entry for the chunk holding node cell nc, or 0 when the node has no
// block this substep (out of window, unmarked, or past the block budget).
fn nodeBlock(nc : vec3<i32>) -> u32 {
  let wc = worldChunkOf(nc);
  if (!chunkInWindow(wc, T.origin)) { return 0u; }
  return atomicLoad(&fluidBlockMap[chunkSlotIndex(wc)]);
}

// First WORD of node nc's accumulator row (block bm), i.e. index * FLUID_GW.
fn nodeWordBase(bm : u32, nc : vec3<i32>) -> u32 {
  let lo = vec3<u32>(nc & vec3<i32>(CHUNK_MASK));
  return ((bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x) * FLUID_GW;
}

// Terrain for the grid boundary condition: solids and powders block; CA
// liquids and gases do not. A settled liquid is NOT a wall — it is a fluid, and
// the way it stops a particle is by weighing something, which is what
// seedSettledMass below gives it. Out-of-window is solid and inert.
fn fluidSolid(c : vec3<i32>) -> bool {
  if (!inWindow(c, T.origin)) { return true; }
  let mat = voxMat(voxWordAt(c));
  if (mat == MAT_AIR) { return false; }
  let k = materials[mat].klass;
  return k == CLASS_SOLID || k == CLASS_POWDER;
}

// The live particle population is GPU-OWNED now (the seam's compaction /
// spawn / excite passes maintain fluidArgs[FA_LIVE] — sim_fluid_seam.wgsl).
// Spawning moved there too (spawnAppend): with settle deleting particles and
// excite creating them on the GPU, no CPU-known base exists any more.
fn liveTotal() -> u32 { return min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP); }

// ---- mark: flag every chunk the particle's node support can touch THIS TICK -
// atomicOr of a constant is order-independent; the map is cleared by a Fill
// row at the top of PT_FLUIDMAP, which runs ONCE per tick.
//
// THE PAD. The map used to be rebuilt before every substep, so marking the
// instantaneous 3-cell node support was exact. Building it once per tick
// (plan §7 item 4) means it must also cover where the particle will BE by the
// last substep. Displacement is bounded by the CFL clamp: g2p advects
// v / FLUID_SUBSTEPS per substep with |v| <= FLUID_VMAX (0.45 cell/substep x
// FLUID_SUBSTEPS), so a particle moves at most 0.45*substeps cells over the
// whole tick, whatever the pressure field does to it in between.
// FLUID_MARK_PAD (common.wgsl, derived from the substep knob) is exactly that
// rounded up: 3 cells at the historical 6 substeps, 5 at the 9-substep
// default. The padded span is [base-pad, base+2+pad] and the pad is capped so
// that stays <= CHUNK, which is what keeps the 8-corner loop below exhaustive.

@compute @workgroup_size(64)
fn mark(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveTotal()) { return; }
  let p = fluidParticles[gid.x];
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }  // frozen out-of-window
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);
  let lo = vec3<i32>(ax.base, ay.base, az.base) - vec3<i32>(FLUID_MARK_PAD);
  let hi = vec3<i32>(ax.base, ay.base, az.base) +
           vec3<i32>(2 + FLUID_MARK_PAD);
  for (var k = 0; k < 2; k++) {
    for (var j = 0; j < 2; j++) {
      for (var i = 0; i < 2; i++) {
        let corner = vec3<i32>(select(lo.x, hi.x, i == 1),
                               select(lo.y, hi.y, j == 1),
                               select(lo.z, hi.z, k == 1));
        let wc = worldChunkOf(corner);
        if (chunkInWindow(wc, T.origin)) {
          atomicOr(&fluidBlockMap[chunkSlotIndex(wc)], 1u);
        }
      }
    }
  }
}

// ---- alloc: deterministic block assignment + node-pass dispatch args --------
// ONE workgroup. Each thread counts the marks in its 128-slot span, thread 0
// turns the counts into exclusive prefixes (256 sequential adds — trivially
// cheap, trivially deterministic), then each thread assigns indices within its
// span in slot order. Chunks past the kFluidBlocks budget lose their mark and
// their particles freeze for the substep — bounded, deterministic degradation
// rather than an overrun (rule 2).
var<workgroup> allocPartial : array<u32, 256>;
var<workgroup> allocTotal : u32;

@compute @workgroup_size(256)
fn alloc(@builtin(local_invocation_index) li : u32) {
  // TRUE SLEEP (plan §7 item 2). Every other row of this table dispatches off
  // an indirect arg and so costs nothing with no particles; this one is a fixed
  // single-workgroup walk of all NUM_CHUNKS slots (the seam's settleScan is the
  // other). Whether the table is RECORDED stays a pure function of the
  // CPU-owned monotone count — never a readback, that is the determinism trap
  // in plan §7 — so a world that poured once and settled goes on recording
  // these passes forever, and making them free is the only sanctioned fix.
  // With no live particles `mark` wrote nothing, so the map is all zero and the
  // scan below could only ever produce zero.
  // (The early-out must not `return` before the workgroupBarriers below — a
  // storage read is non-uniform to the compiler, and a barrier in non-uniform
  // control flow is a WGSL validation error. Skipping the WORK is enough.)
  let asleep = min(atomicLoad(&fluidArgs[FA_LIVE]), FLUID_CAP) == 0u;
  let span = NUM_CHUNKS / 256u;   // 128 slots per thread
  var n = 0u;
  if (!asleep) {
    for (var s = li * span; s < (li + 1u) * span; s++) {
      if (atomicLoad(&fluidBlockMap[s]) != 0u) { n += 1u; }
    }
  }
  allocPartial[li] = n;
  workgroupBarrier();
  if (li == 0u) {
    var sum = 0u;
    for (var t = 0u; t < 256u; t++) {
      let c = allocPartial[t];
      allocPartial[t] = sum;
      sum += c;
    }
    allocTotal = sum;
    let blocks = min(sum, FLUID_BLOCKS);
    atomicStore(&fluidArgs[0], blocks * (CHUNK_VOL / 256u));  // 16 wg per block
    atomicStore(&fluidArgs[1], 1u);
    atomicStore(&fluidArgs[2], 1u);
    atomicStore(&fluidArgs[3], blocks);
  }
  workgroupBarrier();
  if (asleep) { return; }
  var idx = allocPartial[li];
  for (var s = li * span; s < (li + 1u) * span; s++) {
    if (atomicLoad(&fluidBlockMap[s]) == 0u) { continue; }
    if (idx < FLUID_BLOCKS) {
      atomicStore(&fluidBlockMap[s], idx + 1u);
      fluidBlockList[idx] = s;
    } else {
      atomicStore(&fluidBlockMap[s], 0u);  // over budget: freeze this chunk
    }
    idx += 1u;
  }
}

// ---- clear: zero the active blocks' node accumulators -----------------------
// Only active blocks are touched (indirect over blocks*16 workgroups); stale
// data in inactive blocks is unreachable because every access goes through the
// freshly rebuilt blockMap.
// The FOAM FIELD (word 7) is the one accumulator that must SURVIVE this clear.
// Everything else here is a per-substep scatter target and is meaningless
// carried forward; the foam field is a persistent state variable whose whole
// job is to outlive the event that created it (Ihmsen §3.2 dissolution — foam
// ages out on its own clock rather than tracking the instantaneous velocity).
// Zeroing it every substep, as the first version of this did, is exactly what
// makes foam blink on and off with the flow instead of trailing a wave.
//
// Its decay lives in gridUpdate, NOT here: this kernel has no way to tell one
// substep from another (sim_fluid binds only TickParams, which carries no
// fluid substep index), so decaying here would apply the per-tick rate
// FLUID_SUBSTEPS times and silently divide the tuner's "seconds" by six.
// gridUpdate runs once per substep too, but it uses the per-SUBSTEP rate,
// which compounds to exactly the per-tick rate over a tick.
@compute @workgroup_size(256)
fn clearGrid(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let block = wg.x >> 4u;                        // same addressing as gridUpdate
  let localIdx = (wg.x & 15u) * 256u + li;
  let node = block * CHUNK_VOL + localIdx;       // == wg.x * 256 + li
  let b = node * FLUID_GW;
  for (var w = 0u; w < 7u; w++) {
    atomicStore(&fluidGrid[b + w], 0);
  }

  // ---- SETTLED LIQUID AS STATIC MASS (sim.fluidSettledMass) ---------------
  //
  // THE HOLE THIS FILLS. Up to WP4 the solver could not see settled water at
  // all: fluidSolid() blocks only solids and powders, and a fullness voxel
  // scatters nothing into the grid because it has no particles. So the two
  // representations passed straight through each other, and every symptom of
  // that is a bug someone eventually reports:
  //   * MPM water poured onto a basin filled to the rim fell to the FLOOR. The
  //     basin was, to the solver, an empty box.
  //   * A pool that settles chunk by chunk (settle is a per-block decision, so
  //     a body spanning four chunks converts in four steps) sprayed: the moment
  //     one chunk became voxels its neighbours' particles lost the density that
  //     was holding them up, read rho far below rest on that side, and were
  //     pushed into the hole by the pressure of the water behind them.
  // Both are the same missing term, and neither is fixable on the seam side —
  // no amount of excite/settle tuning can make a body of water support
  // something the solver does not know is there.
  //
  // THE TREATMENT is the standard static-boundary one (Akinci-style boundary
  // mass in SPH, prescribed-mass nodes in MPM): a settled cell seeds its node
  // with `fullness/8 * restDensity` of mass carrying ZERO momentum, BEFORE p2g
  // runs. Everything follows from where that lands in the pipeline:
  //   * p2g2 gathers it into rho, so a particle above a full pool sees rest
  //     density below it and gets real EOS pressure — it floats, and a plunging
  //     jet is ejected back out instead of tunnelling to the bed;
  //   * gridUpdate divides momentum by TOTAL mass, so an impacting jet is
  //     diluted against static mass — that is the drag a still pool applies,
  //     and it is what stops the settle-time collapse;
  //   * it is NOT a prescribed-zero-velocity boundary, on purpose. Leaving the
  //     node velocity as (real momentum / total mass) is what keeps the seam's
  //     WAKE trigger alive: at the impact point real momentum still dominates,
  //     so the splash excites the water it lands on, while a node deep in a
  //     still pool reads ~0 and does not. A crater, not a converted lake.
  //
  // DETERMINISM. Voxels do not change during the substeps (the CA and the seam
  // both run outside PT_FLUID), one thread owns one node, integer throughout,
  // and the value is a pure function of the cell's word — rule 1 holds by the
  // same argument gridUpdate's BC probe already makes.
  //
  // WHY IT IS SEEDED EVERY SUBSTEP rather than cached: word 0 is cleared every
  // substep and the only accumulator that survives the clear is the foam field
  // (word 7). Re-reading one voxel per node is the cheaper half of that trade
  // against widening FLUID_GW off its power-of-two stride.
  //
  // Scope note: this seeds where BLOCKS exist, i.e. within FLUID_MARK_PAD of a
  // live particle. Settled water further away than that is water no particle
  // can reach this tick, so it needs no representation here.
  if (FLUID_SETTLED_Q8 <= 0) { return; }
  let slot = fluidBlockList[block];
  let sc = vec3<i32>(i32(slot % NCHUNK), i32((slot / NCHUNK) % NCHUNK),
                     i32(slot / (NCHUNK * NCHUNK)));
  let lo = vec3<i32>(i32(localIdx & 15u), i32((localIdx >> 4u) & 15u),
                     i32(localIdx >> 8u));
  // Node nc sits at the CENTRE of cell nc (see axisOf / the dpos terms in p2g),
  // so "this node's cell" is an identity, not an approximation.
  let c = slotToWorldChunk(sc, T.origin) * i32(CHUNK) + lo;
  if (!inWindow(c, T.origin)) { return; }
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return; }
  if (materials[mat].klass != CLASS_LIQUID) { return; }
  // A full cell is `rest` in Q16.16, i.e. rest >> 6 in the grid's Q10 mass.
  // Bounds: rest <= 32<<16, so (rest>>6)*8/8 <= 32768 and the Q8 scale (<= 512)
  // keeps the product under 2^24.
  let full = FLUID_REST_DENSITY >> 6u;
  let vm = (((full * i32(voxState(w) + 1u)) / 8) * FLUID_SETTLED_Q8) >> 8u;
  if (vm <= 0) { return; }
  atomicStore(&fluidGrid[b + 0u], vm);
  // Species accounting must match p2g1's, or the same-species attraction terms
  // would read this water as "some other fluid" pressing against itself.
  let sp = (mat - 1u) & 3u;
  if (sp != 0u) { atomicStore(&fluidGrid[b + 3u + sp], vm); }
}

// ---- p2g1: scatter mass, momentum and species mass --------------------------
// grid[node] += w * (v + C * dpos), mass += w, speciesMass[s] += w. Particle
// mass is 1.0 (Q10 1024). No stress here: p2g2 applies it after the density
// this pass accumulates can be sampled.
@compute @workgroup_size(64)
fn p2g1(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveTotal()) { return; }
  let p = fluidParticles[gid.x];
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);

  // ---- loop-invariant work, lifted out of the 27 taps ----------------------
  // Every term below was recomputed inside the innermost body, 27 times, for
  // values that vary over at most 3 or 9 distinct inputs. dpos is SEPARABLE —
  // dx depends only on i, dy only on j, dz only on k — so each axis was being
  // evaluated 9x more often than it has values, and `p.p* - FLUID_HALF` (fully
  // invariant) 27 times.
  //
  // The weight split is the same trick and the one place to be careful: `mq`
  // TRUNCATES (common.wgsl), so it is NOT associative and the tree may not be
  // rebracketed. Precomputing the (i,j) product preserves the exact expression
  // `mq(mq(ax.w[i], ay.w[j]), az.w[k])` — same operands, same order, same
  // rounding — and takes the mq count from 54 to 36. Rebracketing to
  // mq(ax.w[i], mq(ay.w[j], az.w[k])) would be a different number and would
  // move the world hash.
  let pcx = p.px - FLUID_HALF;
  let pcy = p.py - FLUID_HALF;
  let pcz = p.pz - FLUID_HALF;
  var dxs : array<i32, 3>; var dys : array<i32, 3>; var dzs : array<i32, 3>;
  for (var a = 0; a < 3; a++) {
    dxs[a] = ((ax.base + a) << 16u) - pcx;
    dys[a] = ((ay.base + a) << 16u) - pcy;
    dzs[a] = ((az.base + a) << 16u) - pcz;
  }
  var wxy : array<i32, 9>;
  for (var j = 0; j < 3; j++) {
    for (var i = 0; i < 3; i++) { wxy[j * 3 + i] = mq(ax.w[i], ay.w[j]); }
  }

  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(wxy[j * 3 + i], az.w[k]);   // Q16.16 <= 0.42
        // dpos = node - particle, Q16.16 in [-1.5, 1.5] per axis.
        let dx = dxs[i];
        let dy = dys[j];
        let dz = dzs[k];
        var vex = p.vx + mq(p.c00, dx) + mq(p.c01, dy) + mq(p.c02, dz);
        var vey = p.vy + mq(p.c10, dx) + mq(p.c11, dy) + mq(p.c12, dz);
        var vez = p.vz + mq(p.c20, dx) + mq(p.c21, dy) + mq(p.c22, dz);
        vex = clamp(vex, -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        vey = clamp(vey, -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        vez = clamp(vez, -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        let ni = nodeWordBase(bm, nc);
        atomicAdd(&fluidGrid[ni + 0u], w >> 6u);        // mass, Q10
        atomicAdd(&fluidGrid[ni + 1u], mq(w, vex));     // momentum, Q16.16
        atomicAdd(&fluidGrid[ni + 2u], mq(w, vey));
        atomicAdd(&fluidGrid[ni + 3u], mq(w, vez));
        if (p.species != 0u) {
          atomicAdd(&fluidGrid[ni + 3u + p.species], w >> 6u);
        }
      }
    }
  }
}

// (rho/rest) as Q16.16. m is clamped to 4*rest (<= 2^23) by the caller, so
// (m >> 4) << 11 = m << 7 stays under 2^30; rest >> 9 >= 128 because
// LoadTuning clamps rest to [1.0, 32.0].
fn densityRatio(m : i32, rest : i32) -> i32 {
  return ((m >> 4u) << 11u) / max(rest >> 9u, 1);
}

// ---- p2g2: sample density, scatter the stress (EOS + viscosity + attraction)
// The grantkot two-pass shape: with p2g1's mass field complete (the pass
// barrier orders it), each particle gathers its true local density and
// scatters the resulting stress as momentum. Reads mass words / adds to
// momentum words — disjoint, both order-independent. Also writes p.density
// (render shading), which is this pass's only particle write.
@compute @workgroup_size(64)
fn p2g2(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveTotal()) { return; }
  var p = fluidParticles[gid.x];
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);

  // The (i,j) weight product, hoisted out of BOTH 27-tap loops below — see the
  // note in p2g1. mq truncates and is not associative, so this precomputes the
  // inner bracket rather than rebracketing the expression. p2g2 runs the
  // 3x3x3 walk TWICE, so the 18 saved mq are saved twice per particle.
  var wxy : array<i32, 9>;
  for (var j = 0; j < 3; j++) {
    for (var i = 0; i < 3; i++) { wxy[j * 3 + i] = mq(ax.w[i], ay.w[j]); }
  }

  // Gather rho and same-species rho (Q16.16 particle masses per cell).
  var rho : i32 = 0;
  var same : i32 = 0;
  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(wxy[j * 3 + i], az.w[k]);
        let ni = nodeWordBase(bm, nc);
        let m = atomicLoad(&fluidGrid[ni + 0u]);
        let m1 = atomicLoad(&fluidGrid[ni + 4u]);
        let m2 = atomicLoad(&fluidGrid[ni + 5u]);
        let m3 = atomicLoad(&fluidGrid[ni + 6u]);
        var own : i32;
        switch (p.species) {
          case 0u: { own = m - m1 - m2 - m3; }
          case 1u: { own = m1; }
          case 2u: { own = m2; }
          default: { own = m3; }
        }
        rho += mq(w, m << 6u);            // Q10 mass -> Q16.16
        same += mq(w, max(own, 0) << 6u);
      }
    }
  }
  p.density = rho;
  fluidParticles[gid.x] = p;

  // EOS: pressure = stiffness * ((rho/rest)^power - 1), floored at -cohesion
  // so a free surface pulls itself together instead of tearing apart.
  let rest = clamp(FLUID_REST_DENSITY, 1 << 16, 32 << 16);
  let ratio = densityRatio(clamp(rho, 0, rest * 4), rest);
  let sameRatio = densityRatio(clamp(same, 0, rest * 4), rest);
  let otherRatio = max(ratio - sameRatio, 0);
  var pw = ratio;
  for (var e = 1; e < clamp(TUNE_FLUID_EOS_POWER, 1, 7); e++) {
    pw = min(mq(pw, ratio), 1 << 22);     // ratio^power, capped at 64.0
  }
  // Hydrostatic blend (seam O-4): a particle pre-compressed at excite time
  // (J < 1, sim_fluid_seam.wgsl) contributes (1 - J) of extra ratio, so a
  // reawakened deep column pushes back against its own weight from substep
  // one instead of freefalling and jello-popping. Poured particles spawn at
  // J = 1 (zero term) and g2p relaxes J toward 1, so the term self-retires
  // as the real density gradient establishes.
  //
  // SYMMETRIC on purpose. The first version clamped at [0, ..] — J below 1
  // added pressure, J above 1 added nothing — and that one-sided clamp
  // RECTIFIED the tr(C) noise around J = 1 into net outward pressure: a
  // sealed drained pool held ~0.45 cells/tick of churn indefinitely against
  // 3%/tick damping, a perpetual-motion pump made of a clamp. Symmetric,
  // J > 1 pulls back in (elastic tension) and the fluctuation averages to
  // zero.
  pw += clamp(FLUID_ONE - p.j, -FLUID_ONE / 2, FLUID_ONE / 2);
  var pr = mq(FLUID_STIFFNESS, clamp(pw - FLUID_ONE, -(1 << 16), 16 << 16));
  pr = max(pr, -FLUID_COHESION);
  // Species attraction: extra negative (pulling) pressure proportional to how
  // much same/other fluid is around. attractDiff < 0 flips to a push — that is
  // what keeps two species layered against each other instead of interleaved.
  pr -= mq(FLUID_ATTRACT_SAME, sameRatio) + mq(FLUID_ATTRACT_DIFF, otherRatio);
  pr = clamp(pr, -(1 << 20), 1 << 25);

  // Fold dt (1/substeps), M_p^-1 = 4 and the particle volume (mass/rho, capped
  // at one cell) into one Q16.16 scale, then build the stress matrix
  //   M = (sv * pressure) * I - (sv * viscosity) * (C + C^T)
  // whose product with dpos is this particle's velocity contribution per node.
  let inv = (1 << 30) / max(max(rho, 1 << 16) >> 2u, 1);   // 1/rho, Q16.16
  let sv = mq(inv, (4 * FLUID_ONE) / FLUID_SUBSTEPS);
  let scalar = clamp(mq(sv, pr), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
  let vs = mq(sv, FLUID_VISCOSITY);
  let m00 = clamp(scalar - mq(vs, p.c00 + p.c00), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
  let m11 = clamp(scalar - mq(vs, p.c11 + p.c11), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
  let m22 = clamp(scalar - mq(vs, p.c22 + p.c22), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
  let m01 = clamp(-mq(vs, p.c01 + p.c10), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
  let m02 = clamp(-mq(vs, p.c02 + p.c20), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
  let m12 = clamp(-mq(vs, p.c12 + p.c21), -FLUID_VEFF_MAX, FLUID_VEFF_MAX);

  // Same separable dpos hoist as p2g1: dx varies only with i, dy with j, dz
  // with k, and `p.p* - FLUID_HALF` is invariant across all 27.
  let pcx = p.px - FLUID_HALF;
  let pcy = p.py - FLUID_HALF;
  let pcz = p.pz - FLUID_HALF;
  var dxs : array<i32, 3>; var dys : array<i32, 3>; var dzs : array<i32, 3>;
  for (var a = 0; a < 3; a++) {
    dxs[a] = ((ax.base + a) << 16u) - pcx;
    dys[a] = ((ay.base + a) << 16u) - pcy;
    dzs[a] = ((az.base + a) << 16u) - pcz;
  }

  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(wxy[j * 3 + i], az.w[k]);
        let dx = dxs[i];
        let dy = dys[j];
        let dz = dzs[k];
        let fx = clamp(mq(m00, dx) + mq(m01, dy) + mq(m02, dz),
                       -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        let fy = clamp(mq(m01, dx) + mq(m11, dy) + mq(m12, dz),
                       -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        let fz = clamp(mq(m02, dx) + mq(m12, dy) + mq(m22, dz),
                       -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        let ni = nodeWordBase(bm, nc);
        atomicAdd(&fluidGrid[ni + 1u], mq(w, fx));
        atomicAdd(&fluidGrid[ni + 2u], mq(w, fy));
        atomicAdd(&fluidGrid[ni + 3u], mq(w, fz));
      }
    }
  }
}

// ---- grid update: momentum -> velocity, gravity, terrain BCs ----------------
// One thread per node of each active block. After this pass, words 1..3 hold
// node VELOCITY (Q16.16) and word 0 still holds mass; G2P only reads 1..3.
// The species-mass words (4..6) are left as p2g1 wrote them — p2g2 already
// consumed them and nothing after this reads them.
//
// v = P * 1024 / M exactly, via q/r decomposition (WGSL has no i64; the
// two-stage division is exact to 1 Q16.16 unit and everything stays in i32
// because |P/M| <= VEFF_MAX/1024 and |r| < M).
@compute @workgroup_size(256)
fn gridUpdate(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let block = wg.x >> 4u;
  let localIdx = (wg.x & 15u) * 256u + li;
  let ni = (block * CHUNK_VOL + localIdx) * FLUID_GW;

  // ---- foam field decay (word 7) ----
  // BEFORE the low-mass early-out on purpose: foam left behind by water that
  // has since drained away must still fade, and those are exactly the nodes
  // that fail the mass test. Skipping them would strand a permanent white
  // patch in mid-air wherever a splash once was.
  // One thread owns one node, so this is a plain read-modify-write with no
  // contention — order-independent by construction (rule 1).
  {
    let f = atomicLoad(&fluidGrid[ni + 7u]);
    if (f > 0) {
      // The -1 floor guarantees termination: a pure Q16 multiply by a factor
      // < 1 stalls at small values, leaving a faint permanent haze.
      atomicStore(&fluidGrid[ni + 7u], max(mq(f, FLUID_FOAM_DECAY) - 1, 0));
    }
  }

  let m = atomicLoad(&fluidGrid[ni + 0u]);
  if (m < FLUID_MASS_MIN) {
    atomicStore(&fluidGrid[ni + 1u], 0);
    atomicStore(&fluidGrid[ni + 2u], 0);
    atomicStore(&fluidGrid[ni + 3u], 0);
    return;
  }
  // This node carries mass: light its y level in the chunk's Y-occupancy mask
  // (common.wgsl). Render-only derived data; atomicOr of a constant is
  // order-independent, and the mask is cleared by the same fill that clears the
  // index half of the map, once per tick.
  atomicOr(&fluidBlockMap[fbmYMaskIndex(fluidBlockList[block])],
           fbmYBits(i32((localIdx >> 4u) & 15u)));
  var v : vec3<i32>;
  for (var a = 0u; a < 3u; a++) {
    let mom = atomicLoad(&fluidGrid[ni + 1u + a]);
    let q = mom / m;
    let r = mom - q * m;
    v[a] = q * 1024 + (r * 1024) / m;
  }
  v.y -= FLUID_GRAVITY / FLUID_SUBSTEPS;

  // Node cell from the block's chunk slot + this thread's local coords.
  let slot = fluidBlockList[block];
  let sc = vec3<i32>(i32(slot % NCHUNK), i32((slot / NCHUNK) % NCHUNK),
                     i32(slot / (NCHUNK * NCHUNK)));
  let wc = slotToWorldChunk(sc, T.origin);
  let lo = vec3<i32>(i32(localIdx & 15u), i32((localIdx >> 4u) & 15u),
                     i32(localIdx >> 8u));
  let c = wc * i32(CHUNK) + lo;

  // ---- wind (research doc §4.6) -------------------------------------------
  // Per NODE, so the field varies across a splash for free — that is the whole
  // reason this belongs here rather than as a per-particle or per-body force.
  //
  // Placed BEFORE the boundary conditions on purpose: wind blowing into a wall
  // must be projected out by the same separate BC that projects gravity out,
  // or a gust would push water through geometry it is resting against.
  //
  // Gated on T.windMode rather than on windAtQ's zero return, because this is a
  // drag term: with a zero field it would still pull every node toward a
  // standstill, which is not "no wind" but "infinite still air", and it would
  // move the pinned hash through the settle seam.
  if (T.windMode != WIND_MODE_OFF && FLUID_WIND_GAIN > 0) {
    // Exposure: 1 for a node with almost nothing around it (spray, the top
    // skin of a pool), falling to 0 as the node fills out to the mass band.
    // Tested before the field is evaluated so a buried node — the great
    // majority in any standing body of water — pays a compare, not a sine.
    let expo = 65536 - phiQ(m, 0, FLUID_WIND_MASS);
    if (expo > 0) {
      let w = windAtScaledQ(c, &T, T.windPartScaleQ);
      // Same rate ramp as the ballistic tier (windDragRampQ, common.wgsl), and
      // it has to be the same one: these two drag laws share the "wind x
      // particles" slider, so if only one of them vanished at 0x the slider
      // would mean two different things depending on whether the droplet had
      // been handed to the solver yet. Calm air drags spray toward nothing.
      let k = mq(mq(FLUID_WIND_DRAG, windDragRampQ(w, &T)),
                 mq(FLUID_WIND_GAIN, expo));
      v.x += mq(w.x / FLUID_WIND_HZ - v.x, k);
      v.y += mq(w.y / FLUID_WIND_HZ - v.y, k);
      v.z += mq(w.z / FLUID_WIND_HZ - v.z, k);
    }
  }

  // ---- the current field (docs/PLAN_water_master.md component 8) -----------
  // THE SIM ARM, and it is the ONLY sim consumer the current field has. A jet
  // out of a drain is MPM, the water it lands in is MPM, and a whirlpool that
  // does not turn either of them is a picture rather than a current.
  //
  // Gated on T.currentMode for the identical reason the wind block above is
  // gated on T.windMode, and the argument is worth restating because it is the
  // reason mode 0 is an EXACT identity rather than an approximate one: this is
  // a DRAG term, so a zero field still pulls every node toward a standstill.
  // That is not "no current", it is "infinite still water", and it would move
  // the pinned hash through the settle seam. Reading the gate is what makes
  // `sim.currentMode = 0` bit-identical to a build without this block.
  //
  // NO EXPOSURE TEST, unlike wind: air only touches the skin of a body of
  // water, but a current runs THROUGH it. That difference is the whole reason
  // this is a second term rather than a second weather vector.
  if (T.currentMode != 0u && FLUID_CURRENT_DRAG > 0) {
    let cv = currentAtQ(c, &T);
    // The AABB reject inside currentAtQ makes this free everywhere there is no
    // primitive, which is everywhere in a world with no drain in it.
    if (cv.x != 0 || cv.y != 0 || cv.z != 0) {
      v.x += mq(cv.x / FLUID_WIND_HZ - v.x, FLUID_CURRENT_DRAG);
      v.y += mq(cv.y / FLUID_WIND_HZ - v.y, FLUID_CURRENT_DRAG);
      v.z += mq(cv.z / FLUID_WIND_HZ - v.z, FLUID_CURRENT_DRAG);
    }
  }

  // ---- separate BC with tangential preservation (plan §5 item 2) ----------
  // Only the velocity component pointing INTO a solid is removed; tangential
  // flow survives, which is what lets water sheet down a slope instead of
  // gluing to it. The old code zeroed ALL components of any node whose own
  // cell was solid — but a particle sliding down a ramp has in-solid nodes
  // inside its 3^3 support (the top layer of the ramp itself), so it lost
  // tangential velocity every substep and piled up mid-slope: reference
  // checklist cause #2, the user's exact complaint.
  //
  // A node whose own cell is SOLID and which carries mass is a SURFACE node
  // (deep-interior nodes get no particle support and exited at the mass gate
  // above). Per axis:
  //   * both neighbours solid  -> the axis runs PARALLEL to the exposed face
  //     (e.g. x/z under a flat floor): pure tangential, KEEP — this is the
  //     load-bearing case, the one the old unconditional v=0 destroyed;
  //   * one side open          -> this is the face-normal axis: remove only
  //     the component that points DEEPER into the solid; outward stays (that
  //     is the EOS ejecting particles that pushed in — anti-crust);
  //   * both sides open        -> a 1-cell wall: outward and through-flow are
  //     indistinguishable, so zero the axis (the sticky choice is the one
  //     that cannot tunnel water through thin geometry).
  // For a NON-solid node the plain directional test is already correct: the
  // into-solid direction IS the face normal of the wall the fluid touches.
  let sxp = fluidSolid(c + vec3<i32>(1, 0, 0));
  let sxn = fluidSolid(c - vec3<i32>(1, 0, 0));
  let syp = fluidSolid(c + vec3<i32>(0, 1, 0));
  let syn = fluidSolid(c - vec3<i32>(0, 1, 0));
  let szp = fluidSolid(c + vec3<i32>(0, 0, 1));
  let szn = fluidSolid(c - vec3<i32>(0, 0, 1));
  var contact = false;
  if (fluidSolid(c)) {
    contact = true;
    if (!(sxp && sxn)) {
      if ((sxp && v.x > 0) || (sxn && v.x < 0) || (!sxp && !sxn)) { v.x = 0; }
    }
    if (!(syp && syn)) {
      if ((syp && v.y > 0) || (syn && v.y < 0) || (!syp && !syn)) { v.y = 0; }
    }
    if (!(szp && szn)) {
      if ((szp && v.z > 0) || (szn && v.z < 0) || (!szp && !szn)) { v.z = 0; }
    }
  } else {
    if (v.x > 0 && sxp) { v.x = 0; contact = true; }
    if (v.x < 0 && sxn) { v.x = 0; contact = true; }
    if (v.y > 0 && syp) { v.y = 0; contact = true; }
    if (v.y < 0 && syn) { v.y = 0; contact = true; }
    if (v.z > 0 && szp) { v.z = 0; contact = true; }
    if (v.z < 0 && szn) { v.z = 0; contact = true; }
  }
  // Optional wall friction on whatever survived the projection (the
  // tangential part). Compiled out at the water default of 0 — see the
  // FLUID_FRICTION const for why this must be a guard, not a blend.
  if (FLUID_FRICTION > 0 && contact) {
    let keep = FLUID_ONE - min(FLUID_FRICTION, FLUID_ONE);
    v = vec3<i32>(mq(v.x, keep), mq(v.y, keep), mq(v.z, keep));
  }
  // CFL-honesty probe (plan §5 item 1): count node-substeps the VMAX clamp
  // actually truncates. The clamp converts pressure work into silent energy
  // loss, so in steady flow this must read ~0 — if it fires, stiffness or
  // substeps are wrong and no amount of damping is the fix. This is the only
  // place the clamp can engage: g2p's per-particle clamp gathers a convex
  // combination of these already-clamped node velocities, so it cannot exceed
  // VMAX except by rounding. Diagnostic only — nothing keys on it, and an
  // atomic counter sum is order-independent (rule 1).
  if (abs(v.x) > FLUID_VMAX || abs(v.y) > FLUID_VMAX || abs(v.z) > FLUID_VMAX) {
    atomicAdd(&fluidArgs[FA_CLAMPED], 1u);
  }
  v.x = clamp(v.x, -FLUID_VMAX, FLUID_VMAX);
  v.y = clamp(v.y, -FLUID_VMAX, FLUID_VMAX);
  v.z = clamp(v.z, -FLUID_VMAX, FLUID_VMAX);
  atomicStore(&fluidGrid[ni + 1u], v.x);
  atomicStore(&fluidGrid[ni + 2u], v.y);
  atomicStore(&fluidGrid[ni + 3u], v.z);
}

// ---- G2P: gather velocity, rebuild C (APIC), update J, damp, advect ---------
// Pure gather: each thread writes only its own particle, so this cannot race.
// A particle with no reachable nodes (out of window, over-budget chunk) sums
// zero weight and freezes in place — deterministic, and it thaws by itself
// when coverage returns.
@compute @workgroup_size(64)
fn g2p(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveTotal()) { return; }
  var p = fluidParticles[gid.x];
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  if (fluidSolid(cell)) {
    p.attr = 0u;
    fluidParticles[gid.x] = p;
    atomicAdd(&fluidArgs[FA_DEAD], 1u);
    return;
  }
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);

  var v = vec3<i32>(0, 0, 0);
  var c0 = vec3<i32>(0, 0, 0);   // row 0 of C (x-velocity gradients)
  var c1 = vec3<i32>(0, 0, 0);
  var c2 = vec3<i32>(0, 0, 0);
  // ---- diffuse-material potentials, gathered on the SAME 27 node taps ------
  // vdiff (Eq. 2): sum over neighbours of |vij| * (1 - vij_hat . xij_hat) * W.
  // The convergence factor is what separates an IMPACT (two fronts closing,
  // factor -> 2) from laminar flow (factor -> 0): the paper's whole reason for
  // preferring relative velocity over the curl. Accumulated in Q16.16 cells
  // per tick, weighted by the node's own B-spline weight as W(xij, h).
  var vdiff : i32 = 0;
  // Mass-weighted density gradient — the surface normal, and (via how sharply
  // it turns across the neighbourhood) the curvature that flags a wave crest.
  var grad = vec3<i32>(0, 0, 0);
  var massSum : i32 = 0;
  // Same two hoists as p2g1/p2g2 — the separable dpos and the (i,j) weight
  // bracket. This is the heaviest of the four 27-tap loops (it also does the
  // foam gather), so it is where they matter most.
  let pcx = p.px - FLUID_HALF;
  let pcy = p.py - FLUID_HALF;
  let pcz = p.pz - FLUID_HALF;
  var dxs : array<i32, 3>; var dys : array<i32, 3>; var dzs : array<i32, 3>;
  for (var a = 0; a < 3; a++) {
    dxs[a] = ((ax.base + a) << 16u) - pcx;
    dys[a] = ((ay.base + a) << 16u) - pcy;
    dzs[a] = ((az.base + a) << 16u) - pcz;
  }
  var wxy : array<i32, 9>;
  for (var j = 0; j < 3; j++) {
    for (var i = 0; i < 3; i++) { wxy[j * 3 + i] = mq(ax.w[i], ay.w[j]); }
  }
  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(wxy[j * 3 + i], az.w[k]);
        let ni = nodeWordBase(bm, nc);
        let nv = vec3<i32>(atomicLoad(&fluidGrid[ni + 1u]),
                           atomicLoad(&fluidGrid[ni + 2u]),
                           atomicLoad(&fluidGrid[ni + 3u]));
        let dx = dxs[i];
        let dy = dys[j];
        let dz = dzs[k];
        let tx = mq(w, nv.x); let ty = mq(w, nv.y); let tz = mq(w, nv.z);
        v += vec3<i32>(tx, ty, tz);
        // C = 4 * sum(w * v_node * dpos^T)  (D^-1 = 4 for quadratic splines)
        c0 += vec3<i32>(mq(tx, dx) << 2u, mq(tx, dy) << 2u, mq(tx, dz) << 2u);
        c1 += vec3<i32>(mq(ty, dx) << 2u, mq(ty, dy) << 2u, mq(ty, dz) << 2u);
        c2 += vec3<i32>(mq(tz, dx) << 2u, mq(tz, dy) << 2u, mq(tz, dz) << 2u);

        // ---- foam gather (costs the mass word this loop did not read) ----
        let nm = atomicLoad(&fluidGrid[ni + 0u]);     // Q10 node mass
        if (nm < FLUID_MASS_MIN) { continue; }
        massSum += nm;
        // Density gradient: mass * direction FROM the particle TO the node.
        // dpos is already that direction; a >>6 keeps the Q10*Q16.16 product
        // inside i32 across all 27 taps (|dpos| <= 1.5 cells, mass <= ~2000).
        grad += vec3<i32>((nm * (dx >> 8u)) >> 6u,
                          (nm * (dy >> 8u)) >> 6u,
                          (nm * (dz >> 8u)) >> 6u);
        // Relative velocity of this node against the particle, and how much
        // the pair is CONVERGING.
        //
        // SCALING, and why it is not the obvious >>8. Fluid velocities are a
        // FRACTION of a cell per tick (CFL caps them at 0.45), so in Q16.16
        // they are ~10^4 and a >>8 leaves values in the tens. Squaring those
        // and then shifting again — the first version of this — underflowed
        // every intermediate to 0 or 1: |rv| rounded to zero, and the cosine's
        // divisor clamped to 1 and saturated. The whole potential read as
        // exactly zero and no foam was ever generated.
        //
        // Instead: keep the RAW Q16.16 difference for the magnitude (isqrt of
        // a Q32 square is a Q16 magnitude — exact, no shift needed), and do
        // the cosine on a >>4 scale where the products still have real
        // precision. |rv| <= 2*VMAX ~ 2^18.4, so rv2 needs the >>4 pair to fit.
        let rvx = nv.x - p.vx; let rvy = nv.y - p.vy; let rvz = nv.z - p.vz;
        let sx = rvx >> 4u; let sy = rvy >> 4u; let sz = rvz >> 4u;
        let rv2 = sx * sx + sy * sy + sz * sz;      // (Q12.12)^2
        if (rv2 <= 0) { continue; }
        // dpos is at most 1.5 cells, so >>4 keeps it exact enough and matches
        // the velocity scale for the dot product below.
        let qx = dx >> 4u; let qy = dy >> 4u; let qz = dz >> 4u;
        let px2 = qx * qx + qy * qy + qz * qz;
        if (px2 <= 0) { continue; }
        // dot < 0 means node and particle are approaching (dpos points at the
        // node, relative velocity points back) -> converging -> trapped air.
        let dotp = sx * qx + sy * qy + sz * qz;
        // cos^2 in Q16 = dot^2 / (rv2 * px2), with the sign carried — no
        // square root, and no normalize. Both operands are >>10'd before the
        // multiply so the product cannot overflow i32 at the CFL ceiling.
        let dnum = ((abs(dotp) >> 10u) * (abs(dotp) >> 10u)) << 16u;
        let dq = clamp(dnum / max((rv2 >> 10u) * max(px2 >> 10u, 1), 1),
                       0, 65536);
        // (1 - cos) in [0,2] Q16: converging (dot<0) gives 1+|cos|.
        let conv = select(65536 - dq, 65536 + dq, dotp < 0);
        // |rv| as a Q16.16 magnitude: isqrt of the (Q12.12)^2 square gives a
        // Q12.12 magnitude, so <<4 restores Q16.16 cells/tick.
        let mag = isqrtI(rv2) << 4u;
        vdiff += mq(mq(mag, conv), w);
      }
    }
  }
  // Tunable settle aid: shave FLUID_DAMPING of the velocity per tick
  // (spread across the substeps). mq is symmetric, so damping cannot drift.
  let damp = FLUID_ONE - FLUID_DAMPING / FLUID_SUBSTEPS;
  p.vx = clamp(mq(v.x, damp), -FLUID_VMAX, FLUID_VMAX);
  p.vy = clamp(mq(v.y, damp), -FLUID_VMAX, FLUID_VMAX);
  p.vz = clamp(mq(v.z, damp), -FLUID_VMAX, FLUID_VMAX);
  p.c00 = clamp(c0.x, -FLUID_CMAX, FLUID_CMAX);
  p.c01 = clamp(c0.y, -FLUID_CMAX, FLUID_CMAX);
  p.c02 = clamp(c0.z, -FLUID_CMAX, FLUID_CMAX);
  p.c10 = clamp(c1.x, -FLUID_CMAX, FLUID_CMAX);
  p.c11 = clamp(c1.y, -FLUID_CMAX, FLUID_CMAX);
  p.c12 = clamp(c1.z, -FLUID_CMAX, FLUID_CMAX);
  p.c20 = clamp(c2.x, -FLUID_CMAX, FLUID_CMAX);
  p.c21 = clamp(c2.y, -FLUID_CMAX, FLUID_CMAX);
  p.c22 = clamp(c2.z, -FLUID_CMAX, FLUID_CMAX);

  // J *= 1 + dt*tr(C), factor clamped to [0.5, 2] per substep so a transient
  // spike cannot flip J's sign or blow the Q16 range. Diagnostic only now —
  // the EOS reads the gathered density — but the fluid-det sanity band and
  // the tuner's intuition ("is this pool compressed?") still use it.
  let trc = p.c00 + p.c11 + p.c22;
  let f = clamp(FLUID_ONE + trc / FLUID_SUBSTEPS, 32768, 131072);
  p.j = clamp(((p.j >> 4u) * (f >> 4u)) >> 8u, FLUID_JMIN, FLUID_JMAX);
  // Relax J toward 1 (~1.5%/substep, ~10-tick time constant). Two jobs: it
  // retires the excite converter's hydrostatic pre-compression once the real
  // density gradient carries the column, and it stops the tr(C) random walk
  // from parking a resting particle's J at a clamp rail — which the p2g2
  // hydrostatic blend would otherwise convert into permanent phantom
  // pressure. Integer, symmetric, deterministic.
  p.j += (FLUID_ONE - p.j) / 64;

  // ---- advect with solid back-projection ------------------------------------
  // The grid BC projects velocity at individual nodes, but the B-spline
  // weighted average across 27 nodes can still net INTO a solid when the
  // kernel straddles the surface.  Without this, particles that cross into a
  // solid cell cascade through geometry one substep at a time.  Try all three
  // axes at once; on hit, revert and re-try per axis so tangential flow along
  // surfaces is preserved (water sheeting down a slope keeps its lateral v).
  let oldPx = p.px; let oldPy = p.py; let oldPz = p.pz;
  p.px += p.vx / FLUID_SUBSTEPS;
  p.py += p.vy / FLUID_SUBSTEPS;
  p.pz += p.vz / FLUID_SUBSTEPS;
  if (fluidSolid(vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u))) {
    p.px = oldPx; p.py = oldPy; p.pz = oldPz;
    p.px += p.vx / FLUID_SUBSTEPS;
    if (fluidSolid(vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u))) {
      p.px = oldPx; p.vx = 0;
    }
    p.py += p.vy / FLUID_SUBSTEPS;
    if (fluidSolid(vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u))) {
      p.py = oldPy; p.vy = 0;
    }
    p.pz += p.vz / FLUID_SUBSTEPS;
    if (fluidSolid(vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u))) {
      p.pz = oldPz; p.vz = 0;
    }
  }
  fluidParticles[gid.x] = p;

  // ---- splash: fast free-surface particles shed micro droplets --------------
  // A fluid particle that is moving hard AND sits at low density (spray, a
  // breaking crest, the sheet of a slosh — not the interior of a pool) has a
  // chance per substep of emitting one PFLAG_MICRO droplet into the ballistic
  // particle system: the same sub-voxel spray gore and explosions use. The
  // droplet flies, catches light as a tiny cube, and on contact deposits its
  // material's authored stain (or nothing, if the material does not stain) —
  // so MPM water leaves wet marks and MPM blood leaves blood.
  //
  // DETERMINISM. The fluid slot index IS a stable identity here: slots are
  // assigned by the seam's slot-order compaction and scans — pure functions
  // of particle state — so hashing (slot, tick, position) is state-keyed,
  // not scheduling-keyed; the position term varies per substep, so one
  // particle does not roll the same dice six times a tick. The droplet's own
  // behaviour (claim hash on landing) keys on droplet state exactly like
  // every other particle. The only scheduling freedom is WHICH pWrite slot
  // the atomicAdd hands out, which nothing keys on (DESIGN.md §4) — same
  // contract as explosion ejecta.
  //
  // BOUNDED (rule 2): expected droplets = rate * eligible particles, eligible
  // requires sustained speed, droplets age out by FLUID_SPLASH_LIFE, and the
  // append drops on the floor at PARTICLE_CAP. A settled pool emits nothing.
  // The particle carries its own material now (attr word — excited water
  // knows it is water, excited blood knows it is blood). fluidSplashMat is
  // the legacy per-species table; attr wins when present so splash droplets
  // and foam land-and-stain as the ACTUAL substance.
  var splashMat = fpMat(p.attr);
  if (splashMat == 0u) { splashMat = T.fluidSplashMat[min(p.species, 3u)]; }
  if (splashMat != 0u && FLUID_SPLASH_CHANCE > 0 &&
      p.density < FLUID_SPLASH_MAX_RHO) {
    // Speed² in Q16.16 (cells/tick)²: (v >> 8)² sums stay well inside i32
    // (|v| <= VMAX 176947 -> 691² * 3 ≈ 1.4e6).
    let sx = p.vx >> 8u; let sy = p.vy >> 8u; let sz = p.vz >> 8u;
    let s2 = sx * sx + sy * sy + sz * sz;
    let th = FLUID_SPLASH_SPEED >> 8u;
    if (s2 > th * th) {
      let h = pcg(gid.x ^ pcg(u32(p.px) ^ pcg(u32(p.py) ^
              pcg(u32(p.pz) ^ pcg(T.tick ^ T.seed)))));
      if ((h & 0xFFFFu) < u32(FLUID_SPLASH_CHANCE)) {
        var d : Particle;
        // Q16.16 cells -> the particle system's 24.8 fixed point.
        d.px = p.px >> 8u; d.py = p.py >> 8u; d.pz = p.pz >> 8u;
        // Droplets leave a touch faster than the surface that shed them (the
        // sheet breaks and the film's tension lets go), plus a per-droplet
        // upward kick keyed off the hash so a churning surface fizzes rather
        // than emitting parallel streaks.
        d.vx = (p.vx * 5 / 4) >> 8u;
        d.vy = ((p.vy * 5 / 4) >> 8u) + i32((h >> 16u) & 63u);
        d.vz = (p.vz * 5 / 4) >> 8u;
        d.payload = splashMat & 0xFFFu;
        d.flags = PFLAG_ALIVE | PFLAG_MICRO |
                  ((u32(TUNE_FLUID_SPLASH_SCALE_IDX) & PMICRO_SCALE_MASK)
                   << PMICRO_SCALE_SHIFT) |
                  (FLUID_SPLASH_LIFE << PMICRO_LIFE_SHIFT);
        let slot = atomicAdd(&counts[1u - T.page], 1u);
        if (slot < PARTICLE_CAP) { pWrite[slot] = d; }
      }
    }
  }

  // ---- diffuse material: spray / foam / bubbles ----------------------------
  // Ihmsen et al. 2012 (CGI), "Unified Spray, Foam and Bubbles for
  // Particle-Based Fluids". Three potentials, each mapped to [0,1] by Phi
  // (Eq. 1), combined into a generation count (Eq. 8):
  //
  //   n_d = I_k * (k_ta * I_ta + k_wc * I_wc) * dt
  //
  // Multiplying by I_k is the paper's key structural claim, and it is what
  // makes this cheap AND well-behaved: air mixes with water at a crest or on
  // an impact, but in BOTH cases the amount scales with kinetic energy, so a
  // slow-but-convergent flow (a pool settling under its own weight, which is
  // convergent everywhere) generates nothing. Without the I_k factor every
  // still pool foams at its own floor.
  //
  // The whole block is gated on foam being switched on and the particle having
  // non-zero energy, so a settled pool costs one compare — rule 2.
  if ((FLUID_FOAM_KTA > 0 || FLUID_FOAM_KWC > 0) && massSum >= FLUID_MASS_MIN) {
    // --- I_k: kinetic energy. Measured on (v >> 8) exactly as the splash
    // test is, so the two eligibility bands are directly comparable in the
    // tuner (both read in the same human units).
    let kx = p.vx >> 8u; let ky = p.vy >> 8u; let kz = p.vz >> 8u;
    let ek = kx * kx + ky * ky + kz * kz;
    let iK = phiQ(ek, FLUID_EK_MIN, FLUID_EK_MAX);
    if (iK > 0) {
      // --- I_ta: trapped air, from the convergence-weighted relative
      // velocity gathered above (Eq. 2).
      let iTa = phiQ(vdiff, FLUID_TA_MIN, FLUID_TA_MAX);

      // --- I_wc: wave crest (Eq. 5-7). The paper sums (1 - ni.nj) over
      // neighbours and keeps only CONVEX regions, then gates on the particle
      // moving along its own normal (Eq. 7, threshold 0.6). Here the surface
      // normal is the normalized density gradient, and "convex" is exactly
      // "the neighbourhood mass sits BEHIND the particle relative to the
      // outward normal" — which the gradient magnitude measures directly: a
      // particle deep inside the fluid has neighbours on all sides and a near
      // zero gradient, one on a crest has them all to one side and a large
      // one. So |grad| / mass IS the convex-curvature estimate, and it needs
      // no second pass over the neighbourhood.
      let gx = grad.x >> 4u; let gy = grad.y >> 4u; let gz = grad.z >> 4u;
      let g2 = gx * gx + gy * gy + gz * gz;
      var iWc : i32 = 0;
      if (g2 > 0) {
        let gmag = isqrtI(g2);                       // scaled |grad|
        // Normalize by the mass that produced it -> a shape measure that does
        // not change when the pool gets denser. Q16.
        let kappa = clamp((gmag << 12u) / max(massSum >> 4u, 1), 0, 1 << 20);
        // Eq. 7: only count it if the fluid is moving OUTWARD along the
        // normal (a crest breaking), not if it is merely a static edge — this
        // is what stops every corner of a resting block of water foaming.
        // cos(v, n) >= 0.6, evaluated SQUARED to avoid both magnitudes.
        //
        // Velocity and gradient must be compared on the SAME scale, and both
        // must keep precision: p.v is Q16.16 but only ~10^4 in practice, so
        // the >>8 used for the energy term is far too coarse here (it rounds a
        // real velocity to single digits and the comparison becomes noise).
        // Both sides are reduced to a common >>6 scale instead.
        let vx6 = p.vx >> 6u; let vy6 = p.vy >> 6u; let vz6 = p.vz >> 6u;
        let v6sq = vx6 * vx6 + vy6 * vy6 + vz6 * vz6;
        let vdotg = vx6 * gx + vy6 * gy + vz6 * gz;
        var moving = false;
        if (vdotg > 0 && v6sq > 0) {
          // (v.g)^2 >= 0.36 * |v|^2 * |g|^2. Shifted by 10 on each factor so
          // the products stay inside i32 for the largest gradient a full
          // neighbourhood can produce.
          let lhs = (vdotg >> 10u) * (vdotg >> 10u);
          let rhs = ((v6sq >> 10u) * (g2 >> 10u) / 256) * 92;  // 92/256 ~= 0.36
          moving = lhs >= rhs;
        }
        if (moving) { iWc = phiQ(kappa, FLUID_WC_MIN, FLUID_WC_MAX); }
      }

      // --- Eq. 8. Both rate constants are already per-substep probabilities
      // in Q16 at full potential, so the product with the potentials IS the
      // per-substep chance in Q16. Bounded above by (kta + kwc), which
      // LoadTuning caps at one emission per substep.
      // NOT mq() here. mq stages its operands as (|a|>>6)*(|b|>>6)>>4, which
      // is exact enough for the Q16.16 quantities it was written for but
      // throws away the low 6 bits of BOTH factors — and these factors are
      // small Q16 probabilities (a few thousand), so a nested mq chain
      // truncated the result to zero and no foam was ever emitted despite
      // both potentials being healthy. The operands here are bounded by
      // 65536 * 65536 = 2^32... so do the reduction in two exact steps
      // instead, each of which provably fits i32.
      //   inner = (KTA*iTa + KWC*iWc) >> 16   <= (kta+kwc) <= 65536
      //   nd    = (iK * inner) >> 16          <= 65536
      // KTA,KWC <= 65536 and iTa,iWc <= 65536, so each product is <= 2^32;
      // shifting each term individually before the add keeps every
      // intermediate inside i32.
      // Each potential is Q16 (<= 65536) and each rate is Q16 (<= 65536), so a
      // direct product is up to 2^32 — one bit too wide. Shifting the
      // POTENTIAL down by 8 (keeping 8 bits of it) and the rate not at all
      // bounds the product at 65536 * 256 = 2^24 with room to spare, and
      // costs a factor-256 quantisation on a term that is already a
      // probability. Shifting BOTH operands (the previous version) threw away
      // ~8 bits of signal and drove nd under the roll threshold everywhere.
      let inner = (FLUID_FOAM_KTA * (iTa >> 8u) +
                   FLUID_FOAM_KWC * (iWc >> 8u)) >> 8u;
      let nd = (min(inner, 65536) * (iK >> 8u)) >> 8u;
      if (nd > 0) {
        // The foam FIELD (grid word 7) is written for every generating
        // particle, not only the ones that win the dice roll: it is a
        // continuous measure of "how aerated is this region", and the
        // renderer marches it as the surface's white. Scattering it to the
        // nearest node with an atomicAdd is order-independent (rule 1) —
        // addition commutes, and nothing keys on which thread got there
        // first. Saturated so a long-running churn cannot overflow.
        let nb = nodeBlock(cell);
        if (nb != 0u) {
          let fi = nodeWordBase(nb, cell) + 7u;
          // nd is a per-substep probability in Q16; scale it up so a sustained
          // moderate churn still saturates the field, then cap at 1.0.
          let add = mq(FLUID_FOAM_GAIN, min(nd << 4u, 65536));
          if (add > 0) {
            let prev = atomicLoad(&fluidGrid[fi]);
            if (prev < FLUID_FOAM_FULL) {
              atomicAdd(&fluidGrid[fi], min(add, FLUID_FOAM_FULL - prev));
            }
          }
        }

        // Roll for an actual particle. Hashed on (slot, position, tick) for
        // the same reason the splash roll is: the fluid slot is a stable
        // IDENTITY (assigned by the seam's deterministic slot-order
        // compaction and scans), so this is state-keyed, not
        // scheduling-keyed. A distinct
        // salt from the splash hash keeps foam and spray from firing on
        // exactly the same particles every time.
        let fh = pcg(0x9E3779B9u ^ gid.x ^ pcg(u32(p.px) ^ pcg(u32(p.py) ^
                 pcg(u32(p.pz) ^ pcg(T.tick ^ T.seed)))));
        if ((fh & 0xFFFFu) < u32(nd)) {
          // --- classification (paper §3.2): by local fluid density, which is
          // the neighbour count the paper uses with the kernel weights already
          // applied. Spray is airborne and ballistic; bubbles are submerged
          // and buoyant; foam is the surface film between them.
          let rho = p.density;
          var fv = vec3<i32>(p.vx, p.vy, p.vz);
          let kd = i32(round(TUNE_FLUID_FOAM_DRAG * 65536.0));
          if (rho >= FLUID_BUBBLE_RHO) {
            // BUBBLE: dragged toward the fluid velocity (kd) and pushed UP by
            // buoyancy counteracting gravity. v is already the fluid velocity
            // here (this particle IS fluid), so the drag term degenerates to
            // "match the flow" — the buoyancy is what distinguishes it.
            let buoy = i32(round(TUNE_FLUID_BUBBLE_BUOY * 65536.0 / 30.0));
            fv.y += mq(buoy, 65536);
          } else if (rho <= FLUID_SPRAY_RHO) {
            // SPRAY: ballistic. Leaves a little faster than the sheet that
            // threw it, exactly as the splash droplets do.
            fv = vec3<i32>((fv.x * 5) / 4, (fv.y * 5) / 4, (fv.z * 5) / 4);
          } else {
            // FOAM: advected by the fluid, damped toward it by kd so it rides
            // the surface rather than flying off it.
            fv = vec3<i32>(mq(fv.x, kd), mq(fv.y, kd), mq(fv.z, kd));
          }
          // Scatter the spawn off the particle centre so a churning cell
          // fizzes instead of emitting a single stacked column (the paper
          // samples a cylinder; one hashed sub-voxel jitter is the same idea
          // at this resolution and costs no extra state).
          let jx = i32((fh >> 4u) & 0x3FFFu) - 8192;
          let jy = i32((fh >> 12u) & 0x3FFFu) - 8192;
          let jz = i32((fh >> 20u) & 0x3FFFu) - 8192;
          var d : Particle;
          d.px = (p.px + jx) >> 8u;
          d.py = (p.py + jy) >> 8u;
          d.pz = (p.pz + jz) >> 8u;
          d.vx = fv.x >> 8u;
          d.vy = fv.y >> 8u;
          d.vz = fv.z >> 8u;
          // Foam is WHITE, and it gets that from the RENDER tuning (see
          // PPAY_FOAM in common.wgsl) rather than from a material id — foam is
          // entrained air, not a substance, so there is no material to name.
          // The fluid's own material rides along in the low bits so the
          // droplet still lands and stains correctly: MPM blood foams
          // pink-white and leaves red.
          d.payload = (splashMat & 0xFFFu) |
                      ((fh >> 8u) & 0x7000u) |  // per-particle colour jitter
                      PPAY_FOAM;
          // Lifetime scales with the generation potential: the paper's
          // observation that large foam clusters are more stable than small
          // ones, captured without ever computing the foam AREA.
          let lifeSpan = i32(FLUID_FOAM_LIFE) - i32(FLUID_FOAM_LIFE_MIN);
          let life = u32(clamp(i32(FLUID_FOAM_LIFE_MIN) +
                               ((lifeSpan * iK) >> 16u), 1, 255));
          d.flags = PFLAG_ALIVE | PFLAG_MICRO |
                    ((u32(TUNE_FLUID_FOAM_SCALE_IDX) & PMICRO_SCALE_MASK)
                     << PMICRO_SCALE_SHIFT) |
                    (life << PMICRO_LIFE_SHIFT);
          let fslot = atomicAdd(&counts[1u - T.page], 1u);
          if (fslot < PARTICLE_CAP) { pWrite[fslot] = d; }
        }
      }
    }
  }
}
