// sim_mutate.wgsl — applies the CPU MutationQueue (brush paints/erases) to the
// grid. Runs before the CA passes each tick. Every world write flows through
// this path (DESIGN.md §2: the MutationQueue is also the future save/replay/
// network format).
//
// Dispatch: (4 * opsCount, 4, 4) workgroups of 4x4x4 threads — each op gets a
// 16^3 thread box centered on it (max brush radius 7).
//
// ---- TWO OPS ON ONE CELL IN ONE TICK: THE WINNER IS DEFINED ---------------
//
// Until docs/PLAN_multiplayer_now.md N3 it was not. Both entry points below
// were plain voxStores over one dispatch with no ordering between
// invocations, so a cell covered by two brush ops of the same tick took
// whichever write the driver happened to schedule last — the exact class of
// scheduling-dependent outcome CLAUDE.md rule 1 forbids, sitting on the one
// path every CPU mutation in the engine goes through.
//
// THE RULE, and it is `sim_explode.wgsl`'s rule word for word: the LOWEST op
// index that covers the cell AND would write it owns the cell. A thread for op
// i returns as soon as it finds a j < i that qualifies. Op index is push
// order, push order is deterministic (a fixed sequence in the CPU tick body),
// so the outcome is a pure function of the op list.
//
// "WOULD WRITE IT" is the full predicate, not just the sphere: op j's mode and
// its transmute from-filter are re-evaluated against the same occupant this
// thread read. A paint-into-air op that lands on stone does NOT get to shadow
// a later overwrite there, because it was never going to write anything.
// `opWouldWrite` below is that predicate and `main` must keep agreeing with
// it — the two are deliberately adjacent for that reason.
//
// WHAT THIS DOES NOT FIX, stated so nobody reads more into it than is there:
// each thread evaluates the predicate against ONE read of the occupant, and a
// lower op's store can still land between that read and this thread's
// decision. Closing that last window needs the two-phase mark/apply
// sim_explode uses (a per-op scratch buffer), which is a binding, a pass-table
// row and a buffer — a separate change. What is closed here is the common
// case: for overlaps where the ops disagree about a cell (paint vs melt, two
// materials), exactly one invocation now writes it.
//
// CELL OPS (the `cells` entry) dedupe on the CPU instead — an 8-byte op has no
// room for a predicate a shader could re-derive, and the choke point can sort.
// See sim/oprecord.h's CanonicalizeCells.

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn  : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(6) var<storage, read> ops : array<BrushOp>;

struct CellOp {
  cellIdx : u32,
  word    : u32,
};
@group(0) @binding(14) var<storage, read> cellOps : array<CellOp>;
// Per-chunk support-loss flags. The MutationQueue can take a supporting voxel
// out of the grid (a brush erase, a laser melt, an island removal) and solids
// never fall in the CA, so without this the matter above simply hangs there —
// island detection is the only thing that can drop it, and this flag is what
// summons island detection. Side channel: see common.wgsl's SUPPORT_LOSS block.
@group(0) @binding(15) var<storage, read_write> supportOut : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Every
// shader that declares `read_write> voxels` must define this: gPtKernel's
// initializer references it, so omitting it is a compile error rather than
// a fault that reports as "unknown".
const PT_KERNEL : u32 = PT_K_MUTATE;

fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, T.origin); }

// c's own chunk plus every chunk it borders (common.wgsl's dirtyFanSlot).
fn markBoth(c : vec3<i32>) {
  for (var k = 0u; k < 8u; k++) {
    let ci = dirtyFanSlot(c, T.origin, k);
    if (ci != SLOT_NONE) {
      atomicOr(&dirtyIn[ci], DIRTY_R_MUTATE);   // simulate this tick
      atomicOr(&dirtyOut[ci], DIRTY_R_MUTATE);  // and re-check next tick
    }
  }
}

