// sim_stream.wgsl — STREAMS BECOME HELD (docs/PLAN_running_water.md phase 2;
// DESIGN.md §5b.13).
//
// THE PROBLEM. Phase 1's held rule lets a map river, lake or sea rest however
// its surface falls, and makes it an infinite reservoir: a bank somebody digs
// through spills, and the river refills what it spilled at once. If the
// breach runs into a hollow, the hollow fills to the river's level and the
// whole thing sleeps on its own. If it runs DOWNHILL -- to the sea, to the
// river further down, to another lake -- it is a stream fed and drained
// forever: the conveyor phase 1 removed from the authored rivers, rebuilt by
// the player, every chunk along it awake for as long as it runs (rule 2).
//
// THE ANSWER is the authored rivers' own: a stream that has stopped changing
// is HELD where it is. A second record per window column, beside the authored
// one in the still table (worldgen.wgsl's STILL-WATER TABLE, words [2] and
// [3]), carries a DYNAMIC target that sim_step.wgsl's stillLevel8 reads when
// the column has no authored one. Once a column holds a dynamic target the
// held rule, the reservoir and the renderer treat it exactly like a river:
// at rest it pours nothing into a full held neighbour, under its target it is
// refilled, and its chunk sleeps. A stream is adopted column by column from
// its source down, and the last held column of it meets whatever it drains
// into (held: at rest; not held: it still spills, so the front keeps moving
// until it reaches something).
//
// ADOPTION, per column, by the two kernels below on every tick the CA ran:
//   * the column's SURFACE is its highest free-surface liquid cell (liquid,
//     non-liquid above) under the column's topmost awake chunk; s8 = 8 y + f;
//   * it is STEADY when its surface has stayed within one voxel (8 eighths)
//     of where it stood when the count began, for STREAM_ADOPT_TICKS visits
//     (a jump restarts the count from the new surface; a tick with no surface
//     halves it: a thin stream flickers). A trench still FILLING rises out of
//     the band and is not adopted part way up: it fills, rests at its
//     feeder's level and sleeps as plain water. (Adoption does not disturb
//     the column below it: a held cell under its target is refilled before
//     it moves -- sim_step.wgsl stage 0 -- so an adopted column hands on what
//     it did before);
//   * it is FED when a lateral neighbour holds a target T >= s8 -- the map's
//     own water (hop 0) or an adopted column (hop h) -- whose water stands at
//     its surface where it can step into this column (its top cell is that
//     liquid; this column's cell at that row is not a wall); the column takes
//     hop = the smallest such h + 1, so hops strictly fall toward the map's
//     water and support can never run in a circle;
//   * it has OUTFLOW: somewhere lower to run beside its surface cell, or a
//     held neighbour (the map's water or an adopted column) whose target is
//     at or under its surface -- the body it drains into (a pool with no way
//     out has neither, and sleeps as plain water);
//   * steady, fed and running: adopted at its current surface.
// RELEASE: an adopted column that is visited (something woke its chunk) and is
// no longer fed at its own target by a neighbour of SMALLER hop is released -- a plugged breach drains its
// stream from the top down: the released water is ordinary water, it flows
// into the next held column as water over its target, that wakes it, and it
// finds its own support gone.
// FLOODED (2026-10-16 audit): an adopted column that has stood OVER its
// target long enough is released too -- a leaky count, +1 a visit its water
// stands over the target, +STREAM_FLOOD_DEEP a visit it stands more than a
// voxel over, -1 a visit it does not, released at STREAM_FLOOD_COUNT. The
// steady test cannot tell a stream from a basin filling slowly (a big hollow
// a breach fills at under a voxel per STREAM_ADOPT_TICKS), so a column of
// such a basin is adopted part way up; the basin then rises past that target,
// and its water, fed for ever through the breach, ran into the column and
// back to the reservoir (GIVE) for ever -- a source and a sink with a chunk
// awake between them (rule 2). Released, the basin rises to its feeder's
// level and sleeps (or is adopted there, at the level it actually holds).
// WHY SO SLOW, measured: at a count of 120 the `rivers` S stream flapped --
// 22, 45, 27, 15, 69, 12, 112 ... of 153 columns adopted, instead of 6, 28,
// 46 ... 153 -- because a column of a stream that is STILL BEING ADOPTED
// stands over its target for as long as the unadopted water upstream of it
// still runs into it (GIVE drains it, the running water refills it), which
// is the source-and-sink pattern exactly, only transient; and one release
// cascades down the whole stream. The gate's reporter (S's floodMax) shows
// it: the largest count among the adopted columns climbs ONE A VISIT from
// tick 150, when the first six are adopted (the columns at the river's
// level, which the river feeds and the stream drains into), to 912 at tick
// 1050, when the last column adopts and the stream stops running, then
// falls. A stream of a thousand columns would take the mouth past 4000 and
// release it -- harmless: nothing upstream rests on the mouth's support, so
// it releases alone and is adopted again. So a shallow flood takes
// STREAM_FLOOD_COUNT visits (about a minute) and only a deep one -- a voxel
// or more over its target, which running water arriving at reach 1 did not
// do in S (the count never stepped by STREAM_FLOOD_DEEP) -- is fast. Either
// way the source and the sink are BOUNDED, which they were not.
//
// RULE 1. ONE WRITER PER COLUMN: only the topmost dirty chunk of a column's
// window stack acts for it (the stack above it is asleep by construction).
// Kernel `scan` reads neighbours' COMMITTED targets (word [2]) and writes only
// its own column's word [3] (observation, age, and a pending action); kernel
// `commit`, after a barrier, applies its own column's action to word [2]. The
// ONE word a scan thread reads that another may be writing is an adopted
// neighbour's [3], for its liquid -- and scan never changes an adopted [3]'s
// low 12 bits (only the flood count above them and the action), so whichever
// value the read sees, the liquid is the same. Every decision is a function
// of the voxels after the CA and the committed table -- the order threads run
// in cannot matter.
// RULE 2. A column is visited only while its chunk is dirty; adoption only
// ever ADDS held columns, release only removes ones whose support is gone or
// that a basin has drowned, and nothing here writes a voxel, so this pass
// cannot keep anything awake (commit's one wake per release, phase 3, happens
// once per adoption).
// Derived, not saved: a reloaded stream runs as water and is adopted again.
//
// ============ THE CONDUIT: A STREAM FROM A FINITE SOURCE (phase 3) ===========
// docs/PLAN_running_water.md phase 3; tickets 2026-10-16_11-16-43 / 11-14-41.
// Everything above roots a stream on the map's water, whose reservoir is
// infinite. A stream whose source is FINITE -- a player's pour, a pool, a
// broken tank -- has nothing held to root on, so every chunk along it stayed
// awake until the source was empty. Now such a stream is adopted too, as a
// POOL stream ([3] bit STREAM_POOL_BIT), and its accounting is a LEDGER
// instead of an infinite reservoir:
//   * ROOT: an unadopted column whose held feeders fail may be FED by an
//     UNHELD lateral neighbour of the same liquid -- the live source -- whose
//     surface stands at least STREAM_POUR eighths over this column's, that can
//     actually pour (sourcePours: a cell able to split, or water under its
//     top cell, or the same row), with an open way in, AND that is itself a
//     ROOT: no lateral neighbour of IT stands STREAM_POUR over it (isRoot).
//     Every column of a running stream is fed by the unheld column upstream
//     of it; only the one at the top of the slope (the pool's edge, whose
//     neighbours are the pool at its own level) is fed by a column nothing
//     higher feeds. So POOL adoption starts at the source and walks down the
//     hop chain exactly as a map stream does (hop 1 at the root, h + 1 after).
//     A column adopted from a POOL feeder is POOL (ties of hop go to the
//     map's: an infinite feeder is the better account).
//   * RELEASE, with HYSTERESIS: an adopted POOL column with no held feeder of
//     smaller hop stays held while ANY unheld neighbour still pours into it
//     (sourcePours, no root test). That is what lets the stream RETREAT from
//     the top as the source runs down instead of cascading: a released head's
//     water is unheld and still pours into the column below, which is now the
//     head; when the leftover film can no longer pour, that column goes too.
//   * THE LEDGER (sim_step.wgsl THE CONDUIT): what the source pours into a
//     POOL column over its target is DEPOSITED, what the mouth spills out is
//     WITHDRAWN to refill it, and a withdrawal needs the ledger as of the
//     start of the tick (the SNAPSHOT this kernel's `commit` copies) to be
//     positive. Mass is moved, never made: voxels + ledger is exact.
//   * STARVED: an adopted POOL column under its target while the snapshot is
//     empty counts on the flood count (+1 a visit), so a mouth waiting on a
//     source that has stopped pouring is released in bounded time.
// Rule 1: the snapshot is written by ONE thread (commit's workgroup 0) after
// the barrier that ends the CA's adds, and read by the next tick's CA and
// scan; nothing reads the live word in the pass that adds to it.
// Dense worldgen clears the whole table (pass_table.def fill_stillWater); a
// LOAD clears the dynamic words (`clearStreams`, pass_table.def
// lr_streamClear -- the load refills the window through the streamed path,
// and a window loaded where the old one stood has every tag unchanged); a
// streamed window column keeps its dynamic words only while its tag (the
// world column) is unchanged (worldgen.wgsl cols), which a vertical window
// shift, regenerating every column, must not cost the streams.

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read>       dirtyIn  : array<u32>;
// `commit` wakes a released column's chunk (WAKE ON RELEASE, phase 3).
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
@group(0) @binding(58) var<storage, read_write> stillWater : array<u32>;
// This pass stores no voxel; the identity only satisfies common.wgsl.
const PT_KERNEL : u32 = PT_K_STEP;

