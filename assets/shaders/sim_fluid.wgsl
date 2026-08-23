// sim_fluid.wgsl — fixed-point MLS-MPM liquid prototype (docs/PLAN_mpm_fluids.md).
//
// An EXPERIMENTAL second liquid representation running alongside the CA liquid
// so the two can be compared in-world before either replaces the other. This
// file is the plan's Phase 0+1 in one: the 88-line MLS-MPM core (Hu et al.
// 2018, taichi_mpm mls-mpm88) ported to integer fixed point, with the sparse
// scratch grid allocated per substep over the chunks the particles actually
// occupy (rule 2: a world with no excited fluid records zero fluid work).
//
// DETERMINISM. The fluid never writes a voxel and no CA kernel reads a fluid
// buffer, so the WORLD hash cannot move (verified by the pinned determinism
// gate staying at its baseline). The fluid's own state is bit-deterministic by
// the same discipline the CA uses, and the fluid_det gate verifies it twice-run:
//   * integer-only math end to end — no f32 anywhere in this file;
//   * the P2G scatter accumulates via i32 atomicAdd: integer addition is
//     associative, so the sum cannot depend on workgroup scheduling (this is
//     the plan's §5.1 fixed-point-atomics bet, and the whole point of Phase 0);
//   * block indices are assigned by a single-workgroup scan in slot order
//     (deterministic), never by first-come atomics;
//   * per-particle work is keyed on particle state only, never on dispatch or
//     buffer order.
//
// THE SUBSTEP (recorded kFluidSubsteps times per tick from EncodeTick):
//   fill blockMap=0 -> mark -> alloc(scan) -> copy args -> clear -> P2G ->
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
//   J Q16 | node mass Q10 (particle mass 1.0 = 1024) | node momentum Q16.16
// Grid nodes sit at CELL CENTERS: node n's collision cell is cell n, and a
// particle at x uses base = floor(x - 1.0) in cell coords with quadratic
// B-spline weights of fx = (x - 0.5) - base in [0.5, 1.5].
//
// Overflow audit (worst cases, all < 2^31): node mass 2000 crowded particles
// x 432 = 8.6e5; node momentum 2000 x mq(0.42, 6.0) = 5.2e8; the staged
// multiplies keep every intermediate under 2^27. Velocities are CFL-clamped to
// FLUID_VMAX (0.45 cell/substep), which bounds everything downstream.

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

// 0.5 cell in Q16.16 — the cell-center offset of the node lattice.
const FLUID_HALF : i32 = 32768;
// CFL cap: 0.45 cell/substep * FLUID_SUBSTEPS, Q16.16 cells/tick (~8.1 m/s).
const FLUID_VMAX : i32 = 176947;
// Bound on the fused velocity+stress+affine term before the staged multiply.
const FLUID_VEFF_MAX : i32 = 393216;   // 6.0 cells/tick
// Affine matrix clamp, Q16.16 per tick.
const FLUID_CMAX : i32 = 262144;       // 4.0 /tick
// J clamps, Q16: [0.6, 1.4]. Weak compressibility never needs more range, and
// the clamp bounds the EOS term (overflow audit above).
const FLUID_JMIN : i32 = 39322;
const FLUID_JMAX : i32 = 91750;
// Node mass floor, Q10 (~0.016 particle masses): below this the node's
// momentum-to-velocity division would amplify noise on a node that cannot
// influence any particle meaningfully anyway.
const FLUID_MASS_MIN : i32 = 16;
// Gravity per tick^2 Q16.16 and the EOS stiffness both come from tuning
// (sim.* — integer, F5-reloadable): TUNE_FLUID_GRAVITY, TUNE_FLUID_STIFFNESS.

// Q16.16 multiply, exact to ~2^-4 fixed-point units, valid for |a|,|b| < 2^21.
// The staging (>>6, >>6, >>4) is what keeps the product inside i32; the
// truncation is identical on every device (rule 1 cares about sameness, not
// about the rounding mode).
fn mq(a : i32, b : i32) -> i32 { return ((a >> 6u) * (b >> 6u)) >> 4u; }

// Quadratic B-spline weights of fx in [0.5, 1.5] (Q16.16 in, Q16.16 out).
//   w0 = 0.5*(1.5-fx)^2   w1 = 0.75-(fx-1)^2   w2 = 0.5*(fx-0.5)^2
fn wsqHalf(a : i32) -> i32 { return ((a >> 8u) * (a >> 8u)) >> 1u; }
fn wsq(a : i32) -> i32 { return (a >> 8u) * (a >> 8u); }

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

