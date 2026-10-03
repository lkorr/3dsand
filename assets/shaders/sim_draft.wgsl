// sim_draft.wgsl — WIND DRAFTS: the shelter volume (2026-09-30;
// docs/RESEARCH_wind.md §14 is the plan, DESIGN.md §9b "Drafts" the binding
// summary, world.h kDraft* the layout, common.wgsl's WIND DRAFTS block the
// reader).
//
// WHAT PROBLEM THIS SOLVES. The ambient wind is a pure function of position
// (common.wgsl WIND FIELD) and knows nothing about walls: smoke in a sealed hut
// leaned at the full storm speed, embers indoors drifted with the gale, and
// the gust streaks flew through the walls of the room the camera stood in. Rain
// had already learned about roofs (the rain exposure map); the wind had not.
//
// WHAT IT COMPUTES. A box of 4-voxel cells round the player (64 x 32 x 64 =
// 25.6 x 12.8 x 25.6 m), and per cell the TRANSFER of the wind: R_x = the
// velocity a unit horizontal wind along +X at infinity becomes here, R_z the
// same for +Z. "Becomes" means: the incompressible flow with no flux through
// blockers that differs from the free stream by a gradient -- porous-media
// potential flow. u = beta * (e - grad phi), div u = 0, phi = 0 on the box's
// faces (the free stream arrives there). The readers combine the two
// responses with the ambient field AT THE SAMPLE (common.wgsl draftApplyQ),
// which is exact for a uniform wind by linearity and close for the gusts,
// whose shortest wavelength (8 m) is long next to a room.
//
// What that does, physically: a uniform flow cannot exist in a closed box, so
// a sealed room is still. One opening gives a little recirculation at the
// mouth and nothing beyond it. A door to windward and a window to leeward
// carry a draft between them whose speed the openings set. A small gap
// between big volumes jets (the interstitial speed is the flux over the open
// fraction). Two walls with an alley speed the wind up by continuity. What it
// does NOT do: no turbulent wake (the calm pocket behind a wall is about one
// wall high, not ten), and fans are added after, unprojected. HEAT is a third
// right-hand side since wind phase 5 (THE STACK EFFECT below): the volume is
// then a function of geometry AND the heat in it, and re-solves when either
// moves.
//
// WHY THE TRANSFER AND NOT THE WIND. The wind changes every tick (gusts,
// meander); the geometry does not. Solving for the transfer makes the volume
// a function of GEOMETRY ONLY, so it is re-solved only on a tick whose blockers
// changed and a settled world pays one mask check over its active chunks
// (rule 2). And it has no history: the field at tick t is F(the blockers at t,
// the box's origin), recomputed from zero whenever either changes, so a load, a
// replay start or a window move has nothing to carry (the rain exposure map's
// discipline).
//
// THE GRID, AND WHY THE MASKS ARE ROWS. Each cell keeps three 16-bit masks: bit
// (u, v) of the axis-a mask is set when the 4-voxel ROW through the cell along
// a, at cross-section (u, v), holds a blocker. The open fraction of the face
// between two cells on axis a is popcount(~(mA | mB)) / 16 -- the rows that get
// through BOTH cells. So a one-voxel wall anywhere in either cell blocks every
// row it crosses, while an 8-voxel doorway or a 2x2 window stays open in
// proportion. "Any blocker in the cell" would close the wall too, but would
// also close a 4-voxel window to nothing and shrink a doorway to one column.
// Blockers are the ray blockers (solids, powders, opaque liquids) minus
// PASSABLE materials (leaves, vines): a crown read as a solid lump would
// squeeze the flow and speed it up underneath, worse than letting it through.
//
// THE SOLVE, in rows (pass_table.def's draft block), all but the masks
// indirect on one args record -- kDraftTiles groups on a tick whose masks
// moved, ZERO otherwise:
//   maskAll / maskDirty  the row masks: every chunk of the box on a rebuild
//                        tick, else its ACTIVE chunks (only an active chunk can
//                        have changed a voxel); raises meta[changed].
//   args                 one thread: arms the solve, resets the flag.
//   coarseBuild          per tile: each CHUNK of the box is one coarse cell
//                        (16 x 8 x 16 of them), and here each finds its AIR
//                        POCKET -- the largest set of its 64 fine cells joined
//                        through open faces inside it.
//   coarseFaces          per tile: the coarse face coefficients, counting only
//                        fine faces between pocket members.
//   coarseSolve          ONE workgroup, in workgroup memory: the coarse
//                        potential by red-black SOR to convergence. The global
//                        picture: which rooms connect to which openings.
//   fineFirst..Last      per 8^3-cell tile, four overlapping (Schwarz) passes
//                        over the 4-voxel grid: the tile plus a 2-cell halo is
//                        relaxed in workgroup memory with the halo's outer ring
//                        held -- at the coarse reconstruction on the first pass,
//                        at the neighbours' previous answer after that -- and
//                        the last pass writes the transfer.
//
// THE POCKET RULE IS NOT OPTIONAL. A coarse cell is one node; its fine cells
// need not be one air. Where a roof meets a wall the coarse cell holds the
// fine cell inside the room AND the one diagonally outside the corner, joined
// only through solid; counted as one node they leaked every building at its
// edges (a sealed hut measured 5% of the outside wind, a one-door hut a
// quarter). The same rule at the FINE level is the slab rule in cellMasks.
//
// COST. Measured in the explosion scenario before this layout: a one-workgroup
// multigrid at 8 voxels cost ~20 ms a solve, because one workgroup is one SM
// walking 16k cells through L2. Here the only single-workgroup pass touches
// 2,048 cells in workgroup memory; everything per-cell runs 256 groups wide.
//
// DETERMINISM (rule 1). Integer fixed point throughout: phi is Q12 in FINE-
// CELL units, a face coefficient is Q8 (0..256, the open fraction, exact from
// a popcount), every divide is an integer divide. Red-black ordering: a
// half-sweep updates cells of one parity from cells of the other only, so no
// thread reads what another writes in the same half-sweep. The pocket labels
// are double-buffered (a Jacobi propagation, never in place), and the only
// atomics are commutative (OR, ADD, MAX on a packed key). The output is a pure
// function of the masks and the knobs.

// `> voxels` with EXACTLY ONE SPACE (resources.cpp BodyAddressesVoxels).
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(12) var<storage, read> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
// The whole volume: masks, coarse solver, field, Schwarz scratch (world.h
// kDraft* layout). The `> draftField` declaration is also what keeps
// common.wgsl's BOUND reader.
@group(0) @binding(46) var<storage, read_write> draftField : array<u32>;
@group(0) @binding(47) var<storage, read_write> draftMeta : array<atomic<u32>>;
// THE TEMPERATURE LAYER (sim_heat.wgsl), for the STACK EFFECT: the snapshot
// samples the heat updraft per fine cell through common.wgsl's windHeatUpQ
// (declaring `> heatPool` is what keeps that reader BOUND here), and `args`
// reads the layer's activity counters to know when the heat moved.
@group(0) @binding(50) var<storage, read> heatPool : array<u32>;
@group(0) @binding(51) var<storage, read_write> heatMeta : array<atomic<u32>>;

// src/sim/heat.h's monotonic activity counters (check_invariants `heat`).
const HM_RELAX_TICKS : u32 = 24u;
const HM_FREES : u32 = 25u;
const HM_ALLOCS : u32 = 26u;
const HM_RELEASED : u32 = 27u;