// worldgen.wgsl's STILL_WORDS / stillIndex / stillTag (and sim_step.wgsl's).
const STILL_WORDS : u32 = 4u;
fn stillIndex(x : i32, z : i32) -> u32 {
  return (bitcast<u32>(z & WORLD_MASK) * WORLD_N + bitcast<u32>(x & WORLD_MASK)) * STILL_WORDS;
}
fn stillTag(x : i32, z : i32) -> u32 {
  let tx = bitcast<u32>(x >> firstTrailingBit(WORLD_N)) & 0x7FFFu;
  let tz = bitcast<u32>(z >> firstTrailingBit(WORLD_N)) & 0x7FFFu;
  return 1u | (tx << 1u) | (tz << 16u);
}

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

// How long a surface must hold still before it is held, in ticks.
const STREAM_ADOPT_TICKS : u32 = 120u;
// How far down from the top of its chunk a column's surface is looked for.
const STREAM_SCAN : i32 = 32;
// THE TWO DYNAMIC WORDS.
//   [2] adopted: the target T8 (20 bits, eighths, signed) | HOP << 20 (12
//       bits, 1..STREAM_HOP_MAX): how many columns of stream lie between this
//       one and the map's water that feeds it. 0 = not adopted.
//   [3] not adopted: the observed surface (20 bits) | age << 20 (10 bits) |
//       the pending action << 30. Adopted: the liquid's material (12 bits) |
//       the flood count << 12 (12 bits) | the pending action << 30 --
//       sim_step.wgsl stillLevel8 reads the material from here (low 12 bits
//       only), so an adopted record never claims another liquid.
// A pending ADOPT carries the hop in the age bits (the commit writes [2]).
const STREAM_AGE_MAX : u32 = 1023u;
const STREAM_HOP_MAX : u32 = 1023u;
const STREAM_ACT_ADOPT : u32 = 1u;
const STREAM_ACT_RELEASE : u32 = 2u;
const STREAM_NONE : i32 = -1073741824;
// THE FLOOD COUNT (see FLOODED above), [3] bits 12..23 of an adopted column.
const STREAM_OVER_SHIFT : u32 = 12u;
const STREAM_OVER_MAX : u32 = 4095u;
const STREAM_FLOOD_COUNT : u32 = 4000u;
const STREAM_FLOOD_DEEP : u32 = 16u;
// THE CONDUIT (phase 3). An adopted [3]'s bit 24 says the stream is rooted on
// a FINITE source (sim_step.wgsl reads it: STILL_POOL_BIT there). A pending
// adopt of a POOL column is action 3 (the age bits carry the hop, as for 1).
const STREAM_POOL_BIT : u32 = 1u << 24u;
const STREAM_ACT_ADOPT_POOL : u32 = 3u;
// "Pours": a source must stand this many eighths over a column's surface --
// the CA's own equalize threshold (TUNE_LIQUID_EQUALIZE is 2): less than that
// and no lateral move exists between them.
const STREAM_POUR : i32 = 2;
// sim_step.wgsl's LIQ_SPLIT_MIN: a cell this full can split into air.
const STREAM_SPLIT_MIN : u32 = 2u * clamp(TUNE_LIQUID_MIN_FILM, 1u, 4u);
// How far up a neighbour column is searched for its surface, in cells.
const STREAM_UP : i32 = 8;
// THE LEDGER: two words after the column records (sim_step.wgsl STILL_LEDGER
// must agree): [0] the live balance in eighths (i32, atomic adds in the CA),
// [1] its snapshot at the end of the last tick that ran (written by `commit`),
// [2] / [3] the eighths deposited / withdrawn so far (diagnostic only).
const STREAM_LEDGER : u32 = WORLD_N * WORLD_N * STILL_WORDS;
// sim_step.wgsl STILL_LEDGER_CAP: the ledger is FULL at this (THE CONDUIT).
const STREAM_LEDGER_CAP : i32 = 512;

