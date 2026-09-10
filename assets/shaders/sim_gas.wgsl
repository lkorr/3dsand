// sim_gas.wgsl — gas that has left the residency window, as particles.
// docs/PLAN_gas_particles.md stage 1.
//
// INSIDE the window nothing changes: smoke is a CLASS_GAS voxel with the whole
// reaction system available to it. What this file owns is what used to be
// impossible — a gas parcel whose next move leaves the window. `tryMove`
// returned false there, indistinguishable from a wall, and the parcel sheeted
// across the top chunk plane until it decayed. sim_step's `gasLeave` now
// deletes that voxel and appends a record here instead; the edge is a SINK.
//
// A gas particle is deliberately NOT a ballistic particle:
//
//   * Its position is 24.8 with a ZERO FRACTION. It lives ON a cell and moves
//     one whole cell per tick, because it must take the moves a voxel would —
//     the same gasIntent buoyancy/wind roll and the same fourteen-step
//     fallback ladder, called out of sim_step.wgsl rather than re-typed.
//   * Its velocity words stay zero, which keeps `particlePriority` a pure
//     function of the visible state.
//   * It carries no age. Death is rolled from the material's OWN RK_DECAY
//     rules in reactions.json, keyed on (seed, tick, position-derived key) —
//     the same shape of roll the CA makes, so the decay chance an author tunes
//     for the voxel is the one the particle obeys.
//
// EVERYTHING IS BOUNDED (rule 2): the per-tick conversion budget, the pool
// cap, an outer box, a ceiling, and the authored decay. A gas particle wakes
// no chunk except when it RE-ENTERS the window, which is the one place it
// meets the page table.
//
// DETERMINISM (rule 1): every roll keys on (seed, tick, a key derived from the
// particle's CELL and payload), never on a buffer slot. Write reach is <=1
// cell and only through the claim path. The one place order does not matter
// and is not made to is `gasOuter`, which is render-only derived data the sim
// never reads and the world hash never covers.

@group(0) @binding(0)  var<storage, read_write> voxels : array<u32>;
@group(0) @binding(2)  var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3)  var<storage, read> materials : array<Material>;
@group(0) @binding(4)  var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Its own bank
// rather than a share of the ballistic kernel's: the two populations fault for
// completely different reasons — a parcel lands through the claim path where
// debris lands through a DDA — and "which kernel dropped the store" is the
// whole point of the per-kernel tally.
const PT_KERNEL : u32 = PT_K_GAS;

@group(1) @binding(0)  var<storage, read_write> gasRead : array<Particle>;
@group(1) @binding(1)  var<storage, read_write> gasWrite : array<Particle>;
@group(1) @binding(2)  var<storage, read_write> gasCounts : array<atomic<u32>>;
// The CA's outbox: GAS_SP_HDR header words then Particle-shaped records. Declared
// atomic here only because sim_step declares it so; this module's reads of the
// records are plain loads through atomicLoad.
@group(1) @binding(3)  var<storage, read_write> gasSpawn : array<atomic<u32>>;
@group(1) @binding(4)  var<storage, read_write> gasClaim : array<atomic<u32>>;
@group(1) @binding(5)  var<storage, read_write> gasArgs : array<u32>;
@group(1) @binding(6)  var<storage, read_write> gasOuter : array<atomic<u32>>;
@group(1) @binding(7)  var<storage, read> farVox : array<u32>;
@group(1) @binding(8)  var<uniform> farP : FarParams;
@group(1) @binding(9)  var<storage, read> reactions : array<Reaction>;
// CPU-authored gas spawns (the gas-reenter gate; any future emitter that is
// not a grid cell). Same shape as gasSpawn: word 0 is the count, records from
// word GAS_SP_HDR. Part of the per-tick input stream, exactly like spawnOps.
@group(1) @binding(10) var<storage, read> gasSpawnOps : array<u32>;