// world.h kDraftMeta* / kDraft*Base / kDraftTiles (check_invariants `drafts`
// holds these to world.h).
const DM_CHANGED : u32 = 0u;
const DM_SOLVES : u32 = 1u;
const DM_LAST : u32 = 2u;
const DM_CELLS : u32 = 3u;
const DM_ARGS : u32 = 8u;   // six stage records, 4 words apart
const DRAFT_COARSE_BASE : u32 = 262144u;
const DRAFT_COARSE_WORDS : u32 = 6u;
const DRAFT_PHI_A : u32 = 667648u;
const DRAFT_PHI_B : u32 = 929792u;
const DRAFT_KBASE : u32 = 1191936u;
// The stack effect's regions (world.h kDraftStack*; the published field and
// its live word are common.wgsl's DRAFT_STACK_FIELD / DRAFT_STACK_LIVE).
const DRAFT_SC_BASE : u32 = 1323008u;   // per coarse cell: mean b | phi b
const DRAFT_SB : u32 = 1327104u;        // b per fine cell, Q12 of the cap
const DRAFT_SPHI_A : u32 = 1458176u;
const DRAFT_SPHI_B : u32 = 1589248u;
const DM_HEAT_SEEN : u32 = 32u;
const DM_HEAT_CLOCK : u32 = 33u;
const DM_BOX_HOT : u32 = 34u;
const DM_HEAT_START : u32 = 35u;
const DRAFT_TILES : u32 = 256u;
// Chunks of the box per axis (kDraftChunks*) = the coarse grid.
const DRAFT_CX : i32 = 16;
const DRAFT_CY : i32 = 8;
const DRAFT_CZ : i32 = 16;
const DRAFT_CCELLS : u32 = 2048u;

// Coarse words, per cell.
const CW_PX : u32 = 0u;    // phi x (Q12, fine-cell units)
const CW_PZ : u32 = 1u;    // phi z
const CW_KP : u32 = 2u;    // + face coefficients, 9 bits each: x | y << 9 | z << 18
const CW_KB : u32 = 3u;    // - face coefficients at the box's - faces, same packing
const CW_MEM0 : u32 = 4u;  // pocket members, fine-local bits 0..31 (lx + 4 ly + 16 lz)
const CW_MEM1 : u32 = 5u;  // ...and 32..63

const Q_ONE : i32 = 4096;      // unit velocity / one fine cell of potential, Q12
const K_MASK : u32 = 0x1FFu;   // one 9-bit face coefficient (0..256)
const FIXED_BIT : u32 = 0x8000000u;

fn axisE(a : u32) -> vec3<i32> {
  return vec3<i32>(select(0, 1, a == 0u), select(0, 1, a == 1u), select(0, 1, a == 2u));
}
fn kGet(kp : u32, a : u32) -> i32 { return i32((kp >> (9u * a)) & K_MASK); }
fn tileOf(t : u32) -> vec3<i32> {
  let ti = i32(t);
  return vec3<i32>(ti % (DRAFT_NX / 8), (ti / (DRAFT_NX / 8)) % (DRAFT_NY / 8),
                   ti / ((DRAFT_NX / 8) * (DRAFT_NY / 8)));
}

// ---- THE STACK EFFECT (wind phase 5, 2026-10-02) -----------------------------
// Heat as a third right-hand side of the same projection. The heat updraft b
// (common.wgsl windHeatUpQ: what the sim's wind adds over hot ground) is
// sampled per fine cell at the solve's snapshot, and the solve finds phi_b
// with div(beta (b e_y - grad phi_b)) = 0 -- the rising air made to respect
// the walls. The published S = P(b) - b is the CORRECTION the readers add to
// the local lift (draftStackAtQ), so inside the box lift + S = P(b):
//   * a sealed room's hot column turns over in place (up over the fire, down
//     the cold walls), and no air crosses its walls;
//   * a room with a low and a high opening BREATHES: the column's lift has a
//     path out through the high one, and continuity pulls air in at the low
//     one -- the stack effect, a burning house venting through its window or
//     roof while fresh air comes in at the door;
//   * in open air S is the plume's own inflow at its base and spread at its
//     top, little else.
// Units: b and S in Q12 of sim.windUpdraftCap (4096 = the cap), phi_b in Q12
// fine-cell units like phi x / z. Every pass skips the b work when the
// snapshot saw no heat (DM_BOX_HOT), so a cold box costs what it did.
//
// WHEN IT RE-SOLVES. b moves without any mask moving, so `args` also starts a
// solve when the box holds heat (the last snapshot saw b != 0, or an active
// chunk in the box has a heat page) AND the heat layer's activity counters
// moved since the last solve began AND at least DRAFT_HEAT_PERIOD ticks have
// passed -- a fire's field lags its heat by at most ~22 ticks (0.7 s), and a
// box with no heat in it never solves for heat at all. Determinism: the
// counters are the heat rows' own monotonic atomics, final before this row
// reads them, so the verdict is a function of the tick's inputs.
const DRAFT_HEAT_PERIOD : u32 = 16u;

fn stackOn() -> bool { return T.draftStackQ > 0 && T.updraftGainQ > 0; }
// Did this solve's snapshot see heat? Set by coarseBuild, read by the later
// stages, reset by `args` only when a new solve starts.
fn stackLive() -> bool { return atomicLoad(&draftMeta[DM_BOX_HOT]) != 0u; }
fn heatClock() -> u32 {
  return atomicLoad(&heatMeta[HM_RELAX_TICKS]) + atomicLoad(&heatMeta[HM_FREES]) +
         atomicLoad(&heatMeta[HM_ALLOCS]) + atomicLoad(&heatMeta[HM_RELEASED]);
}

// ---- the masks --------------------------------------------------------------

fn draftBlocks(w : u32) -> bool {
  let m = voxMat(w);
  if (m == MAT_AIR) { return false; }
  let mt = materials[m];
  return isRayBlocker(mt) && (mt.flags & MATF_PASSABLE) == 0u;
}

// The three row masks of fine cell `cell`, packed as the buffer stores them.
// A cell is 4-aligned inside a 16-voxel chunk, so all 64 voxels share one
// page-table entry, and a sentinel chunk is one material throughout.
fn cellMasks(cell : vec3<i32>) -> vec2<u32> {
  let base = T.draftOrigin + cell * 4;
  if (!inWindow(base, T.origin)) { return vec2<u32>(0u, 0u); }
  let e = pageTable[voxSlotOfCell(base)];
  if ((e & PT_SENTINEL_BIT) != 0u) {
    let b = draftBlocks(synthWordAt(e, base, ptSeed()));
    return select(vec2<u32>(0u, 0u), vec2<u32>(0xFFFFFFFFu, 0xFFFFu), b);
  }
  var mx = 0u;
  var my = 0u;
  var mz = 0u;
  for (var lz = 0u; lz < 4u; lz++) {
    for (var ly = 0u; ly < 4u; ly++) {
      for (var lx = 0u; lx < 4u; lx++) {
        let w = voxWordAtEntry(e, base + vec3<i32>(i32(lx), i32(ly), i32(lz)));
        if (draftBlocks(w)) {
          mx |= 1u << (ly + 4u * lz);
          my |= 1u << (lx + 4u * lz);
          mz |= 1u << (lx + 4u * ly);
        }
      }
    }
  }
  // A COMPLETE SLAB through the cell -- every row along some axis blocked: a
  // floor, a roof, a wall -- makes the whole cell solid. A cell is one node,
  // so without this the air on the two sides of a one-voxel slab is the SAME
  // air: a hut's floor cell joined the room above the pad to the open air
  // under it, its roof cell the room to the sky, and a "sealed" hut measured
  // a quarter of the outside wind. Costs at most 3 voxels (0.3 m) of air
  // beside a thin wall, which no consumer can tell from the wall itself.
  if (mx == 0xFFFFu || my == 0xFFFFu || mz == 0xFFFFu) {
    return vec2<u32>(0xFFFFFFFFu, 0xFFFFu);
  }
  return vec2<u32>(mx | (my << 16u), mz);
}