fn sdec(v : u32) -> i32 { return bitcast<i32>(v << 12u) >> 12u; }
fn ledgerSnap() -> i32 { return bitcast<i32>(stillWater[STREAM_LEDGER + 1u]); }

struct Surf {
  ok   : bool,
  y    : i32,
  s8   : i32,
  mat  : u32,
};

// The column's surface under this chunk's top: the first liquid cell with a
// non-liquid cell above it, walking down at most STREAM_SCAN cells. `top` is
// the chunk's top cell y; a liquid cell ABOVE it means the column's surface is
// in a sleeping chunk above, which is not this pass's business.
fn columnSurface(x : i32, top : i32, z : i32) -> Surf {
  var s : Surf;
  s.ok = false;
  let above = vec3<i32>(x, top + 1, z);
  if (inBounds(above)) {
    let am = voxMat(voxWordAt(above));
    if (am != MAT_AIR && materials[am].klass == CLASS_LIQUID) { return s; }
  }
  for (var k = 0; k < STREAM_SCAN; k++) {
    let c = vec3<i32>(x, top - k, z);
    if (!inBounds(c)) { break; }
    let w = voxWordAt(c);
    let m = voxMat(w);
    if (m != MAT_AIR && materials[m].klass == CLASS_LIQUID) {
      s.ok = true;
      s.y = c.y;
      s.s8 = 8 * c.y + i32(voxState(w) + 1u);
      s.mat = m;
      return s;
    }
    // A wall over the column ends the look: whatever is under it is a cave.
    if (m != MAT_AIR && materials[m].klass != CLASS_GAS) { break; }
  }
  return s;
}

