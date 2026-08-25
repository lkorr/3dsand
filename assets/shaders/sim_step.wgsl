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
// ---- MLS-MPM excited-fluid coupling (sim_fluid_seam.wgsl; DESIGN.md §5) ----
// The solver's block map + node grid (LAST tick's final substep) and the
// seam's per-cell intent/flags scratch. Read here so authored PAIR rules see
// excited liquid as a real neighbour (fluidOccMat below); the one write is
// the consume flag when such a rule fires. A world with no fluid has an
// all-zero block map and none of this costs more than one load per air
// neighbour of a reacting cell — and the pinned hash is the gate on "no
// fluid means no behaviour change".
@group(0) @binding(20) var<storage, read> fluidBlockMapS : array<u32>;
@group(0) @binding(21) var<storage, read> fluidGridS : array<i32>;
@group(0) @binding(22) var<storage, read_write> fluidCellScratch : array<atomic<u32>>;
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
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;

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
    let nmat = voxMat(voxWordAt((n)));
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
  let di = voxWordIndex((dst));
  let tw = voxWordAt((dst));
  if (!canDisplace(myDensity, rising, tw)) { return false; }
  let stamp = stampFor(T.tick, P.substep);
  // Stain travels WITH the voxel, not with the cell: a stained pebble that
  // falls is still stained, and the air it left behind is not. Both sides of
  // the swap therefore carry their own source word's stain bits.
  voxStore(di, packVoxKeepStain(voxMat(myWord), voxState(myWord), stamp, myWord));
  // displaced fluid (or air) swaps into the source cell, stamped so it does
  // not act again this tick
  voxStore(voxWordIndex((src)),
           packVoxKeepStain(voxMat(tw), voxState(tw), stamp, tw));
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
  let si = voxWordIndex((src));
  let di = voxWordIndex((dst));
  // Stain and flowing liquid: the DESTINATION keeps its own stain, and the
  // source keeps its own. A liquid moving through a cell does not pick the
  // cell's stain up and carry it downstream — stain marks the SURFACE that was
  // soaked, and it stays on that surface until the stain rule itself changes
  // it. (A stained liquid voxel is a normal thing to have: blood that has
  // pooled on stained ground reads stained through, which is right.)
  //
  // A source cell that empties completely goes to 0 — full air, no stain. That
  // is deliberate: the stain belonged to the liquid that just left, and an
  // empty cell of air has no surface to hold it.
  let sw = voxWordAt((src));
  let dw = voxWordAt((dst));
  if (t >= sf) { voxStore(si, 0u); }
  else { voxStore(si, packVoxKeepStain(mat, sf - t - 1u, stamp, sw)); }
  voxStore(di, packVoxKeepStain(mat, df + t - 1u, stamp, dw));
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

// ---- sky exposure (daylight-gated reactions) --------------------------------
// "Is this cell exposed to the sky?" — answered by looking at the ONE cell
// directly above it, and nothing further.
//
// ---- WHY THIS IS NOT A COLUMN WALK ----
// The obvious implementation is to march up until something blocks, so that a
// pond in a cave is correctly "indoors". That was tried here and it BREAKS
// DETERMINISM, for a reason worth recording because it is easy to talk
// yourself out of:
//
// The 3x3x3 color lattice guarantees that two cells acting in the same pass
// are >=3 apart and that WRITES reach <=1 cell, so writes never collide. It
// guarantees nothing about READS. A 48-cell probe column crosses dozens of
// cells that other threads in this very pass are legally writing, so whether
// the probe sees a cell before or after its update depends on scheduling —
// exactly the "no scheduling-dependent outcomes" ban (CLAUDE.md rule 1). It
// reproduced as a hash divergence at tick 1, with water freezing under
// different roofs on each run.
//
// A one-cell look-up stays inside the guarantee: the cell above is either
// >=3 away (so it is not acting this pass) or is the mover that is writing
// into this very cell, which the stamp check already serializes.
//
// The cost of the honest version is that "sky" means "nothing directly on top
// of me" rather than "open to the heavens", so water in a lit cave evaporates
// too. That is a content inaccuracy, not a correctness one, and the trade is
// forced: the deterministic alternative is a separate mark/apply pass over a
// sky-exposure buffer (the pattern sim_explode.wgsl uses), which is the right
// answer if this ever needs to tell a cave from a meadow.
fn seesSky(c : vec3<i32>) -> bool {
  let n = c + vec3<i32>(0, 1, 0);
  // Left the window going up = open sky above.
  if (!inBounds(n)) { return true; }
  let nmat = voxMat(voxWordAt(n));
  if (nmat == MAT_AIR) { return true; }
  // Translucent things (water, glass, smoke, steam) let light through; only a
  // ray blocker really shades the cell below it.
  return !isRayBlocker(materials[nmat]);
}

// Does the cell's light environment satisfy this rule's condition?
// Encoded in Reaction.cond (see materials.h ReactionGpu.cond):
//   bit0 RCOND_SKY   — requires open sky above
//   bit1 RCOND_DAY   — requires daytime
//   bit2 RCOND_NIGHT — requires night
// `minLight` (bits 8..15) is a daylight-strength floor, so "only near noon"
// rules are expressible without a second condition bit.
fn lightMatches(rule : Reaction, c : vec3<i32>) -> bool {
  let cond = rule.cond & 0xFFu;
  if (cond == 0u) { return true; }  // unconditional: the common case, free
  let day = daylightStrength(T.dayPhase);
  if ((cond & RCOND_DAY) != 0u && day == 0u) { return false; }
  if ((cond & RCOND_NIGHT) != 0u && day != 0u) { return false; }
  let minLight = (rule.cond >> 8u) & 0xFFu;
  if (day < minLight) { return false; }
  // Sky probe last: it is the only expensive test, so the cheap phase checks
  // above reject most cells before it ever runs.
  if ((cond & RCOND_SKY) != 0u && !seesSky(c)) { return false; }
  return true;
}

// Excited-fluid occupancy of an AIR cell: the material id the seam's intent
// carries, or 0 when no meaningful fluid mass is there. One tick latent by
// design (the seam wrote both after last tick's substeps), deterministic, and
// gated on the block map so a fluid-free world pays one zero-load.
fn fluidOccMat(n : vec3<i32>) -> u32 {
  let wc = worldChunkOf(n);
  if (!chunkInWindow(wc, T.origin)) { return 0u; }
  let bm = fluidBlockMapS[chunkSlotIndex(wc)];
  if (bm == 0u) { return 0u; }
  let lo = vec3<u32>(n & vec3<i32>(CHUNK_MASK));
  let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  if (fluidGridS[ci * FLUID_GW] < 1024) { return 0u; }  // < 1 particle mass
  return atomicLoad(&fluidCellScratch[ci * 2u]) >> 16u;
}