fn fineIndex(c : vec3<i32>) -> u32 {
  return u32((c.z * DRAFT_NY + c.y) * DRAFT_NX + c.x);
}

// One chunk of the box (vc in chunk units inside it), one thread per cell.
// Returns whether this thread's cell changed.
fn maskCell(vc : vec3<i32>, li : u32) -> bool {
  let cell = vc * 4 + vec3<i32>(i32(li & 3u), i32((li >> 2u) & 3u), i32(li >> 4u));
  let m = cellMasks(cell);
  let i = 2u * fineIndex(cell);
  let changed = draftField[i] != m.x || draftField[i + 1u] != m.y;
  if (changed) {
    draftField[i] = m.x;
    draftField[i + 1u] = m.y;
  }
  return changed;
}

// EVERY chunk of the box: a rebuild tick (the box moved, the materials or the
// gate changed, or the buffer is new). Raises `changed` unconditionally: a
// fresh buffer's zero masks equal an all-air box's, and the field still has to
// be solved once.
@compute @workgroup_size(64)
fn maskAll(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  let g = i32(wg.x);
  let vc = vec3<i32>(g % DRAFT_CX, (g / DRAFT_CX) % DRAFT_CY, g / (DRAFT_CX * DRAFT_CY));
  _ = maskCell(vc, li);
  if (wg.x == 0u && li == 0u) { atomicOr(&draftMeta[DM_CHANGED], 1u); }
}

// The tick's ACTIVE chunks (the CA's own dispatch list), skipping the ones
// outside the box. Only an active chunk can hold a voxel that changed since
// the last tick: settled matter writes nothing, and every writer -- the CA, an
// op, a landing particle, a streamed refill -- marks the chunk it wrote.
@compute @workgroup_size(64)
fn maskDirty(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  let wc = slotWorldChunk(dirtyList[wg.x], T.origin);
  let vc = wc - (T.draftOrigin >> vec3<u32>(CHUNK_SHIFT));
  if (vc.x < 0 || vc.y < 0 || vc.z < 0 ||
      vc.x >= DRAFT_CX || vc.y >= DRAFT_CY || vc.z >= DRAFT_CZ) { return; }
  // The stack effect's doorbell: an active chunk of the box holds heat.
  if (li == 0u && stackOn() && windHeatEntry(wc, T.origin) != 0u) {
    atomicOr(&draftMeta[DM_HEAT_SEEN], 1u);
  }
  if (maskCell(vc, li)) {
    atomicOr(&draftMeta[DM_CHANGED], 1u);
    atomicAdd(&draftMeta[DM_CELLS], 1u);
  }
}

// One thread: arm (or not) the solve, and reset the flag for the next tick.
// THE SOLVE IS A PIPELINE, one stage a tick (DRAFT_STAGES of them):
//   1  coarseBuild + coarseFaces   (the snapshot: pockets, coarse and fine faces)
//   2  coarseSolve
//   3  fineFirst   4 fineMid   5 fineMid2   6 fineLast (publishes the field)
// A new solve starts only on a tick with no solve in flight and a mask that
// moved since the last one began; masks that move DURING a solve are picked up
// by the next. So a burning house costs one stage a tick, never a whole solve,
// and the field it publishes lags its geometry by at most DRAFT_STAGES ticks.
// Every stage after the snapshot reads only what the snapshot wrote (the
// pockets, the coarse and packed fine faces, the scratch potentials), so a
// mask changing mid-solve cannot tear it.
//
// A BURST (TickParams.draftMode bit 1: the box moved, or the gate, the
// materials or the buffer is new) runs every stage on this one tick -- the
// field must match the box's new origin before anything reads it -- and
// abandons any solve in flight.
//
// Deterministic: the stage word is GPU state advanced once a tick by this one
// thread from the tick's own inputs, so two runs of one input stream agree.
// It is the one piece of history in the volume: a load or a replay start
// bursts, and so sees the current geometry up to DRAFT_STAGES ticks before an
// uninterrupted run's in-flight solve would have published it.
const DRAFT_STAGES : u32 = 6u;
const DM_STAGE : u32 = 4u;

@compute @workgroup_size(1)
fn args() {
  let burst = (T.draftMode & 2u) != 0u;
  let prev = atomicLoad(&draftMeta[DM_STAGE]);
  var next = 0u;
  var start = false;
  if (burst) {
    atomicStore(&draftMeta[DM_CHANGED], 0u);
    atomicAdd(&draftMeta[DM_SOLVES], 1u);
    atomicStore(&draftMeta[DM_LAST], T.tick);
    start = true;
  } else if (prev != 0u && prev < DRAFT_STAGES) {
    next = prev + 1u;
  } else {
    let masks = atomicExchange(&draftMeta[DM_CHANGED], 0u) != 0u;
    // THE STACK EFFECT'S TRIGGER (block above): heat in the box, moved, due.
    var heat = false;
    if (stackOn()) {
      let hot = atomicLoad(&draftMeta[DM_HEAT_SEEN]) != 0u || stackLive();
      let moved = heatClock() != atomicLoad(&draftMeta[DM_HEAT_CLOCK]);
      let due = T.tick - atomicLoad(&draftMeta[DM_HEAT_START]) >= DRAFT_HEAT_PERIOD;
      heat = hot && moved && due;
    }
    if (masks || heat) {
      next = 1u;
      atomicAdd(&draftMeta[DM_SOLVES], 1u);
      start = true;
    }
  }
  // A solve begins: its snapshot (coarseBuild, this tick) re-decides whether
  // the box is hot, and the heat it sees is the heat as of now.
  if (start) {
    atomicStore(&draftMeta[DM_HEAT_SEEN], 0u);
    atomicStore(&draftMeta[DM_BOX_HOT], 0u);
    atomicStore(&draftMeta[DM_HEAT_CLOCK], heatClock());
    atomicStore(&draftMeta[DM_HEAT_START], T.tick);
  }
  if (next == DRAFT_STAGES) { atomicStore(&draftMeta[DM_LAST], T.tick); }
  atomicStore(&draftMeta[DM_STAGE], next);
  for (var s = 1u; s <= DRAFT_STAGES; s++) {
    let o = DM_ARGS + 4u * (s - 1u);
    atomicStore(&draftMeta[o], select(0u, DRAFT_TILES, burst || s == next));
    atomicStore(&draftMeta[o + 1u], 1u);
    atomicStore(&draftMeta[o + 2u], 1u);
  }
}

// ---- fine-grid faces ----------------------------------------------------------

fn fineMask(c : vec3<i32>, a : u32) -> u32 {
  let i = 2u * fineIndex(c);
  if (a == 0u) { return draftField[i] & 0xFFFFu; }
  if (a == 1u) { return draftField[i] >> 16u; }
  return draftField[i + 1u] & 0xFFFFu;
}