// The target a neighbour column holds for liquid `mat` and its hop (x = T8 or
// STREAM_NONE, y = hop: 0 for the map's own water, z = 1 for a POOL stream).
// The POOL bit of an adopted neighbour's [3] is constant for the whole scan
// (a pending release keeps it), so reading it while that neighbour's thread
// rewrites the word is as safe as reading its liquid.
fn neighbourTarget(x : i32, z : i32, mat : u32) -> vec3<i32> {
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z)) { return vec3<i32>(STREAM_NONE, 0, 0); }
  let a = stillWater[si + 1u];
  if (a != 0u) {
    if ((a >> 20u) == mat) { return vec3<i32>(sdec(a), 0, 0); }
    return vec3<i32>(STREAM_NONE, 0, 0);
  }
  let d = stillWater[si + 2u];
  let w3 = stillWater[si + 3u];
  if (d != 0u && (w3 & 0xFFFu) == mat) {
    return vec3<i32>(sdec(d), i32(d >> 20u), select(0, 1, (w3 & STREAM_POOL_BIT) != 0u));
  }
  return vec3<i32>(STREAM_NONE, 0, 0);
}

// A lateral neighbour column's SURFACE for liquid `mat`, searched from row
// `y` up at most STREAM_UP cells: the top cell of the first run of `mat`
// found (x = s8 = 8 y + f, or STREAM_NONE), y = that cell's row, z = 1 when
// it can POUR -- it can split into air (f >= STREAM_SPLIT_MIN) or has more of
// its liquid under it, so it is not a stranded one-eighth film. A wall above
// the start row ends the search (a cave over a ledge is not a source).
// Water deeper than the search (still `mat` at its last row) is a source
// standing at least that high, and pours.
fn sideSurface(x : i32, y : i32, z : i32, mat : u32) -> vec3<i32> {
  var top = -1;   // offset of the last `mat` cell of the run, -1 = none yet
  for (var k = 0; k < STREAM_UP; k++) {
    let c = vec3<i32>(x, y + k, z);
    if (!inBounds(c)) { break; }
    let m = voxMat(voxWordAt(c));
    if (m == mat) { top = k; continue; }
    if (top >= 0) { break; }
    if (m != MAT_AIR && materials[m].klass != CLASS_GAS) { return vec3<i32>(STREAM_NONE, 0, 0); }
  }
  if (top < 0) { return vec3<i32>(STREAM_NONE, 0, 0); }
  let t = vec3<i32>(x, y + top, z);
  let f = voxState(voxWordAt(t)) + 1u;
  var pours = f >= STREAM_SPLIT_MIN || top > 0;
  let b = t - vec3<i32>(0, 1, 0);
  if (!pours && inBounds(b) && voxMat(voxWordAt(b)) == mat) { pours = true; }
  return vec3<i32>(8 * t.y + i32(f), t.y, select(0, 1, pours));
}

