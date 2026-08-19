// sim_step.wgsl — one 3x3x3-color pass of the cellular automaton.
// Dispatched INDIRECTLY over the compacted dirty-chunk list (sim_compact.wgsl):
// one workgroup per dirty chunk, threads map to that chunk's cells of the
// current color (local = colorPhase + lid*3). Any two acting cells are >=3
// apart on every axis while writes reach <=1 cell: destination writes are
// provably disjoint. No atomics on voxel data, fixed pass order =>
// bit-deterministic (DESIGN.md §4). The dirty-list ORDER is scheduling-
// dependent, but each dirty chunk appears exactly once and writes are
// disjoint, so processing order cannot affect sim state.
//
// Each acting cell: reaction scan (substep 0 only, DESIGN.md §6 rules compiled
// from reactions.json) -> movement (powder/gas rules, mass-conserving fullness
// flow for liquids, viscosity gate, critter wander).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(5) var<uniform> P : PassParams;
@group(0) @binding(11) var<storage, read>      reactions : array<Reaction>;
// read_write to match the shared layout entry (sim_compact writes it);
// this shader only reads.
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
// NOTE: binding 13 (dispatch args) must stay undeclared here — statically
// unused bindings are excluded from the dispatch usage scope, which is what
// makes the same buffer legal as the INDIRECT source in this compute pass.
// Per-chunk support-loss flags: set when a supporting voxel (solid/powder)
// vacates or transforms next to a solid. Side-channel only — read back by the
// CPU to queue island checks (debris.cpp), never fed back into voxel state,
// so determinism is unaffected (all writers store the same value 1).
@group(0) @binding(15) var<storage, read_write> supportOut : array<atomic<u32>>;

// Unloaded space is solid and inert (DESIGN.md §3): the sim's world edge is
// the residency window, not a fixed cube.
fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

// A supporting cell at c stopped supporting (its occupant left or became
// non-solid/non-powder). If a CLASS_SOLID voxel rests on / hangs off it, flag
// that solid's chunk so the CPU runs a bounded island check there.
// oldKlass==POWDER checks up only (solids REST on powder; a solid merely
// beside a shifting sand pile is not supported by it — checking laterals
// would flag every wall next to settling sand).
fn flagSupportLoss(c : vec3<i32>, oldKlass : u32, newMat : u32) {
  if (oldKlass != CLASS_SOLID && oldKlass != CLASS_POWDER) { return; }
  let nm = newMat & 0xFFFu;  // 12-bit id; sentinel values land on a zeroed entry
  if (nm != MAT_AIR) {
    let nk = materials[nm].klass;
    if (nk == CLASS_SOLID || nk == CLASS_POWDER) { return; }  // still supports
  }
  for (var i = 0u; i < 6u; i++) {
    if (oldKlass == CLASS_POWDER && i != 1u) { continue; }  // up only
    let n = c + faceDir(i);
    if (!inBounds(n)) { continue; }
    let nmat = voxMat(voxels[cellIndexW((n))]);
    if (nmat != MAT_AIR && materials[nmat].klass == CLASS_SOLID) {
      atomicStore(&supportOut[chunkIndexW(n)], 1u);
      return;
    }
  }
}