// Would op `o` write world cell `c`, given the occupant material `prevMat`?
// EXACTLY the gate sequence `main` applies to its own op, restated as a
// predicate so the overlap dedupe can ask it about an EARLIER op. Any change
// to main's gates has to land here in the same edit or the two disagree and
// the dedupe starts shadowing writes that would never have happened.
fn opWouldWrite(o : BrushOp, c : vec3<i32>, prevMat : u32) -> bool {
  let d = c - vec3<i32>(o.cx, o.cy, o.cz);
  if (dot(d, d) > o.radius * o.radius) { return false; }
  if (o.mode == 0u) { return prevMat == MAT_AIR; }  // paint fills air only
  // the spell transmute's from-filter (see the note in main)
  if (o._p0 != 0u && prevMat != o._p0) { return false; }
  if ((o._p1 & 1u) != 0u && prevMat == MAT_AIR) { return false; }
  if (o.mode == 2u) {  // melt: air and 255-hardness matter are immune
    return prevMat != MAT_AIR && materials[prevMat].hardness < 255u;
  }
  return true;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_id) lid : vec3<u32>) {
  let opIdx = wg.x / 4u;
  if (opIdx >= T.opsCount) { return; }
  let op = ops[opIdx];

  let local = vec3<i32>(vec3<u32>((wg.x % 4u), wg.y, wg.z) * 4u + lid) - vec3<i32>(8, 8, 8);
  if (dot(local, local) > op.radius * op.radius) { return; }
  let c = vec3<i32>(op.cx, op.cy, op.cz) + local;
  if (!inBounds(c)) { return; }

  // TWO BASES (§4.1): slotIdx keys the palette-variant RNG, idx addresses
  // memory. See the note in sim_step:main.
  let slotIdx = cellIndexW(c);
  // Read the OCCUPANT ONCE, ahead of every branch that wants it: the paint
  // mode's air test, the melt mode's source material, and the support-loss
  // flag at the bottom, which needs what was here BEFORE the store. Index and
  // word come from one page-table resolution.
  let iw = voxIndexAndWord(c);
  let idx = iw.x;
  let prevMat = voxMat(iw.y);
  // OVERLAP DEDUPE (see the header): the lowest op index that covers this cell
  // and would write it owns it. One read of the occupant feeds every decision
  // in this thread, so the answer is a function of the op list and that read.
  // opsCount <= 64 and the loop only runs for ops after the first, so a tick
  // with one brush op pays a single compare.
  for (var j = 0u; j < opIdx; j++) {
    if (opWouldWrite(ops[j], c, prevMat)) { return; }
  }
  if (op.mode == 0u && prevMat != MAT_AIR) { return; }  // paint fills air only
  // A spell's transmute (game/spell.cpp Convert) is an overwrite with a FROM
  // filter: _p0 names the only material it may replace (0 = any), and bit 0 of
  // _p1 says the wildcard matches matter, not the void. Both are zero for every
  // other producer, so the brush and the laser are unchanged.
  if (op.mode != 0u && op._p0 != 0u && prevMat != op._p0) { return; }
  if (op.mode != 0u && (op._p1 & 1u) != 0u && prevMat == MAT_AIR) { return; }

  var mat = op.material;
  if (op.mode == 2u) {
    // melt (laser, PLAN §C1): each cell converts to ITS OWN molten product
    // from the material table — stone becomes lava while the sand next to it
    // becomes molten glass. Air stays air, 255-hardness matter is immune.
    if (prevMat == MAT_AIR || materials[prevMat].hardness >= 255u) { return; }
    mat = materials[prevMat].molten;
  }

  let rnd = hash3(T.seed ^ 0x5EEDu, T.tick, slotIdx);
  // liquids are born full (their state nibble is fullness); everything else
  // gets a palette variant. STAMP_NEVER = "hasn't acted": falls this tick.
  var state = rnd % 3u;
  if (mat != MAT_AIR && materials[mat].klass == CLASS_LIQUID) {
    state = LIQ_FULL_STATE;
  }
  // A powder brush with a GRAIN size (world.h BrushOp.pad1 bits 4..7, 1..7
  // eighths; 0 = whole cells) paints partial cells: a fine brush lays a
  // dusting instead of a block (common.wgsl POWDER MASS).
  let grain = (op._p1 >> 4u) & 0xFu;
  if (grain >= 1u && grain < POWDER_FULL && mat != MAT_AIR &&
      matHasPowderMass(materials[mat])) {
    state = grain + 2u;
  }
  voxStore(idx, packVox(mat, state, STAMP_NEVER));
  markBoth(c);
  // An erase (mode 1 writing air) or a melt (solid -> lava) can be the thing a
  // ledge was standing on. Air-into-air and paint-into-air cost one compare:
  // prevMat is MAT_AIR, whose class is neither SOLID nor POWDER, so the call
  // returns on its first line.
  flagSupportLoss(c, materials[prevMat].klass, mat);
}

