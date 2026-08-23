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
//   [7] unused (cleared with the rest)
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
@group(1) @binding(1) var<storage, read>       fluidSpawnOps : array<FluidSpawnOp>;
@group(1) @binding(2) var<storage, read_write> fluidBlockMap : array<atomic<u32>>;
@group(1) @binding(3) var<storage, read_write> fluidBlockList : array<u32>;
@group(1) @binding(4) var<storage, read_write> fluidGrid : array<atomic<i32>>;
@group(1) @binding(5) var<storage, read_write> fluidArgs : array<u32>;
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
// 0.5 cell in Q16.16 — the cell-center offset of the node lattice.
const FLUID_HALF : i32 = 32768;
// CFL cap: 0.45 cell/substep * FLUID_SUBSTEPS, Q16.16 cells/tick (~8.1 m/s).
const FLUID_VMAX : i32 = 176947;
// Bound on the fused velocity+stress+affine term before the staged multiply.
const FLUID_VEFF_MAX : i32 = 393216;   // 6.0 cells/tick
// Affine matrix clamp, Q16.16 per tick.
const FLUID_CMAX : i32 = 262144;       // 4.0 /tick
// J clamps, Q16: [0.6, 1.4]. J is diagnostic now (the EOS reads density), but
// the clamp keeps the fluid-det sanity band meaningful.
const FLUID_JMIN : i32 = 39322;
const FLUID_JMAX : i32 = 91750;
// Node mass floor, Q10 (~0.016 particle masses): below this the node's
// momentum-to-velocity division would amplify noise on a node that cannot
// influence any particle meaningfully anyway.
const FLUID_MASS_MIN : i32 = 16;
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
// liquids and gases do not (the two liquid systems pass through each other in
// this side-by-side prototype). Out-of-window is solid and inert.
fn fluidSolid(c : vec3<i32>) -> bool {
  if (!inWindow(c, T.origin)) { return true; }
  let mat = voxMat(voxWordAt(c));
  if (mat == MAT_AIR) { return false; }
  let k = materials[mat].klass;
  return k == CLASS_SOLID || k == CLASS_POWDER;
}

fn liveTotal() -> u32 { return min(T.fluidBase + T.fluidSpawnCount, FLUID_CAP); }

// ---- spawn: CPU op stream -> particles, at CPU-known offsets ----------------
// No atomics: the append base is T.fluidBase and the count is CPU-owned, so
// slot assignment is a pure function of the op index (plan §5.1's "deletion/
// compaction order likewise" concern does not arise — this prototype never
// deletes).
@compute @workgroup_size(64)
fn spawn(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.fluidSpawnCount) { return; }
  let slot = T.fluidBase + gid.x;
  if (slot >= FLUID_CAP) { return; }  // CPU charges the budget; belt+braces
  let op = fluidSpawnOps[gid.x];
  var p : FluidParticle;
  p.px = op.px; p.py = op.py; p.pz = op.pz;
  p.vx = clamp(op.vx, -FLUID_VMAX, FLUID_VMAX);
  p.vy = clamp(op.vy, -FLUID_VMAX, FLUID_VMAX);
  p.vz = clamp(op.vz, -FLUID_VMAX, FLUID_VMAX);
  p.c00 = 0; p.c01 = 0; p.c02 = 0;
  p.c10 = 0; p.c11 = 0; p.c12 = 0;
  p.c20 = 0; p.c21 = 0; p.c22 = 0;
  p.j = FLUID_ONE;
  p.species = op.species & 3u;
  p.density = 0;
  fluidParticles[slot] = p;
}