// The Q8 coefficient of the face between fine cell g and g + e_a. A face on
// the box's surface counts the inside cell's own rows (the outside is the free
// stream); a face wholly outside is closed.
fn fineFaceK(g : vec3<i32>, a : u32) -> u32 {
  let nb = g + axisE(a);
  let inG = draftInside(g);
  let inN = draftInside(nb);
  var m = 0xFFFFu;
  if (inG && inN) {
    m = fineMask(g, a) | fineMask(nb, a);
  } else if (inG) {
    m = fineMask(g, a);
  } else if (inN) {
    m = fineMask(nb, a);
  }
  return countOneBits(~m & 0xFFFFu) * 16u;
}

// ---- the coarse grid: one cell per chunk of the box ------------------------------

fn cInside(c : vec3<i32>) -> bool {
  return c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < DRAFT_CX && c.y < DRAFT_CY && c.z < DRAFT_CZ;
}
fn cIndex(c : vec3<i32>) -> u32 { return u32((c.z * DRAFT_CY + c.y) * DRAFT_CX + c.x); }
fn cAddr(c : vec3<i32>, f : u32) -> u32 { return DRAFT_COARSE_BASE + DRAFT_COARSE_WORDS * cIndex(c) + f; }
fn cCoord(i : u32) -> vec3<i32> {
  let ii = i32(i);
  return vec3<i32>(ii % DRAFT_CX, (ii / DRAFT_CX) % DRAFT_CY, ii / (DRAFT_CX * DRAFT_CY));
}
// Is fine cell g (inside coarse cell p) a member of p's air pocket?
fn memberOf(p : vec3<i32>, g : vec3<i32>) -> bool {
  let l = g - p * 4;
  let bit = u32(l.x + 4 * l.y + 16 * l.z);
  let w = draftField[cAddr(p, select(CW_MEM0, CW_MEM1, bit >= 32u))];
  return ((w >> (bit & 31u)) & 1u) != 0u;
}

// ---- coarseBuild: each coarse cell's air pocket -------------------------------
// One workgroup per 8^3-cell tile = 2 x 2 x 2 coarse cells, two fine cells a
// thread. Labels propagate by MIN through open fine faces that stay inside one
// coarse cell, double-buffered so every round reads only the last one's
// labels (a fixed number of rounds: 12 covers any path through a 4 x 4 x 4
// block but a coiled one; a pocket split by too few rounds is two pockets,
// the larger kept -- deterministic either way). The pocket is the label with the most
// open ROWS -- a cell sliced by a wall has fewer than clean air, so the room
// side of a wall beats a sliver of outside -- ties to the lower label.

const NO_LABEL : u32 = 0xFFFFFFFFu;
const POCKET_ROUNDS : u32 = 12u;
var<workgroup> pbMask : array<vec2<u32>, 512>;
var<workgroup> pbLab : array<array<u32, 512>, 2>;
var<workgroup> pbWgt : array<atomic<u32>, 512>;
var<workgroup> pbBest : array<atomic<u32>, 8>;
var<workgroup> pbMem : array<atomic<u32>, 16>;
// The stack effect's snapshot: b per fine cell, its pocket sum and member
// count per coarse cell, and "any heat in this tile".
var<workgroup> pbB : array<u32, 512>;
var<workgroup> pbBs : array<atomic<u32>, 8>;
var<workgroup> pbBn : array<atomic<u32>, 8>;
var<workgroup> pbHot : atomic<u32>;

fn tLocal(t : u32) -> vec3<i32> { return vec3<i32>(i32(t & 7u), i32((t >> 3u) & 7u), i32(t >> 6u)); }
fn rowsOpen(m : vec2<u32>) -> u32 {
  return countOneBits(~m.x & 0xFFFFu) + countOneBits(~m.x >> 16u) + countOneBits(~m.y & 0xFFFFu);
}
fn axisMask(m : vec2<u32>, a : u32) -> u32 {
  if (a == 0u) { return m.x & 0xFFFFu; }
  if (a == 1u) { return m.x >> 16u; }
  return m.y & 0xFFFFu;
}

@compute @workgroup_size(256)
fn coarseBuild(@builtin(workgroup_id) wg : vec3<u32>,
               @builtin(local_invocation_index) li : u32) {
  let tile = tileOf(wg.x);
  if (li == 0u) { atomicStore(&pbHot, 0u); }
  workgroupBarrier();
  for (var k = 0u; k < 2u; k++) {
    let t = li + 256u * k;
    let g = tile * 8 + tLocal(t);
    let i = 2u * fineIndex(g);
    let m = vec2<u32>(draftField[i], draftField[i + 1u] & 0xFFFFu);
    pbMask[t] = m;
    pbLab[0][t] = select(t, NO_LABEL, rowsOpen(m) == 0u);
    // The fine faces' coefficients, once a solve, for the four fine passes.
    draftField[DRAFT_KBASE + fineIndex(g)] =
        fineFaceK(g, 0u) | (fineFaceK(g, 1u) << 9u) | (fineFaceK(g, 2u) << 18u);
    atomicStore(&pbWgt[t], 0u);
    // THE STACK EFFECT'S SOURCE: the heat updraft at the cell's centre, Q12 of
    // the cap -- the same function the sim's wind adds (windHeatUpQ), so the
    // projection and the local lift agree. A cell with no open row holds no
    // air and gets none.
    var bq = 0u;
    if (stackOn() && rowsOpen(m) != 0u) {
      let up = windHeatUpQ(T.draftOrigin + g * 4 + vec3<i32>(2), &T);
      bq = u32(clamp(up / max(T.updraftCapQ >> 12u, 1), 0, Q_ONE));
    }
    draftField[DRAFT_SB + fineIndex(g)] = bq;
    pbB[t] = bq;
    if (bq != 0u) { atomicOr(&pbHot, 1u); }
  }
  if (li < 8u) {
    atomicStore(&pbBest[li], 0u);
    atomicStore(&pbBs[li], 0u);
    atomicStore(&pbBn[li], 0u);
  }
  if (li < 16u) { atomicStore(&pbMem[li], 0u); }
  workgroupBarrier();
  for (var r = 0u; r < POCKET_ROUNDS; r++) {
    let src = r & 1u;
    for (var k = 0u; k < 2u; k++) {
      let t = li + 256u * k;
      let lt = tLocal(t);
      var l = pbLab[src][t];
      if (l != NO_LABEL) {
        for (var a = 0u; a < 3u; a++) {
          let e = axisE(a);
          for (var s = 0u; s < 2u; s++) {
            let n = select(lt - e, lt + e, s == 1u);
            if (n.x < 0 || n.y < 0 || n.z < 0 || n.x > 7 || n.y > 7 || n.z > 7) { continue; }
            if (any((n >> vec3<u32>(2u)) != (lt >> vec3<u32>(2u)))) { continue; }  // other coarse cell
            let tn = u32(n.x + 8 * n.y + 64 * n.z);
            let ln = pbLab[src][tn];
            if (ln == NO_LABEL) { continue; }
            if ((~(axisMask(pbMask[t], a) | axisMask(pbMask[tn], a)) & 0xFFFFu) == 0u) { continue; }
            l = min(l, ln);
          }
        }
      }
      pbLab[1u - src][t] = l;
    }
    workgroupBarrier();
  }
  let fin = POCKET_ROUNDS & 1u;
  for (var k = 0u; k < 2u; k++) {
    let t = li + 256u * k;
    let l = pbLab[fin][t];
    if (l != NO_LABEL) { atomicAdd(&pbWgt[l], rowsOpen(pbMask[t])); }
  }
  workgroupBarrier();
  for (var k = 0u; k < 2u; k++) {
    let t = li + 256u * k;
    let l = pbLab[fin][t];
    let lt = tLocal(t);
    let cs = u32((lt.x >> 2u) + 2 * (lt.y >> 2u) + 4 * (lt.z >> 2u));
    if (l != NO_LABEL) { atomicMax(&pbBest[cs], (atomicLoad(&pbWgt[l]) << 16u) | (0xFFFFu - l)); }
  }
  workgroupBarrier();
  for (var k = 0u; k < 2u; k++) {
    let t = li + 256u * k;
    let l = pbLab[fin][t];
    let lt = tLocal(t);
    let cs = u32((lt.x >> 2u) + 2 * (lt.y >> 2u) + 4 * (lt.z >> 2u));
    if (l != NO_LABEL &&
        ((atomicLoad(&pbWgt[l]) << 16u) | (0xFFFFu - l)) == atomicLoad(&pbBest[cs])) {
      let lc = lt & vec3<i32>(3);
      let bit = u32(lc.x + 4 * lc.y + 16 * lc.z);
      atomicOr(&pbMem[cs * 2u + (bit >> 5u)], 1u << (bit & 31u));
      // The coarse cell's b is the mean over its POCKET, for the reason its
      // faces count only pocket members: the sliver beyond a wall is not
      // this cell's air.
      atomicAdd(&pbBs[cs], pbB[t]);
      atomicAdd(&pbBn[cs], 1u);
    }
  }
  workgroupBarrier();
  if (li < 8u) {
    let p = tile * 2 + vec3<i32>(i32(li & 1u), i32((li >> 1u) & 1u), i32(li >> 2u));
    draftField[cAddr(p, CW_MEM0)] = atomicLoad(&pbMem[li * 2u]);
    draftField[cAddr(p, CW_MEM1)] = atomicLoad(&pbMem[li * 2u + 1u]);
    draftField[DRAFT_SC_BASE + 2u * cIndex(p)] =
        atomicLoad(&pbBs[li]) / max(atomicLoad(&pbBn[li]), 1u);
  }
  if (li == 0u && atomicLoad(&pbHot) != 0u) { atomicOr(&draftMeta[DM_BOX_HOT], 1u); }
}