// A rule fired against synthesized excited fluid and takes the neighbour:
// flag the cell so the seam's consumeApply kills its particle bin this tick.
// atomicOr — order-free, idempotent.
fn flagFluidConsume(n : vec3<i32>) {
  let wc = worldChunkOf(n);
  if (!chunkInWindow(wc, T.origin)) { return; }
  let bm = fluidBlockMapS[chunkSlotIndex(wc)];
  if (bm == 0u) { return; }
  let lo = vec3<u32>(n & vec3<i32>(CHUNK_MASK));
  let ci = (bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
  atomicOr(&fluidCellScratch[ci * 2u + 1u], 1u);
}

fn nbrMatches(rule : Reaction, nmat : u32, nm : Material) -> bool {
  if (rule.nbrClass != 0u && ((1u << nm.klass) & rule.nbrClass) == 0u) { return false; }
  if (rule.nbrMat != NBR_ANY) { return nmat == rule.nbrMat; }
  if (rule.nbrTags != 0u) { return (nm.tagMask & rule.nbrTags) != 0u; }
  return true;  // wildcard "any"
}

// Scales a rule's chance by how many of the 6 face neighbours match its
// neighbour predicate. Returns the effective chance in units of
// 1/REACT_CHANCE_DEN; 0 means the rule cannot fire at all this tick.
//
// This is what makes a rule spread from a FRONTIER instead of nucleating
// uniformly. Water freezing is the motivating case: scaled by the count of
// non-water neighbours, a pond's banks and surface freeze first and the ice
// creeps inward, because every new ice voxel raises its liquid neighbours'
// odds. Deep water is surrounded by water, counts 0, and cannot freeze until
// the front reaches it.
//
// `minCount` generalizes the count-0 gate into a count-<N gate, which is what
// evaporation needs: it counts NON-water neighbours too, but demands at least
// 4 of them, so a lone droplet (6 non-water) boils off fast, a rim cell (4-5)
// goes slowly, and the flat surface of a pond (1 non-water — just the air
// above) is immune. Without the floor, a pond surface would fire at the full
// base chance, which is exactly the "way too much steam" failure.
//
// The return is in a FINER denominator than the authored per-mille, because
// the interesting rules are authored at chance 1-2: computing
// `(chance * q) / 4` per-mille would truncate 1.5x and 2.75x onto the same
// integer, collapsing a 6-step ramp to 4 steps. Scaling the numerator instead
// keeps every step distinct at chance 1.
//
// Determinism (rule 1): this READS a 1-cell neighbourhood and writes nothing,
// so it stays inside the colour lattice's guarantee — the lattice bounds
// WRITES to 1 cell, and no cell within 1 of an acting cell is itself acting.
// All integer: the multiplier is quarters, biased by 1.0x, and the whole
// expression is done in u32 with the divide last so it rounds identically on
// every vendor. A float here would be a determinism bug.
fn scaledChance(rule : Reaction, c : vec3<i32>) -> u32 {
  if ((rule.cond & RSCALE_ON) == 0u) { return rule.chance; }
  let invert = (rule.cond & RSCALE_INVERT) != 0u;

  var count = 0u;
  for (var i = 0u; i < 6u; i++) {
    let n = c + faceDir(i);
    // Out-of-window space is solid and inert, and reads as "not the counted
    // material" — which is right for ice: the residency edge acts like a bank
    // rather than like more water.
    var hit = false;
    if (inBounds(n)) {
      let nmat = voxMat(voxWordAt(n));
      // MAT_AIR has no Material entry worth matching on tags/class, so an
      // air neighbour only counts via an exact nbrMat == 0 predicate.
      if (nmat == MAT_AIR) { hit = rule.nbrMat == MAT_AIR; }
      else { hit = nbrMatches(rule, nmat, materials[nmat]); }
    }
    if (hit != invert) { count++; }
  }
  // Hard gate: below the minimum count there is no frontier, so no reaction.
  // minCount defaults to 1 (any matching neighbour will do), which is the
  // freezing case. Evaporation raises it so that a water voxel with a couple
  // of watery neighbours still counts as "part of the pond" and is immune,
  // while an exposed droplet is not.
  let minCount = ((rule.cond >> RSCALE_MIN_SHIFT) & RSCALE_MIN_MASK) + 1u;
  if (count < minCount) { return 0u; }

  // chance * lerp(1.0x, maxMul, (count-1)/5), integer throughout, evaluated
  // with the single divide LAST so nothing is truncated mid-ramp.
  //   scaled = chance * SCALE * (4 + span*(count-1)/5) / 4
  // Numerator first, then one divide by (RSCALE_MUL_UNIT * 5) = 20, which
  // divides REACT_CHANCE_SCALE exactly — so every one of the 6 steps lands on
  // a distinct integer even at chance 1.
  let maxQ = ((rule.cond >> RSCALE_MUL_SHIFT) & RSCALE_MUL_MASK) + RSCALE_MUL_UNIT;
  let span = maxQ - RSCALE_MUL_UNIT;  // quarters above 1.0x
  let num = rule.chance * (RSCALE_MUL_UNIT * 5u + span * (count - 1u));
  let scaled = num / (RSCALE_MUL_UNIT * 5u);
  return min(scaled, REACT_CHANCE_DEN);
}

// Runs the cell's reaction bucket. At most one rule fires per tick. Returns
// true if SELF changed material (caller then skips movement this substep).
// Matching-but-unfired rules mark the chunk dirty so reactive neighborhoods
// stay awake until they resolve — sleeping stays activity-bounded because
// every chain (fire, growth, decay) terminates by transforming its inputs.
// TWO BASES (§4.1), exactly as in main: `idx` is the PHYSICAL word index and is
// only ever a memory address for voxStore; `slotIdx` is the SLOT cell index and
// is the only thing that may key the RNG. Passing `idx` to hash3 here made every
// reaction roll a function of ALLOCATION HISTORY, so a paged run diverged from a
// dense one with no other symptom — silently, because under the identity map
// (dense) the two are equal. Found as a deterministic, reproducible lava/stone
// swap at slot 9450 t43 in --vk-smoke-loud --residency paged.
fn doReactions(c : vec3<i32>, idx : u32, slotIdx : u32, w : u32, mat : u32,
               m : Material, rnd : u32) -> bool {
  var keepAwake = false;
  let stamp = stampFor(T.tick, P.substep);

  for (var ri = 0u; ri < m.reactCount; ri++) {
    let rule = reactions[m.reactOffset + ri];
    let kind = rule.packed & 3u;
    let dmask = (rule.packed >> 2u) & 7u;
    let rr = hash3(rnd, ri, slotIdx);  // SLOT index: never the page index
    let rot = rr >> 12u;

    // Light/phase gate. A rule whose condition is not met is skipped WITHOUT
    // setting keepAwake — that is what lets a lit pond go back to sleep at
    // night instead of spinning on a rule that cannot fire (rule 2). The
    // chunk is re-woken when the phase crosses back, see wakeOnPhaseChange.
    if (!lightMatches(rule, c)) { continue; }

    // A light-gated rule does not hold its chunk awake even when it MATCHES.
    //
    // The unconditional rules use keepAwake to mean "this neighbourhood is
    // reactive, re-examine it next tick", which is right for chains that
    // resolve on their own (fire burns out, growth terminates). A light-gated
    // rule has no such terminus: it stays matched for as long as the sun is in
    // the right part of the sky, which is thousands of ticks. Letting it set
    // keepAwake pins every affected chunk awake for half of every in-game day
    // — measured at 292/32768 chunks still active from ONE such rule, against
    // a budget of 32 (CLAUDE.md rule 2).
    //
    // Instead the day phase itself is the wake signal: Simulation::
    // EncodeWakeAll re-dirties the world on the tick daylight switches on or
    // off, which is a handful of ticks per in-game day. Between those
    // boundaries a chunk with only light-gated work sleeps, and the rules
    // still fire on the ticks it is awake for other reasons.
    let lightGated = (rule.cond & 0xFFu) != 0u;

    if (kind == RK_DECAY) {
      // Neighbour-count scaling (frontier rules — see scaledChance). Returns
      // rule.chance untouched for the ordinary unscaled case; 0 means the cell
      // has no qualifying neighbours and the rule is inert here this tick.
      let chance = scaledChance(rule, c);
      if (chance == 0u) { continue; }
      keepAwake = keepAwake || !lightGated;
      if ((rr % REACT_CHANCE_DEN) < chance) {
        if (rule.prodSelf == 0u) { voxStore(idx, 0u); }
        else { voxStore(idx, packVox(rule.prodSelf, productState(rule.prodSelf, rnd), stamp)); }
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
        let ni = voxWordIndex((n));
        if (voxMat(voxWordAt((n))) != MAT_AIR) { continue; }
        keepAwake = keepAwake || !lightGated;
        if ((rr % REACT_CHANCE_DEN) < rule.chance) {
          voxStore(ni, packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp));
          markDirty(n);
          markDirty(c);
          if (rule.prodSelf != PROD_KEEP) {
            if (rule.prodSelf == 0u) { voxStore(idx, 0u); }
            else { voxStore(idx, packVox(rule.prodSelf, productState(rule.prodSelf, rnd), stamp)); }
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
        let ni = voxWordIndex((n));
        let nw = voxWordAt((n));
        var nmat = voxMat(nw);
        // Excited-fluid synthesis: an air cell holding MPM particles reads
        // as a liquid neighbour of the particles' material, so every
        // authored PAIR rule works against excited water exactly as against
        // a fullness voxel. Consumption crosses the seam through the flag —
        // the particles die in consumeApply this same tick (plan §6.2).
        var synthFluid = false;
        if (nmat == MAT_AIR) {
          nmat = fluidOccMat(n);
          if (nmat == 0u) { continue; }
          synthFluid = true;
        }
        if (!nbrMatches(rule, nmat, materials[nmat])) { continue; }
        keepAwake = keepAwake || !lightGated;
        if ((rr % REACT_CHANCE_DEN) < rule.chance) {
          if (rule.prodNbr != PROD_KEEP) {
            if (synthFluid) { flagFluidConsume(n); }
            // For a synthesized neighbour ni is the air cell: a product
            // writes into it (condensed stone, grown plant); prodNbr == 0
            // rewrites air over air, harmless.
            if (rule.prodNbr == 0u) { voxStore(ni, 0u); }
            else { voxStore(ni, packVox(rule.prodNbr, productState(rule.prodNbr, rr >> 4u), stamp)); }
            markDirty(n);
            if (!synthFluid) {
              flagSupportLoss(n, materials[nmat].klass, rule.prodNbr);
            }
          }
          if (rule.prodSelf != PROD_KEEP) {
            if (rule.prodSelf == 0u) { voxStore(idx, 0u); }
            else { voxStore(idx, packVox(rule.prodSelf, productState(rule.prodSelf, rnd), stamp)); }
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

// ---- staining (DESIGN.md §6) ------------------------------------------------
// A staining liquid marks the voxels it touches. The mark lives in the voxel
// word's spare bits (STAIN_* in common.wgsl) as a 3-bit type + 4-bit amount:
// no side buffer, no struct growth, and it survives movement because every sim
// write carries the stain across (packVoxKeepStain).
//
// Authored per material, not per material PAIR — "blood stains what it touches"
// is one line in materials.json and applies to every surface in the game,
// present and future, which is the same anti-N×M argument tags exist for
// (CLAUDE.md conventions). Rules:
//
//   * A staining liquid rolls once per tick against `chance` (per-mille).
//   * On success it stains ONE face neighbour — chosen by an RNG rotation, so
//     which one is deterministic but not biased toward an axis.
//   * The stain ADDS to whatever the neighbour already carries, saturating at
//     STAIN_AMT_MAX. Repeated contact deepens a stain rather than resetting it.
//   * Having stained, it may CONSUME the voxel (per-mille `consume`), which
//     deletes it to air and lets the liquid flow into the hole.
//
// ---- DETERMINISM (rule 1) ----
// Write reach is exactly 1 cell (a face neighbour), which is what the 3x3x3
// colour lattice bounds; two cells acting in the same pass are >=3 apart, so
// no two stainers can write the same neighbour. All integer, all from
// hash3(seed, tick, cell) — no scheduling, no atomics, no float.
//
// ---- SLEEP (rule 2) ----
// This is the subtle half, and it is why the rule tracks `progress`.
// A pool of blood sitting on stone is a PERMANENT condition: the liquid is
// there, the stone is there, and a rule that says "keep this chunk awake while
// I am touching something stainable" would pin every blood-soaked chunk awake
// forever — the exact failure the light-gated rules hit (see the keepAwake note
// in doReactions, and gotcha: light-gated rules never sleep).
//
// So the chunk is kept awake ONLY while there is work left to do: a neighbour
// that is not yet stained to the full amount this material applies. Once every
// touching surface has taken all the stain it can, nothing marks the chunk and
// the pool settles and sleeps. That termination is what makes the rule
// decisively subcritical: the reachable surface is finite, each cell's stain
// is bounded by STAIN_AMT_MAX, and stain only ever increases.
//
// ---- absorption and washing (DESIGN.md §6) ----------------------------------
// Two behaviours layered on the rule above, both authored as data:
//
//   * ABSORPTION. A substrate declares `absorb: {capacity: N}` — how deep a
//     stain it will hold. A staining liquid soaking into UNSATURATED ground
//     SPENDS ITSELF doing it: one eighth of the source cell's fullness per
//     successful contact. So a puddle on dry grass drains away into the grass,
//     and only once the ground under it is saturated does the water stop
//     vanishing and start to persist as a pool on top. That "spend a unit of
//     mass" step is the whole feature — without it the puddle stains the ground
//     and then sits there full forever, which is the current behaviour and is
//     not absorption at all.
//
//     The capacity comes off the NEIGHBOUR (the ground), and the per-contact
//     step off the STAINER (the liquid): how deep the ground can get wet is a
//     property of the ground, how fast it soaks is a property of the liquid.
//     Capacity 0 (every material that predates this, all stone) means the
//     liquid never soaks in and pools immediately.
//
//   * WASHING. A liquid with `stain: {washes: true}` rinses a FOREIGN stain out
//     instead of repainting it: the foreign amount is stepped DOWN toward 0, and
//     only once it is gone does the washer's own stain start to build. This is
//     water cleaning blood off the ground. Overwriting (the old behaviour) would
//     have relabelled blood as "wet" at full strength — the colour would change
//     but the mess would never actually come out.
//
// ---- SLEEP (rule 2) ----
// Both additions preserve the termination argument, and that is the thing to
// check when editing this. Every one of these is a monotone step toward a
// bounded fixed point: stain rises only to min(capacity, addAmt); a washed
// stain falls only to 0; absorbed fullness falls only to 0 (the cell dies).
// Nothing here ever increases the work remaining, so `progress` goes false and
// stays false, and a saturated puddle on saturated ground sleeps. A rule that
// could both wet and dry the same cell would NOT terminate — that is exactly
// the trap in the drying variant, and why saturation here is terminal.
//
// Returns whether the caller should keep the cell awake.
fn doStaining(c : vec3<i32>, idx : u32, m : Material, rnd : u32) -> bool {
  let stainType = matStainType(m);
  let addAmt = matStainAmount(m);
  let washes = matWashes(m);
  let stamp = stampFor(T.tick, P.substep);

  // Rotate the scan so the stained neighbour is not biased toward -Y. One roll
  // decides WHETHER we stain this tick; the rotation decides WHICH neighbour.
  let rot = rnd >> 7u;
  let fires = (rnd % 1000u) < matStainChance(m);

  // This cell's own fullness, for the absorption debit below. Re-read rather
  // than passed in: a reaction earlier this tick may have rewritten it.
  let selfWord = voxWordAt(c);
  let selfMat = voxMat(selfWord);
  let selfIsLiquid = materials[selfMat].klass == CLASS_LIQUID;

  var progress = false;  // is there still unstained surface in reach?
  for (var i = 0u; i < 6u; i++) {
    let di = (i + rot) % 6u;
    let n = c + faceDir(di);
    if (!inBounds(n)) { continue; }
    // TWO BASES, and conflating them is a silent desync (§4.1's rule applied
    // to an RNG key rather than to the hash): `niSlot` is the SLOT cell index
    // and is what the consumption roll below hashes on, so the RNG stream is a
    // property of WHERE the cell is in the world and not of which page happens
    // to hold it. `ni` is the physical word index and is only ever a memory
    // address. Feeding a page index into hash3 would make the sim's random
    // stream depend on allocation history — the world would still be
    // self-consistent and would still diverge from a dense run.
    let niSlot = cellIndexW(n);
    let ni = voxWordIndex(n);
    let nw = voxWordAt(n);
    let nmat = voxMat(nw);
    if (nmat == MAT_AIR) { continue; }
    // Don't stain other liquids or gases: a stain is something that soaks into
    // a SURFACE. Blood mixing into water is a different (and unimplemented)
    // thing, and staining a gas would mark smoke that then drifts away with it.
    let nk = materials[nmat].klass;
    if (nk != CLASS_SOLID && nk != CLASS_POWDER) { continue; }

    let cur = voxStainAmt(nw);
    let curType = voxStainType(nw);
    // How deep this particular ground will take this stain. The liquid's own
    // amount is still the ceiling, so `absorb` can only ever hold LESS than the
    // liquid would otherwise apply — a material opts into being soakable, it
    // cannot opt into being stained harder than the liquid stains.
    let capacity = matAbsorbCapacity(materials[nmat]);
    let ceiling = min(addAmt, max(capacity, 1u));

    // Is there work left on this neighbour? A foreign stain is work for a
    // washer (rinse it out) and for a non-washer alike (overwrite it, the
    // pre-existing behaviour). Our own stain is work only while it sits under
    // the ceiling this ground allows.
    let foreign = cur != 0u && curType != stainType;
    let canWash = washes && foreign;
    let canStain = foreign || cur < ceiling;
    if (!canWash && !canStain) { continue; }
    progress = true;
    if (!fires) { break; }  // work remains, but not this tick

    // ---- washing: step the foreign stain DOWN rather than repainting it ----
    if (canWash) {
      // Rinse ONE level per contact, not `addAmt` levels. Water authors a large
      // amount (it wets ground to whatever depth the ground allows), and reusing
      // that here would erase any stain in a single touch — blood would blink
      // out the instant water reached it instead of visibly fading under the
      // flow. One level per successful contact makes washing a process you can
      // watch, and it still terminates: the amount only ever decreases.
      //
      // When it hits 0 the TYPE goes with it, so the cell reads as genuinely
      // unstained rather than as "blood, amount 0" (voxStained() checks both
      // halves, and a stale type would let the next blood contact resume from
      // the old slot).
      let washed = cur - 1u;
      var washedType = curType;
      if (washed == 0u) { washedType = 0u; }
      voxStore(ni, (nw & ~STAIN_BITS) | packStain(washedType, washed));
      markDirty(n);
      markDirty(c);
      break;
    }

    // ---- ordinary staining, now clamped by the substrate's capacity ----
    // On ABSORBENT ground the stain climbs ONE level per contact, in lockstep
    // with the eighth of liquid spent below, so the ground visibly darkens as
    // it drinks and the depth reached is paid for in real mass. Jumping
    // straight to the ceiling would soak grass to full for one eighth of water,
    // which is both free mass and an instant, un-watchable transition.
    //
    // On NON-absorbent ground (capacity 0, all stone) nothing is spent, so
    // there is no rate to keep in step with and the original behaviour stands:
    // the stainer applies its full amount at once. That is what keeps blood on
    // stone looking exactly as it did before this feature.
    var amt = ceiling;
    if (capacity > 0u) {
      amt = min(cur + 1u, ceiling);
      if (curType != stainType) { amt = 1u; }  // foreign stain: start over at 1
    } else if (curType == stainType) {
      amt = min(cur + addAmt, ceiling);
    }
    voxStore(ni, (nw & ~STAIN_BITS) | packStain(stainType, amt));
    markDirty(n);

    // ---- absorption: the liquid SPENDS itself soaking in ----
    // Only when the ground actually declared a capacity, and only for a liquid
    // that has mass to give. One eighth per contact, and the cell dies when it
    // gives its last — mass-conserving in the same units stepLiquid speaks.
    //
    // This writes SELF, which the stain rule otherwise never does. It is safe
    // for the same reason the neighbour write is: reach is still <= 1 cell, so
    // the colour lattice still guarantees no other thread touches either cell
    // this pass. The stamp is set so the movement code below cannot ALSO move
    // this cell in the same substep and double-spend the eighth.
    //
    // `amt > cur` is the load-bearing test, not a redundant one: it charges the
    // liquid ONLY for a contact that actually deepened the stain. Saturated
    // ground never gets here (canStain is false, so the loop skipped it), but
    // stating the invariant locally is what stops a future edit to the ceiling
    // logic from silently turning this into a puddle that drains into ground
    // it is no longer wetting — water disappearing for free.
    if (capacity > 0u && selfIsLiquid && amt > cur) {
      let sf = voxState(selfWord) + 1u;  // fullness 1..8
      if (sf <= 1u) {
        voxStore(idx, 0u);               // last eighth soaked in — gone
      } else {
        voxStore(idx, packVoxKeepStain(selfMat, sf - 2u, stamp, selfWord));
      }
    }

    // Consumption: the stain eats the voxel it just marked. Rolled from a
    // DIFFERENT slice of the hash than the stain roll, so the two are
    // independent — reusing the same bits would correlate "stained" with
    // "consumed" and every stain would either always or never eat.
    let croll = hash3(rnd, 0x51A17u, niSlot) % 1000u;
    if (croll < matStainConsume(m)) {
      voxStore(ni, 0u);
      // The voxel that vanished may have been holding a solid up.
      flagSupportLoss(n, nk, MAT_AIR);
    }
    markDirty(c);
    break;  // one neighbour per tick — bounds the rule's rate (rule 2)
  }
  return progress;
}

// ============================== LIQUID DESCENT ===============================
// PLAN_fluid_overhaul.md §1.1 defect 3, and the single largest of the four.
//
// A liquid's descent set is the 9 cells of the layer below that the colour
// lattice's 1-cell write reach allows: straight down, the 4 axis diagonals and
// the 4 CORNERS. Before this the CA used 5 of those 9, and used them only as
// WHOLE-CELL moves, which meant `canDisplace` refused any target already
// holding the same liquid (equal density is not "lighter"). A partly filled
// lower step was therefore an impassable wall to the water standing above it.
//
// DETERMINISM (rule 1). Corners cost nothing new: two acting cells in one pass
// are >= 3 apart on EVERY axis, so their 3x3x3 write neighbourhoods are
// disjoint whether a move is axis-aligned or not. Every read below is a
// neighbour at Chebyshev distance 1, which is inside the guarantee (a read at
// distance 2 is NOT — see the column-walk note on seesSky).
//
// TERMINATION (rule 2). Every descent moves >= 1 eighth exactly one level down,
// so it strictly decreases the world's gravitational potential SUM(f * y),
// which is a bounded integer. Descents alone can therefore never keep a chunk
// awake forever; only the lateral rules need their own argument.

// The four CORNER lateral directions — the diagonal complement of lateralDir().
fn cornerDir(i : u32) -> vec2<i32> {
  switch (i & 3u) {
    case 0u: { return vec2<i32>( 1,  1); }
    case 1u: { return vec2<i32>(-1,  1); }
    case 2u: { return vec2<i32>(-1, -1); }
    default: { return vec2<i32>( 1, -1); }
  }
}

// Is `c` a WALL as far as a liquid is concerned? Solids and powders are; air,
// gases and other liquids are not; unloaded space outside the residency window
// is (DESIGN.md §3 — the sim's world edge is the window, and it is solid).
fn liquidWall(c : vec3<i32>) -> bool {
  if (!inBounds(c)) { return true; }
  let wm = voxMat(voxWordAt(c));
  if (wm == MAT_AIR) { return false; }
  let k = materials[wm].klass;
  return k == CLASS_SOLID || k == CLASS_POWDER;
}

// A corner descent must not squeeze through a SEALED DIAGONAL CRACK. Two walls
// that meet at a corner leave a diagonal seam with no volume; letting water
// through it would drain any box whose walls join, which is most of them. So a
// corner counts as a path only when at least one of the two axis descents that
// flank it is itself passable — the water goes AROUND the corner, never
// through it.
fn cornerDescentOpen(c : vec3<i32>, d : vec2<i32>) -> bool {
  return !liquidWall(c + vec3<i32>(d.x, -1, 0)) ||
         !liquidWall(c + vec3<i32>(0, -1, d.y));
}

// Move mass from `c` into `n`, one level below it. A same-liquid target takes a
// PARTIAL transfer — exactly what stage 1 always did for the cell directly
// below — and everything else goes through the whole-cell tryMove. Returns
// whether anything moved. Mass-exact in both arms (transferLiquid and tryMove
// are the only writers).
fn tryDescend(c : vec3<i32>, n : vec3<i32>, w : u32, mat : u32, f : u32,
              dens : i32) -> bool {
  if (!inBounds(n)) { return false; }
  let nw = voxWordAt(n);
  if (voxMat(nw) == mat) {
    let nf = voxState(nw) + 1u;
    if (nf >= 8u) { return false; }
    transferLiquid(c, n, mat, f, nf, min(f, 8u - nf));
    return true;
  }
  return tryMove(c, n, w, dens, false);
}

// Read-only mirror of tryDescend, for canFlowAnywhere. These two MUST agree:
// a predicate broader than the rule pins chunks awake forever (rule 2), one
// narrower lets a cell sleep with work left.
fn canDescend(c : vec3<i32>, n : vec3<i32>, mat : u32, dens : i32) -> bool {
  if (!inBounds(n)) { return false; }
  let nw = voxWordAt(n);
  if (voxMat(nw) == mat) { return voxState(nw) + 1u < 8u; }
  return canDisplace(dens, false, nw);
}

// ---- the last eighth: PLAN §1.1 defect 1, and its termination argument ------
//
// Lateral spread into air is repeated halving (8 -> 4 -> 2 -> 1) and stopped
// dead at `f >= 2`, so every blob decayed into fullness-1 films that could
// never move again. On the hill's stepped ramp that is fatal: a 2-cell tread's
// INNER cell has terrain below it and terrain on both down-diagonals, so its
// only exit is one lateral step to the tread's lip — which the halving rule
// refuses to make once the cell is down to its last eighth.
//
// WHY THIS IS NOT SIMPLY "LET FULLNESS 1 SPREAD". A lone eighth allowed to
// move sideways into air on FLAT ground is an unbounded random walk: it never
// finds a resting state, its chunk never sleeps, and rule 2 is gone. There is
// no reach-1 rule that can tell "one cell from the lip of a tread" from "one
// cell from the middle of a floor" — that information is two cells away, and a
// two-cell read is scheduling-dependent (the seesSky note). So the move has to
// be justified by something the cell can actually see.
//
// WHAT IT CAN SEE: the STEP BEHIND IT. The condition is
//
//     the cell opposite the move is a wall, and the cell above THAT is not
//
// i.e. "I am standing at the foot of a riser exactly one voxel proud of my own
// level" — which is precisely a terrace tread, and precisely where the water
// wants to go on. The move is away from the riser, which on a terrace is
// downhill.
//
// TERMINATION. After the step, the cell behind the film is the one it just
// vacated: AIR, not a wall, so the same move cannot repeat. On flat open ground
// there is no riser and the film never moves at all. Films therefore make at
// most one step per riser they are standing against, and the world still
// settles.
//
// THE ONE GEOMETRY THIS DOES NOT TERMINATE ON, stated because it is the honest
// limit of the rule: two risers facing each other exactly 3 apart (walls, two
// air cells between, both risers exactly one voxel tall) is symmetric, so a
// film in it steps back and forth forever. No reach-1 predicate can break that
// tie — the two cells have byte-identical neighbourhoods — and the `sleep` gate
// is the arbiter for whether the generated world contains it. The "one voxel
// tall" clause is what keeps the far more common case (a 2-wide slot between
// two ordinary walls) out of the rule entirely.
// ---- the MINIMUM FILM, and why the halving needed a floor -------------------
//
// The rule above says a lone eighth may not wander. This one says how the
// halving is allowed to GET to a lone eighth, and the answer used to be "all
// the way": `f >= 2` let any cell split f/2 into an empty neighbour with no
// floor at all, so one placed water voxel ran 8 -> (4,4) -> (2,2,2,2) ->
// (1,1,1,1,1,1,1,1) and came to rest as EIGHT cells one eighth deep. The mass
// is exactly right and the shape is absurd: eight times the footprint the
// player placed, an eighth as tall, and since b799a58 draws liquid at
// fullness-proportional height that reads on screen as a splash rather than a
// voxel of water. It is also the worst possible input to the MPM side — a
// 1-eighth film gathers rho far below rest inside the solver's 3-cell support,
// so its EOS pressure is zero and excite converts it into particles that
// cannot move (the thin-film gap, RESEARCH_water_architecture.md).
//
// The floor is stated on BOTH halves, not just the source: a split is allowed
// only when `f >= 2 * minFilm`, so the cell keeps `f - f/2 >= minFilm` and the
// neighbour receives `f/2 >= minFilm`. That makes the equilibrium film
// minFilm..2*minFilm-1 eighths and shrinks a placement's footprint by minFilm.
//
// WHAT IT DOES NOT TOUCH, deliberately: the same-liquid EQUALIZE branch. That
// one moves mass between two cells that already hold water, which is what
// levels a pond, and gating it on a film thickness would leave real pools
// permanently stepped. Only the leading edge advancing into AIR is floored —
// and descent (stages 1 and 2) is untouched too, so water still runs downhill
// at any fullness and a spill on a slope behaves exactly as before.
//
// minFilm 1 reproduces the old `f >= 2` bit-for-bit, which is what makes this
// an A/B rather than a one-way change.
//
// ---- AND THE OWNER PUT IT BACK TO 1 (2026-08-25) ----------------------------
// The paragraph above is still an accurate description of what the floor does;
// the taste call it rests on was overruled, and the reasoning is worth keeping
// because the two halves of this file now pull in opposite directions.
//
// The ask: "if I use the smallest brush and place water on a flat plane I want
// it to keep spreading until every single voxel is the smallest height
// possible." That is minFilm 1 by definition — the flattest state a lattice
// quantised in eighths can represent is every wetted cell holding exactly one
// eighth. At minFilm 2 the same water rests two to three eighths deep over half
// the footprint, which is a lower, wider version of the same mound.
//
// It is a knob, not a decision: minFilm 2 is one edit away and everything below
// (filmPressed included) is written against LIQ_SPLIT_MIN rather than against a
// literal, so both settings behave consistently. What DOES change with it is the
// thin-film handoff to the solver — a 1-eighth film gathers rho far below rest
// inside the MPM's 3-cell support (RESEARCH_water_architecture.md), so the
// thinner the resting film, the more water the CA owns outright. That is the
// real cost of 1, and it is the reason the excite seam grew a surface-step
// trigger in the same change: disturbed water goes to the solver on purpose
// instead of being left to the CA by accident.
const LIQ_MIN_FILM : u32 = clamp(TUNE_LIQUID_MIN_FILM, 1u, 4u);
const LIQ_SPLIT_MIN : u32 = 2u * LIQ_MIN_FILM;

fn filmStepAllowed(c : vec3<i32>, d : vec2<i32>) -> bool {
  let back = c - vec3<i32>(d.x, 0, d.y);
  return liquidWall(back) && !liquidWall(back + vec3<i32>(0, 1, 0));
}

// ---- the PRESSED FILM: why a puddle went DOMED instead of flat --------------
//
// The rule above frees a film standing against a riser. This one frees the rim
// of a puddle, and it is what makes the resting shape actually LEVEL.
//
// THE DEFECT. Lateral spread into air is halving, and the same-liquid EQUALIZE
// branch only fires at a difference of TUNE_LIQUID_EQUALIZE (2). Put those
// together and a slope of exactly ONE eighth per cell is a STABLE state: no
// adjacent pair differs by 2 so nothing equalizes, and only the RIM touches air
// so nothing splits. A blob dropped on flat ground therefore relaxes into a
// CONE — 8 in the middle, 7 around that, ... 1 at the rim — and stops there
// forever. Dropped on a pond (where descent is refused because the water below
// is already full) that cone is a mound of water sitting proud of the surface
// that never disperses, which is exactly what the owner reported. It was
// invisible while liquid drew as full cubes and became obvious at b799a58,
// which draws a partial cell at fullness height.
//
// PLAN_fluid_overhaul.md §1.1 defect 2 names `liquidEqualize = 2` for this, and
// the long note in stepLiquid explains why lowering it to 1 cannot work: a
// difference of 1 transfers `(f - nf) / 2u` == 0 eighths, and forcing the odd
// eighth across instead is flat in the diffusion's own Lyapunov function, so the
// pair trades it back and forth forever. That analysis is right about the PAIR
// and wrong about the CHAIN — (8,7,6,5,4,3,2,1) has no unstable pair in it and
// is still a dome. The dome is not held up by the equalize threshold; it is held
// up by the RIM, which cannot move at all: a lone eighth may not split (that
// would leave nothing behind) and has no neighbour two lower to equalize with,
// so the footprint can never grow and the cone behind it has nowhere to go.
// Free the rim and the whole thing unwinds from the outside in.
//
// THE RULE. A cell too thin to split moves its WHOLE content one step into air
// when some OTHER lateral neighbour is thick enough to split — "there is water
// pressing behind me and there is room in front of me".
//
// TERMINATION (rule 2), and the gate is chosen for this and not for taste.
// SUM(f*f) is the lateral rules' Lyapunov function: splitting strictly
// decreases it (2 -> (1,1) is 4 -> 2), equalizing strictly decreases it, and
// descent strictly decreases SUM(f*y) instead. This move is NEUTRAL in both — it
// only relocates a film — so on its own it could cycle forever, which is the
// exact objection that kept the last eighth pinned in place.
//
// The gate is what forbids the cycle. The cell the film VACATES becomes air, and
// it is a face neighbour of the cell that justified the move, which by the gate
// holds >= LIQ_SPLIT_MIN. That cell can therefore split into the hole, and
// splitting strictly decreases SUM(f*f). So: SUM(f*f) is non-increasing and
// bounded below, hence eventually constant; over any stretch where it is
// constant no split and no equalize happens; but every advance in that stretch
// hands the neighbour behind it a split it can take. Contradiction — there are
// only finitely many advances. A puddle whose cells all hold the same amount has
// no cell at or over the split floor at all, so it makes no advance, finds
// nothing else to do, and sleeps. That level state is what this exists to reach.
//
// `nf >= LIQ_SPLIT_MIN` and not the more natural `nf > f` IS that argument: a
// neighbour merely thicker than the film can still be too thin to split into the
// hole the film leaves, and at minFilm > 1 that gap is where the proof (and the
// settling) breaks.
fn filmPressed(c : vec3<i32>, mat : u32) -> bool {
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    if (voxMat(nw) != mat) { continue; }
    if (voxState(nw) + 1u >= LIQ_SPLIT_MIN) { return true; }
  }
  return false;
}

// ---- the BRIDGED EQUALIZE: reaching past a cell that cannot itself move -----
//
// filmPressed frees the rim; this is what drains the CORE behind it, and the two
// together are what actually lower a mound rather than merely widening it.
//
// WHAT IS LEFT AFTER filmPressed, measured on the ca-level gate: 216 eighths
// dropped on a flat floor rest over 113 cells with the profile
// 1:59 2:23 3:17 4:10 5:4 — an apron of single eighths around a core still five
// eighths deep. The apron is the trap. A cell holding 1 with 1s around it cannot
// split (nothing to halve), cannot equalize (its neighbours are equal) and is
// not pressed (no neighbour over the split floor), so once the frontier has run
// one cell ahead of the core the core is SEALED OFF from the only thing that was
// draining it. Every pairwise rule is blind here: adjacent cells differ by
// exactly one eighth all the way down the slope, which is the integer
// equilibrium of a PAIR and nothing like the equilibrium of a surface.
//
// THE MOVE. A cell may transfer between TWO OF ITS OWN LATERAL NEIGHBOURS. Both
// are one cell away, so the write reach is unchanged and the colour lattice's
// disjointness argument is untouched — this is the same licence tryMove has
// always used, spent on a pair of neighbours instead of on self and one
// neighbour. What it buys is a look at a distance the direct rules do not have:
// two cells that straddle a mediator are 2 apart (opposite laterals) or diagonal
// (perpendicular ones), and on a slope of one eighth per cell they differ by
// exactly 2 — which is the ordinary equalize threshold. The dome is therefore
// unstable again, from the inside.
//
// It is physically the right picture as well as a convenient one: the mediator
// is WATER. Pressure crosses a connected body of water; it does not have to be
// carried cell by cell. Requiring the middle cell to hold this same liquid is
// what keeps that true, and it also means the diagonal case needs no
// crack-check — the path between two perpendicular neighbours runs through the
// mediator, which is water by construction (compare cornerDescentOpen, which
// exists precisely because a corner descent has no such guarantee).
//
// TERMINATION (rule 2) IS FREE, which is the reason this rule is worth having
// and the diff-1 rules are not. It is an ordinary equalize — it moves
// (fa - fb) / 2 eighths from the fuller cell to the emptier one across a gap of
// at least TUNE_LIQUID_EQUALIZE — so it strictly decreases SUM(f*f) exactly as
// the direct branch does. No new Lyapunov argument, no new risk: the same
// bounded integer that already forbids the lateral rules from cycling forbids
// this one.
//
// THE REMAINING LIMIT, since this is the last rung reach-1 can climb. The joint
// fixpoint is now "no two cells within a mediated hop differ by 2", i.e. a
// surface may still slope by one eighth per TWO cells instead of per cell — half
// the dome, not no dome. Going further needs a look 4 cells wide, and a mediator
// can only bridge cells that are both inside ITS write reach, so 2 is the end of
// the line for this shape of rule. A genuinely level surface needs what a level
// surface physically is: a global pressure solve (the MPM owns that) or a
// mark/apply pass over a flux field (docs/RESEARCH_water_architecture.md option
// B). Both are architecture, not a rule tweak.
//
// `apply` rather than a mirrored read-only twin, deliberately: the settled path
// and the moving path MUST agree or a chunk either pins awake forever or sleeps
// with work left (see canFlowAnywhere), and the cheapest way to guarantee that
// is to have one function and one scan order. The scan is also NOT rng-rotated,
// unlike every other lateral scan here: the read-only mirror makes no roll, so
// rotating would let the two disagree about WHICH pair is available.
fn bridgeLevel(c : vec3<i32>, mat : u32, apply : bool) -> bool {
  // The four laterals' fullness, 0 meaning "not this liquid" (air, a wall,
  // another material, out of window). Gathered before any write, so the pair the
  // scan picks is a function of pre-move state.
  var fl = array<u32, 4>(0u, 0u, 0u, 0u);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    if (voxMat(nw) == mat) { fl[i] = voxState(nw) + 1u; }
  }
  for (var ia = 0u; ia < 4u; ia++) {
    if (fl[ia] == 0u) { continue; }
    for (var ib = 0u; ib < 4u; ib++) {
      if (ib == ia || fl[ib] == 0u) { continue; }
      if (fl[ib] + TUNE_LIQUID_EQUALIZE > fl[ia]) { continue; }
      if (!apply) { return true; }
      let da = lateralDir(ia);
      let db = lateralDir(ib);
      transferLiquid(c + vec3<i32>(da.x, 0, da.y), c + vec3<i32>(db.x, 0, db.y),
                     mat, fl[ia], fl[ib], (fl[ia] - fl[ib]) / 2u);
      return true;
    }
  }
  return false;
}

// Would stepLiquid() find anything to do for this cell? PURE READ — it makes
// no writes at all, and every read is a face/diagonal neighbour (reach 1), so
// it stays inside the colour lattice's guarantee exactly like the reaction
// neighbour scans do.
//
// Exists for two callers now: a viscous liquid on an off-tick deciding whether
// it is worth staying awake for (the moveEvery gate in main), and the SETTLED
// path of stepLiquid — PLAN §1.1 defect 4, "a cell that found no move sleeps
// and never retries". The conditions below MIRROR stepLiquid's stages one for
// one; drift in the loose direction pins chunks awake forever (rule 2), drift
// in the tight direction lets a cell sleep with work left, so keep them in
// step. Same shape as the powder path's "nothing to do: cell settles".
fn canFlowAnywhere(c : vec3<i32>, w : u32, mat : u32, m : Material) -> bool {
  let f = voxState(w) + 1u;

  // 1) down: a partial same-liquid cell to top up, or anything displaceable.
  if (canDescend(c, c + vec3<i32>(0, -1, 0), mat, m.density)) { return true; }

  // 2a) the four axis down-diagonals.
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    if (canDescend(c, c + vec3<i32>(d.x, -1, d.y), mat, m.density)) { return true; }
  }
  // 2b) the four corner down-diagonals, path-gated exactly as stepLiquid does.
  for (var i = 0u; i < 4u; i++) {
    let d = cornerDir(i);
    if (!cornerDescentOpen(c, d)) { continue; }
    if (canDescend(c, c + vec3<i32>(d.x, -1, d.y), mat, m.density)) { return true; }
  }

  // 3) laterals: equalize into a same-liquid neighbour holding >= 2 less,
  //    split into air, step a film off a riser or out from under the water
  //    pressing on it, or displace something lighter.
  // Hoisted out of the direction loop: it does not depend on `d`, and it is only
  // ever asked of a cell too thin to split.
  let pressed = f < LIQ_SPLIT_MIN && filmPressed(c, mat);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt(n);
    let nmat = voxMat(nw);
    if (nmat == mat) {
      if (voxState(nw) + 1u + TUNE_LIQUID_EQUALIZE <= f) { return true; }
    } else if (nmat == MAT_AIR) {
      if (f >= LIQ_SPLIT_MIN) { return true; }
      if ((pressed || filmStepAllowed(c, d)) &&
          canDisplace(m.density, false, nw)) { return true; }
    } else if (canDisplace(m.density, false, nw)) {
      return true;
    }
  }
  // 4) last resort: level two of the neighbours THROUGH this cell. Same scan and
  //    same predicate as the moving path — one function, so they cannot drift.
  return bridgeLevel(c, mat, false);
}

// Mass-conserving liquid flow (fullness in eighths, DESIGN.md §4).
// Returns whether the cell moved any mass this substep; the caller uses that to
// decide whether a settled cell still has a reason to stay awake (defect 4).
fn stepLiquid(c : vec3<i32>, idx : u32, w : u32, mat : u32, m : Material, rnd : u32) -> bool {
  let f = voxState(w) + 1u;  // fullness 1..8

  // 1) straight down: top a partial same-liquid cell up, else move/swap whole.
  if (tryDescend(c, c + vec3<i32>(0, -1, 0), w, mat, f, m.density)) { return true; }

  // 2a) the four AXIS down-diagonals, RNG order.
  let r = rnd >> 10u;
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (tryDescend(c, c + vec3<i32>(d.x, -1, d.y), w, mat, f, m.density)) { return true; }
  }
  // 2b) the four CORNER down-diagonals, RNG order, after the axis ones (the
  //     shorter path wins) and only where the corner is a real path and not a
  //     sealed diagonal crack.
  for (var i = 0u; i < 4u; i++) {
    let d = cornerDir(i + r);
    if (!cornerDescentOpen(c, d)) { continue; }
    if (tryDescend(c, c + vec3<i32>(d.x, -1, d.y), w, mat, f, m.density)) { return true; }
  }

  // 3) lateral: equalize into a same-liquid neighbor holding at least the
  //    equalize threshold less, split into air, step the LAST eighth off a
  //    riser (filmStepAllowed), or whole-cell displace a lighter fluid.
  //    RNG order.
  //
  //    On TUNE_LIQUID_EQUALIZE, which PLAN §1.1 names as defect 2: it stays 2,
  //    and the reason is worth stating because "just lower it to 1" is the
  //    obvious move and it does not work. `(f - nf) / 2u` is 0 when the
  //    difference is exactly 1, so a threshold of 1 makes a cell permanently
  //    "unstable", transfers nothing, and re-dirties its chunk forever. Forcing
  //    the odd eighth across instead turns (k+1, k) into (k, k+1), which has
  //    the SAME sum of squares — the diffusion's own Lyapunov function is flat
  //    on that move — so the pair trades it back and forth and never rests. The
  //    only tie-breaks available at reach 1 are position (a canonical direction
  //    ratchets mass toward +x/+z and makes an ASCENDING wedge a stable state,
  //    strictly worse) or the RNG (which oscillates). At eighth resolution
  //    (k, k+1) IS the integer equilibrium of two same-height cells, so there
  //    is nothing to fix there. The stable "1-eighth staircase" the plan is
  //    actually complaining about is between cells at DIFFERENT heights, and
  //    stage 2's new partial descent dissolves it outright: a cell one level
  //    down that is not full now receives, with no threshold at all, because
  //    moving mass downhill always strictly decreases SUM(f * y) and so can
  //    never oscillate.
  let r2 = rnd >> 14u;
  // See filmPressed: a film under pressure may advance even though it is too
  // thin to split. Hoisted for the same reason the mirror hoists it.
  let pressed = f < LIQ_SPLIT_MIN && filmPressed(c, mat);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r2);
    let n = c + vec3<i32>(d.x, 0, d.y);
    if (!inBounds(n)) { continue; }
    let nw = voxWordAt((n));
    let nmat = voxMat(nw);
    if (nmat == mat) {
      let nf = voxState(nw) + 1u;
      if (nf + TUNE_LIQUID_EQUALIZE <= f) {
        transferLiquid(c, n, mat, f, nf, (f - nf) / 2u);
        return true;
      }
    } else if (nmat == MAT_AIR) {
      // Split only if BOTH halves clear the film floor — see LIQ_MIN_FILM.
      if (f >= LIQ_SPLIT_MIN) {
        transferLiquid(c, n, mat, f, 0u, f / 2u);
        return true;
      }
      // Too thin to split: the whole film steps. Two justifications, both
      // reach-1 and both with their own termination argument (see the blocks
      // on filmStepAllowed and filmPressed):
      //   * it is standing against a one-voxel riser — the terrace tread case,
      //     and the cell it vacates is air so the move cannot repeat; or
      //   * water thick enough to split is pressing on it from behind, so
      //     advancing hands that neighbour a split and the puddle levels.
      // The second is what dissolves a dome; without it the rim is frozen and
      // the cone behind it is a stable resting shape.
      if ((pressed || filmStepAllowed(c, d)) &&
          tryMove(c, n, w, m.density, false)) {
        return true;
      }
    } else if (tryMove(c, n, w, m.density, false)) {
      return true;
    }
  }

  // 4) nothing this cell can do with its OWN mass — but it may still be the
  //    bridge two of its neighbours need. See the bridgeLevel block: this is
  //    what drains a mound whose rim has already run away from it, and it is an
  //    ordinary equalize (SUM(f*f) strictly down), just reaching one cell
  //    further.
  if (bridgeLevel(c, mat, true)) { return true; }
  return false;  // settled — the caller decides whether to stay awake
}

// ============================== WIND IN THE CA ==============================
// docs/RESEARCH_wind.md §4.5, phase 4. This is the only part of the wind system
// that writes voxels, and therefore the only part that can move the world hash.
// It is behind T.windMode, which ships at 0 (kWindMode* in world.h).
//
// TWO MECHANISMS, and the split is Bagnold's: matter already in motion is
// steered, matter at rest has to be picked up, and those need different
// thresholds. The hysteresis that separates them is not written anywhere — it
// falls out of the sleep machinery, because "already moving" and "settled" are
// states this kernel already distinguishes by which stage a cell reaches.
//
//   * DRIFT BIAS reorders the direction candidates a MOVING voxel was going to
//     try anyway. It cannot make a voxel move that would not have moved, it adds
//     no write, and its reach is still <= 1 cell — so it changes what the world
//     does without changing anything about how the world sleeps (invariant 3:
//     the ambient field never wakes a chunk).
//     Instantaneous advection, not acceleration, is the CORRECT model for a
//     gas: a parcel of smoke has no inertia worth modelling at this scale, it
//     goes where the air goes. The particle tier (sim_particle.wgsl) is where
//     momentum lives, and §4.6 is explicit that the two tiers divide that way.
//
//   * ENTRAINMENT unlocks a move for a SETTLED grain when the wind on one axis
//     beats that material's authored friction. That is saltation, and it is
//     what makes a fan blow a sand pile flat. It is gated one step further out
//     (WIND_MODE_ENTRAIN) because it is the one mechanism here that is not
//     rule-2 clean on its own — see kWindModeEntrain in world.h.
//
// LIQUIDS ARE DELIBERATELY EXCLUDED from both. They move by mass transfer
// through stepLiquid, not by the direction rotation these hook into, and what
// wind does to standing water is drive surface waves and spray — a different
// phenomenon, owned by the MPM node force (sim_fluid.wgsl) where a wave can
// actually exist. Biasing eighths downwind would just make a pond flow uphill.

// Wind speed at which the drift bias reaches its cap, Q16.16 world cells/s
// (windAtQ's unit). Authored in m/s; converted at const-eval, so the kernel is
// integer (the sim_fluid.wgsl discipline).
const WIND_DRIFT_REF : i32 = i32(round(
    clamp(TUNE_WIND_DRIFT_SPEED, 0.5, 200.0) * 65536.0 / VOXEL_METERS));
// That cap, in 1024ths. Below 1024 by construction (LoadTuning clamps to 0.95):
// at certainty the RNG order is gone entirely and a gas stops looking like a
// gas and starts looking like a conveyor belt.
const WIND_DRIFT_CAP : i32 = i32(round(clamp(TUNE_WIND_DRIFT_MAX, 0.0, 0.95) * 1024.0));
// Per-axis wind that just lifts a settled grain of windFriction 1, same units.
const WIND_ENTRAIN_REF : i32 = i32(round(
    clamp(TUNE_WIND_ENTRAIN_SPEED, 0.5, 200.0) * 65536.0 / VOXEL_METERS));
// ...and how often a grain over that threshold actually hops, in 1024ths per
// TICK (the entrainment stage runs on substep 0 only, so this is per tick with
// no substep division to keep in step). A rate, not a certainty: this is the
// bound that makes a dune creep instead of detonate (rule 2).
const WIND_ENTRAIN_CHANCE : i32 =
    i32(round(clamp(TUNE_WIND_ENTRAIN_RATE, 0.0, 30.0) * 1024.0 / 30.0));
// Distinct salt for the wind RNG stream. NOT a bit-slice of `rnd`: the movement
// tail already spends bits 10.., 14.. and 18.. of that word on the direction
// rotations these decisions sit next to, and correlating "does it go downwind"
// with "which way did it pick" is exactly the kind of hidden coupling the
// worldgen salt rule exists to forbid. One extra hash3, drawn only when a
// material actually responds to wind.
const WIND_RNG_SALT : u32 = 0x5719u;

fn windRnd(slotIdx : u32) -> u32 {
  return hash3(T.seed ^ WIND_RNG_SALT, T.tick * 2u + P.substep, slotIdx);
}

// Which of lateralDir's four codes points most nearly downwind. lateralDir is
// 0:+x 1:+z 2:-x 3:-z, so this is the dominant horizontal axis and its sign.
fn windLateralCode(w : vec3<i32>) -> u32 {
  if (abs(w.x) >= abs(w.z)) { return select(2u, 0u, w.x > 0); }
  return select(3u, 1u, w.z > 0);
}

// The starting index for a 4-direction lateral rotation, biased downwind.
//
// Returns `base` (the RNG's own offset) unchanged in every case where wind
// should not apply, so the two call sites read as "the same rotation, sometimes
// started somewhere else". That framing is the safety argument: no branch here
// can add a move candidate, only reorder the four that were already going to be
// tried, so tryMove's write reach and the stamp discipline are untouched.
fn windLateralStart(c : vec3<i32>, base : u32, m : Material,
                    slotIdx : u32) -> u32 {
  if (T.windMode == WIND_MODE_OFF) { return base; }
  let resp = i32(matWindResponse(m));
  if (resp == 0) { return base; }          // most materials: one compare
  let w = windAtQ(c, T);
  let mag = max(abs(w.x), abs(w.z));
  if (mag == 0) { return base; }
  // Ramp to the cap over [0, WIND_DRIFT_REF], then scale by the authored
  // response. Both operands are pre-scaled by 1024 before multiplying: a
  // storm-force Q16.16 speed times 1024 leaves i32, and this runs per moving
  // voxel per substep.
  let frac = (min(mag, WIND_DRIFT_REF) >> 10u) * 1024 /
             max(WIND_DRIFT_REF >> 10u, 1);            // 0..1024
  var p = ((frac * WIND_DRIFT_CAP) / 1024) * resp / 15;
  // The dev force multiplier, applied to the PROBABILITY and AFTER the cap —
  // not to the field, and the difference is the whole reason this tier scales
  // a different quantity from the particle tier (windAtScaledQ says so at
  // length). `frac` above saturates once the wind passes windDriftSpeed, which
  // the default weather already nearly does, so a velocity multiplier here
  // would move the slider for the first ~2x and then do nothing. Scaling `p`
  // instead runs all the way to CERTAINTY: at the top of the range every moving
  // gas voxel tries downwind first and smoke stops looking like smoke and
  // starts looking like a conveyor belt, which is exactly the thing a "what
  // does drastic look like" control exists to show.
  //
  // The == is an exact-identity guard, not an optimisation: at the shipping 1x
  // this is arithmetically untouched, so "the slider is at 1x" and "the pinned
  // hash holds" are one statement.
  if (T.windGasScaleQ != WINDQ_SCALE_ONE) {
    p = min((p * T.windGasScaleQ) / WINDQ_SCALE_ONE, 1024);
  }
  if (i32(windRnd(slotIdx) & 1023u) < p) { return windLateralCode(w); }
  return base;
}

// Saltation: a settled grain of powder pulled loose by a wind that beats its
// authored friction. Returns true if it moved (the caller is then done with
// this cell, exactly as a successful tryMove leaves it).
//
// The threshold is PER AXIS, not on the magnitude, because that is what makes
// friction behave like friction: a wind blowing diagonally has to beat the
// threshold on the axis it is trying to push along, so a grain does not creep
// sideways off a component too weak to move it.
//
// Two candidates, in the order a real grain takes them: slide along the surface
// first, and only if that is blocked, hop UP and over the obstruction. Both are
// ordinary tryMoves at reach 1.
fn windEntrain(c : vec3<i32>, w32 : u32, m : Material, slotIdx : u32) -> bool {
  let resp = i32(matWindResponse(m));
  if (resp == 0) { return false; }
  let thresh = i32(matWindFriction(m)) * WIND_ENTRAIN_REF;
  // Raw field, NOT windAtScaledQ and not the gas multiplier either. Both dev
  // sliders scale a tier's RESPONSE to the wind; this test is a property of
  // the wind itself — whether it beats a material's authored friction — and
  // running a debug multiplier through a physical threshold would make the
  // slider silently retune every material's saltation point. The knob for
  // that is sim.windEntrainSpeed, which is what it is for.
  let wv = windAtQ(c, T);
  var d = vec2<i32>(0, 0);
  // Only the horizontal axes are tested. A vertical component lifts nothing on
  // its own — a grain needs somewhere lateral to go, and an updraft that
  // levitated powder in place would be a fountain, not saltation. It reaches
  // the grain anyway, through the up-diagonal candidate below.
  if (abs(wv.x) > thresh) { d.x = select(-1, 1, wv.x > 0); }
  if (abs(wv.z) > thresh) { d.y = select(-1, 1, wv.z > 0); }
  if (d.x == 0 && d.y == 0) { return false; }
  // The rate gate. Drawn AFTER the threshold test so a becalmed dune costs a
  // compare, and drawn at all so that a wind sitting just over the threshold
  // moves a surface slowly rather than all at once.
  if (i32((windRnd(slotIdx) >> 10u) & 1023u) >= WIND_ENTRAIN_CHANCE) {
    return false;
  }
  if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w32, m.density, false)) {
    return true;
  }
  return tryMove(c, c + vec3<i32>(d.x, 1, d.y), w32, m.density, false);
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

  // TWO BASES (§4.1). `slotIdx` is the SLOT cell index and keys the per-cell
  // RNG below; `idx` is the physical word index and is only a memory address.
  // Under the identity map they are equal, which is why commit 1 could
  // introduce the split while it cannot differ — but keying the RNG on a page
  // index would make every cell's random stream a function of allocation
  // history, and a paged run would diverge from a dense one with no other
  // symptom.
  let slotIdx = cellIndexW(c);
  let idx = voxWordIndex(c);
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return; }
  if (voxStamp(w) == stampFor(T.tick, P.substep)) { return; }  // already acted this substep

  let m = materials[mat];
  // Provably inert cell: no reaction bucket, no staining, and a SOLID class, so
  // every branch below is skipped and nothing is written. Structurally a no-op
  // — the three tests it folds are each still there underneath — but it is the
  // SAME predicate worldgen's streaming wake refuses on (matCanAct, common.wgsl),
  // and having one function stand for "this cell cannot act" is what keeps the
  // two from drifting. If a future rule lets a plain solid do something, this
  // return is where it breaks first, loudly, in the world hash.
  if (!matCanAct(m)) { return; }
  let rnd = hash3(T.seed, T.tick * 2u + P.substep, slotIdx);

  // Reactions roll once per tick (substep 0 of the two gravity substeps).
  if (P.substep == 0u && m.reactCount > 0u) {
    if (doReactions(c, idx, slotIdx, w, mat, m, rnd)) { return; }
  }

  // Staining, same once-per-tick budget as reactions. Gated on the material
  // flag first so the ~all materials that do not stain pay one comparison.
  // Keeps the chunk awake only while unstained surface remains in reach — see
  // the sleep note on doStaining.
  if (P.substep == 0u && matStains(m)) {
    if (doStaining(c, idx, m, rnd)) { markDirty(c); }
    // Absorption can have emptied this cell (the liquid soaked away) or docked
    // its fullness and stamped it. Re-read before the movement code below acts
    // on a stale word: moving an already-spent eighth would create mass.
    let after = voxWordAt(c);
    if (voxMat(after) == MAT_AIR) { return; }
    if (voxStamp(after) == stampFor(T.tick, P.substep)) { return; }
  }

  if (m.klass == CLASS_SOLID) { return; }

  // Viscosity: thick liquids (lava, molten glass, blood) only move on their
  // tick. On an off-tick the cell stays awake for the tick it MAY move on —
  // but only if it has somewhere to go.
  //
  // The unconditional markDirty this replaces meant a settled pool of any
  // viscous liquid re-dirtied its chunk on every off-tick, forever: those
  // chunks could never sleep, at any pool size, for the rest of the session
  // (CLAUDE.md rule 2). It went unnoticed because the sleep selftest only ever
  // settled water and powders — moveEvery is 1 for both, so they take the fast
  // path below and this branch never ran in the test.
  //
  // `canFlowAnywhere` is the same predicate stepLiquid() uses to decide it has
  // work, evaluated read-only: if it is false the cell would do nothing on its
  // move tick either, so there is nothing to stay awake for. A pool whose
  // surface is flat and whose floor is solid therefore sleeps, and any change
  // around it (a wall broken, liquid added) marks it dirty through the normal
  // paths and wakes it back up.
  if (m.moveEvery > 1u && (T.tick % m.moveEvery) != 0u) {
    if (canFlowAnywhere(c, w, mat, m)) { markDirty(c); }
    return;
  }

  if (m.klass == CLASS_LIQUID) {
    // re-read: a reaction may have rewritten this cell's fullness
    let lw = voxWordAt(c);
    if (!stepLiquid(c, idx, lw, mat, m, rnd)) {
      // PLAN §1.1 defect 4. A cell that moved nothing this substep used to fall
      // straight out with no markDirty, on the assumption that "found no move"
      // means "settled". It does not always: the RNG-ordered scans return on
      // the FIRST success, a neighbour may have taken the only exit earlier in
      // the tick, and every stage can be refused for a reason that is gone next
      // tick. So ask the mirror predicate, exactly as the viscous off-tick
      // branch above does, and stay awake only while genuinely flow-unstable.
      // A flat pool answers false and the chunk sleeps, which is the guarantee
      // this line must not break (rule 2) — the `sleep` gate is its test.
      if (canFlowAnywhere(c, lw, mat, m)) { markDirty(c); }
    }
    return;
  }

  let rising = m.klass == CLASS_GAS;
  let dy = select(-1, 1, rising);

  // 1) straight fall / rise
  if (tryMove(c, c + vec3<i32>(0, dy, 0), w, m.density, rising)) { return; }

  // 2) the four diagonal cells one step down (up for gas), RNG order — started
  //    DOWNWIND with a probability set by the wind and the material's authored
  //    response (windLateralStart; no-op when the gate is off). The rotation
  //    itself is unchanged: this only decides where it begins.
  let r = windLateralStart(c, rnd >> 10u, m, slotIdx);
  for (var i = 0u; i < 4u; i++) {
    let d = lateralDir(i + r);
    if (tryMove(c, c + vec3<i32>(d.x, dy, d.y), w, m.density, rising)) { return; }
  }

  // 3) gases also spread laterally, RNG order — and this is the stage that
  //    actually makes smoke stream downwind, since a gas that has already risen
  //    as far as it can spends most of its life here.
  if (m.klass == CLASS_GAS) {
    let r2 = windLateralStart(c, rnd >> 14u, m, slotIdx);
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + r2);
      if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w, m.density, rising)) { return; }
    }
  }

  // 4) wandering powders (mites): scuttle laterally, occasionally hop up.
  if (m.klass == CLASS_POWDER && (m.flags & MATF_WANDER) != 0u) {
    let r3 = rnd >> 18u;
    if ((r3 & TUNE_WANDER_HOP_MASK) == 0u &&
        tryMove(c, c + vec3<i32>(0, 1, 0), w, m.density, false)) { return; }
    for (var i = 0u; i < 4u; i++) {
      let d = lateralDir(i + (r3 >> 3u));
      if (tryMove(c, c + vec3<i32>(d.x, 0, d.y), w, m.density, false)) { return; }
    }
  }

  // 5) entrainment: this grain has now failed every move its own weight can
  //    justify, which is exactly the definition of SETTLED — so this is the one
  //    place a wind is allowed to pick it up (invariant 4). Substep 0 only, the
  //    same once-per-tick budget reactions and staining take, which is also
  //    what lets WIND_ENTRAIN_CHANCE be a plain per-tick rate.
  //
  //    Reaching here at all means the chunk was already awake. The ambient
  //    field cannot wake it (invariant 3) — but a grain that DOES hop marks its
  //    chunk through the ordinary tryMove path and so keeps it awake, which is
  //    why this step is gated one notch further out than the drift bias. See
  //    kWindModeEntrain in world.h.
  if (T.windMode >= WIND_MODE_ENTRAIN && P.substep == 0u &&
      m.klass == CLASS_POWDER) {
    if (windEntrain(c, w, m, slotIdx)) { return; }
  }
  // Nothing to do: cell settles. If the whole chunk settles, nothing marks it
  // dirty and it sleeps until a neighbor wakes it.
}
