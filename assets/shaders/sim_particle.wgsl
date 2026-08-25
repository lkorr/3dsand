// sim_particle.wgsl — ballistic voxels-in-flight (DESIGN.md §5).
// Runs after the CA color passes each tick, double-buffered:
//   args1    -> integrate (read page, indirect) -> args2 -> resolve (write page)
// integrate: gravity + fixed-point flight, sampling the grid every <=half
// voxel; a particle that would enter a blocking cell proposes a reinsertion at
// the last empty cell by atomicMax-ing a state-derived priority into a claim
// hash. resolve: claim winners write themselves back into the grid; losers
// (two particles wanting one cell, or a hash collision) rest one tick and
// retry. Everything is integer and keyed on particle state + tick, never on
// buffer slot order, so the grid stays bit-deterministic (DESIGN.md §2/§4).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;

@group(1) @binding(0) var<storage, read_write> pRead  : array<Particle>;
@group(1) @binding(1) var<storage, read_write> pWrite : array<Particle>;
@group(1) @binding(2) var<storage, read_write> counts : array<atomic<u32>>;
@group(1) @binding(3) var<storage, read_write> claim  : array<atomic<u32>>;
@group(1) @binding(4) var<storage, read_write> pArgs  : array<u32>;
@group(1) @binding(7) var<storage, read>       spawnOps : array<Particle>;

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

// ---- wind (docs/RESEARCH_wind.md §4.6, phase 3) -----------------------------
// windAtQ speaks Q16.16 world cells per SECOND; particles speak Q24.8 cells per
// TICK. 65536/256 = 256 fixed-point units and 30 ticks a second, so the divisor
// is 256*30. A divisor rather than a multiply-shift because it is exact at both
// ends and this runs once per particle per tick, not once per cell.
const PART_WIND_SCALE : i32 = 7680;
// Fraction of the gap between a particle's velocity and the local wind that
// closes in ONE TICK at a material's full windResponse of 15, in Q16. Human
// units in, integer out, at shader compile time — the sim_fluid.wgsl discipline
// (IEEE-exact const folding, so the kernel stays integer and deterministic).
const PART_WIND_DRAG : i32 =
    i32(round(clamp(TUNE_WIND_DRAG, 0.0, 30.0) * 65536.0 / 30.0));

// Blocks flight: solids, powders and liquids (splash = plop onto the surface).
fn blocksParticle(c : vec3<i32>) -> bool {
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return false; }
  return materials[mat].klass != CLASS_GAS;
}

// Next-tick dirty mark incl. boundary neighbors (particles run post-CA, so
// their writes are next tick's business).
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

fn liveCount(page : u32) -> u32 {
  return min(atomicLoad(&counts[page]), PARTICLE_CAP);
}

// pArgs layout: [0..3] indirect draw {36, instances, 0, 0},
//               [4..6] indirect dispatch {groups, 1, 1}
@compute @workgroup_size(1)
fn args1() {
  let n = liveCount(T.page);
  pArgs[4] = (n + 63u) / 64u;
  pArgs[5] = 1u;
  pArgs[6] = 1u;
}

@compute @workgroup_size(1)
fn args2() {
  let n = liveCount(1u - T.page);
  pArgs[0] = 36u;
  pArgs[1] = n;
  pArgs[2] = 0u;
  pArgs[3] = 0u;
  pArgs[4] = (n + 63u) / 64u;
  pArgs[5] = 1u;
  pArgs[6] = 1u;
}

fn append(p : Particle) {
  let slot = atomicAdd(&counts[1u - T.page], 1u);
  if (slot < PARTICLE_CAP) { pWrite[slot] = p; }
}