// A LIVE SOURCE for a column whose surface is (or whose target is) `t8`:
// an UNHELD lateral neighbour (no target for `mat`) whose water stands
// STREAM_POUR or more over it and can pour, with an open way in (this
// column's cell at the source's top row is not a wall). `rootOnly` also asks
// that nothing stands STREAM_POUR over the SOURCE beside it (isRoot): the
// adoption test, which makes POOL adoption start at the top of the slope.
// Release asks without it (the hysteresis in the header).
fn liveSource(x : i32, y : i32, z : i32, t8 : i32, mat : u32, rootOnly : bool) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    if (neighbourTarget(nx, nz, mat).x != STREAM_NONE) { continue; }
    let s = sideSurface(nx, y, nz, mat);
    if (s.x == STREAM_NONE || s.x < t8 + STREAM_POUR || s.z == 0) { continue; }
    let mc = vec3<i32>(x, s.y, z);
    if (!inBounds(mc)) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    if (rootOnly && !isRoot(nx, s.y, nz, s.x, mat)) { continue; }
    return true;
  }
  return false;
}

// ROOT: no lateral neighbour of the source column (nx, nz), whose surface is
// sN at row ys, has `mat` standing STREAM_POUR or more over sN -- tested on
// the two rows that can hold such water next to a top cell (ys and ys + 1;
// water higher than that has water at ys + 1 too).
fn isRoot(nx : i32, ys : i32, nz : i32, sN : i32, mat : u32) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    for (var k = 0; k < 2; k++) {
      let c = vec3<i32>(nx + d.x, ys + k, nz + d.y);
      if (!inBounds(c)) { continue; }
      let w = voxWordAt(c);
      if (voxMat(w) == mat && 8 * c.y + i32(voxState(w) + 1u) >= sN + STREAM_POUR) { return false; }
    }
  }
  return true;
}