// ---- constants that must agree with sim_step.wgsl and src/sim/world.h ------
// Kept out of common.wgsl on purpose (CLAUDE.md: a constant only its consumers
// read is declared next to them; a common.wgsl edit costs the whole SPIR-V
// cache and the worldgen far-cascade recompile). check_invariants.py compares
// these against world.h.
const GAS_SP_COUNT     : u32 = 0u;
const GAS_SP_REFUSED   : u32 = 1u;
const GAS_SP_EDGE      : u32 = 2u;
const GAS_SP_POOLFULL  : u32 = 3u;  // spawn refused: the particle pool was full
const GAS_SP_REENTER   : u32 = 4u;  // particles that became voxels this tick
const GAS_SP_DIED      : u32 = 5u;  // decay / outer box / ceiling
const GAS_SP_ABOVE     : u32 = 6u;  // live particles above the window's top face
const GAS_SP_LIVE      : u32 = 7u;  // live particles after integrate
// The determinism digest: SUM of every surviving parcel's particlePriority.
// A sum because pool append order is scheduling-dependent and a digest that
// depended on it would report a false divergence on every run. Outside the
// window a parcel touches no voxel, so the world hash cannot see it and this
// is the only thing that can — see the determinism gate.
const GAS_SP_DIGEST    : u32 = 8u;
const GAS_SP_HDR       : u32 = 16u;
const GAS_SP_STRIDE    : u32 = 8u;
const GAS_SPAWN_CAP    : u32 = 65536u;   // kGasSpawnPerTick
const GAS_CPU_SPAWN_CAP: u32 = 1024u;    // kGasCpuSpawnPerTick
const GAS_PARTICLE_CAP : u32 = 262144u;  // kGasParticleCap

// The outer density box (§2.5). GAS_OUTER_N cells per axis, GAS_OUTER_SHIFT
// fine voxels per cell, so the box edge is 2x the residency window's and it is
// centred on the window. One 16-BIT COUNT per cell, two to a word (stage 1b
// widened it from a byte; world.h kGasOuterWords has the argument).
//
// DEVIATION from the plan text, which asked for 0.4 m cells AND 2x the window
// edge AND 2 MiB — three numbers that cannot all hold (128^3 bytes is 2 MiB
// and 128 * 0.4 m is 51.2 m, half the stated span). Coverage is kept; the cell
// is 8 voxels = 0.8 m and, since stage 1b, 4 MiB rather than 2.
const GAS_OUTER_N     : u32 = 128u;
const GAS_OUTER_SHIFT : u32 = 3u;
// The per-cell saturation guard. The cell is a 16-BIT count now (stage 1b:
// world.h kGasOuterWords), two to a word, and what the guard buys is unchanged
// from the byte era — that an add cannot CARRY into the neighbouring cell's
// half of the word, which would read as a bright cell one over. 60,000 leaves
// 5,535 of headroom above it, and the most simultaneous adders a cell can have
// between the load and the add is the 512 fine voxels it contains plus the
// parcels stacked in it, so the carry is out of reach rather than merely
// unlikely. A plume dense enough to reach 60,000 in a 0.8 m cell saturates,
// which is what it should look like anyway.
const GAS_OUTER_MAX   : u32 = 60000u;
// Die this many voxels above the window's top face (kGasCeilingVox). Inside
// the outer box on purpose, so the ceiling is a test that can be observed to
// fire rather than one the box would have caught anyway.
const GAS_CEILING_VOX : i32 = 192;
// RNG salts. Distinct streams so the decay roll, the motion roll and the key
// derivation cannot alias.
const GAS_KEY_SALT   : u32 = 0x9A17u;
const GAS_DECAY_SALT : u32 = 0x2C05u;

