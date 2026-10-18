// flow_measure.wgsl — THE MEASURED FLOW's fold (DESIGN.md §9d.12;
// src/sim/flowfield.h kFlowMeas* is the layout and the reference).
//
// WHAT PROBLEM THIS SOLVES. The current the renderer draws (common.wgsl
// currentAt: the arrows, the streaks, the boils, the foam) was the map's
// SOLVED field plus the drain primitives. The solved field is a function of
// the map, so a dam somebody breaches has no current through it at all: the
// arrows go on pointing the way the lake drained before (ticket
// 2026-10-14_22-57-12). The water the simulation is actually moving is the
// only honest answer, and the CA and the MPM `mark` pass now record it: every
// lateral liquid move adds its eighths x displacement to the node bin it left
// (sim_step.wgsl flowMeasNote, sim_fluid.wgsl flowMeasParticle).
//
// THIS KERNEL turns that tick's raw flux into the field the renderer reads.
// One thread per node of the window grid (kFlowWinNodes per axis, flowGrid's
// lattice exactly), after the CA:
//   1. GATHER the flux over the node's 3 x 3 neighbourhood (a [1 2 1] tent):
//      the CA moves water in 1-cell hops at the free surface, and a node bin
//      is 8 x 8 columns, so the raw bins are lumpy; the tent spreads a breach
//      over its neighbours the way the water's pull spreads. Then THE REACH
//      (2026-10-18): a wider tent (sim.currentMeasuredReach nodes) over which
//      every bin counts as its PULL on the node -- toward it, where the node is
//      upstream of it (flowmPull) -- added where it is stronger than the
//      node's own reading, so the lake converges on a breach for a few metres
//      and fades into the solved field instead of stopping one node out. The
//      heaviest pull also names where in the column that water runs: THE
//      VERTICAL (below), published in its own plane.
//   2. CONVERT eighths x cells per tick into a velocity: the discharge per
//      unit width over the bin, divided by a reference depth (FLOWM_DEPTH).
//      The CA moves the SURFACE of a body, not its depth, so the flux is per
//      surface layer and a fixed (calibrated) depth is the honest conversion.
//   3. SMOOTH in time: an exponential follow, fast up (tau 0.5 s) so a breach
//      shows at once, slow down (tau 1.5 s) so a flood fades instead of
//      flickering off between ticks. Below FLOWM_SNAP it is zero.
//   4. PUBLISH the node in flowGrid's own layout (GRID region): the reader is
//      flowGridAt's loop over different words.
// The STATE is kept per toroidal slot (world node mod kFlowMeasN) with the
// world node as a tag, so a window move keeps every node that stays in view
// and starts the ones that scroll in from zero.
//
// AUTHORITY: THE PUBLISHED GRID IS SIM INPUT, SO THIS FOLD IS INTEGER ONLY.
// Since 2026-10-15 the snapshot ring copies GRID (World::EncodeReadbacks) and,
// exactly World::kSnapshotLatency ticks later, the bodies the water holds feel
// it: the player's swim stroke and an NPC afloat (CurrentAtCpuSim). Until then
// this kernel smoothed in f32, which is bit-exact on one GPU and not across
// vendors -- an FMA contracted on one driver and not another moves the last
// bit, the round to i16 below turns that into a different published word, and
// where an NPC drifts would depend on whose GPU ran the tick (rule 1). So the
// follow is fixed point: STATE holds i32 velocities in 1/(FLOW_VEL_Q x
// FLOWM_SUB) cells/s, every step is an add, a multiply or a truncating divide
// with its range bounded below, and the chain -- move, bin, tent, follow,
// publish -- is a function of the moves alone. The accumulator it reads is a
// set of integer adds and maxes, which commute, so it never depended on the
// GPU's schedule. No GPU sim kernel reads flowMeas; the CPU reads its GRID
// only through the fixed-latency snapshot, never on the frame path.
//
// COST. 67 x 67 threads, (2R+1)^2 4-word loads each (R = the reach, 81 at the
// default 4; nine at 1), plus the pulling bin's three moment words. Recorded on the ticks the CA
// or the fluid seam ran, then for kFlowMeasTailTicks after the last one (the
// decay), then never: a settled world records nothing, and one Fill zeroes the
// published field when the tail ends (Simulation::EncodeTick, Cond::FlowMeasure
// / FlowMeasureClear).