// SUPPORT: the smallest hop among the lateral neighbours that hold a target
// at or over `t8` (water can stand that high there) with a hop under
// `hopBelow`, and an OPEN way from that neighbour's surface into this column
// (this column's cell at that row is not a wall). Hops strictly fall toward
// the map's water, so support can never run in a circle. Returns
// STREAM_HOP_MAX + 1 when there is none. y = 1 when that best feeder is a
// POOL stream's (a tie of hop goes to the map-rooted one).
// Not "and the neighbour's water stands there now": an adopted thin film
// pours its top cell into the next, still unadopted column every substep, and
// the reservoir refills a cell only when it has nothing else to do, so that
// cell is mostly empty while the stream below it is still running -- the
// check stalled adoption a few columns in. A PLUGGED mouth is a wall in this
// column at the row (refused here), and the columns below it lose their
// support one tick later, when the plugged column's record is gone.
fn streamSupport(x : i32, z : i32, t8 : i32, mat : u32, hopBelow : u32) -> vec2<u32> {
  var best = STREAM_HOP_MAX + 1u;
  var pool = 0u;
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    let t = neighbourTarget(nx, nz, mat);
    if (t.x == STREAM_NONE || t.x < t8) { continue; }
    let hop = u32(t.y);
    let np = u32(t.z);
    // Smaller hop wins; an equal hop replaces a POOL best with a map one.
    if (hop >= hopBelow || hop > best || (hop == best && (np != 0u || pool == 0u))) { continue; }
    let yt = (t.x - 1) >> 3u;
    let mc = vec3<i32>(x, yt, z);
    if (!inBounds(mc)) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    best = hop;
    pool = np;
  }
  return vec2<u32>(best, pool);
}

// OUTFLOW: this column's surface cell has somewhere LOWER to go beside it --
// a lateral neighbour that is air at the surface row, or liquid standing at
// least two eighths lower. A stream always has one (it runs downhill); a pool
// being filled has none (it is a bowl), so a filling pool is never frozen part
// way up: it fills to its feeder's level and sleeps as plain water.
fn streamOutflow(x : i32, y : i32, z : i32, s8 : i32, mat : u32) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let n = vec3<i32>(x + d.x, y, z + d.y);
    if (!inBounds(n)) { continue; }
    let w = voxWordAt(n);
    let m = voxMat(w);
    if (m == MAT_AIR) { return true; }
    if (m == mat && 8 * y + i32(voxState(w) + 1u) + 2 <= s8) { return true; }
    let t = neighbourTarget(n.x, n.z, mat);
    if (t.x != STREAM_NONE && t.x <= s8) { return true; }
  }
  return false;
}

// Is this workgroup's chunk the TOPMOST dirty chunk of its window stack?
// Thread k (1 <= k < NCHUNK) asks about the k-th window row above it, in
// world order (slot row (cy + k) & mask, up to the row that holds the
// window's top), and ORs the answer into the workgroup's word: one dirtyIn
// latency, not up to NCHUNK - 1 of them in a row on one thread while the
// other 255 wait. An OR is order-free, so the answer does not depend on which
// thread runs first. Called by EVERY thread of the workgroup, in uniform
// control flow (it holds two barriers). A ticket slot (>= NUM_CHUNKS: no
// window column) is never the top.
var<workgroup> wgAbove : atomic<u32>;
fn stackTop(centre : u32, li : u32) -> bool {
  if (li == 0u) { atomicStore(&wgAbove, 0u); }
  workgroupBarrier();
  if (centre < NUM_CHUNKS && li >= 1u) {
    let cy = (centre / NCHUNK) % NCHUNK;
    let cz = centre / (NCHUNK * NCHUNK);
    let cx = centre % NCHUNK;
    let oy = bitcast<u32>(T.origin.y & i32(NCHUNK_MASK));
    let rowsAbove = (oy + NCHUNK - 1u - cy) & u32(NCHUNK_MASK);
    if (li <= rowsAbove) {
      let sy = (cy + li) & u32(NCHUNK_MASK);
      if (dirtyIn[(cz * NCHUNK + sy) * NCHUNK + cx] != 0u) { atomicOr(&wgAbove, 1u); }
    }
  }
  workgroupBarrier();
  return centre < NUM_CHUNKS && atomicLoad(&wgAbove) == 0u;
}

