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
//   * it is STEADY when s8 stays within one voxel (8 eighths) of the last
//     observation: the age counts up, a jump restarts it, a tick with no
//     surface halves it (a thin stream flickers);
//   * it is FED when a lateral neighbour holds a target T >= s8 if that
//     neighbour is authored, T > s8 if it is dynamic (strictly downhill along
//     dynamic support, so support can never run in a circle), and that
//     neighbour's water stands at its surface where it can step into this
//     column (its top cell is that liquid; this column's cell at that row is
//     not a wall);
//   * it has OUTFLOW: somewhere lower to run beside its surface cell (a pool
//     being filled has none, so it fills to its feeder's level and sleeps as
//     plain water instead of being frozen part way up);
//   * steady for STREAM_ADOPT_TICKS, fed and running: adopted at its current
//     surface.
// RELEASE: an adopted column that is visited (something woke its chunk) and is
// no longer fed by the same test is released -- a plugged breach drains its
// stream from the top down: the released water is ordinary water, it flows
// into the next held column as water over its target, that wakes it, and it
// finds its own support gone.
//
// RULE 1. ONE WRITER PER COLUMN: only the topmost dirty chunk of a column's
// window stack acts for it (the stack above it is asleep by construction).
// Kernel `scan` reads neighbours' COMMITTED targets (word [2]) and writes only
// its own column's word [3] (observation, age, and a pending action); kernel
// `commit`, after a barrier, applies its own column's action to word [2]. So
// no thread ever reads a word another thread writes in the same kernel, and
// every decision is a function of the voxels after the CA and the committed
// table -- the order threads run in cannot matter.
// RULE 2. A column is visited only while its chunk is dirty; adoption only
// ever ADDS held columns, release only removes ones whose support is gone,
// and nothing here writes a voxel, so this pass cannot keep anything awake.
// Derived, not saved: a reloaded stream runs as water and is adopted again.
// Dense worldgen clears the whole table (pass_table.def fill_stillWater); a
// streamed window column keeps its dynamic words only while its tag (the
// world column) is unchanged (worldgen.wgsl cols).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read>       dirtyIn  : array<u32>;
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
const STREAM_ADOPT_TICKS : u32 = 180u;
// How far down from the top of its chunk a column's surface is looked for.
const STREAM_SCAN : i32 = 32;
// Word [3]: the observed surface (20 bits, eighths, signed) | age << 20 (10
// bits) | the pending action << 30.
const STREAM_AGE_MAX : u32 = 1023u;
const STREAM_ACT_ADOPT : u32 = 1u;
const STREAM_ACT_RELEASE : u32 = 2u;
const STREAM_NONE : i32 = -1073741824;

fn sdec(v : u32) -> i32 { return bitcast<i32>(v << 12u) >> 12u; }

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
  var upLiquid = false;
  for (var k = 0; k < STREAM_SCAN; k++) {
    let c = vec3<i32>(x, top - k, z);
    if (!inBounds(c)) { break; }
    let w = voxWordAt(c);
    let m = voxMat(w);
    let liq = m != MAT_AIR && materials[m].klass == CLASS_LIQUID;
    if (liq && !upLiquid) {
      s.ok = true;
      s.y = c.y;
      s.s8 = 8 * c.y + i32(voxState(w) + 1u);
      s.mat = m;
      return s;
    }
    // A wall over the column ends the look: whatever is under it is a cave.
    if (!liq && m != MAT_AIR && materials[m].klass != CLASS_GAS) { break; }
    upLiquid = liq;
  }
  return s;
}

// The target a neighbour column holds for liquid `mat`, and whether it is
// authored (x = T8 or STREAM_NONE, y = 1 authored / 0 dynamic).
fn neighbourTarget(x : i32, z : i32, mat : u32) -> vec2<i32> {
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z)) { return vec2<i32>(STREAM_NONE, 0); }
  let a = stillWater[si + 1u];
  if (a != 0u && (a >> 20u) == mat) { return vec2<i32>(sdec(a), 1); }
  let d = stillWater[si + 2u];
  if (d != 0u && (d >> 20u) == mat) { return vec2<i32>(sdec(d), 0); }
  return vec2<i32>(STREAM_NONE, 0);
}

// FED: some lateral neighbour holds a target this column's surface s8 can be
// fed from (see the header), with its water standing where it can step in.
fn streamFed(x : i32, z : i32, s8 : i32, mat : u32) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    let t = neighbourTarget(nx, nz, mat);
    if (t.x == STREAM_NONE) { continue; }
    if (t.y == 1 && t.x < s8) { continue; }
    if (t.y == 0 && t.x <= s8) { continue; }
    let yt = (t.x - 1) >> 3u;
    let nc = vec3<i32>(nx, yt, nz);
    let mc = vec3<i32>(x, yt, z);
    if (!inBounds(nc) || !inBounds(mc)) { continue; }
    if (voxMat(voxWordAt(nc)) != mat) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    return true;
  }
  return false;
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
  }
  return false;
}