// CPU-authored spawns (debris shatter: body fragments re-entering the world
// as voxels-in-flight). Appended to the READ page before args1/integrate —
// same page explosion ejecta uses — so they fly this very tick. The op data
// is part of the tick's input stream, so replays capture it for free.
@compute @workgroup_size(64)
fn spawn(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.spawnCount) { return; }
  let slot = atomicAdd(&counts[T.page], 1u);
  if (slot >= PARTICLE_CAP) { return; }  // ring full: fragment just vaporizes
  var p = spawnOps[gid.x];
  // Keep the CPU's micro bits (PFLAG_MICRO, scale, life) and force only the
  // liveness/pending state, so a malformed op cannot inject a particle that is
  // already claiming a cell. Masking to the fields the CPU is allowed to
  // author is what keeps this an input stream rather than raw state injection.
  p.flags = PFLAG_ALIVE | (p.flags & (PFLAG_MICRO |
            (PMICRO_SCALE_MASK << PMICRO_SCALE_SHIFT) |
            (PMICRO_LIFE_MASK << PMICRO_LIFE_SHIFT)));
  pRead[slot] = p;
}

@compute @workgroup_size(64)
fn integrate(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveCount(T.page)) { return; }
  var p = pRead[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }

  let startCell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
  if (!inBounds(startCell)) { return; }  // fell out of the world: gone

  // ---- micro particles: age out ----
  // Spray is an effect with a finite budget, not conserved matter. Expiring in
  // mid-air is the common exit for a droplet that never hits anything, and it
  // is what guarantees a fight settles back to zero live particles (rule 2).
  if (isMicro(p)) {
    let life = microLifeOf(p.flags);
    if (life == 0u) { return; }  // dead: not appended, slot reclaimed
    p.flags = withMicroLife(p.flags, life - 1u);
  }

  // buried (CA moved material onto us): rise one voxel per tick until free
  if (blocksParticle(startCell)) {
    // A micro particle has no voxel to dig out to. Being buried means the CA
    // flowed over it, so it is inside something now — stain that something and
    // be gone, rather than tunnelling upward through solid rock.
    if (isMicro(p)) {
      p.flags |= PFLAG_PENDING;
      atomicMax(&claim[claimSlot(cellIndexW(startCell))], microStainPriority(p));
      append(p);
      return;
    }
    p.py += PART_ONE;
    p.vx = 0; p.vy = 0; p.vz = 0;
    append(p);
    return;
  }

  // gravity + clamp
  p.vy -= PART_GRAVITY;

  // ---- wind (research doc §4.6) -------------------------------------------
  // The one force site in this kernel besides gravity, which is the point: a
  // second place that touched velocity would be a second place to keep in step
  // with the substep sampling below.
  //
  // A DRAG law, not a push: the particle accelerates toward the air it is in
  // and stops when it gets there. That is what §4.6 means by "accelerates over
  // time" — an ember lofted by a gust keeps gaining speed for as long as the
  // gust outruns it — and it is also the bound (rule 2), because no amount of
  // wind or knob can make debris travel faster than the air is moving. A force
  // term with no velocity feedback has no such ceiling.
  //
  // The gate is tested HERE and not left to windAtQ's zero return, because a
  // drag term reading zero wind is not a no-op: it would drag every particle in
  // the world toward a standstill and quietly change the pinned hash. This is
  // the difference between "the field is off" and "the field is calm".
  //
  // ...and the RATE is ramped by the wind for the same reason one step in: a
  // fixed rate says the still air of a calm day resists a falling ember as hard
  // as a gale does. It does not, and modelling it that way is what made
  // gravity look broken the day wind shipped (windDragRampQ, common.wgsl, has
  // the numbers). Gravity is untouched above and stays untouched; what changes
  // is how much air there is to fall through.
  if (T.windMode != WIND_MODE_OFF) {
    let resp = i32(matWindResponse(materials[p.payload & 0xFFFu]));
    // Almost every material is 0 (stone chips do not blow around), so the
    // common case is one comparison and no field evaluation at all.
    if (resp > 0) {
      let w = windAtScaledQ(startCell, T, T.windPartScaleQ);
      // 0 in calm air, and 0 at a 0x dev multiplier — the same statement, which
      // is the point of ramping on the SCALED field. Tested before the gaps are
      // formed so a becalmed world pays a max and a divide, not six.
      let ramp = windDragRampQ(w, T);
      if (ramp > 0) {
        // The authored per-tick rate, thinned to the air actually present. Q16
        // times Q16 over 65536 stays Q16; PART_WIND_DRAG is at most 65536 and
        // ramp at most 65536, so the product is 2^32 >> 16 = 2^16. In range.
        let rate = (PART_WIND_DRAG * ramp) / 65536;
        // Micro spray gets the same law as a whole voxel. It is the same air,
        // and droplets drifting downwind off a splash is most of what phase 3
        // buys.
        let gx = w.x / PART_WIND_SCALE - p.vx;
        let gy = w.y / PART_WIND_SCALE - p.vy;
        let gz = w.z / PART_WIND_SCALE - p.vz;
        // Two steps rather than one product: gap * resp * drag would leave i32
        // at the top of both knobs' ranges. Integer division truncates toward
        // zero, which is symmetric — the asymmetry an arithmetic shift would
        // introduce reads as a permanent drift down-axis (the mq() lesson).
        p.vx += ((gx * resp) / 15) * rate / 65536;
        p.vy += ((gy * resp) / 15) * rate / 65536;
        p.vz += ((gz * resp) / 15) * rate / 65536;
      }
    }
  }

  p.vx = clamp(p.vx, -PART_MAX_VEL, PART_MAX_VEL);
  p.vy = clamp(p.vy, -PART_MAX_VEL, PART_MAX_VEL);
  p.vz = clamp(p.vz, -PART_MAX_VEL, PART_MAX_VEL);

  // sample the flight path every <= half voxel
  let maxc = max(max(abs(p.vx), abs(p.vy)), abs(p.vz));
  let n = max(1, (maxc + 127) / 128);
  var lastAir = vec3<i32>(p.px, p.py, p.pz);
  for (var k = 1; k <= n; k++) {
    let sx = p.px + p.vx * k / n;
    let sy = p.py + p.vy * k / n;
    let sz = p.pz + p.vz * k / n;
    let cell = vec3<i32>(sx >> 8u, sy >> 8u, sz >> 8u);
    if (!inBounds(cell)) { return; }  // left the world: particle dies
    if (blocksParticle(cell)) {
      // ---- micro: land ON the surface, stain it, and stop existing ----
      // The droplet is parked at the CONTACT point (first blocked sample), not
      // backed off to the last air cell the way a reinserting particle is. The
      // stain target must be recoverable in `resolve` from particle state
      // alone, and re-deriving it there from a backed-off position would mean
      // re-tracing the step — with the velocity, which resolve must not touch
      // because microStainPriority hashes it. Parking on contact makes the
      // target simply "the cell I am in", and the half-voxel overlap is
      // invisible: the renderer shrinks a micro cube to a fraction of a cell.
      if (isMicro(p)) {
        p.px = sx; p.py = sy; p.pz = sz;
        p.flags |= PFLAG_PENDING;
        atomicMax(&claim[claimSlot(cellIndexW(cell))], microStainPriority(p));
        append(p);
        return;
      }
      // propose reinsertion at the last empty position
      p.px = lastAir.x; p.py = lastAir.y; p.pz = lastAir.z;
      p.flags |= PFLAG_PENDING;
      let tgt = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
      atomicMax(&claim[claimSlot(cellIndexW(tgt))], particlePriority(p));
      append(p);
      return;
    }
    lastAir = vec3<i32>(sx, sy, sz);
  }
  p.px += p.vx;
  p.py += p.vy;
  p.pz += p.vz;
  append(p);
}