@compute @workgroup_size(256)
fn scan(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  // Every thread asks (the barriers in stackTop), then the not-top return.
  if (!stackTop(centre, li) || T.stillWaterRelax == 0u) { return; }
  let wc = slotWorldChunk(centre, T.origin);
  let x = wc.x * i32(CHUNK) + i32(li & 15u);
  let z = wc.z * i32(CHUNK) + i32(li >> 4u);
  let top = wc.y * i32(CHUNK) + i32(CHUNK) - 1;
  let si = stillIndex(x, z);
  // A column with no record at all (never generated into this slot) or one
  // the map already holds is not a stream.
  if (stillWater[si] != stillTag(x, z) || stillWater[si + 1u] != 0u) { return; }
  let dyn = stillWater[si + 2u];
  let old3 = stillWater[si + 3u];
  let w3 = old3 & 0x3FFFFFFFu;
  var word = 0u;
  if (dyn != 0u) {
    // ADOPTED: still supported at its own target by a neighbour nearer the
    // map's water (its surface may be under or over the target right now --
    // the reservoir's business), and not drowned (the flood count), or
    // released.
    let mat = w3 & 0xFFFu;
    let pool = w3 & STREAM_POOL_BIT;
    let t8 = sdec(dyn);
    var over = (w3 >> STREAM_OVER_SHIFT) & STREAM_OVER_MAX;
    let s = columnSurface(x, top, z);
    if (s.ok && s.mat == mat && s.s8 > t8) {
      // A CAPPED head (THE CONDUIT: a POOL column waiting over its target for
      // the mouth to draw the ledger down) counts +1, never the deep +16.
      let deep = s.s8 > t8 + 8 && !(pool != 0u && ledgerSnap() >= STREAM_LEDGER_CAP);
      over = min(over + select(1u, STREAM_FLOOD_DEEP, deep), STREAM_OVER_MAX);
    } else if (pool != 0u && ledgerSnap() <= 0 && (!s.ok || s.s8 < t8)) {
      // STARVED (the conduit's mouth waiting on an empty ledger): counted
      // like a flood, so a mouth whose source has stopped pouring is let go.
      over = min(over + 1u, STREAM_OVER_MAX);
    } else if (over > 0u) {
      over = over - 1u;
    }
    var held = over < STREAM_FLOOD_COUNT &&
               streamSupport(x, z, t8, mat, dyn >> 20u).x <= STREAM_HOP_MAX;
    // A POOL column with no held feeder nearer the source stays while unheld
    // water still pours into it -- it is the stream's head now (the
    // hysteresis: no root test here, see THE CONDUIT).
    if (!held && pool != 0u && over < STREAM_FLOOD_COUNT) {
      held = liveSource(x, (t8 - 1) >> 3u, z, t8, mat, false);
    }
    word = mat | pool | (over << STREAM_OVER_SHIFT) | select(STREAM_ACT_RELEASE << 30u, 0u, held);
  } else {
    let s = columnSurface(x, top, z);
    var age = (w3 >> 20u) & STREAM_AGE_MAX;
    if (!s.ok) {
      word = (w3 & 0xFFFFFu) | ((age >> 1u) << 20u);
    } else {
      age = min(age + 1u, STREAM_AGE_MAX);   // this visit; reset below on a jump
      // The band's centre stays where the count began; a jump out of it
      // restarts the count from here.
      var obs = s.s8;
      if (age > 0u && abs(s.s8 - sdec(w3)) <= 8) { obs = sdec(w3); } else { age = 0u; }
      word = (bitcast<u32>(obs) & 0xFFFFFu) | (age << 20u);
      if (age >= STREAM_ADOPT_TICKS && streamOutflow(x, s.y, z, s.s8, s.mat)) {
        let sup = streamSupport(x, z, s.s8, s.mat, STREAM_HOP_MAX);
        var hop = sup.x + 1u;
        var act = select(STREAM_ACT_ADOPT, STREAM_ACT_ADOPT_POOL, sup.y != 0u);
        // No held feeder: a live source at the top of a slope roots a POOL
        // stream (THE CONDUIT).
        if (hop > STREAM_HOP_MAX && liveSource(x, s.y, z, s.s8, s.mat, true)) {
          hop = 1u;
          act = STREAM_ACT_ADOPT_POOL;
        }
        if (hop <= STREAM_HOP_MAX) {
          word = (bitcast<u32>(s.s8) & 0xFFFFFu) | (hop << 20u) | (act << 30u);
        }
      }
    }
  }
  // Only a CHANGED word is stored: every dry column of every topmost dirty
  // chunk passes through here each tick, and almost all of them hold 0 and
  // stay 0. (No pending action is ever left standing in an unchanged word:
  // `commit` clears every action it reads.)
  if (word != old3) { stillWater[si + 3u] = word; }
}