// Mark the chunk containing world cell c dirty for next tick, plus every
// neighbor chunk c borders (cross-chunk neighbors re-evaluate; sleeping is
// per-chunk). Chunks outside the residency window don't exist to mark.
fn markDirty(c : vec3<i32>) {
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

// Can a mover of (klass, density) enter the cell holding word tw?
// rising=true for gases (they seek lower density above), false for falling.
fn canDisplace(myDensity : i32, rising : bool, tw : u32) -> bool {
  let tmat = voxMat(tw);
  var td : i32;
  if (tmat == MAT_AIR) {
    td = AIR_DENSITY;
  } else {
    let t = materials[tmat];
    if (t.klass == CLASS_SOLID || t.klass == CLASS_POWDER) { return false; }
    td = t.density;
  }
  if (rising) { return td > myDensity; }
  return td < myDensity;
}

fn tryMove(src : vec3<i32>, dst : vec3<i32>, myWord : u32, myDensity : i32, rising : bool) -> bool {
  if (!inBounds(dst)) { return false; }
  let di = cellIndexW((dst));
  let tw = voxels[di];
  if (!canDisplace(myDensity, rising, tw)) { return false; }
  let stamp = stampFor(T.tick, P.substep);
  voxels[di] = packVox(voxMat(myWord), voxState(myWord), stamp);
  // displaced fluid (or air) swaps into the source cell, stamped so it does
  // not act again this tick
  voxels[cellIndexW((src))] = packVox(voxMat(tw), voxState(tw), stamp);
  markDirty(src);
  markDirty(dst);
  // a powder sliding out from under a solid may leave it floating
  let myKlass = materials[voxMat(myWord)].klass;
  if (myKlass == CLASS_POWDER) { flagSupportLoss(src, myKlass, voxMat(tw)); }
  return true;
}

// Move t eighths of liquid `mat` from src (fullness sf) onto dst (fullness df,
// 0 = air). Mass-conserving: src empties to air when it gives everything.
fn transferLiquid(src : vec3<i32>, dst : vec3<i32>, mat : u32,
                  sf : u32, df : u32, t : u32) {
  let stamp = stampFor(T.tick, P.substep);
  let si = cellIndexW((src));
  let di = cellIndexW((dst));
  if (t >= sf) { voxels[si] = 0u; }
  else { voxels[si] = packVox(mat, sf - t - 1u, stamp); }
  voxels[di] = packVox(mat, df + t - 1u, stamp);
  markDirty(src);
  markDirty(dst);
}

// The six face directions with their RDIR_* bits: -y, +y, then laterals.
fn faceDir(i : u32) -> vec3<i32> {
  switch (i % 6u) {
    case 0u: { return vec3<i32>(0, -1, 0); }
    case 1u: { return vec3<i32>(0,  1, 0); }
    case 2u: { return vec3<i32>( 1, 0, 0); }
    case 3u: { return vec3<i32>(-1, 0, 0); }
    case 4u: { return vec3<i32>(0, 0,  1); }
    default: { return vec3<i32>(0, 0, -1); }
  }
}
fn faceDirBit(i : u32) -> u32 {
  switch (i % 6u) {
    case 0u: { return RDIR_DOWN; }
    case 1u: { return RDIR_UP; }
    default: { return RDIR_SIDE; }
  }
}

// State nibble for a freshly created voxel: liquids are born full (their
// nibble is fullness, not a palette variant).
fn productState(mat : u32, r : u32) -> u32 {
  if (mat != MAT_AIR && materials[mat].klass == CLASS_LIQUID) { return LIQ_FULL_STATE; }
  return r % 3u;
}

fn nbrMatches(rule : Reaction, nmat : u32, nm : Material) -> bool {
  if (rule.nbrClass != 0u && ((1u << nm.klass) & rule.nbrClass) == 0u) { return false; }
  if (rule.nbrMat != NBR_ANY) { return nmat == rule.nbrMat; }
  if (rule.nbrTags != 0u) { return (nm.tagMask & rule.nbrTags) != 0u; }
  return true;  // wildcard "any"
}

// Runs the cell's reaction bucket. At most one rule fires per tick. Returns
// true if SELF changed material (caller then skips movement this substep).
// Matching-but-unfired rules mark the chunk dirty so reactive neighborhoods
// stay awake until they resolve — sleeping stays activity-bounded because
// every chain (fire, growth, decay) terminates by transforming its inputs.
fn doReactions(c : vec3<i32>, idx : u32, w : u32, mat : u32, m : Material, rnd : u32) -> bool {
  var keepAwake = false;
  let stamp = stampFor(T.tick, P.substep);

  for (var ri = 0u; ri < m.reactCount; ri++) {
    let rule = reactions[m.reactOffset + ri];
    let kind = rule.packed & 3u;
    let dmask = (rule.packed >> 2u) & 7u;
    let rr = hash3(rnd, ri, idx);
    let rot = rr >> 12u;

    if (kind == RK_DECAY) {
      keepAwake = true;
      if ((rr % 1000u) < rule.chance) {
        if (rule.prodSelf == 0u) { voxels[idx] = 0u; }
        else { voxels[idx] = packVox(rule.prodSelf, productState(rule.prodSelf, rnd), stamp); }
        markDirty(c);
        flagSupportLoss(c, m.klass, rule.prodSelf);  // ember->ash drops the wood above
        return true;
      }
    } else if (kind == RK_EMIT) {
      // first air cell among allowed dirs (RNG-rotated scan)
      for (var i = 0u; i < 6u; i++) {
        let di = (i + rot) % 6u;
        if ((faceDirBit(di) & dmask) == 0u) { continue; }
        let n = c + faceDir(di);
        if (!inBounds(n)) { continue; }
        let ni = cellIndexW((n));
        if (voxMat(voxels[ni]) != MAT_AIR) { continue; }
        keepAwake = true;
        if ((rr % 1000u) < rule.chance) {
          voxels[ni] = packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp);
          markDirty(n);
          markDirty(c);
          if (rule.prodSelf != PROD_KEEP) {
            if (rule.prodSelf == 0u) { voxels[idx] = 0u; }
            else { voxels[idx] = packVox(rule.prodSelf, productState(rule.prodSelf, rnd), stamp); }
            flagSupportLoss(c, m.klass, rule.prodSelf);
            return true;
          }
          return false;  // emitted; self unchanged, may still move
        }
        break;  // one roll per rule per tick
      }
    } else {  // RK_PAIR
      for (var i = 0u; i < 6u; i++) {
        let di = (i + rot) % 6u;
        if ((faceDirBit(di) & dmask) == 0u) { continue; }
        let n = c + faceDir(di);
        if (!inBounds(n)) { continue; }
        let ni = cellIndexW((n));
        let nw = voxels[ni];
        let nmat = voxMat(nw);
        if (nmat == MAT_AIR) { continue; }
        if (!nbrMatches(rule, nmat, materials[nmat])) { continue; }
        keepAwake = true;
        if ((rr % 1000u) < rule.chance) {
          if (rule.prodNbr != PROD_KEEP) {
            if (rule.prodNbr == 0u) { voxels[ni] = 0u; }
            else { voxels[ni] = packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp); }
            markDirty(n);
            flagSupportLoss(n, materials[nmat].klass, rule.prodNbr);
          }
          if (rule.prodSelf != PROD_KEEP) {
            if (rule.prodSelf == 0u) { voxels[idx] = 0u; }
            else { voxels[idx] = packVox(rule.prodSelf, productState(rule.prodSelf, rnd), stamp); }
            markDirty(c);
            flagSupportLoss(c, m.klass, rule.prodSelf);
            return true;
          }
          markDirty(c);
          return false;  // neighbor transformed; self may still move
        }
        break;  // one roll per rule per tick
      }
    }
  }
  if (keepAwake) { markDirty(c); }
  return false;
}

