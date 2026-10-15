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
//   2. CONVERT eighths x cells per tick into cells/s: the discharge per unit
//      width over the bin, divided by a reference depth (FLOWM_DEPTH). The CA
//      moves the SURFACE of a body, not its depth, so the flux is per surface
//      layer and a fixed (calibrated) depth is the honest conversion.
//   3. SMOOTH in time: an exponential follow, fast up (FLOWM_TAU_RISE_S) so a
//      breach shows at once, slow down (FLOWM_TAU_FALL_S) so a flood fades
//      instead of flickering off between ticks. Below FLOWM_SNAP it is zero.
//   4. PUBLISH the node in flowGrid's own layout (GRID region): the reader is
//      flowGridAt's loop over different words.
// The STATE is kept per toroidal slot (world node mod kFlowMeasN) with the
// world node as a tag, so a window move keeps every node that stays in view
// and starts the ones that scroll in from zero.
//
// AUTHORITY. Render-only, and that is the safety argument rather than a
// description: the sim never reads `flowMeas`, it is not hashed, saved or put
// on the wire, and no CPU code reads it on the frame path. f32 is therefore
// allowed here (rule 1 is about state the sim reads). The accumulator it reads
// is a set of integer adds and maxes, which commute, so even this buffer is a
// function of the moves and not of the GPU's schedule.
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
const FLOWM_ACC_Q : f32 = 16.0;
const FLOWM_Y_BIAS : i32 = 1048576;
const FLOWM_STATE : u32 = 17956u;
// FLOWM_GRID is common.wgsl's (the reader's) constant.

// The conversion's one calibration, cells: the discharge per unit width over
// a bin is divided by this. It stands for the moving SURFACE layer (the CA
// levels a body by its top cells) less the dilution a jet narrower than a bin
// takes through the tent. Measured on the `flow-measure` gate's breach (a
// 0.8 m head through a 0.6 m gap): 1.0 read the gap at ~0.9 m/s, half of what
// that head drives (sqrt(g h) ~ 2.8 m/s); 0.5 reads it at ~2 m/s and the
// reservoir 1.6 m back at ~0.6 m/s. render.currentMeasured scales the result.
const FLOWM_DEPTH : f32 = 0.5;
// Seconds to follow a rise / a fall (30 Hz fold): exp(-1 / (tau x 30)).
const FLOWM_K_RISE : f32 = 0.93551;   // tau 0.5 s
const FLOWM_K_FALL : f32 = 0.97802;   // tau 1.5 s (FLOWM_TAU_FALL_S)
// Under this, cells/s, the node is still: 5 mm/s, under the i16 grid's
// resolution for anything that would show.
const FLOWM_SNAP : f32 = 0.05;
// eighths x cells x Q per tick -> cells/s: / Q / 8 (eighths -> cells) x 30
// (ticks/s) / 64 (the bin's columns) / FLOWM_DEPTH; the tent's weights sum to 16.
const FLOWM_TO_VEL : f32 = 30.0 / (FLOWM_ACC_Q * 8.0 * 64.0 * FLOWM_DEPTH * 16.0);

fn flowmSlot(k : vec2<i32>) -> u32 {
  let m = ((k % vec2<i32>(FLOWM_N)) + vec2<i32>(FLOWM_N)) % vec2<i32>(FLOWM_N);
  return u32(m.y * FLOWM_N + m.x);
}
fn flowmTag(k : vec2<i32>) -> u32 {
  return (u32(k.x) & 0x7FFFu) | ((u32(k.y) & 0x7FFFu) << 15u) | 0x80000000u;
}
fn flowmI16(v : i32) -> u32 { return u32(clamp(v, -32767, 32767)) & 0xFFFFu; }

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
  var flux = vec2<f32>(0.0);
  var topE = 0;
  var botE = 0;
  for (var dj = -1; dj <= 1; dj++) {
    for (var di = -1; di <= 1; di++) {
      // A neighbour off the grid would alias the far edge's slot.
      if (i + di < 0 || i + di >= n || j + dj < 0 || j + dj >= n) { continue; }
      let a = flowmSlot(k + vec2<i32>(di, dj)) * 4u;
      let w = f32((2 - abs(di)) * (2 - abs(dj)));
      flux += w * vec2<f32>(f32(bitcast<i32>(flowMeasRW[a])),
                            f32(bitcast<i32>(flowMeasRW[a + 1u])));
      topE = max(topE, bitcast<i32>(flowMeasRW[a + 2u]));
      botE = max(botE, bitcast<i32>(flowMeasRW[a + 3u]));
    }
  }
  let inst = flux * FLOWM_TO_VEL;

  // ---- 2. the node's state, reset if the slot held another node ----
  let s = FLOWM_STATE + flowmSlot(k) * 4u;
  let tag = flowmTag(k);
  var v = vec2<f32>(0.0);
  var band = (u32(FLOW_DRY) & 0xFFFFu);
  if (flowMeasRW[s + 2u] == tag) {
    v = vec2<f32>(bitcast<f32>(flowMeasRW[s]), bitcast<f32>(flowMeasRW[s + 1u]));
    band = flowMeasRW[s + 3u];
  }

  // ---- 3. follow: fast up, slow down ----
  let kf = select(FLOWM_K_FALL, FLOWM_K_RISE, dot(inst, inst) > dot(v, v));
  v = v * kf + inst * (1.0 - kf);
  if (topE != 0) {
    band = flowmI16(topE - FLOWM_Y_BIAS) | (flowmI16(FLOWM_Y_BIAS - botE) << 16u);
  }
  if (dot(v, v) < FLOWM_SNAP * FLOWM_SNAP) {
    v = vec2<f32>(0.0);
    band = u32(FLOW_DRY) & 0xFFFFu;
  }
  flowMeasRW[s] = bitcast<u32>(v.x);
  flowMeasRW[s + 1u] = bitcast<u32>(v.y);
  flowMeasRW[s + 2u] = tag;
  flowMeasRW[s + 3u] = band;

  // ---- 4. publish, flowGrid's layout ----
  let q = vec2<i32>(round(v * FLOW_VEL_Q));
  let g = FLOWM_GRID + FLOW_HEADER + 2u * u32(j * n + i);
  flowMeasRW[g] = flowmI16(q.x) | (flowmI16(q.y) << 16u);
  flowMeasRW[g + 1u] = band;
}