@compute @workgroup_size(256)
fn commit(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  // THE LEDGER'S SNAPSHOT (THE CONDUIT): the balance the CA's adds left this
  // tick, for the next tick's decisions. One thread, after the barrier that
  // ends the CA rows (pass_table.def streamScan / streamCommit); nothing in
  // this tick reads the snapshot after this (`scan` ran before it). Every
  // tick the CA ran has a dirty list, so this runs; a tick with none added
  // nothing, so the snapshot it keeps is still the balance.
  if (wg.x == 0u && li == 0u) { stillWater[STREAM_LEDGER + 1u] = stillWater[STREAM_LEDGER]; }
  if (!stackTop(centre, li) || T.stillWaterRelax == 0u) { return; }
  let wc = slotWorldChunk(centre, T.origin);
  let x = wc.x * i32(CHUNK) + i32(li & 15u);
  let z = wc.z * i32(CHUNK) + i32(li >> 4u);
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z) || stillWater[si + 1u] != 0u) { return; }
  let w3 = stillWater[si + 3u];
  let act = w3 >> 30u;
  if (act == STREAM_ACT_RELEASE) {
    // WAKE ON RELEASE (phase 3): the chunk(s) at the released target's top
    // cell are marked for the next tick, so the columns that rested on this
    // one are looked at again even when the released water does not move
    // (a finite source's stream ends in films that stay put, and a held
    // column whose chunk never wakes keeps a stale target -- a point where
    // water would later be deposited into the ledger). One mark per release;
    // a release is once per adoption, so this cannot keep anything awake.
    let tc = vec3<i32>(x, (sdec(stillWater[si + 2u]) - 1) >> 3u, z);
    for (var k = 0u; k < 8u; k++) {
      let ns = dirtyFanSlot(tc, T.origin, k);
      if (ns != SLOT_NONE) { atomicOr(&dirtyOut[ns], DIRTY_R_WRITE); }
    }
    stillWater[si + 2u] = 0u;
    stillWater[si + 3u] = 0u;
  } else if (act == STREAM_ACT_ADOPT || act == STREAM_ACT_ADOPT_POOL) {
    // The surface `scan` saw is still this column's (nothing writes a voxel
    // between the two kernels); its material goes into [3].
    let s = columnSurface(x, wc.y * i32(CHUNK) + i32(CHUNK) - 1, z);
    if (s.ok) {
      stillWater[si + 2u] = (w3 & 0xFFFFFu) | (((w3 >> 20u) & STREAM_HOP_MAX) << 20u);
      stillWater[si + 3u] = (s.mat & 0xFFFu) | select(0u, STREAM_POOL_BIT, act == STREAM_ACT_ADOPT_POOL);
    } else {
      stillWater[si + 3u] = 0u;
    }
  }
}

// THE LOAD RESET (pass_table.def lr_streamClear, PT_LOADRESET). A stream's
// words describe the world they were adopted in, and a LOAD replaces that
// world. Dense worldgen clears the whole table, but a load refills the window
// through the streamed path, whose `cols` keeps a column's dynamic words
// while its tag is unchanged -- and a save loaded where the window already
// stood changes no tag. So the old session's streams outlived the load: a
// stale target held whatever water the loaded world had in that column at
// the old level (refilling it from nothing), and the same save behaved
// differently loaded into a fresh process and into one that had been playing.
// One thread per window column, the dynamic words only: the authored [0] /
// [1], which the refill's `cols` has just written, stay. The load's streams
// run as water and are adopted again, as the header says they are.
@compute @workgroup_size(256)
fn clearStreams(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i >= WORLD_N * WORLD_N) { return; }
  // The conduit's ledger is water in a stream the load replaced: gone with it
  // (a bounded loss -- see sim_step.wgsl THE CONDUIT).
  if (i == 0u) {
    stillWater[STREAM_LEDGER] = 0u;
    stillWater[STREAM_LEDGER + 1u] = 0u;
    stillWater[STREAM_LEDGER + 2u] = 0u;   // the diagnostic deposit / withdrawal totals
    stillWater[STREAM_LEDGER + 3u] = 0u;
  }
  stillWater[i * STILL_WORDS + 2u] = 0u;
  stillWater[i * STILL_WORDS + 3u] = 0u;
}