// Exact-cell writes (island removal / rubble handoff, DESIGN.md §7). Same
// MutationQueue discipline as the brush: the op stream is the only CPU->grid
// path, so saves/replays/networking capture island events for free.
// Dispatch: ceil(cellCount / 64) workgroups of 64.
// The scoop ledger's words in the page-fault record (world.h
// kPageFaultScoop*). Declared here, next to their only writer, not in
// common.wgsl: a constant one shader reads belongs to that shader.
const SCOOP_EIGHTHS_WORD : u32 = 36u;
const SCOOP_APPLIED_WORD : u32 = 37u;
const SCOOP_REFUSED_WORD : u32 = 38u;

@compute @workgroup_size(64)
fn cells(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.cellCount) { return; }
  let op = cellOps[gid.x];
  if (op.cellIdx >= WORLD_N * WORLD_N * WORLD_N) { return; }
  var word = op.word;
  // cellIdx is a SLOT index, so under paging it is NOT a physical word index
  // (§5.5). Decompose it — which this entry point already did below, to
  // reconstruct the world cell for markBoth — and translate through the table.
  // The same decomposition the CPU uses to build the materialization set is
  // the one the shader uses to index the table, which is the point.
  let ci = op.cellIdx / CHUNK_VOL;
  let lo = op.cellIdx % CHUNK_VOL;
  // Index and occupant from ONE table resolution (they used to be two).
  let iw = voxIndexAndWordInChunk(ci, lo);
  let wordIdx = iw.x;
  // prefab paint mode: fill air only (flag is spare-bit metadata, never stored)
  //
  // Reading a sentinel HERE is legal and correct: a paint-into-air op against
  // an EMPTY chunk should see air and proceed. That it can proceed is the
  // CPU's obligation — §3.3 materializes every op target unfiltered, precisely
  // so a brush into open sky is not silently a no-op.
  // Hoisted out of the fill-air-only branch below, because the support flag
  // needs it on EVERY path: an exact-cell op is how island removal and the
  // rubble handoff take matter out of the grid, and those are precisely the
  // writes that can leave something above them unsupported.
  let prevMat = voxMat(iw.y);
  if ((word & CELLOP_IF_AIR) != 0u) {
    if ((word & 0xFFFu) == MAT_AIR) {
      // CONDITIONAL CLEAR (world.h CellOpClearIfMat): the flag on an AIR word
      // means "empty this cell only if it still holds the material in bits
      // 12..23". A vessel's scoop is decided off last tick's snapshot and
      // liquid moves, so whatever flowed in since is left alone.
      if (prevMat != ((word >> 12u) & 0xFFFu)) {
        atomicAdd(&pageFaults[SCOOP_REFUSED_WORD], 1u);
        return;
      }
      // THE LEDGER: what this clear really took, in eighths -- a liquid's
      // fullness as it is NOW, not as the (older) snapshot the CPU scooped
      // from said it was. Summed with atomicAdd, which is order-independent,
      // so the total is a pure function of the tick's ops. The vessel is
      // credited from this number, never from its own guess.
      let prevWord = voxWordInChunkAt(ci, lo);
      var units = 8u;
      if (materials[prevMat].klass == CLASS_LIQUID) { units = ((prevWord >> 12u) & 7u) + 1u; }
      // A powder is paid its MASS (common.wgsl POWDER MASS); container.cpp
      // ContainerCellUnits is the CPU twin and must agree.
      if (matHasPowderMass(materials[prevMat])) { units = powderMass(prevWord); }
      atomicAdd(&pageFaults[SCOOP_EIGHTHS_WORD], units);
      atomicAdd(&pageFaults[SCOOP_APPLIED_WORD], 1u);
      word = 0u;
    } else {
      if (prevMat != MAT_AIR) { return; }
      word &= ~CELLOP_IF_AIR;
    }
  }
  voxStore(wordIdx, word);

  let l = vec3<i32>(vec3<u32>(lo % CHUNK, (lo / CHUNK) % CHUNK, lo / (CHUNK * CHUNK)));
  let wc = slotWorldChunk(ci, T.origin) * i32(CHUNK) + l;
  markBoth(wc);
  // Island removal and the rubble handoff write air here; settle-back writes a
  // SOLID, which flagSupportLoss rejects on its second line (a solid still
  // supports). Body-burn escapes are CELLOP_IF_AIR ops that only land on air,
  // so prevMat is MAT_AIR and they never reach the neighbour walk — which is
  // what keeps DESIGN.md §7's "burn ops must not starve island detection"
  // property intact without a special case here.
  flagSupportLoss(wc, materials[prevMat].klass, voxMat(word));
}