@compute @workgroup_size(64)
fn resolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveCount(1u - T.page)) { return; }
  var p = pWrite[gid.x];
  if ((p.flags & (PFLAG_ALIVE | PFLAG_PENDING)) != (PFLAG_ALIVE | PFLAG_PENDING)) {
    return;
  }

  let cell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
  // TWO BASES (§4.1, the same rule as the hash key): `tgtSlot` is the SLOT
  // cell index and is what the reinsertion CLAIM hashes on — the claim lattice
  // must be a property of the world cell, not of which page currently holds
  // it, or two particles targeting the same cell could hash to different
  // claim slots after a reallocation and both win. `tgt` is the physical word
  // index and is only ever a memory address.
  let tgtSlot = cellIndexW(cell);
  let tgt = voxWordIndex(cell);

  // ---- micro particles: deposit a stain, never a voxel ----
  // Whether it won the claim or not, the droplet is spent — it is sub-voxel
  // matter with nowhere to go. Losing only means another droplet stained this
  // cell on this tick, which is visually identical. Retrying (the ordinary
  // particle's behaviour) would leave spray hovering against a wall until a
  // slot freed up.
  if (isMicro(p)) {
    p.flags = 0u;  // dead either way
    pWrite[gid.x] = p;
    if (atomicLoad(&claim[claimSlot(tgtSlot)]) != microStainPriority(p)) { return; }

    let w = voxWordAt(cell);
    let hit = voxMat(w);
    // The cell may have been emptied by the CA between integrate and resolve;
    // staining air would paint a stain onto nothing and it would render as a
    // floating smear.
    if (hit == MAT_AIR) { return; }
    let hk = materials[hit].klass;
    // Same surface rule the CA's own staining uses (doStaining in
    // sim_step.wgsl): a stain soaks into a SURFACE. Blood spray landing in
    // water or drifting through smoke leaves nothing behind.
    if (hk != CLASS_SOLID && hk != CLASS_POWDER) { return; }

    // The droplet stains with ITS OWN material's authored stain, so this is
    // driven by materials.json and works for anything authored to stain — not
    // just blood (conventions: no hardcoded material IDs).
    let sm = materials[p.payload & 0xFFFu];
    let stainType = matStainType(sm);
    if (stainType == 0u) { return; }  // this material does not stain: vanish
    let addAmt = matStainAmount(sm);
    let cur = voxStainAmt(w);
    let curType = voxStainType(w);
    var amt = addAmt;
    if (curType == stainType) {
      if (cur >= STAIN_AMT_MAX) { return; }  // saturated: nothing to write
      amt = min(cur + addAmt, STAIN_AMT_MAX);
    }
    voxStore(tgt, (w & ~STAIN_BITS) | packStain(stainType, amt));
    markDirtyNext(cell);
    return;
  }

  let won = atomicLoad(&claim[claimSlot(tgtSlot)]) == particlePriority(p);

  if (won && voxMat(voxWordAt(cell)) == MAT_AIR) {
    // rejoin the grid; stamp 0xFF = "hasn't acted", falls next tick
    let mat = p.payload & 0xFFFu;
    var state = (p.payload >> 12u) & 0xFu;
    // A LIQUID is born full, exactly as sim_mutate.wgsl does for a painted one
    // — its state nibble is FULLNESS, not a variant index.
    //
    // Without this, a spawn that leaves the nibble at 0 lands at 1/8 fullness,
    // and the renderer's smooth density field (liquidDensityAt) then sees a
    // scatter of near-empty cells with no coherent isosurface between them.
    // The result reads as separately shaded translucent cubes — the exact
    // "gelatin" failure shadeViscous is written to avoid. Fullness is what
    // makes flung blood shade like the blood a brush paints.
    if (materials[mat].klass == CLASS_LIQUID) { state = LIQ_FULL_STATE; }
    // STAMP_NEVER: a reinserted particle has not acted as a grid voxel yet, so
    // it is free to move on the tick it lands.
    voxStore(tgt, packVox(mat, state, STAMP_NEVER));
    markDirtyNext(cell);
    p.flags = 0u;  // dead
  } else {
    // lost the claim (or the cell got taken): rest, retry next tick
    p.flags = PFLAG_ALIVE;
    p.vx = 0; p.vy = 0; p.vz = 0;
  }
  pWrite[gid.x] = p;
}