@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(61) var<storage, read_write> flowMeasRW : array<u32>;

// Must match flowfield.h kFlowMeasN / kFlowMeasAccQ / kFlowMeasStateBase /
// kFlowMeasGridBase and sim_step.wgsl's FLOWM_* (check_invariants.py
// "measured flow").
const FLOWM_N : i32 = 67;
const FLOWM_ACC_Q : i32 = 16;
const FLOWM_Y_BIAS : i32 = 1048576;
const FLOWM_STATE : u32 = 35912u;
// Words a slot in ACC and in STATE (flowfield.h kFlowMeasAccStride /
// kFlowMeasStateStride), and the VERTICAL plane's place in GRID: after the
// header and the two-word nodes (flowfield.h kFlowMeasVertOffset).
const FLOWM_ACC_STRIDE : u32 = 8u;
const FLOWM_STATE_STRIDE : u32 = 8u;
// FLOWM_GRID is common.wgsl's (the reader's) constant.

// STATE's fixed point: FLOWM_SUB steps per published unit (1/FLOW_VEL_Q
// cells/s), so a STATE word counts 1/16384 cells/s.
const FLOWM_SUB : i32 = 128;
// The published words are i16, so a node never reads faster than 32767 units
// (256 cells/s, 25.6 m/s). STATE is held to the same range: it is what bounds
// every product below.
const FLOWM_STATE_MAX : i32 = 4194176;   // 32767 x FLOWM_SUB
// THE CONVERSION, and its one calibration. The discharge per unit width over a
// bin is divided by FLOWM_DEPTH = 0.5 cells: it stands for the moving SURFACE
// layer (the CA levels a body by its top cells) less the dilution a jet
// narrower than a bin takes through the tent. Measured on the `flow-measure`
// gate's breach (a 0.8 m head through a 0.6 m gap): 1.0 read the gap at
// ~0.9 m/s, half of what that head drives (sqrt(g h) ~ 2.8 m/s); 0.5 reads it
// at ~2 m/s and the reservoir 1.6 m back at ~0.6 m/s. render.currentMeasured
// scales what the renderer draws, sim.currentMeasuredCarry what a body feels.
// In STATE units a flux unit (eighths x cells x FLOWM_ACC_Q per tick, tent
// weights summing to 16) is
//   30 ticks/s x FLOW_VEL_Q 128 x FLOWM_SUB 128
//     / (FLOWM_ACC_Q 16 x 8 eighths x 64 columns x 16 x FLOWM_DEPTH 0.5)
//   = 491520 / 65536 = 15 / 2,
// taken as (flux x FLOWM_TO_NUM) / FLOWM_TO_DEN, truncating toward zero.
const FLOWM_TO_NUM : i32 = 15;
const FLOWM_TO_DEN : i32 = 2;
// The flux that converts to FLOWM_STATE_MAX: anything beyond is the same
// 25.6 m/s, and clamping first keeps flux x FLOWM_TO_NUM inside i32.
const FLOWM_FLUX_MAX : i32 = 559223;     // FLOWM_STATE_MAX x 2 / 15
// One accumulator word, clamped before it is weighted: nine of them at
// weights summing to 16 then stay under 2^30.
const FLOWM_TERM_MAX : i32 = 67108864;   // 2^26
// THE REACH (sim.currentMeasuredReach, step 1 below): at most FLOWM_REACH_MAX
// nodes, so its weights sum to at most (6+1)^4 = 2401. Each neighbour's flux
// is held to 2^18 a component (32 full rows of a bin moved a cell in one
// tick: never binding), so its pull (flowmPull: at most |F| x 1.03 long, the
// octagonal norm's worst case) is under 2^18 x 1.375 x 1.03 a component, and
// 2401 of those sum to 8.9e8, inside i32.
const FLOWM_REACH_MAX : i32 = 6;
const FLOWM_WIDE_TERM_MAX : i32 = 262144;  // 2^18
// The follow, a fraction of the gap closed per tick in Q11:
// 1 - exp(-1 / (tau x 30)) x 2048.
const FLOWM_RISE_Q11 : i32 = 132;        // tau 0.50 s
const FLOWM_FALL_Q11 : i32 = 45;         // tau 1.50 s (FLOWM_TAU_FALL_S)
// Under FLOWM_SNAP the node is still: 0.05 cells/s (5 mm/s), under the i16
// grid's resolution for anything that would show. Squared, in STATE units:
// (0.05 x 16384)^2 = 671088.6.
const FLOWM_SNAP2 : u32 = 671089u;
// The tag's format bit. STATE written by the f32 fold (before 2026-10-15)
// carries a tag without it, so a shader hot-reloaded over a live field reads
// those slots as another node's and restarts them from rest instead of
// reading float bits as velocities.
const FLOWM_TAG_I32 : u32 = 0x40000000u;