// ---- WIND PRIMITIVE FOOTPRINT WAKE (docs/RESEARCH_wind.md §4.3, §10) -------
//
// The one thing in the engine that dirty-marks a chunk WITHOUT writing a voxel,
// and the reason phase 2 had to land before entrainment could be switched on.
//
// A settled sand dune is asleep. Its chunk carries no dirty flag, it is not in
// the compacted dispatch list, and no CA invocation ever visits it — so a fan
// pointed at it would do nothing at all, however hard it blew. Something has to
// wake the footprint, and the ambient field is categorically not allowed to
// (invariant 3: an exposed dune under a steady breeze would re-mark its own
// chunks for as long as the weather held, which is rule 2 with the sign
// flipped).
//
// A PRIMITIVE can, because it is bounded and player-caused. The CPU resolves
// the footprint (WindPrimSystem::BuildWake), filters it against the snapshot's
// occupancy so a cube of sky costs nothing, charges it against a per-tick chunk
// budget, and ships the surviving SLOT indices in TickParams. This kernel is
// the last step: set the flag.
//
// WHY THAT ORDER IS THE WHOLE POINT. The same CPU pass that fills this list
// also declares those chunks to the page table as op targets, so they are
// materialized WITH THEIR 26-RING before the command buffer is built. The page
// table's materialization set is tightened against a lagging snapshot on the
// argument that settled matter writes nothing — entrainment breaks that
// argument, and this is what repairs it: by the time a grain steps into a
// neighbouring chunk, the CPU had already said that chunk could be written.
// Without it the write lands on a sentinel and the voxel is simply lost (62
// reproducible faults over two 160-tick runs; §10).
//
// Both flags, exactly as markBoth sets them: dirtyIn so the chunk simulates
// THIS tick (the compaction runs after this kernel), dirtyOut so it is
// re-checked next tick even if nothing moved. Idempotent stores, never atomic
// arithmetic, so the order two invocations land in cannot matter.
//
// Dispatch: ceil(windWakeCount / 64) workgroups of 64. Zero primitives with the
// entrainment licence means zero count means the row is not recorded at all.
@compute @workgroup_size(64)
fn windWake(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.windWakeCount) { return; }
  // The list is four slots to a std140 row (world.h TickParams).
  let slot = T.windWake[gid.x / 4u][gid.x % 4u];
  if (slot >= NUM_SLOTS) { return; }
  atomicOr(&dirtyIn[slot], DIRTY_R_MUTATE);
  atomicOr(&dirtyOut[slot], DIRTY_R_MUTATE);
}

// ---- RAIN ON THE GROUND (2026-09-25) ----------------------------------------
//
// Rain does not delete stains. It STAINS THE GROUND WITH WATER, through the
// same common.wgsl stainStep a pouring liquid uses -- and the newest stain wins,
// so a drop landing on blood-soaked ground replaces the blood with wet, and the
// wet then dries (a stain type's `dries`). The rain material is data: the one
// that authors `"stain": {"rain": true}` (water), published in stain palette
// entry 0's `_r3` (materials.h kStainPal*).
//
// THE COST IS FIXED, NOT THE WORLD'S (rule 2). A storm falls on every surface
// of the window, and a kernel that woke every surface chunk for it would keep
// the whole visible world awake for as long as it rained. So this is a SAMPLER:
// one thread per RAIN_TILE x RAIN_TILE tile of columns, each tick landing on
// one column of its tile (a hash of seed, tick and tile), walking down past
// sky -- a whole chunk at a time over an EMPTY sentinel -- to the first thing
// that is not air or gas, and acting on that one cell:
//   * while it rains (T.weatherRain's amount, as that chance out of 255): wet
//     it. A cell that was dry or wore another stain dirty-marks its chunk --
//     the chunk has changed what it IS (modified tracking; its coat rules must
//     see the water) -- but re-wetting what is already wet marks nothing, so a
//     steady storm on a wet meadow keeps nothing awake;
//   * while it does not: a wet top surface loses a level, at its type's `dries`
//     scaled up by the tile area (a column is visited once per RAIN_TILE^2
//     ticks on average), and never while the liquid that made it touches it.
//     This is why sim_step's stainDry does not hold a chunk awake for a cell
//     open above: the top surfaces dry here, asleep.
// A liquid on top (a pond) takes the rain and nothing is stained; a solid
// SENTINEL on top (a uniform chunk whose top face is the surface) has no page
// to write, so the drop is dropped rather than faulted.
//
// DETERMINISM (rule 1). Every tile is one thread and owns its columns, so no
// two threads write one cell; each writes only the STAIN bits of its cell, and
// what it reads of a neighbour is its material, which nothing here changes.
// The column and the rolls are hash3 of (seed, tick, tile); the rain word is
// TickParams, which ops-replay records.
const RAIN_TILE : u32 = 8u;            // pass_table.cpp kRainTile
const RAIN_TILES : u32 = WORLD_N / RAIN_TILE;
const RAIN_SALT : u32 = 0x5A1D0F00u;