// (a) RESIDENCY, in the tickets-P0 sense (docs/tickets_p0_audit.md): every
// caller here asks "may I read or write this cell", never "where is the window
// box". Re-entry proposes a voxel, gasBlocked asks whether the GRID is the
// authority at a cell, and the splat asks whether the parcel is already being
// drawn as a voxel — all three are true for a ticket chunk, which is resident
// and simulated and rendered even though it is nowhere near the window.
//
// The WINDOW-BOX questions in this file are gasOuterOrigin/gasCeilY, and they
// deliberately keep saying WORLD_N: the outer density box really is two window
// edges centred on the window, and the kill ceiling really is measured from the
// window's top face. That is class (c) and it does not move.
fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, T.origin); }

fn gasLive(page : u32) -> u32 {
  return min(atomicLoad(&gasCounts[page]), GAS_PARTICLE_CAP);
}

fn gasAppend(p : Particle) {
  let slot = atomicAdd(&gasCounts[1u - T.page], 1u);
  if (slot < GAS_PARTICLE_CAP) { gasWrite[slot] = p; }
}

// The gas motion model — gasRndK, windLateralStartK, gasIntentK, gasLadderStep
// and the GasIntent struct — is in common.wgsl, which is where it belongs and
// where the CA reads it from too. That is the plan's §2.2 requirement made
// structural: a parcel takes the moves a voxel would because it calls the same
// function, not because two copies were kept in step.
//
// The one thing this kernel supplies differently is the roll's inputs. The CA
// keys on its cell's slot index and rolls once per gravity SUBSTEP; a parcel
// keys on gasKey (a hash of its cell and payload) and rolls once per TICK, so
// every call below passes substep 0.

// ---- the outer box ---------------------------------------------------------
// T.origin is in CHUNK units. The box edge is 2x the window's and centred on
// it, so its min corner sits half a window below the window's min corner. The
// window origin is chunk-aligned (a multiple of 16 voxels) and half a window
// is 256, so this is always a multiple of the cell size: no rounding, and the
// mapping the renderer reproduces is exactly this expression.
fn gasOuterOrigin() -> vec3<i32> {
  return T.origin * i32(CHUNK) - vec3<i32>(i32(WORLD_N) / 2);
}

// The kill ceiling, in world voxels.
fn gasCeilY() -> i32 {
  return T.origin.y * i32(CHUNK) + i32(WORLD_N) + GAS_CEILING_VOX;
}

fn gasOuterCell(c : vec3<i32>) -> vec3<i32> {
  return (c - gasOuterOrigin()) >> vec3<u32>(GAS_OUTER_SHIFT);
}

fn gasInOuterBox(c : vec3<i32>) -> bool {
  let d = gasOuterCell(c);
  let n = i32(GAS_OUTER_N);
  return d.x >= 0 && d.y >= 0 && d.z >= 0 && d.x < n && d.y < n && d.z < n;
}

// ---- blocking outside the window (§2.4) ------------------------------------
// The far cascade's level-1 byte, which is the same test the far march uses to
// decide the ray hit something. A dense toroidal array addressed directly —
// NOT the page table, which does not extend past the window and has no
// business being consulted for a parcel of smoke 60 m away. Coarse (one byte
// per 2^(1+FAR_SHIFT_BASE) fine voxels) and right for a plume: what it is for
// is "does the plume go around the hill", not per-voxel collision.
//
// OUTSIDE THE CASCADE reads as OPEN, not as blocked. The conservative
// direction elsewhere in the engine is "assume solid" (the residency edge is
// inert), but here the parcel is already outside the simulated world and the
// alternative is a wall at the cascade boundary that would pin every plume
// against an invisible surface.
fn gasFarBlocked(c : vec3<i32>) -> bool {
  let cell = c >> vec3<u32>(farCellShift(1u));
  if (!farInBox(cell, farP.origins[0].xyz)) { return false; }
  let bi = farVoxByteIndex(1u, cell);
  let b = (farVox[bi >> 2u] >> (8u * (bi & 3u))) & 0xFFu;
  if (b == 0u) { return false; }               // air
  // The blocker flag alone is terrain the centre sample missed — solid, and
  // it carries no material of its own. (This used to fall out of indexing the
  // material table with the whole byte: 128..255 are unwritten entries, whose
  // zeroed klass is CLASS_SOLID. Same answer, said on purpose.)
  if ((b & FAR_BLOCKER_BIT) != 0u) { return true; }
  // A gas does not block a gas — smoke downsampled into the cascade must not
  // wall its own plume off. The byte is a far PALETTE SLOT, so translate it
  // back to a material first (common.wgsl FAR_PAL_MASK).
  return materials[farPalMat(&materials, b & FAR_PAL_MASK)].klass != CLASS_GAS;
}