// ---- coarseFaces: the coarse face coefficients, between pockets only ----------
// A coarse face is 4 x 4 fine faces; each counts only if the fine cell on
// each side belongs to its own coarse cell's pocket. Normalised to the MEAN
// open fraction (0..256). The box's - faces against the free stream likewise.

var<workgroup> cfK : array<atomic<u32>, 24>;
var<workgroup> cfB : array<atomic<u32>, 24>;

@compute @workgroup_size(256)
fn coarseFaces(@builtin(workgroup_id) wg : vec3<u32>,
               @builtin(local_invocation_index) li : u32) {
  let tile = tileOf(wg.x);
  if (li < 24u) {
    atomicStore(&cfK[li], 0u);
    atomicStore(&cfB[li], 0u);
  }
  workgroupBarrier();
  // 8 cells x 3 axes x 16 fine faces = 384 tasks, plus as many boundary ones.
  for (var task = li; task < 384u; task += 256u) {
    let cs = task / 48u;
    let a = (task / 16u) % 3u;
    let j = task % 16u;
    let p = tile * 2 + vec3<i32>(i32(cs & 1u), i32((cs >> 1u) & 1u), i32(cs >> 2u));
    let e = axisE(a);
    let u = i32(j & 3u);
    let v = i32(j >> 2u);
    var cross = vec3<i32>(0, u, v);
    if (a == 1u) { cross = vec3<i32>(u, 0, v); }
    if (a == 2u) { cross = vec3<i32>(u, v, 0); }
    let fLo = p * 4 + cross;        // the - side layer of p
    let fHi = fLo + e * 3;          // the + side layer of p
    let q = p + e;
    if (memberOf(p, fHi) && (!cInside(q) || memberOf(q, fHi + e))) {
      atomicAdd(&cfK[cs * 3u + a], fineFaceK(fHi, a));
    }
    if (dot(p, e) == 0 && memberOf(p, fLo)) {
      atomicAdd(&cfB[cs * 3u + a], fineFaceK(fLo - e, a));
    }
  }
  workgroupBarrier();
  if (li < 8u) {
    let p = tile * 2 + vec3<i32>(i32(li & 1u), i32((li >> 1u) & 1u), i32(li >> 2u));
    var kp = 0u;
    var kb = 0u;
    for (var a = 0u; a < 3u; a++) {
      kp |= ((atomicLoad(&cfK[li * 3u + a]) + 8u) >> 4u) << (9u * a);
      kb |= ((atomicLoad(&cfB[li * 3u + a]) + 8u) >> 4u) << (9u * a);
    }
    draftField[cAddr(p, CW_KP)] = kp;
    draftField[cAddr(p, CW_KB)] = kb;
  }
}

// ---- coarseSolve: the coarse potential, in workgroup memory -------------------
// THE OPERATOR at a spacing of 4 fine cells, phi in fine-cell units: the flux
// through a face is K (e.n - dphi / 4), so div = 0 reads
//     phi_C sum K = sum K phi_nb - 4 (K+a - K-a) Q_ONE      (a = the stream axis)
// with phi_nb = 0 across the box's surface. Red-black SOR at omega 7/4 (in
// integers, exactly), 1,024 threads: one cell a thread a half-sweep, so a sweep
// costs a few hundred cycles and TUNE_DRAFT_COARSE_SWEEPS of them converge a
// 16-cell span. A pocket with no way out is singular but consistent (its
// sources sum to zero), and SOR settles it like any other.

var<workgroup> csPx : array<i32, 2048>;
var<workgroup> csPz : array<i32, 2048>;
var<workgroup> csKp : array<u32, 2048>;
// The stack effect's phi b and its per-cell source term. 40 KiB with the
// three above: the boundary coefficients (CW_KB) are read from the buffer
// instead of held here -- only the box's minus faces use them.
var<workgroup> csPb : array<i32, 2048>;
var<workgroup> csSb : array<i32, 2048>;

fn scB(c : vec3<i32>) -> i32 { return i32(draftField[DRAFT_SC_BASE + 2u * cIndex(c)]); }