@compute @workgroup_size(64)
fn rainFall(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= RAIN_TILES * RAIN_TILES) { return; }
  let h = hash3(T.seed ^ RAIN_SALT, T.tick, gid.x);
  let base = T.origin * i32(CHUNK);
  let x = base.x + i32((gid.x % RAIN_TILES) * RAIN_TILE + (h % RAIN_TILE));
  let z = base.z + i32((gid.x / RAIN_TILES) * RAIN_TILE + ((h / RAIN_TILE) % RAIN_TILE));
  let rain = T.weatherRain & RAIN_AMOUNT_MASK;
  let roll = hash3(h, 0x7A11u, T.tick);
  // A dry sky dries; the drying chance is read off the cell's stain below.
  // Rolled FIRST while it rains, so a drizzle's idle threads skip the walk.
  if (rain != 0u && (roll % 255u) >= rain) { return; }

  // Down the column to the first surface.
  var y = base.y + i32(WORLD_N) - 1;
  var idx = PT_NO_WORD;
  var w = 0u;
  loop {
    if (y < base.y) { return; }
    let e = pageEntryOf(voxSlotOfCell(vec3<i32>(x, y, z)));
    if ((e & PT_SENTINEL_BIT) != 0u) {
      let sm = e & PT_MAT_MASK;
      if (sm != MAT_AIR && materials[sm].klass != CLASS_GAS) { return; }
      y = ((y >> CHUNK_SHIFT) << CHUNK_SHIFT) - 1;  // the whole chunk is sky
      continue;
    }
    let lo = vec3<u32>(vec3<i32>(x, y, z) & vec3<i32>(CHUNK_MASK));
    let i = e * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x;
    let cw = voxels[i];
    let cm = voxMat(cw);
    if (cm == MAT_AIR || materials[cm].klass == CLASS_GAS) { y -= 1; continue; }
    idx = i;
    w = cw;
    break;
  }
  let c = vec3<i32>(x, y, z);
  let m = materials[voxMat(w)];
  if (m.klass != CLASS_SOLID && m.klass != CLASS_POWDER) { return; }

  if (rain != 0u) {
    let rm = materials[STAIN_PALETTE_BASE]._r3 & 0xFFFu;
    if (rm == 0u) { return; }
    let r = materials[rm];
    let d = stainStep(matStainType(r), matStainAmount(r), matWashes(r), w, m);
    if ((d.y & STAIN_WORK) == 0u) { return; }
    voxStore(idx, d.x);
    if (!voxStained(w) || voxStainType(w) != matStainType(r)) {
      let own = dirtyFanSlot(c, T.origin, 0u);
      if (own != SLOT_NONE) { atomicOr(&dirtyOut[own], DIRTY_R_STAINW); }
    }
    return;
  }

  if (!voxStained(w)) { return; }
  let pal = materials[STAIN_PALETTE_BASE + voxStainType(w)]._r3;
  let chance = min(stainDryChance(pal) * RAIN_TILE * RAIN_TILE, 1000u);
  if (chance == 0u || (roll % 1000u) >= chance) { return; }
  let wetter = pal & 0xFFFu;
  for (var k = 0u; k < 6u; k++) {
    let n = c + faceDir(k);
    if (inBounds(n) && voxMat(voxWordAt(n)) == wetter) { return; }
  }
  let left = voxStainAmt(w) - 1u;
  var nw = w & ~STAIN_BITS;
  if (left > 0u) { nw = nw | packStain(voxStainType(w), left); }
  voxStore(idx, nw);
}