// Mass-conserving liquid flow (fullness in eighths, DESIGN.md §4).
fn stepLiquid(c : vec3<i32>, idx : u32, w : u32, mat : u32, m : Material, rnd : u32) {
  let f = voxState(w) + 1u;  // fullness 1..8

  // 1) below: fill a partial same-liquid cell, else whole-cell move/swap
  let below = c + vec3<i32>(0, -1, 0);
  if (inBounds(below)) {
    let bw = voxels[cellIndexW((below))];
    let bmat = voxMat(bw);
    if (bmat == mat) {
      let bf = voxState(bw) + 1u;
      if (bf < 8u) {
        transferLiquid(c, below, mat, f, bf, min(f, 8u - bf));
        return;
      }
    } else if (tryMove(c, below, w, m.density, false)) {
      return;  // air or a displaceable lighter fluid
    }
  }

  // 2) four down-diagonals, whole-cell only, RNG order
  let r = rnd >> 10u;
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (tryMove(c, c + vec3<i32>(d.x, -1, d.y), w, m.density, false)) { return; }
  }

  // 3) lateral: equalize into a same-liquid neighbor holding >=2 less, split
  //    into air (never below fullness 1 — thin films sit still and sleep),
  //    or whole-cell displace a lighter fluid. RNG order.
  let r2 = rnd >> 14u;
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r2);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxels[cellIndexW((n))];
    let nmat = voxMat(nw);
    if (nmat == mat) {
      let nf = voxState(nw) + 1u;
      if (nf + 2u <= f) {
        transferLiquid(c, n, mat, f, nf, (f - nf) / 2u);
        return;
      }
    } else if (nmat == MAT_AIR) {
      if (f >= 2u) {
        transferLiquid(c, n, mat, f, 0u, f / 2u);
        return;
      }
    } else if (tryMove(c, n, w, m.density, false)) {
      return;
    }
  }
  // settled: no markDirty — a flat pool (all lateral diffs < 2) sleeps
}