@compute @workgroup_size(1024)
fn coarseSolve(@builtin(workgroup_id) wg : vec3<u32>,
               @builtin(local_invocation_index) li : u32) {
  // Dispatched with the tile count (one args record for every solve row);
  // only the first group works.
  if (wg.x != 0u) { return; }
  let live = stackLive();
  for (var i = li; i < DRAFT_CCELLS; i += 1024u) {
    let c = cCoord(i);
    csKp[i] = draftField[cAddr(c, CW_KP)];
    csPx[i] = 0;
    csPz[i] = 0;
    csPb[i] = 0;
  }
  workgroupBarrier();
  let st = vec3<u32>(1u, u32(DRAFT_CX), u32(DRAFT_CX * DRAFT_CY));
  // The b source per coarse cell, once: the y faces' flux of b at the face
  // means (zero outside the box: the free stream is cold), times the spacing
  // of 4 fine cells -- the stream's `4 (K+ - K-) Q_ONE` with b for Q_ONE.
  for (var i = li; i < DRAFT_CCELLS; i += 1024u) {
    var sb = 0;
    if (live) {
      let c = cCoord(i);
      let bc = scB(c);
      let kpy = kGet(csKp[i], 1u);
      var kmy = 0;
      var bd = 0;
      if (c.y == 0) {
        kmy = kGet(draftField[cAddr(c, CW_KB)], 1u);
      } else {
        kmy = kGet(csKp[i - st.y], 1u);
        bd = scB(c - vec3<i32>(0, 1, 0));
      }
      var bu = 0;
      if (cInside(c + vec3<i32>(0, 1, 0))) { bu = scB(c + vec3<i32>(0, 1, 0)); }
      sb = -4 * (kpy * ((bc + bu) / 2) - kmy * ((bc + bd) / 2));
    }
    csSb[i] = sb;
  }
  workgroupBarrier();
  for (var it = 0; it < TUNE_DRAFT_COARSE_SWEEPS; it++) {
    for (var color = 0u; color < 2u; color++) {
      for (var i = li; i < DRAFT_CCELLS; i += 1024u) {
        let c = cCoord(i);
        if ((u32(c.x + c.y + c.z) & 1u) != color) { continue; }
        var sum = 0;
        var ax = 0;
        var az = 0;
        var ab = 0;
        var sx = 0;
        var sz = 0;
        for (var a = 0u; a < 3u; a++) {
          let e = axisE(a);
          let kpa = kGet(csKp[i], a);
          var kma = 0;
          var pmx = 0;
          var pmz = 0;
          var pmb = 0;
          if (dot(c, e) == 0) {
            kma = kGet(draftField[cAddr(c, CW_KB)], a);
          } else {
            kma = kGet(csKp[i - st[a]], a);
            pmx = csPx[i - st[a]];
            pmz = csPz[i - st[a]];
            pmb = csPb[i - st[a]];
          }
          var ppx = 0;
          var ppz = 0;
          var ppb = 0;
          if (cInside(c + e)) {
            ppx = csPx[i + st[a]];
            ppz = csPz[i + st[a]];
            ppb = csPb[i + st[a]];
          }
          sum += kpa + kma;
          ax += kpa * ppx + kma * pmx;
          az += kpa * ppz + kma * pmz;
          ab += kpa * ppb + kma * pmb;
          if (a == 0u) { sx = kpa - kma; }
          if (a == 2u) { sz = kpa - kma; }
        }
        if (sum > 0) {
          let gx = (ax - 4 * sx * Q_ONE) / sum;
          let gz = (az - 4 * sz * Q_ONE) / sum;
          csPx[i] += (7 * (gx - csPx[i])) / 4;
          csPz[i] += (7 * (gz - csPz[i])) / 4;
          if (live) {
            let gb = (ab + csSb[i]) / sum;
            csPb[i] += (7 * (gb - csPb[i])) / 4;
          }
        } else {
          csPx[i] = 0;
          csPz[i] = 0;
          csPb[i] = 0;
        }
      }
      workgroupBarrier();
    }
  }
  for (var i = li; i < DRAFT_CCELLS; i += 1024u) {
    let c = cCoord(i);
    draftField[cAddr(c, CW_PX)] = bitcast<u32>(csPx[i]);
    draftField[cAddr(c, CW_PZ)] = bitcast<u32>(csPz[i]);
    draftField[DRAFT_SC_BASE + 2u * cIndex(c) + 1u] = bitcast<u32>(csPb[i]);
  }
}

// ---- the fine passes ----------------------------------------------------------
// One workgroup per 8^3-cell tile. The region is the tile plus a 2-cell halo
// (12^3); its outermost ring is HELD (a Dirichlet boundary), everything inside
// it relaxes, and the tile's own cells are stored. Four passes, ping-ponged
// through two scratch buffers so no pass reads what another group is writing:
//   fineFirst  ring + start from the coarse reconstruction    -> phi A
//   fineMid    ring + start from phi A (the neighbours' answer) -> phi B
//   fineMid2   ring + start from phi B                        -> phi A
//   fineLast   ring + start from phi A                        -> THE FIELD
// The overlapping passes are what let the fine answer stand on its own: the
// coarse grid is 16 voxels, too coarse to be trusted next to a door.

const RG : i32 = 12;
const RG3 : u32 = 1728u;
var<workgroup> wfx : array<i32, 1728>;
// The stack effect's phi b and b over the region (zero outside the box).
var<workgroup> wfb : array<i32, 1728>;
var<workgroup> wbb : array<i32, 1728>;
var<workgroup> wfz : array<i32, 1728>;
var<workgroup> wkp : array<u32, 1728>;

fn cPhi(c : vec3<i32>, f : u32) -> i32 { return bitcast<i32>(draftField[cAddr(c, f)]); }
// The - face coefficient of coarse cell c on axis a.
fn cKm(c : vec3<i32>, a : u32) -> i32 {
  let e = axisE(a);
  if (dot(c, e) == 0) { return kGet(draftField[cAddr(c, CW_KB)], a); }
  return kGet(draftField[cAddr(c - e, CW_KP)], a);
}

// The coarse potential reconstructed at fine cell g (inside the box): its
// coarse cell's value plus the offset times that cell's gradient. The gradient
// across an OPEN face is the difference to that neighbour; across a CLOSED one
// it is the stream itself (no flux through the face means e - grad phi = 0
// there), which is what makes a sealed room's reconstruction already still.
// A fine cell OUTSIDE its coarse cell's pocket (the sliver beyond a wall) is
// reconstructed from the face neighbour on its side instead.
fn reconstruct(g : vec3<i32>) -> vec2<i32> {
  var p = g >> vec3<u32>(2u);
  if (!memberOf(p, g)) {
    let l = g - p * 4;
    for (var a = 0u; a < 3u; a++) {
      let e = axisE(a);
      if (dot(l, e) == 0 && cInside(p - e)) { p = p - e; break; }
      if (dot(l, e) == 3 && cInside(p + e)) { p = p + e; break; }
    }
  }
  let px = cPhi(p, CW_PX);
  let pz = cPhi(p, CW_PZ);
  let kpp = draftField[cAddr(p, CW_KP)];
  let off = (g - p * 4) * 2 - vec3<i32>(3);   // offset from p's centre, half fine cells
  var ox = px;
  var oz = pz;
  for (var a = 0u; a < 3u; a++) {
    let e = axisE(a);
    let oa = dot(off, e);
    let ex = select(0, Q_ONE, a == 0u);
    let ez = select(0, Q_ONE, a == 2u);
    var gx = ex;
    var gz = ez;
    if (oa > 0) {
      if (kGet(kpp, a) > 0) {
        if (cInside(p + e)) {
          gx = (cPhi(p + e, CW_PX) - px) / 4;
          gz = (cPhi(p + e, CW_PZ) - pz) / 4;
        } else {
          gx = -px / 2;
          gz = -pz / 2;
        }
      }
    } else {
      if (cKm(p, a) > 0) {
        if (cInside(p - e)) {
          gx = (px - cPhi(p - e, CW_PX)) / 4;
          gz = (pz - cPhi(p - e, CW_PZ)) / 4;
        } else {
          gx = px / 2;
          gz = pz / 2;
        }
      }
    }
    ox += (oa * gx) / 2;
    oz += (oa * gz) / 2;
  }
  return vec2<i32>(ox, oz);
}