fn flowmSlot(k : vec2<i32>) -> u32 {
  let m = ((k % vec2<i32>(FLOWM_N)) + vec2<i32>(FLOWM_N)) % vec2<i32>(FLOWM_N);
  return u32(m.y * FLOWM_N + m.x);
}
fn flowmTag(k : vec2<i32>) -> u32 {
  return (u32(k.x) & 0x7FFFu) | ((u32(k.y) & 0x7FFFu) << 15u) | FLOWM_TAG_I32 |
         0x80000000u;
}
fn flowmI16(v : i32) -> u32 { return u32(clamp(v, -32767, 32767)) & 0xFFFFu; }
// |v|^2 in published units: each component is at most 32767 there, so the sum
// of two squares stays under 2^31 and fits a u32 with room.
fn flowmMag2(v : vec2<i32>) -> u32 {
  let g = vec2<u32>(min(abs(v), vec2<i32>(FLOWM_STATE_MAX)) / vec2<i32>(FLOWM_SUB));
  return g.x * g.x + g.y * g.y;
}
// The snap test, in STATE units. Each component is capped where its square
// would overflow the sum -- 46340, fifty-odd times the threshold, so the cap
// can never decide it.
fn flowmStill(v : vec2<i32>) -> bool {
  let s = vec2<u32>(min(abs(v), vec2<i32>(46340)));
  return s.x * s.x + s.y * s.y < FLOWM_SNAP2;
}
// An integer length, the octagonal norm max + 3/8 min: within -3% / +0% of
// the Euclidean one (exact on the axes, 1.375 for 1.414 on the diagonal).
// Integer because bodies read what it feeds (the AUTHORITY note).
fn flowmOct(v : vec2<i32>) -> i32 {
  let a = abs(v);
  return max(a.x, a.y) + (3 * min(a.x, a.y)) / 8;
}
// THE PULL of neighbour `d` nodes away (d != 0), whose bin moved flux `f`, on
// this node (ticket 2026-10-18_11-30-37: "every vector influenced by the dam
// is pointing in the same direction ... they should all be pointing towards
// the hole itself"). Not `f` itself -- spreading a breach's own vector over
// its neighbours draws every arrow round it parallel to the jet, which is
// what the reach did until this -- but a pull TOWARD the neighbour, as strong
// as its flux, where this node is UPSTREAM of it (the neighbour's water runs
// away from here, so water here is drawn after it):
//   pull = |f| x clamp(3 cos(theta), 0, 1) x d / |d|,
// theta the angle between f and d. Full strength anywhere within 70 degrees of
// the flow's axis behind it, fading to nothing square to it, and nothing at
// all ahead of it (a flood front does not suck the water it is about to reach
// backwards; the water there measures its own flow when it arrives). Summed
// over a breach's nodes, every node of the lake behind it points AT the
// breach, from whatever side it is approached -- the field of a sink, not of
// a copied jet. Integer (cos in Q8 over octagonal lengths); bounds in the
// FLOWM_WIDE_TERM_MAX note.
fn flowmPull(f : vec2<i32>, d : vec2<i32>) -> vec2<i32> {
  let mF = flowmOct(f);                    // <= 360448
  if (mF == 0) { return vec2<i32>(0); }
  let dQ = flowmOct(d * 16);               // |d| in 1/16 nodes, 16 .. 132
  let dotFD = f.x * d.x + f.y * d.y;       // |.| <= 3.2e6
  let cos8 = (dotFD * 256) / max((mF * dQ) / 16, 1);
  let s8 = clamp(3 * cos8, 0, 256);
  let mS = (mF * s8) / 256;
  return (mS * d * 16) / vec2<i32>(dQ);
}