// Can this parcel enter cell `c`? Inside the window the grid answers (and the
// caller then RE-ENTERS rather than moving); outside, the cascade does.
// Gas particles do not exclude each other: several may share a cell, which is
// what makes the outer density box a density instead of an occupancy map.
fn gasBlocked(c : vec3<i32>) -> bool {
  if (inBounds(c)) {
    let mm = voxMat(voxWordAt(c));
    if (mm == MAT_AIR) { return false; }
    return materials[mm].klass != CLASS_GAS;
  }
  return gasFarBlocked(c);
}

// ---- the splat (§2.5) ------------------------------------------------------
// RENDER-ONLY DERIVED DATA. The sim never reads gasOuter, the world hash never
// covers it, and it is rebuilt from scratch every tick — so the load-then-add
// below is a benign race and is stated as one rather than defended. See
// GAS_OUTER_MAX above for what the guard buys.
//
// TWO SPLATTERS SHARE THIS BOX (stage 1b). This one takes a PARCEL, and the
// CA's gasOuterSplat in sim_step.wgsl takes an in-window VOXEL, once per tick.
// They are separate functions because the two kernels bind gasOuter at
// different numbers and neither may declare it in common.wgsl (the SPIR-V
// cache), but the CELL MAPPING is the same expression in both and
// check_invariants.py pins the constants it is built from.
fn gasSplat(c : vec3<i32>) {
  let d = gasOuterCell(c);
  let n = i32(GAS_OUTER_N);
  if (d.x < 0 || d.y < 0 || d.z < 0 || d.x >= n || d.y >= n || d.z >= n) { return; }
  let li = (u32(d.z) * GAS_OUTER_N + u32(d.y)) * GAS_OUTER_N + u32(d.x);
  let word = li >> 1u;
  let sh = 16u * (li & 1u);
  if (((atomicLoad(&gasOuter[word]) >> sh) & 0xFFFFu) >= GAS_OUTER_MAX) { return; }
  atomicAdd(&gasOuter[word], 1u << sh);
}

// Next-tick dirty mark incl. boundary neighbours — the gas passes run after
// the CA, so a re-entry landing is next tick's business. Same rule and the
// same reason bit as sim_particle.wgsl's markDirtyNext: a gas particle
// rejoining the grid IS a particle landing.
fn gasMarkDirtyNext(c : vec3<i32>) {
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
        let ns = chunkSlotOf(n, T.origin);
        if (ns != SLOT_NONE) {
          atomicOr(&dirtyOut[ns], DIRTY_R_PARTICLE);
        }
      }
    }
  }
}

// ---- the identity key ------------------------------------------------------
// Derived from the parcel's CELL and its payload, never from its buffer slot
// (rule 1, and the particle system's standing rule). Two parcels sharing a
// cell AND a payload therefore roll identically and move together, which is
// correct rather than merely tolerable: they are indistinguishable states, and
// making them diverge would need per-particle identity the 32-byte record does
// not carry. The payload's state nibble (worldgen's palette variant) gives a
// plume a few independent sub-populations for free.
fn gasKey(c : vec3<i32>, payload : u32) -> u32 {
  return hash3(GAS_KEY_SALT ^ payload, bitcast<u32>(c.x),
               bitcast<u32>(c.y) ^ pcg(bitcast<u32>(c.z)));
}