// ---- mark: flag every chunk the particle's 3^3 node support touches ---------
// atomicOr of a constant is order-independent; the map is cleared by a Fill
// row at the top of every substep.
@compute @workgroup_size(64)
fn mark(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveTotal()) { return; }
  let p = fluidParticles[gid.x];
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }  // frozen out-of-window
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);
  let lo = vec3<i32>(ax.base, ay.base, az.base);
  // Node support spans cells [base, base+2]: at most 2 chunks per axis.
  for (var k = 0; k < 2; k++) {
    for (var j = 0; j < 2; j++) {
      for (var i = 0; i < 2; i++) {
        let corner = lo + vec3<i32>(i * 2, j * 2, k * 2);
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
  let span = NUM_CHUNKS / 256u;   // 128 slots per thread
  var n = 0u;
  for (var s = li * span; s < (li + 1u) * span; s++) {
    if (atomicLoad(&fluidBlockMap[s]) != 0u) { n += 1u; }
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
    fluidArgs[0] = blocks * (CHUNK_VOL / 256u);  // 16 workgroups per block
    fluidArgs[1] = 1u;
    fluidArgs[2] = 1u;
    fluidArgs[3] = blocks;
  }
  workgroupBarrier();
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
@compute @workgroup_size(256)
fn clearGrid(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let node = wg.x * 256u + li;   // wg.x spans blocks*16, so node < blocks*4096
  let b = node * FLUID_GW;
  for (var w = 0u; w < FLUID_GW; w++) {
    atomicStore(&fluidGrid[b + w], 0);
  }
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

  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(mq(ax.w[i], ay.w[j]), az.w[k]);   // Q16.16 <= 0.42
        // dpos = node - particle, Q16.16 in [-1.5, 1.5] per axis.
        let dx = (nc.x << 16u) - (p.px - FLUID_HALF);
        let dy = (nc.y << 16u) - (p.py - FLUID_HALF);
        let dz = (nc.z << 16u) - (p.pz - FLUID_HALF);
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

  // Gather rho and same-species rho (Q16.16 particle masses per cell).
  var rho : i32 = 0;
  var same : i32 = 0;
  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(mq(ax.w[i], ay.w[j]), az.w[k]);
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

  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(mq(ax.w[i], ay.w[j]), az.w[k]);
        let dx = (nc.x << 16u) - (p.px - FLUID_HALF);
        let dy = (nc.y << 16u) - (p.py - FLUID_HALF);
        let dz = (nc.z << 16u) - (p.pz - FLUID_HALF);
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
  let m = atomicLoad(&fluidGrid[ni + 0u]);
  if (m < FLUID_MASS_MIN) {
    atomicStore(&fluidGrid[ni + 1u], 0);
    atomicStore(&fluidGrid[ni + 2u], 0);
    atomicStore(&fluidGrid[ni + 3u], 0);
    return;
  }
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

  if (fluidSolid(c)) {
    // Node inside terrain: sticky. (The plan's separate+friction BC is a
    // later refinement; embedded nodes must not carry momentum regardless.)
    v = vec3<i32>(0, 0, 0);
  } else {
    // Separate BC: zero the component pointing into a solid neighbor.
    if (v.x > 0 && fluidSolid(c + vec3<i32>(1, 0, 0))) { v.x = 0; }
    if (v.x < 0 && fluidSolid(c - vec3<i32>(1, 0, 0))) { v.x = 0; }
    if (v.y > 0 && fluidSolid(c + vec3<i32>(0, 1, 0))) { v.y = 0; }
    if (v.y < 0 && fluidSolid(c - vec3<i32>(0, 1, 0))) { v.y = 0; }
    if (v.z > 0 && fluidSolid(c + vec3<i32>(0, 0, 1))) { v.z = 0; }
    if (v.z < 0 && fluidSolid(c - vec3<i32>(0, 0, 1))) { v.z = 0; }
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
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);

  var v = vec3<i32>(0, 0, 0);
  var c0 = vec3<i32>(0, 0, 0);   // row 0 of C (x-velocity gradients)
  var c1 = vec3<i32>(0, 0, 0);
  var c2 = vec3<i32>(0, 0, 0);
  for (var k = 0; k < 3; k++) {
    for (var j = 0; j < 3; j++) {
      for (var i = 0; i < 3; i++) {
        let nc = vec3<i32>(ax.base + i, ay.base + j, az.base + k);
        let bm = nodeBlock(nc);
        if (bm == 0u) { continue; }
        let w = mq(mq(ax.w[i], ay.w[j]), az.w[k]);
        let ni = nodeWordBase(bm, nc);
        let nv = vec3<i32>(atomicLoad(&fluidGrid[ni + 1u]),
                           atomicLoad(&fluidGrid[ni + 2u]),
                           atomicLoad(&fluidGrid[ni + 3u]));
        let dx = (nc.x << 16u) - (p.px - FLUID_HALF);
        let dy = (nc.y << 16u) - (p.py - FLUID_HALF);
        let dz = (nc.z << 16u) - (p.pz - FLUID_HALF);
        let tx = mq(w, nv.x); let ty = mq(w, nv.y); let tz = mq(w, nv.z);
        v += vec3<i32>(tx, ty, tz);
        // C = 4 * sum(w * v_node * dpos^T)  (D^-1 = 4 for quadratic splines)
        c0 += vec3<i32>(mq(tx, dx) << 2u, mq(tx, dy) << 2u, mq(tx, dz) << 2u);
        c1 += vec3<i32>(mq(ty, dx) << 2u, mq(ty, dy) << 2u, mq(ty, dz) << 2u);
        c2 += vec3<i32>(mq(tz, dx) << 2u, mq(tz, dy) << 2u, mq(tz, dz) << 2u);
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

  p.px += p.vx / FLUID_SUBSTEPS;
  p.py += p.vy / FLUID_SUBSTEPS;
  p.pz += p.vz / FLUID_SUBSTEPS;
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
  // DETERMINISM. The fluid slot index IS a stable identity here (fluid
  // particles are appended at CPU-known offsets and never die), so hashing
  // (slot, tick, position) is state-keyed, not scheduling-keyed; the position
  // term varies per substep, so one particle does not roll the same dice six
  // times a tick. The droplet's own behaviour (claim hash on landing) keys on
  // droplet state exactly like every other particle. The only scheduling
  // freedom is WHICH pWrite slot the atomicAdd hands out, which nothing keys
  // on (DESIGN.md §4) — same contract as explosion ejecta.
  //
  // BOUNDED (rule 2): expected droplets = rate * eligible particles, eligible
  // requires sustained speed, droplets age out by FLUID_SPLASH_LIFE, and the
  // append drops on the floor at PARTICLE_CAP. A settled pool emits nothing.
  let splashMat = T.fluidSplashMat[min(p.species, 3u)];
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
}