fn nodeIndex(bm : u32, nc : vec3<i32>) -> u32 {
  let lo = vec3<u32>(nc & vec3<i32>(CHUNK_MASK));
  return (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
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
  let b = node * 4u;
  atomicStore(&fluidGrid[b + 0u], 0);
  atomicStore(&fluidGrid[b + 1u], 0);
  atomicStore(&fluidGrid[b + 2u], 0);
  atomicStore(&fluidGrid[b + 3u], 0);
}

// ---- P2G: scatter mass + momentum with the fused MLS-MPM force term ---------
// grid[node] += w * (v + (stress*I + C) * dpos), mass += w. Particle mass is
// 1.0; stress folds dt, V0, M_p^-1=4 and the EOS into one Q16.16 scalar:
//   keff = E / (2 * FLUID_SUBSTEPS)   (= dt * 4 * E * V0 with V0 = 1/8, dx = 1)
//   stress = -keff * (J - 1)
@compute @workgroup_size(64)
fn p2g(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveTotal()) { return; }
  let p = fluidParticles[gid.x];
  let cell = vec3<i32>(p.px >> 16u, p.py >> 16u, p.pz >> 16u);
  if (!inWindow(cell, T.origin)) { return; }
  let ax = axisOf(p.px); let ay = axisOf(p.py); let az = axisOf(p.pz);

  let keff = TUNE_FLUID_STIFFNESS / (2 * FLUID_SUBSTEPS);
  let j1 = p.j - FLUID_ONE;
  let stress = -(((keff >> 4u) * (j1 >> 4u)) >> 8u);   // Q16.16

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
        var vex = p.vx + mq(stress, dx) + mq(p.c00, dx) + mq(p.c01, dy) + mq(p.c02, dz);
        var vey = p.vy + mq(stress, dy) + mq(p.c10, dx) + mq(p.c11, dy) + mq(p.c12, dz);
        var vez = p.vz + mq(stress, dz) + mq(p.c20, dx) + mq(p.c21, dy) + mq(p.c22, dz);
        vex = clamp(vex, -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        vey = clamp(vey, -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        vez = clamp(vez, -FLUID_VEFF_MAX, FLUID_VEFF_MAX);
        let ni = nodeIndex(bm, nc) * 4u;
        atomicAdd(&fluidGrid[ni + 0u], w >> 6u);        // mass, Q10
        atomicAdd(&fluidGrid[ni + 1u], mq(w, vex));     // momentum, Q16.16
        atomicAdd(&fluidGrid[ni + 2u], mq(w, vey));
        atomicAdd(&fluidGrid[ni + 3u], mq(w, vez));
      }
    }
  }
}

// ---- grid update: momentum -> velocity, gravity, terrain BCs ----------------
// One thread per node of each active block. After this pass, words 1..3 hold
// node VELOCITY (Q16.16) and word 0 still holds mass; G2P only reads 1..3.
//
// v = P * 1024 / M exactly, via q/r decomposition (WGSL has no i64; the
// two-stage division is exact to 1 Q16.16 unit and everything stays in i32
// because |P/M| <= VEFF_MAX/1024 and |r| < M).
@compute @workgroup_size(256)
fn gridUpdate(@builtin(workgroup_id) wg : vec3<u32>,
              @builtin(local_invocation_index) li : u32) {
  let block = wg.x >> 4u;
  let localIdx = (wg.x & 15u) * 256u + li;
  let ni = (block * CHUNK_VOL + localIdx) * 4u;
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
  v.y -= TUNE_FLUID_GRAVITY / FLUID_SUBSTEPS;

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

// ---- G2P: gather velocity, rebuild C (APIC), update J, advect ---------------
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
        let ni = nodeIndex(bm, nc) * 4u;
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
  p.vx = clamp(v.x, -FLUID_VMAX, FLUID_VMAX);
  p.vy = clamp(v.y, -FLUID_VMAX, FLUID_VMAX);
  p.vz = clamp(v.z, -FLUID_VMAX, FLUID_VMAX);
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
  // spike cannot flip J's sign or blow the Q16 range.
  let trc = p.c00 + p.c11 + p.c22;
  let f = clamp(FLUID_ONE + trc / FLUID_SUBSTEPS, 32768, 131072);
  p.j = clamp(((p.j >> 4u) * (f >> 4u)) >> 8u, FLUID_JMIN, FLUID_JMAX);

  p.px += p.vx / FLUID_SUBSTEPS;
  p.py += p.vy / FLUID_SUBSTEPS;
  p.pz += p.vz / FLUID_SUBSTEPS;
  fluidParticles[gid.x] = p;
}