// ---- decay, from the material's own bucket (§2.7) --------------------------
// Walks reactOffset..+reactCount for RK_DECAY entries and rolls each once,
// first hit wins — the same shape as doReactions. Returns 0xFFFFFFFF for "no
// rule fired"; otherwise the product material id (0 = air = die).
//
// TWO KINDS OF RULE ARE SKIPPED, both because the information is not out here:
//   * neighbour-COUNT scaled rules (RSCALE_ON). Outside the window a parcel
//     has no neighbours to count, and scaledChance's answer would be a
//     fabrication. Skipping is the conservative direction: the rule simply
//     does not apply to gas that has left.
//   * nothing else. RCOND_SKY is SATISFIED by construction — a parcel above
//     the window has nothing over it — and the day/night gates read T.dayPhase
//     exactly as the CA does.
fn gasDecayProduct(m : Material, key : u32) -> u32 {
  let day = daylightStrength(T.dayPhase);
  for (var ri = 0u; ri < m.reactCount; ri++) {
    let rule = reactions[m.reactOffset + ri];
    if ((rule.packed & 3u) != RK_DECAY) { continue; }
    if ((rule.cond & RSCALE_ON) != 0u) { continue; }
    let cond = rule.cond & 0xFFu;
    if (cond != 0u) {
      if ((cond & RCOND_DAY) != 0u && day == 0u) { continue; }
      if ((cond & RCOND_NIGHT) != 0u && day != 0u) { continue; }
      if (day < ((rule.cond >> 8u) & 0xFFu)) { continue; }
    }
    let rr = hash3(key, ri, GAS_DECAY_SALT);
    if ((rr % REACT_CHANCE_DEN) < rule.chance) { return rule.prodSelf; }
  }
  return 0xFFFFFFFFu;
}

// ============================== entry points ================================

// pArgs-shaped: [4..6] indirect dispatch {groups, 1, 1}. Gas particles are not
// drawn as instances (they render through gasOuter, in the far march), so the
// draw-args words the ballistic pair carries are absent here.
@compute @workgroup_size(1)
fn gasArgs1() {
  // ZERO THE WRITE PAGE'S CURSOR, here and only here. This kernel is the one
  // point in the tick that runs after the last read of the write page's old
  // value (last tick's resolve) and before the first append into it (this
  // tick's integrate) — so the gas pool needs no per-tick buffer fill, and the
  // "who resets the count" question has one answer instead of a convention.
  atomicStore(&gasCounts[1u - T.page], 0u);
  let n = gasLive(T.page);
  gasArgs[4] = (n + 63u) / 64u;
  gasArgs[5] = 1u;
  gasArgs[6] = 1u;
}

@compute @workgroup_size(1)
fn gasArgs2() {
  let n = gasLive(1u - T.page);
  gasArgs[4] = (n + 63u) / 64u;
  gasArgs[5] = 1u;
  gasArgs[6] = 1u;
  atomicStore(&gasSpawn[GAS_SP_LIVE], n);
}