// Is this workgroup's chunk the TOPMOST dirty chunk of its window stack? One
// thread asks, the workgroup shares it.
var<workgroup> wgTop : u32;
fn topOfStack(centre : u32) -> bool {
  let cy = (centre / NCHUNK) % NCHUNK;
  let cz = centre / (NCHUNK * NCHUNK);
  let cx = centre % NCHUNK;
  let oy = bitcast<u32>(T.origin.y & i32(NCHUNK_MASK));
  // Window rows ABOVE this one, in world order: slot rows (cy + k) & mask
  // until the row that holds the window's top.
  let rowsAbove = (oy + NCHUNK - 1u - cy) & u32(NCHUNK_MASK);
  for (var k = 1u; k <= rowsAbove; k++) {
    let sy = (cy + k) & u32(NCHUNK_MASK);
    if (dirtyIn[(cz * NCHUNK + sy) * NCHUNK + cx] != 0u) { return false; }
  }
  return true;
}

@compute @workgroup_size(256)
fn scan(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  if (li == 0u) {
    wgTop = select(0u, 1u, centre < NUM_CHUNKS && T.stillWaterRelax != 0u && topOfStack(centre));
  }
  if (workgroupUniformLoad(&wgTop) == 0u) { return; }
  let wc = slotWorldChunk(centre, T.origin);
  let x = wc.x * i32(CHUNK) + i32(li & 15u);
  let z = wc.z * i32(CHUNK) + i32(li >> 4u);
  let top = wc.y * i32(CHUNK) + i32(CHUNK) - 1;
  let si = stillIndex(x, z);
  // A column with no record at all (never generated into this slot) or one
  // the map already holds is not a stream.
  if (stillWater[si] != stillTag(x, z) || stillWater[si + 1u] != 0u) { return; }
  let s = columnSurface(x, top, z);
  let dyn = stillWater[si + 2u];
  var obs = stillWater[si + 3u] & 0x3FFFFFFFu;
  if (dyn != 0u) {
    // ADOPTED: still fed at its own target (its surface may be under or over
    // it right now -- the reservoir's business), or released.
    let t8 = sdec(dyn);
    let fed = streamFed(x, z, t8, dyn >> 20u);
    stillWater[si + 3u] = select(STREAM_ACT_RELEASE << 30u, 0u, fed);
    return;
  }
  if (!s.ok) {
    let age = (obs >> 20u) & STREAM_AGE_MAX;
    stillWater[si + 3u] = (obs & 0xFFFFFu) | ((age >> 1u) << 20u);
    return;
  }
  var age = (obs >> 20u) & STREAM_AGE_MAX;
  let prev = sdec(obs);
  if (obs != 0u && abs(s.s8 - prev) <= 8) {
    age = min(age + 1u, STREAM_AGE_MAX);
  } else {
    age = 0u;
  }
  var act = 0u;
  if (age >= STREAM_ADOPT_TICKS && streamOutflow(x, s.y, z, s.s8, s.mat) && streamFed(x, z, s.s8, s.mat)) {
    act = STREAM_ACT_ADOPT;
  }
  stillWater[si + 3u] = (bitcast<u32>(s.s8) & 0xFFFFFu) | (age << 20u) | (act << 30u);
}

@compute @workgroup_size(256)
fn commit(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  if (li == 0u) {
    wgTop = select(0u, 1u, centre < NUM_CHUNKS && T.stillWaterRelax != 0u && topOfStack(centre));
  }
  if (workgroupUniformLoad(&wgTop) == 0u) { return; }
  let wc = slotWorldChunk(centre, T.origin);
  let x = wc.x * i32(CHUNK) + i32(li & 15u);
  let z = wc.z * i32(CHUNK) + i32(li >> 4u);
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z) || stillWater[si + 1u] != 0u) { return; }
  let w3 = stillWater[si + 3u];
  let act = w3 >> 30u;
  if (act == STREAM_ACT_RELEASE) {
    stillWater[si + 2u] = 0u;
    stillWater[si + 3u] = 0u;
  } else if (act == STREAM_ACT_ADOPT) {
    // The surface `scan` saw is still this column's: scan and commit run on
    // the same voxels (nothing writes one between them).
    let s = columnSurface(x, wc.y * i32(CHUNK) + i32(CHUNK) - 1, z);
    if (s.ok) { stillWater[si + 2u] = (bitcast<u32>(s.s8) & 0xFFFFFu) | (s.mat << 20u); }
    stillWater[si + 3u] = 0u;
  }
}