@compute @workgroup_size(6, 6, 6)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_id) lid : vec3<u32>) {
  // one workgroup per compacted dirty chunk (indirect dispatch). The list
  // holds SLOT indices; reconstruct the world chunk from the window origin.
  let ci = dirtyList[wg.x];
  let sc = vec3<i32>(vec3<u32>(ci % NCHUNK, (ci / NCHUNK) % NCHUNK,
                               ci / (NCHUNK * NCHUNK)));
  let wc = slotToWorldChunk(sc, T.origin);
  let base = wc * i32(CHUNK);  // world cell of the chunk corner (may be < 0)
  // The color lattice is GLOBAL in WORLD coords: cell ≡ colorPhase (mod 3).
  // Coloring by slot coords would race at the toroidal wrap (world-adjacent
  // cells whose slots are WORLD_N apart would share a color); world coords
  // keep same-color cells >=3 apart in the space movement happens in.
  let bmod = ((base % vec3<i32>(3)) + vec3<i32>(3)) % vec3<i32>(3);
  let start = (vec3<i32>(P.colorPhase) + vec3<i32>(3) - bmod) % vec3<i32>(3);
  let local = start + vec3<i32>(lid) * 3;
  if (local.x >= i32(CHUNK) || local.y >= i32(CHUNK) || local.z >= i32(CHUNK)) { return; }
  let c = base + local;  // world cell this thread acts on

  let idx = cellIndexW(c);
  let w = voxels[idx];
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return; }
  if (voxStamp(w) == stampFor(T.tick, P.substep)) { return; }  // already acted this substep

  let m = materials[mat];
  let rnd = hash3(T.seed, T.tick * 2u + P.substep, idx);

  // Reactions roll once per tick (substep 0 of the two gravity substeps).
  if (P.substep == 0u && m.reactCount > 0u) {
    if (doReactions(c, idx, w, mat, m, rnd)) { return; }
  }

  if (m.klass == CLASS_SOLID) { return; }

  // Viscosity: thick liquids (lava, molten glass) only move on their tick.
  if (m.moveEvery > 1u && (T.tick % m.moveEvery) != 0u) {
    markDirty(c);  // stay awake for the tick it may move on
    return;
  }

  if (m.klass == CLASS_LIQUID) {
    // re-read: a reaction may have rewritten this cell's fullness
    stepLiquid(c, idx, voxels[idx], mat, m, rnd);
    return;
  }

  let rising = m.klass == CLASS_GAS;
  let dy = select(-1, 1, rising);

  // 1) straight fall / rise
  if (tryMove(c, c + vec3<i32>(0, dy, 0), w, m.density, rising)) { return; }

  // 2) the four diagonal cells one step down (up for gas), RNG order
  let r = rnd >> 10u;
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (tryMove(c, c + vec3<i32>(d.x, dy, d.y), w, m.density, rising)) { return; }
  }

  // 3) gases also spread laterally, RNG order
  if (m.klass == CLASS_GAS) {
    let r2 = rnd >> 14u;
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + r2);
      if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w, m.density, rising)) { return; }
    }
  }

  // 4) wandering powders (mites): scuttle laterally, occasionally hop up.
  if (m.klass == CLASS_POWDER && (m.flags & MATF_WANDER) != 0u) {
    let r3 = rnd >> 18u;
    if ((r3 & 7u) == 0u &&
        tryMove(c, c + vec3<i32>(0, 1, 0), w, m.density, false)) { return; }
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + (r3 >> 3u));
      if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w, m.density, false)) { return; }
    }
  }
  // Nothing to do: cell settles. If the whole chunk settles, nothing marks it
  // dirty and it sleeps until a neighbor wakes it.
}