// Drains BOTH spawn streams into the READ page, before gasArgs1 sizes the
// integrate dispatch — so a parcel that left the grid this tick flies this
// tick, the same latency the ballistic `spawn` path has.
//
// One dispatch covers both lists: [0, GAS_SPAWN_CAP) is the CA's outbox and
// [GAS_SPAWN_CAP, +GAS_CPU_SPAWN_CAP) is the CPU's. Threads past either
// list's live count return immediately, so the fixed dispatch costs one load
// per idle thread and nothing else.
@compute @workgroup_size(64)
fn gasSpawnStep(@builtin(global_invocation_id) gid : vec3<u32>) {
  var p : Particle;
  if (gid.x < GAS_SPAWN_CAP) {
    if (gid.x >= min(atomicLoad(&gasSpawn[GAS_SP_COUNT]), GAS_SPAWN_CAP)) { return; }
    let b = GAS_SP_HDR + gid.x * GAS_SP_STRIDE;
    p.px = bitcast<i32>(atomicLoad(&gasSpawn[b + 0u]));
    p.py = bitcast<i32>(atomicLoad(&gasSpawn[b + 1u]));
    p.pz = bitcast<i32>(atomicLoad(&gasSpawn[b + 2u]));
    p.payload = atomicLoad(&gasSpawn[b + 6u]);
  } else {
    let i = gid.x - GAS_SPAWN_CAP;
    if (i >= min(gasSpawnOps[GAS_SP_COUNT], GAS_CPU_SPAWN_CAP)) { return; }
    let b = GAS_SP_HDR + i * GAS_SP_STRIDE;
    p.px = bitcast<i32>(gasSpawnOps[b + 0u]);
    p.py = bitcast<i32>(gasSpawnOps[b + 1u]);
    p.pz = bitcast<i32>(gasSpawnOps[b + 2u]);
    p.payload = gasSpawnOps[b + 6u];
  }
  // Velocity is ZERO for every gas particle, always: this population moves in
  // whole cells by the CA's ladder, and a nonzero velocity word would change
  // particlePriority without changing anything visible.
  p.vx = 0; p.vy = 0; p.vz = 0;
  // Liveness and pendingness are FORCED, exactly as sim_particle's `spawn`
  // forces them: a malformed op must not be able to inject a particle that is
  // already claiming a cell.
  p.flags = PFLAG_ALIVE | PFLAG_GAS;
  let slot = atomicAdd(&gasCounts[T.page], 1u);
  if (slot >= GAS_PARTICLE_CAP) {
    // At the cap the parcel vaporizes. Degradation, not failure — and it is
    // counted, so "the pool was the bound" is a printed number rather than an
    // inference from a plume that looks thin.
    atomicAdd(&gasSpawn[GAS_SP_POOLFULL], 1u);
    return;
  }
  gasRead[slot] = p;
}

@compute @workgroup_size(64)
fn gasIntegrate(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= gasLive(T.page)) { return; }
  var p = gasRead[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }

  let c = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);

  // ---- kill bounds (rule 2). All three are stateless tests on the parcel's
  // own position, so nothing has to remember anything about it.
  if (!gasInOuterBox(c) || c.y >= gasCeilY()) {
    atomicAdd(&gasSpawn[GAS_SP_DIED], 1u);
    return;                       // not appended: the slot is reclaimed
  }
  if (c.y >= T.origin.y * i32(CHUNK) + i32(WORLD_N)) {
    atomicAdd(&gasSpawn[GAS_SP_ABOVE], 1u);
  }

  let key = gasKey(c, p.payload);
  var mat = p.payload & 0xFFFu;
  var m = materials[mat];

  // ---- decay, before motion: a parcel that dies this tick does not move ----
  let prod = gasDecayProduct(m, key);
  if (prod != 0xFFFFFFFFu && prod != PROD_KEEP) {
    if (prod == 0u) {                       // -> air: gone
      atomicAdd(&gasSpawn[GAS_SP_DIED], 1u);
      return;
    }
    let pm = materials[prod];
    if (pm.klass == CLASS_GAS) {
      // Morph and keep going. The state nibble is re-derived from the key so
      // the new material gets its own palette variant rather than inheriting
      // an index that means something else in its table.
      mat = prod;
      m = pm;
      p.payload = prod | (((key >> 24u) % 3u) << 12u);
    } else {
      // A grid product (steam -> water). It can only land where the grid
      // exists: inside the window it proposes itself through the claim path
      // below; outside, it is rain on unloaded space, which is nothing today
      // either.
      if (!inBounds(c)) {
        atomicAdd(&gasSpawn[GAS_SP_DIED], 1u);
        return;
      }
      p.payload = prod | (((key >> 24u) % 3u) << 12u);
      p.flags |= PFLAG_PENDING;
      atomicMax(&gasClaim[claimSlot(cellIndexW(c))], particlePriority(p));
      gasAppend(p);
      return;
    }
  }

  // ---- motion: the grid model, verbatim (§2.2) -----------------------------
  let rnd = gasRndK(key, 2u, 0u, &T);
  let g = gasIntentK(c, m, key, rnd >> 10u, 0u, &T);
  var tgt = c;
  var found = false;
  for (var i = 0u; i < GAS_LADDER_RING; i++) {
    let s = gasLadderStep(g, 0u, 0u, i);
    if (s.w == 0) { continue; }
    let n = c + s.xyz;
    if (!gasBlocked(n)) { tgt = n; found = true; break; }
  }
  if (!found) {
    let rUp  = windLateralStartK(c, rnd >> 10u, m, key, 0u, &T);
    let rLat = windLateralStartK(c, rnd >> 14u, m, key, 0u, &T);
    for (var i = GAS_LADDER_RING; i < GAS_LADDER_N; i++) {
      let n = c + gasLadderStep(g, rUp, rLat, i).xyz;
      if (!gasBlocked(n)) { tgt = n; found = true; break; }
    }
  }
  // Nowhere to go: stay put and try again next tick. Bounded by the decay.

  p.px = tgt.x << 8u;
  p.py = tgt.y << 8u;
  p.pz = tgt.z << 8u;

  // ---- re-entry = RECONVERT (§2.6) ----------------------------------------
  // A parcel whose next cell is inside the window has nowhere to render (there
  // is no inner density volume in stage 1) and, more to the point, it has
  // rejoined a place where reactions happen. So it proposes itself as a voxel
  // through the SAME atomicMax claim path the ballistic particles use: one
  // claim per parcel per tick, deterministic winner, losers retry.
  if (inBounds(tgt)) {
    p.flags |= PFLAG_PENDING;
    atomicMax(&gasClaim[claimSlot(cellIndexW(tgt))], particlePriority(p));
  }
  gasAppend(p);
}