// phi b, reconstructed at fine cell g by reconstruct()'s rule with b for the
// stream: across a CLOSED y face the gradient is the cell's mean b (no flux
// means b - dphi/dy = 0 there), across a closed x or z face it is 0.
fn cPhiB(c : vec3<i32>) -> i32 { return bitcast<i32>(draftField[DRAFT_SC_BASE + 2u * cIndex(c) + 1u]); }
fn reconstructB(g : vec3<i32>) -> i32 {
  var p = g >> vec3<u32>(2u);
  if (!memberOf(p, g)) {
    let l = g - p * 4;
    for (var a = 0u; a < 3u; a++) {
      let e = axisE(a);
      if (dot(l, e) == 0 && cInside(p - e)) { p = p - e; break; }
      if (dot(l, e) == 3 && cInside(p + e)) { p = p + e; break; }
    }
  }
  let pb = cPhiB(p);
  let bc = scB(p);
  let kpp = draftField[cAddr(p, CW_KP)];
  let off = (g - p * 4) * 2 - vec3<i32>(3);
  var ob = pb;
  for (var a = 0u; a < 3u; a++) {
    let e = axisE(a);
    let oa = dot(off, e);
    var gb = select(0, bc, a == 1u);
    if (oa > 0) {
      if (kGet(kpp, a) > 0) {
        if (cInside(p + e)) { gb = (cPhiB(p + e) - pb) / 4; } else { gb = -pb / 2; }
      }
    } else {
      if (cKm(p, a) > 0) {
        if (cInside(p - e)) { gb = (pb - cPhiB(p - e)) / 4; } else { gb = pb / 2; }
      }
    }
    ob += (oa * gb) / 2;
  }
  return ob;
}

// The interstitial velocity along one axis at a cell, from its two faces on
// that axis: the open-area-weighted mean of e - dphi across each.
fn faceVel(kpa : i32, kma : i32, e : i32, pp : i32, pc : i32, pm : i32) -> i32 {
  let ks = kpa + kma;
  if (ks == 0) { return 0; }
  return (kpa * (e - (pp - pc)) + kma * (e - (pc - pm))) / ks;
}

fn pack16(lo : i32, hi : i32) -> u32 {
  let a = u32(clamp(lo, -32767, 32767)) & 0xFFFFu;
  let b = u32(clamp(hi, -32767, 32767)) & 0xFFFFu;
  return a | (b << 16u);
}

// The two transfers at region cell r (its neighbours must be in the region),
// and the cell's total open face area (0 = closed on every face).
struct CellV { rx : vec3<i32>, rz : vec3<i32>, open : i32 };
fn cellVel(r : u32) -> CellV {
  let sy = u32(RG);
  let sz = u32(RG * RG);
  let k = wkp[r];
  let kpx = kGet(k, 0u);
  let kpy = kGet(k, 1u);
  let kpz = kGet(k, 2u);
  let kmx = kGet(wkp[r - 1u], 0u);
  let kmy = kGet(wkp[r - sy], 1u);
  let kmz = kGet(wkp[r - sz], 2u);
  var v : CellV;
  v.rx = vec3<i32>(faceVel(kpx, kmx, Q_ONE, wfx[r + 1u], wfx[r], wfx[r - 1u]),
                   faceVel(kpy, kmy, 0, wfx[r + sy], wfx[r], wfx[r - sy]),
                   faceVel(kpz, kmz, 0, wfx[r + sz], wfx[r], wfx[r - sz]));
  v.rz = vec3<i32>(faceVel(kpx, kmx, 0, wfz[r + 1u], wfz[r], wfz[r - 1u]),
                   faceVel(kpy, kmy, 0, wfz[r + sy], wfz[r], wfz[r - sy]),
                   faceVel(kpz, kmz, Q_ONE, wfz[r + sz], wfz[r], wfz[r - sz]));
  v.open = kpx + kmx + kpy + kmy + kpz + kmz;
  return v;
}

// THE STACK CORRECTION at region cell r: the face-flux velocity of
// b e_y - grad phi_b (b on a y face = the mean of its two cells), minus the
// cell's own b, so the reader's local lift plus this is the projected flow.
// .w = the cell's open face area, as CellV.open. Bounds: K <= 256, |b| <=
// 4096 and |phi_b| a few hundred thousand, so every product fits in i32.
fn stackBase(scratch : u32) -> u32 {
  return select(DRAFT_SPHI_B, DRAFT_SPHI_A, scratch == DRAFT_PHI_A);
}
fn cellStack(r : u32) -> vec4<i32> {
  let sy = u32(RG);
  let sz = u32(RG * RG);
  let k = wkp[r];
  let kpx = kGet(k, 0u);
  let kpy = kGet(k, 1u);
  let kpz = kGet(k, 2u);
  let kmx = kGet(wkp[r - 1u], 0u);
  let kmy = kGet(wkp[r - sy], 1u);
  let kmz = kGet(wkp[r - sz], 2u);
  let bp = (wbb[r] + wbb[r + sy]) / 2;
  let bm = (wbb[r - sy] + wbb[r]) / 2;
  var uy = 0;
  if (kpy + kmy > 0) {
    uy = (kpy * (bp - (wfb[r + sy] - wfb[r])) + kmy * (bm - (wfb[r] - wfb[r - sy]))) /
         (kpy + kmy);
  }
  return vec4<i32>(faceVel(kpx, kmx, 0, wfb[r + 1u], wfb[r], wfb[r - 1u]),
                   uy - wbb[r],
                   faceVel(kpz, kmz, 0, wfb[r + sz], wfb[r], wfb[r - sz]),
                   kpx + kmx + kpy + kmy + kpz + kmz);
}