// THE VERTICAL (ticket 2026-10-18_11-30-37: "if the hole in the wall is
// located a bit below ... that magnitude arrow should influence vectors
// directly next to it to point in its direction from above and below ... so
// current should be able to pull material down into the water"). The field
// is a grid of columns, so a node cannot hold a velocity per height; what it
// holds is WHERE in the column the water pulling it runs, and the reader
// (common.wgsl flowMeasRise, flowfield.cpp SampleMeasuredSpan) turns that
// into a vertical velocity at any height:
//   H   the centre of that water's rows (the flux-weighted mean of the rows
//       its moves left, the writers' height moments), world cells;
//   HW  their half-height, sqrt(3) x the flux-weighted spread (a uniform
//       band of N rows reads N/2), so a full-depth breach or a river's
//       moving surface covers its whole band and pulls nothing up or down;
//   D   how far away it is (the pulling bin's distance, at least 4 cells):
//       a point dy outside [H - HW, H + HW] is pulled toward it at
//       |v_h| x clamp(dy / D, -1.5, 1.5) -- a point sink's geometry, 45
//       degrees at dy = D -- by the reader;
//   ON  only where that water is SUBMERGED: its rows end FLOWM_VERT_GAP or
//       more inside the band of water moving here (above or below), so the
//       pull points somewhere there is water to come from.
// The pulling bin is the one whose pull (flowmPull, or the node's own flux)
// weighs most in the reach; ties go to the first in the fixed loop order.
// STATE follows H / HW / D at the rise rate and ON at the horizontal one,
// so the published words are as smooth in time as the velocity.
const FLOWM_VERT_GAP_Q4 : i32 = 32;      // 2 cells, in 1/16 cells
const FLOWM_VERT_W_MAX : i32 = 32767;    // the writers' trust bound (sim_step.wgsl)
const FLOWM_ON_Q10 : i32 = 1024;
// Publish: H i16 world cells | HW (cells, 0..63) << 16 | D / 2 (cells, 1..63)
// << 22 | ON (0..15) << 28.
fn flowmVertWord(hQ : i32, hwQ : i32, dQ : i32, on : i32) -> u32 {
  let h = (hQ + select(-8, 8, hQ >= 0)) / 16;
  let hw = clamp((hwQ + 8) / 16, 0, 63);
  let d2 = clamp((dQ + 16) / 32, 1, 63);
  let o = clamp((on * 15 + 512) / FLOWM_ON_Q10, 0, 15);
  return flowmI16(h) | (u32(hw) << 16u) | (u32(d2) << 22u) | (u32(o) << 28u);
}

// STATE -> published units, round half away from zero.
fn flowmPublish(v : i32) -> i32 {
  let q = (abs(v) + FLOWM_SUB / 2) / FLOWM_SUB;
  return select(-q, q, v >= 0);
}

