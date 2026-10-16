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
//      over its neighbours the way the water's pull spreads.
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
// COST. 67 x 67 threads, nine 4-word loads each. Recorded on the ticks the CA
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
const FLOWM_STATE : u32 = 17956u;
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

  // ---- 1. the tick's flux over the 3 x 3 tent ----
  var flux = vec2<i32>(0);
  var topE = 0;
  var botE = 0;
  for (var dj = -1; dj <= 1; dj++) {
    for (var di = -1; di <= 1; di++) {
      // A neighbour off the grid would alias the far edge's slot.
      if (i + di < 0 || i + di >= n || j + dj < 0 || j + dj >= n) { continue; }
      let a = flowmSlot(k + vec2<i32>(di, dj)) * 4u;
      let w = (2 - abs(di)) * (2 - abs(dj));
      let acc = vec2<i32>(bitcast<i32>(flowMeasRW[a]), bitcast<i32>(flowMeasRW[a + 1u]));
      flux += w * clamp(acc, vec2<i32>(-FLOWM_TERM_MAX), vec2<i32>(FLOWM_TERM_MAX));
      topE = max(topE, bitcast<i32>(flowMeasRW[a + 2u]));
      botE = max(botE, bitcast<i32>(flowMeasRW[a + 3u]));
    }
  }
  // ---- 2. convert: STATE units, |inst| <= FLOWM_STATE_MAX ----
  let inst = (clamp(flux, vec2<i32>(-FLOWM_FLUX_MAX), vec2<i32>(FLOWM_FLUX_MAX)) *
              FLOWM_TO_NUM) / FLOWM_TO_DEN;

  // ---- the node's state, reset if the slot held another node ----
  let s = FLOWM_STATE + flowmSlot(k) * 4u;
  let tag = flowmTag(k);
  var v = vec2<i32>(0);
  var band = (u32(FLOW_DRY) & 0xFFFFu);
  if (flowMeasRW[s + 2u] == tag) {
    v = clamp(vec2<i32>(bitcast<i32>(flowMeasRW[s]), bitcast<i32>(flowMeasRW[s + 1u])),
              vec2<i32>(-FLOWM_STATE_MAX), vec2<i32>(FLOWM_STATE_MAX));
    band = flowMeasRW[s + 3u];
  }

  // ---- 3. follow: fast up, slow down ----
  // |inst - v| <= 2 x FLOWM_STATE_MAX, times 132 is 1.1e9: inside i32. The
  // divide truncates toward zero, so the follow never overshoots inst.
  let kq = select(FLOWM_FALL_Q11, FLOWM_RISE_Q11, flowmMag2(inst) > flowmMag2(v));
  v = v + ((inst - v) * kq) / 2048;
  if (topE != 0) {
    band = flowmI16(topE - FLOWM_Y_BIAS) | (flowmI16(FLOWM_Y_BIAS - botE) << 16u);
  }
  if (flowmStill(v)) {
    v = vec2<i32>(0);
    band = u32(FLOW_DRY) & 0xFFFFu;
  }
  flowMeasRW[s] = bitcast<u32>(v.x);
  flowMeasRW[s + 1u] = bitcast<u32>(v.y);
  flowMeasRW[s + 2u] = tag;
  flowMeasRW[s + 3u] = band;

  // ---- 4. publish, flowGrid's layout ----
  let g = FLOWM_GRID + FLOW_HEADER + 2u * u32(j * n + i);
  flowMeasRW[g] = flowmI16(flowmPublish(v.x)) | (flowmI16(flowmPublish(v.y)) << 16u);
  flowMeasRW[g + 1u] = band;
}