@compute @workgroup_size(64)
fn gasResolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= gasLive(1u - T.page)) { return; }
  var p = gasWrite[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }
  let cell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);

  if ((p.flags & PFLAG_PENDING) != 0u) {
    // TWO BASES, the same rule as sim_particle's resolve: the claim hashes on
    // the SLOT cell index (an identity), and only the store uses the physical
    // word index (an address).
    let tgtSlot = cellIndexW(cell);
    let won = atomicLoad(&gasClaim[claimSlot(tgtSlot)]) == particlePriority(p);
    if (won && inBounds(cell) && voxMat(voxWordAt(cell)) == MAT_AIR) {
      let mat = p.payload & 0xFFFu;
      var state = (p.payload >> 12u) & 0xFu;
      if (materials[mat].klass == CLASS_LIQUID) { state = LIQ_FULL_STATE; }
      // STAMP_NEVER: it has not acted as a grid voxel yet, so it may move on
      // the tick it lands.
      voxStore(voxWordIndex(cell), packVox(mat, state, STAMP_NEVER));
      gasMarkDirtyNext(cell);
      atomicAdd(&gasSpawn[GAS_SP_REENTER], 1u);
      p.flags = 0u;                       // dead: it is a voxel now
      gasWrite[gid.x] = p;
      return;
    }
    // Lost the claim, or the cell was taken between integrate and resolve.
    // Stay a parcel and retry next tick; it is not stuck, because next tick
    // the cell reads non-air and the ladder routes it somewhere else.
    p.flags = PFLAG_ALIVE | PFLAG_GAS;
    gasWrite[gid.x] = p;
  }

  // The splat, folded into resolve rather than given its own dispatch: it is
  // one atomic per live parcel and this is already the pass that walks every
  // live parcel exactly once. Only cells OUTSIDE the window splat — inside,
  // smoke is a voxel and renders as one, and counting it twice would put a
  // bright halo on the window boundary.
  if (!inBounds(cell)) { gasSplat(cell); }

  // The determinism digest. Every parcel that is still a parcel after this
  // pass contributes; one that became a voxel returned above and is covered by
  // the world hash instead, so the two never double-count a parcel and never
  // drop one.
  atomicAdd(&gasSpawn[GAS_SP_DIGEST], particlePriority(p));
}