@compute @workgroup_size(8, 8)
fn fold(@builtin(global_invocation_id) gid : vec3<u32>) {
  let n = FLOWM_N;
  let i = i32(gid.x);
  let j = i32(gid.y);
  // The grid's origin is flowGrid's: one node step before the window's low
  // corner (flowfield.cpp UpdateAt): window base - FLOW_STEP, an exact
  // multiple of the step (the base is chunk-aligned), so the division is exact.
  let stepI = i32(FLOW_STEP);
  let node0 = (vec2<i32>(T.origin.x, T.origin.z) * i32(CHUNK) - vec2<i32>(stepI)) / stepI;
  if (i == 0 && j == 0) {
    let g = FLOWM_GRID;
    flowMeasRW[g + 0u] = bitcast<u32>(node0.x * i32(FLOW_STEP));
    flowMeasRW[g + 1u] = bitcast<u32>(node0.y * i32(FLOW_STEP));
    flowMeasRW[g + 2u] = u32(n);
    flowMeasRW[g + 3u] = T.tick;
  }
  if (i >= n || j >= n) { return; }
  let k = node0 + vec2<i32>(i, j);

  // ---- 1. the tick's flux over the 3 x 3 tent, and over the REACH ----
  // The 3 x 3 tent is the node's own reading (`flux`). THE REACH (ticket
  // 2026-10-17_20-56-48: "the arrows discretely change from the natural
  // current of the lake to this manmade flow ... all arrows should be
  // influencing other arrows ... the sphere of influence a bit larger"): the
  // same flux over a tent of radius R = sim.currentMeasuredReach nodes
  // (`wide`), weights (R+1-|di|)(R+1-|dj|), normalised to the 3 x 3 tent's
  // sum of 16 so a broad flood reads the same through either. With the 3 x 3
  // alone a breach's pull ended 1.6 m out, and an arrow one node further on
  // pointed wherever the map's solved field did. The band is gathered over
  // the reach too: a node the reach alone moves must be read at its rows.
  let R = clamp(TUNE_CURRENT_MEASURED_REACH, 1, FLOWM_REACH_MAX);
  var flux = vec2<i32>(0);
  var wide = vec2<i32>(0);
  var topE = 0;
  var botE = 0;
  // The pulling bin (THE VERTICAL above): its weight and offset.
  var bestA = 0;
  var bestD = vec2<i32>(0);
  for (var dj = -R; dj <= R; dj++) {
    for (var di = -R; di <= R; di++) {
      // A neighbour off the grid would alias the far edge's slot.
      if (i + di < 0 || i + di >= n || j + dj < 0 || j + dj >= n) { continue; }
      let a = flowmSlot(k + vec2<i32>(di, dj)) * FLOWM_ACC_STRIDE;
      let acc = vec2<i32>(bitcast<i32>(flowMeasRW[a]), bitcast<i32>(flowMeasRW[a + 1u]));
      if (abs(di) <= 1 && abs(dj) <= 1) {
        let w = (2 - abs(di)) * (2 - abs(dj));
        flux += w * clamp(acc, vec2<i32>(-FLOWM_TERM_MAX), vec2<i32>(FLOWM_TERM_MAX));
      }
      // The node's own bin counts as its flux; every other bin as its PULL
      // on this node (flowmPull): the reach turns the water toward a breach
      // instead of copying the breach's direction onto it.
      let ww = (R + 1 - abs(di)) * (R + 1 - abs(dj));
      let fw = clamp(acc, vec2<i32>(-FLOWM_WIDE_TERM_MAX), vec2<i32>(FLOWM_WIDE_TERM_MAX));
      var pw = fw;
      if (di != 0 || dj != 0) { pw = flowmPull(fw, vec2<i32>(di, dj)); }
      if (R > 1) { wide += ww * pw; }
      // ww <= 49, |pw| <= 3.7e5 by octagon: the weight stays under 2^25.
      let aw = ww * flowmOct(pw);
      if (aw > bestA) {
        bestA = aw;
        bestD = vec2<i32>(di, dj);
      }
      topE = max(topE, bitcast<i32>(flowMeasRW[a + 2u]));
      botE = max(botE, bitcast<i32>(flowMeasRW[a + 3u]));
    }
  }
  // ---- 2. convert: STATE units, |inst| <= FLOWM_STATE_MAX ----
  var inst = (clamp(flux, vec2<i32>(-FLOWM_FLUX_MAX), vec2<i32>(FLOWM_FLUX_MAX)) *
              FLOWM_TO_NUM) / FLOWM_TO_DEN;
  // ---- 2b. the reach: added where it is stronger than the node's own ----
  // v = local + wide x (1 - a^2), a^2 = min(|local|^2 / |wide|^2, 1): where
  // the node measures as much as its surroundings (inside a flood, at a jet's
  // core) it reads exactly what it did with the 3 x 3 alone; where it measures
  // nothing (outside the flood) it reads its surroundings, falling to zero at
  // R + 1 nodes; in between, continuously. Every step is an integer with its
  // range bounded (the AUTHORITY note above): the normalisation divides in two
  // halves so 16 x the sum never leaves i32, a^2 is Q10, and the reach is
  // scaled down by 8 before its Q10 weight.
  if (R > 1) {
    let s2 = (R + 1) * (R + 1);
    let wideFlux = ((wide / vec2<i32>(s2)) * 16) / vec2<i32>(s2);
    let instW = (clamp(wideFlux, vec2<i32>(-FLOWM_FLUX_MAX), vec2<i32>(FLOWM_FLUX_MAX)) *
                 FLOWM_TO_NUM) / FLOWM_TO_DEN;
    let m2L = flowmMag2(inst);
    let m2W = flowmMag2(instW);
    if (m2W > m2L) {
      var a2 = 0u;
      if (m2W >= (1u << 21u)) { a2 = m2L / (m2W >> 10u); } else { a2 = (m2L << 10u) / m2W; }
      let keepW = i32(1024u - min(a2, 1024u));
      inst = clamp(inst + ((instW / 8) * keepW) / 128, vec2<i32>(-FLOWM_STATE_MAX),
                   vec2<i32>(FLOWM_STATE_MAX));
    }
  }

  // ---- 2c. the vertical's target (THE VERTICAL above) ----
  // From the pulling bin's height moments: mean and spread of the rows its
  // water ran in, about the window's middle row (the writers' frame). Every
  // product is bounded by the trust test: |M1| <= W x 256, M2 <= W x 65536.
  var tOk = false;
  var tH = 0;
  var tHW = 0;
  var tD = 0;
  if (bestA > 0) {
    let b = flowmSlot(k + bestD) * FLOWM_ACC_STRIDE;
    let W = bitcast<i32>(flowMeasRW[b + 4u]);
    let M1 = bitcast<i32>(flowMeasRW[b + 5u]);
    let M2 = bitcast<i32>(flowMeasRW[b + 6u]);
    if (W > 0 && W <= FLOWM_VERT_W_MAX && M2 >= 0) {
      let mu16 = (M1 * 16) / W;                       // |.| <= 4096
      let var256 = max((M2 / W) * 256 - mu16 * mu16, 0);
      tHW = (curISqrt(var256) * 1774) / 1024;        // sqrt(3) sigma, 1/16 cells
      // The rows' CENTRE (a row y spans y .. y + 1), world cells x 16.
      tH = mu16 + 8 + (T.origin.y * i32(CHUNK) + 256) * 16;
      tD = max(flowmOct(bestD * 16) * 8, 64);
      tOk = true;
    }
  }

  // ---- the node's state, reset if the slot held another node ----
  let s = FLOWM_STATE + flowmSlot(k) * FLOWM_STATE_STRIDE;
  let tag = flowmTag(k);
  var v = vec2<i32>(0);
  var band = (u32(FLOW_DRY) & 0xFFFFu);
  var vH = 0;
  var vHW = 0;
  var vD = 0;
  var vOn = 0;
  if (flowMeasRW[s + 2u] == tag) {
    v = clamp(vec2<i32>(bitcast<i32>(flowMeasRW[s]), bitcast<i32>(flowMeasRW[s + 1u])),
              vec2<i32>(-FLOWM_STATE_MAX), vec2<i32>(FLOWM_STATE_MAX));
    band = flowMeasRW[s + 3u];
    vH = bitcast<i32>(flowMeasRW[s + 4u]);
    vHW = clamp(bitcast<i32>(flowMeasRW[s + 5u]), 0, 1 << 20);
    vD = clamp(bitcast<i32>(flowMeasRW[s + 6u]), 0, 1 << 20);
    vOn = clamp(bitcast<i32>(flowMeasRW[s + 7u]), 0, FLOWM_ON_Q10);
  }

  // ---- 3. follow: fast up, slow down ----
  // |inst - v| <= 2 x FLOWM_STATE_MAX, times 132 is 1.1e9: inside i32. The
  // divide truncates toward zero, so the follow never overshoots inst.
  let kq = select(FLOWM_FALL_Q11, FLOWM_RISE_Q11, flowmMag2(inst) > flowmMag2(v));
  v = v + ((inst - v) * kq) / 2048;
  if (topE != 0) {
    band = flowmI16(topE - FLOWM_Y_BIAS) | (flowmI16(FLOWM_Y_BIAS - botE) << 16u);
  }
  // ---- 3b. the vertical follows its target ----
  // Submerged: the pulling water's rows end FLOWM_VERT_GAP inside the band
  // moving here, above or below. A tick with no moves in the pulling bin
  // keeps the last target and lets ON fall.
  var onT = 0;
  if (tOk) {
    let topQ = (flowI16Lo(band) + 1) * 16;
    let botQ = flowI16Hi(band) * 16;
    if (flowI16Lo(band) != FLOW_DRY &&
        (tH + tHW <= topQ - FLOWM_VERT_GAP_Q4 || tH - tHW >= botQ + FLOWM_VERT_GAP_Q4)) {
      onT = FLOWM_ON_Q10;
    }
    if (vOn == 0) {
      vH = tH;
      vHW = tHW;
      vD = tD;
    } else {
      // |t - v| < 2^21 (world heights in Q4), x 132 < 2^29.
      vH += (clamp(tH - vH, -(1 << 20), 1 << 20) * FLOWM_RISE_Q11) / 2048;
      vHW += ((tHW - vHW) * FLOWM_RISE_Q11) / 2048;
      vD += ((tD - vD) * FLOWM_RISE_Q11) / 2048;
    }
  }
  let kOn = select(FLOWM_FALL_Q11, FLOWM_RISE_Q11, onT > vOn);
  vOn += ((onT - vOn) * kOn) / 2048;
  if (abs(onT - vOn) < 16) { vOn = onT; }   // the truncating follow's last step
  if (flowmStill(v)) {
    v = vec2<i32>(0);
    band = u32(FLOW_DRY) & 0xFFFFu;
    vOn = 0;
  }
  flowMeasRW[s] = bitcast<u32>(v.x);
  flowMeasRW[s + 1u] = bitcast<u32>(v.y);
  flowMeasRW[s + 2u] = tag;
  flowMeasRW[s + 3u] = band;
  flowMeasRW[s + 4u] = bitcast<u32>(vH);
  flowMeasRW[s + 5u] = bitcast<u32>(vHW);
  flowMeasRW[s + 6u] = bitcast<u32>(vD);
  flowMeasRW[s + 7u] = bitcast<u32>(vOn);

  // ---- 4. publish, flowGrid's layout ----
  let g = FLOWM_GRID + FLOW_HEADER + 2u * u32(j * n + i);
  flowMeasRW[g] = flowmI16(flowmPublish(v.x)) | (flowmI16(flowmPublish(v.y)) << 16u);
  flowMeasRW[g + 1u] = band;
  // The vertical plane (flowfield.h kFlowMeasVertOffset). Published whether
  // or not it is ON, so a gate can read where the pulling water is.
  flowMeasRW[FLOWM_GRID + FLOW_HEADER + 2u * u32(n * n) + u32(j * n + i)] =
      select(0u, flowmVertWord(vH, vHW, vD, vOn), vH != 0 || vOn != 0);
}