// One fine pass. `src`: 0 = start from the coarse reconstruction, else the
// scratch buffer at that word base. `dst`: the scratch base to store the tile
// into, or 0 = write THE FIELD.
fn finePass(wgx : u32, li : u32, src : u32, dst : u32) {
  let ro = tileOf(wgx) * 8 - vec3<i32>(2);
  // The stack effect rides along only when this solve's snapshot saw heat;
  // otherwise every b line below is skipped and the pass is what it was.
  let live = stackLive();
  for (var r = li; r < RG3; r += 256u) {
    let ri = i32(r);
    let rc = vec3<i32>(ri % RG, (ri / RG) % RG, ri / (RG * RG));
    let g = ro + rc;
    let inD = draftInside(g);
    // Inside the box: the coefficients coarseBuild packed. Outside (a tile at
    // the box's face): the boundary rule, computed here.
    var k = 0u;
    if (inD) {
      k = draftField[DRAFT_KBASE + fineIndex(g)];
    } else {
      k = fineFaceK(g, 0u) | (fineFaceK(g, 1u) << 9u) | (fineFaceK(g, 2u) << 18u);
    }
    let ring = rc.x == 0 || rc.y == 0 || rc.z == 0 ||
               rc.x == RG - 1 || rc.y == RG - 1 || rc.z == RG - 1;
    if (ring || !inD) { k |= FIXED_BIT; }
    wkp[r] = k;
    var ph = vec2<i32>(0, 0);
    if (inD) {
      if (src == 0u) {
        ph = reconstruct(g);
      } else {
        let o = src + 2u * fineIndex(g);
        ph = vec2<i32>(bitcast<i32>(draftField[o]), bitcast<i32>(draftField[o + 1u]));
      }
    }
    wfx[r] = ph.x;
    wfz[r] = ph.y;
    var pb = 0;
    var bb = 0;
    if (live && inD) {
      bb = i32(draftField[DRAFT_SB + fineIndex(g)]);
      if (src == 0u) {
        pb = reconstructB(g);
      } else {
        pb = bitcast<i32>(draftField[stackBase(src) + fineIndex(g)]);
      }
    }
    wfb[r] = pb;
    wbb[r] = bb;
  }
  workgroupBarrier();

  // ---- relax: red-black SOR, omega 3/2 ----
  let sy = u32(RG);
  let sz = u32(RG * RG);
  for (var it = 0; it < TUNE_DRAFT_FINE_SWEEPS; it++) {
    for (var color = 0u; color < 2u; color++) {
      for (var r = li; r < RG3; r += 256u) {
        let k = wkp[r];
        if ((k & FIXED_BIT) != 0u) { continue; }
        let ri = i32(r);
        let g = ro + vec3<i32>(ri % RG, (ri / RG) % RG, ri / (RG * RG));
        if ((u32(g.x + g.y + g.z) & 1u) != color) { continue; }
        let kpx = kGet(k, 0u);
        let kpy = kGet(k, 1u);
        let kpz = kGet(k, 2u);
        let kmx = kGet(wkp[r - 1u], 0u);
        let kmy = kGet(wkp[r - sy], 1u);
        let kmz = kGet(wkp[r - sz], 2u);
        let sum = kpx + kmx + kpy + kmy + kpz + kmz;
        if (sum > 0) {
          let ax = kpx * wfx[r + 1u] + kmx * wfx[r - 1u] + kpy * wfx[r + sy] + kmy * wfx[r - sy] +
                   kpz * wfx[r + sz] + kmz * wfx[r - sz] - (kpx - kmx) * Q_ONE;
          let az = kpx * wfz[r + 1u] + kmx * wfz[r - 1u] + kpy * wfz[r + sy] + kmy * wfz[r - sy] +
                   kpz * wfz[r + sz] + kmz * wfz[r - sz] - (kpz - kmz) * Q_ONE;
          wfx[r] += (3 * (ax / sum - wfx[r])) / 2;
          wfz[r] += (3 * (az / sum - wfz[r])) / 2;
          if (live) {
            // The b right-hand side: the y faces' flux of b at the face means
            // (the stream's (K+ - K-) Q_ONE with b for Q_ONE).
            let bp = (wbb[r] + wbb[r + sy]) / 2;
            let bm = (wbb[r - sy] + wbb[r]) / 2;
            let ab = kpx * wfb[r + 1u] + kmx * wfb[r - 1u] + kpy * wfb[r + sy] + kmy * wfb[r - sy] +
                     kpz * wfb[r + sz] + kmz * wfb[r - sz] - (kpy * bp - kmy * bm);
            wfb[r] += (3 * (ab / sum - wfb[r])) / 2;
          }
        } else {
          wfx[r] = 0;
          wfz[r] = 0;
          wfb[r] = 0;
        }
      }
      workgroupBarrier();
    }
  }

  // ---- store the tile's own 8^3 cells ----
  for (var i = li; i < 512u; i += 256u) {
    let ii = i32(i);
    let rc = vec3<i32>(ii % 8, (ii / 8) % 8, ii / 64) + vec3<i32>(2);
    let r = u32((rc.z * RG + rc.y) * RG + rc.x);
    let g = ro + rc;
    if (dst != 0u) {
      let o = dst + 2u * fineIndex(g);
      draftField[o] = bitcast<u32>(wfx[r]);
      draftField[o + 1u] = bitcast<u32>(wfz[r]);
      if (live) { draftField[stackBase(dst) + fineIndex(g)] = bitcast<u32>(wfb[r]); }
      continue;
    }
    var v = cellVel(r);
    // A CLOSED cell (a wall, a floor, a roof -- the slab rule above -- or a
    // pocket with no way out) takes the mean of its open neighbours. Up to
    // three voxels of real air share a cell with a thin slab, and smoke pooled
    // under a ceiling or a grain resting against a wall lives exactly there:
    // left at zero it would sit in dead air while the room drafts past it. A
    // voxel truly inside the wall never reads it (solids do not drift).
    if (v.open == 0) {
      var ax = vec3<i32>(0);
      var az = vec3<i32>(0);
      var n = 0;
      for (var a = 0u; a < 6u; a++) {
        let stp = select(1u, select(sy, sz, a >= 4u), a >= 2u);
        let rn = select(r - stp, r + stp, (a & 1u) == 0u);
        let vn = cellVel(rn);
        if (vn.open != 0) {
          ax += vn.rx;
          az += vn.rz;
          n++;
        }
      }
      if (n > 0) {
        v.rx = ax / n;
        v.rz = az / n;
      }
    }
    var rx = v.rx;
    var rz = v.rz;
    // The box's outer two cells blend to the identity, so its face is seamless
    // (and a building the face cuts through, whose interior sees a false
    // opening there, fades out instead of ending in a wall of wind).
    let dm = min(min(min(g.x, DRAFT_NX - 1 - g.x), min(g.y, DRAFT_NY - 1 - g.y)),
                 min(g.z, DRAFT_NZ - 1 - g.z));
    if (dm < 2) {
      let ex = vec3<i32>(Q_ONE, 0, 0);
      let ez = vec3<i32>(0, 0, Q_ONE);
      rx = ex + ((rx - ex) * dm) / 2;
      rz = ez + ((rz - ez) * dm) / 2;
    }
    let o = DRAFT_FIELD_BASE + 3u * fineIndex(g);
    draftField[o] = pack16(rx.x, rx.y);
    draftField[o + 1u] = pack16(rx.z, rz.x);
    draftField[o + 2u] = pack16(rz.y, rz.z);
    // THE STACK FIELD, by the transfer's rules: a closed cell takes its open
    // neighbours' mean, and the outer two cells fade it to nothing (outside
    // the box the reader adds no correction at all). Not written for a cold
    // snapshot: the live word below tells the readers not to look.
    if (live) {
      var s = cellStack(r);
      if (s.w == 0) {
        var acc = vec3<i32>(0);
        var n = 0;
        for (var a = 0u; a < 6u; a++) {
          let stp = select(1u, select(sy, sz, a >= 4u), a >= 2u);
          let rn = select(r - stp, r + stp, (a & 1u) == 0u);
          let sn = cellStack(rn);
          if (sn.w != 0) {
            acc += sn.xyz;
            n++;
          }
        }
        if (n > 0) { s = vec4<i32>(acc / n, 0); }
      }
      var sv = s.xyz;
      if (dm < 2) { sv = (sv * dm) / 2; }
      let so = DRAFT_STACK_FIELD + 2u * fineIndex(g);
      draftField[so] = pack16(sv.x, sv.y);
      draftField[so + 1u] = pack16(sv.z, 0);
    }
  }
}

@compute @workgroup_size(256)
fn fineFirst(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  finePass(wg.x, li, 0u, DRAFT_PHI_A);
}
@compute @workgroup_size(256)
fn fineMid(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  finePass(wg.x, li, DRAFT_PHI_A, DRAFT_PHI_B);
}
@compute @workgroup_size(256)
fn fineMid2(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  finePass(wg.x, li, DRAFT_PHI_B, DRAFT_PHI_A);
}
@compute @workgroup_size(256)
fn fineLast(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  finePass(wg.x, li, DRAFT_PHI_A, 0u);
  // Publish whether the stack field is live, with the field it describes.
  if (wg.x == 0u && li == 0u) {
    draftField[DRAFT_STACK_LIVE] = select(0u, 1u, stackLive());
  }
}
