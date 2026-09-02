/* perfview.js — the tuner's Performance tab.
 *
 * WHAT THIS PAGE IS FOR
 * ---------------------
 * One question: "where did my frame go, and which part of the engine do I fix?"
 * Everything here is subordinate to answering that in the first five seconds of
 * looking at it. It reads two sources, in the SAME shape, so one set of charts
 * draws both:
 *
 *   RECORDED   build/perf.json, written by `sandvox --perf` (src/measure/
 *              perfsuite.cpp). Five scenarios, per-frame CPU scopes, per-frame
 *              GPU pass times, and the counters that explain them.
 *   LIVE       the --telemetry WebSocket, one v2 sample per frame while you
 *              play (src/telemetry.cpp). Task-manager mode.
 *
 * The component taxonomy is NOT defined here. It arrives in the JSON's `nodes`
 * array, generated from src/measure/perfnodes.h, whose ids are the same
 * ARCH_NODES keys the Engine tab's map uses — so a bar on this page and a box on
 * that one are the same thing, and scripts/check_invariants.py fails if they
 * ever stop being.
 *
 * A NOTE ON THE TWO KINDS OF "CPU TIME", because reading them as one is the
 * fastest way to draw a wrong conclusion off this page:
 *
 *   CPU BUSY     work the CPU actually did — encoding, physics, streaming.
 *   CPU WAITING  the `present` scope: the CPU parked in front of a full frame
 *                queue. It is not cost, it is the SHAPE of the bottleneck. A
 *                large `present` means the GPU is the limit and the CPU has
 *                spare capacity.
 *
 * They are drawn differently and never summed into one "CPU" bar.
 *
 * COLOUR. The categorical palette is the eight-slot dark set from the data-viz
 * skill, validated against this page's surface (#1a2029) with
 * scripts/validate_palette.js: all five checks pass, worst adjacent CVD dE 8.4.
 * Hues are bound to NODE IDENTITY through PV_HUE_GPU / PV_HUE_CPU, not to a sort
 * position,
 * so filtering or switching scenario never repaints a component the reader has
 * already learned. Magnitude charts (the component breakdown, the scenario
 * heatmap) use the single-hue sequential ramp instead — a ramp on nominal
 * categories would burn the colour channel re-encoding the bar length.
 */
'use strict';

/* ================= palette ================= */

// Categorical, 8 slots, dark, validated on surface #1a2029.
// ADJACENT-PAIR palette: legal for stacks, bars and lines (which is all this
// page draws). Do not reuse it for a scatter or a choropleth without re-running
// the validator with --pairs all.
const PV_CAT = ['#3987e5', '#d95926', '#199e70', '#c98500',
                '#d55181', '#008300', '#9085e9', '#e66767'];
// Sequential, one hue, monotone lightness — for magnitude, never for identity.
const PV_SEQ = ['#31628f', '#3576ad', '#3a8bcd', '#4b9fe3', '#78b9ef'];
// The tail, and the GPU wait. Both are deliberately NOT a hue: "everything
// else" and "not working" are not series, and giving them a colour would imply
// they are.
const PV_OTHER = '#5c667a';
const PV_WAIT  = '#39445a';

// WHICH COMPONENTS GET A HUE, in fixed order. Eight slots each for the GPU and
// CPU charts; everything past them folds into "Other" — and folds in the STACK
// as well as the legend, which is the part that is easy to get wrong. Drawing
// the top-N by cost as separate bands while colouring by identity means two
// bands that both fell off the hue list render in the same grey and the legend
// lists them twice: two entities, one colour, which is exactly what the legend
// exists to prevent.
//
// Two lists rather than one because ~20 components can appear and eight hues is
// the hard ceiling (a ninth is indistinguishable under CVD). The GPU and CPU
// charts never share a series, so a per-chart assignment is unambiguous, and
// each list is fixed — colour follows the entity, never its rank, so switching
// scenario or filtering never repaints a component the reader has learned.
//
// The order within each list is by how often a component is the answer, not
// alphabetical: the first thing that goes wrong gets the first colour.
const PV_HUE_GPU = [
  'raymarch',      // the render pass; usually the largest single number
  'caLoop',        // the CA
  'fluidSys',      // MLS-MPM
  'particleSys',
  'explode',
  'worldgen',
  'occupancy',
  'waterBodies',
];
const PV_HUE_CPU = [
  'gpuBackend',    // submit + the generated barriers
  'physicsSys',
  'postStep',
  'renderPass',    // draw-call encode
  'readback',
  'readbackStall', // the blocking fallback, split out of readback
  'worldStorage',  // streaming
  'gameSystems',
];
function pvHue(nodeId, side){
  const list = side === 'cpu' ? PV_HUE_CPU : PV_HUE_GPU;
  const i = list.indexOf(nodeId);
  return i >= 0 ? PV_CAT[i] : PV_OTHER;
}
function pvHasHue(nodeId, side){
  return (side === 'cpu' ? PV_HUE_CPU : PV_HUE_GPU).indexOf(nodeId) >= 0;
}

/* ================= state ================= */

// NOTE for anything reaching in from outside (the headless harness, the
// console): these are `let` in a CLASSIC script, so they live in the global
// LEXICAL environment and are NOT properties of `window`. `window.PERF` is
// undefined while `PERF` is fine — which is a genuinely confusing five minutes
// the first time. `window.pvState()` below is the supported way in.
let PERF = null;             // the parsed build/perf.json
let pvScenario = null;       // selected scenario id, or '__live__'
let pvRendered = false;
let pvSock = null;
// The live ring. Sized to about 20 s at 60 fps: long enough to see a hitch in
// context, short enough that the charts redraw in well under a frame.
const PV_LIVE_CAP = 1200;
// `raw` and `nonV2` count what ARRIVED, not what was kept. Without them the
// page cannot tell "the peer is silent" from "the peer is talking and we are
// dropping all of it", and those have opposite causes: a game that never
// reached its frame loop versus an older sandvox still holding the port and
// speaking only the v1 `stages` message. Both look like "waiting for the first
// frame" forever, which is what made this bug take a session to find.
let pvLive = { samples: [], connected: false, status: 'not connected', raf: 0,
               dirty: false, raw: 0, nonV2: 0, sawV2: false, probe: 0 };

/* ---- the rolling-percentile plot -----------------------------------------
 *
 * WHY PERCENTILES AND NOT A SMOOTHED LINE. A moving average of frame time
 * answers "what did the frame cost on average lately", which is the one
 * question that was never in doubt — the mean tile already says it. It also
 * ERASES the thing you are looking for: a 200 ms hitch inside a 60-frame window
 * moves the mean by 3 ms and vanishes. p95 and p99 keep the hitch, because a
 * percentile is order statistics, not a low-pass filter.
 *
 * Each series is the percentile over a TRAILING window ending at that frame, so
 * the line at x is "the distribution of the last `pvPctlWin` frames".
 */
const PV_PCTL_SERIES = [
  { key:'p50', p:0.50, col:PV_CAT[2], label:'p50 (median)',
    tip:'The typical frame. Half of the last N frames were faster than this. '
      + 'A p50 that sits on the 60 fps line with a high p99 is a smooth game '
      + 'with hitches, which is a completely different bug from a slow one.' },
  { key:'p95', p:0.95, col:PV_CAT[3], label:'p95',
    tip:'One frame in twenty is worse than this. This is the number a player '
      + 'feels as "not quite smooth" — it is the first one to move when a '
      + 'system starts occasionally overrunning its budget.' },
  // Red, not the orange next to p95's amber: those two are adjacent hues and at
  // 2px, overlapping, they read as one thickened line. The green/amber/red ramp
  // is also the order of badness, which is what these three actually are.
  { key:'p99', p:0.99, col:PV_CAT[7], label:'p99',
    tip:'One frame in a hundred is worse than this. The hitch. Note the window '
      + 'size below: a p99 over fewer than ~100 frames is arithmetically just '
      + 'the window maximum, and reads far spikier than the real p99.' },
  // Literal, not PV_INK: that const is declared with the chart primitives far
  // below this line, and a `const` referenced above its declaration is a
  // temporal-dead-zone ReferenceError that kills the whole script at load.
  { key:'raw', p:null, col:'var(--fg)', label:'raw frame time',
    tip:'Every individual frame, unsmoothed and unsummarised. Off by default '
      + 'because at 60 fps it is a solid band of ink; turn it on to confirm a '
      + 'percentile line is tracking something real.' },
];
// Which series are drawn. Module state, so it survives the full DOM rebuild
// that every live redraw does.
let pvPctlOn = { p50:true, p95:true, p99:true, raw:false };
// Trailing window, in frames. 120 is the smallest window at which p99 is not
// simply the maximum (see the p99 tip above); the cycle offers wider.
const PV_PCTL_WINS = [60, 120, 300, 600];
let pvPctlWin = 120;

/* ---- WHICH STATISTIC THE COMPONENT BREAKDOWN REPORTS -----------------------
 *
 * The breakdown was mean-only, and a mean is the wrong tool for the thing the
 * page is most often opened to answer. Streaming is the standing example: its
 * mean is 7.9 ms, its median is 0.00 ms and its p99 is 42 ms, because a window
 * shift is all-or-nothing and lands on ONE frame. The mean of that is a number
 * describing no frame that ever happened, and it makes a violent stall look
 * like a moderate constant cost.
 *
 * The rule the tail exposes, which the mean cannot: a component with mean 0.1
 * and p99 30 is a STALL; one with mean 3 and p99 4 is a COST. They need
 * opposite fixes.
 *
 * Module state, so it survives the DOM rebuild every live redraw does.
 */
const PV_STATS = [
  { key:'mean', label:'mean',
    tip:'The arithmetic mean over the window. Right for "what does this cost '
      + 'me per second"; wrong for anything bursty, because a stall on one '
      + 'frame in a hundred is divided by a hundred.' },
  { key:'p50', label:'p50',
    tip:'The median — half the frames were cheaper, half dearer. This is the '
      + 'TYPICAL frame. A component whose p50 is 0.00 and whose mean is large '
      + 'does nothing on most frames and something violent on a few.' },
  { key:'p95', label:'p95',
    tip:'The worst frame in twenty. Roughly once a second at 60 fps, so this '
      + 'is the tier you actually feel as roughness rather than as a number.' },
  { key:'p99', label:'p99',
    tip:'The worst frame in a hundred. This is the hitch — the one that reads '
      + 'as a stutter while the average looks fine. Needs a window of at least '
      + '100 frames to mean anything; below that it is just the maximum.' },
];
let pvStat = 'mean';
const pvStatDef = () => PV_STATS.find(s => s.key === pvStat) || PV_STATS[0];

// Apply the selected statistic to a per-frame series.
function pvStatOf(arr, stat){
  stat = stat || pvStat;
  if (!arr || !arr.length) return 0;
  if (stat === 'mean') return pvMean(arr);
  return pvPctl(arr, stat === 'p50' ? 0.50 : stat === 'p95' ? 0.95 : 0.99);
}
// Same, over the frames whose GPU queries actually resolved. A pending frame
// carries a zero, and under a PERCENTILE a batch of zeros does not merely drag
// the number down the way it does under a mean — it shifts the rank, so a p50
// can land inside the pending block and report 0.00 for a pass that never
// stopped running.
function pvGpuStatOf(V, arr, stat){
  if (!arr || !arr.length) return 0;
  const gv = (V && V.gpuValid) || [];
  if (!gv.length) return pvStatOf(arr, stat);
  const keep = [];
  for (let i = 0; i < arr.length; i++) if (gv[i]) keep.push(arr[i]);
  return keep.length ? pvStatOf(keep, stat) : 0;
}
// PERCENTILES DO NOT ADD UP, and the summed rows have to be honest about it.
// The p99 of a total is NOT the sum of the components' p99s: that would assume
// every component has its worst frame on the SAME frame, which for streaming
// vs the raymarch is close to never. So a summed row is the statistic of the
// PER-FRAME SUM — build the sum series first, then take the percentile of it.
// Under `mean` the two agree exactly (the mean is linear), which is why this
// was invisible while the page was mean-only.
function pvSumSeries(series, keys, gvFilter, V){
  let n = 0;
  for (const k of keys) n = Math.max(n, (series[k] || []).length);
  const out = [];
  for (let i = 0; i < n; i++){
    if (gvFilter && V && V.gpuValid && V.gpuValid.length && !V.gpuValid[i]) continue;
    let s = 0;
    for (const k of keys) s += (series[k] || [])[i] || 0;
    out.push(s);
  }
  return out;
}

/* ---- what each CPU scope actually IS ---------------------------------------
 *
 * The bars are labelled with ARCHITECTURE node names ("Render Pass"), because
 * that is the box on the Engine map. But the thing being timed is a CPU SCOPE
 * (`renderCpu`), and the two are not the same sentence: the node is a system,
 * the scope is the span of the frame loop that ran. Hovering a row should say
 * which span, in cost terms.
 *
 * SOURCE OF TRUTH. The keys are `kPerfScopeKeys` in src/measure/perfnodes.h and
 * scripts/check_invariants.py fails if this map and that array disagree — add a
 * PerfScope enumerator without a line here and the check names it. The text
 * lives here rather than in the header because it is page prose that changes
 * with the page, and putting it in C++ would mean a rebuild AND a multi-minute
 * `--perf` re-record before a typo fix showed up.
 */
const PV_SCOPE_NOTE = {
  input:     'Key/mouse edge detection, the camera, the character screen and '
           + 'hotbar, and player.Update — movement integration plus the voxel '
           + 'collision sweep against the CPU mirror. Per FRAME, not per tick. '
           + 'This row used to be the frame RESIDUAL rather than a measurement, '
           + 'so every unmeasured span in the engine reported here: flying spiked '
           + 'it to 30 ms because the toroidal window shift had no timer. The '
           + 'residual is `other` now.',
  stream:    'Toroidal window shift, chunk fetch/evict and page fills. Costs '
           + 'nothing standing still and everything while flying — this is the '
           + 'row that moves when you cross a chunk boundary.',
  gameLogic: 'Brush, spell, melee and item drivers deciding what to emit this '
           + 'tick. Scales with what the player is doing, not with the world.',
  waterBody: 'The CPU half of the lake registry (WaterBodySystem::Tick). Costs '
           + 'per BODY, not per water voxel, and records nothing at all while '
           + 'sim.waterBodyMode is 0.',
  upload:    'Assembling the MutationQueue op stream — TickParams, the wind and '
           + 'current primitives — and WriteBuffer-ing it. Scales with the '
           + 'number of ops; the budget is <1 MB/tick.',
  pageTableCpu:
             'The CPU half of the page table, inside SubmitTick: the conservative '
           + 'dirty mirror (BeginTick, the op contributors, TightenFromSnapshot), '
           + 'Materialize — which is Classify plus the allocator — then '
           + 'ConsumeOccupancy and RetirePages. Unlike the CA this does NOT '
           + 'sleep: it runs every tick whether or not anything moved, so it is '
           + 'the block that can be expensive on a frame where you are standing '
           + 'still. It also contains the free-confirmation probe, which issues '
           + 'its own vkQueueSubmit and memcpys up to 2 MiB out of a mapped '
           + 'staging buffer.',
  encode:    'Simulation::EncodeTick — walking the pass table and recording '
           + 'dispatches, plus the readback copies. This is CPU time spent '
           + 'describing GPU work, so it grows with the number of PASSES, not '
           + 'with how much they do.',
  submit:    'CommandEncoder::Finish (which runs the barrier generator over the '
           + 'recorded rows) and vkQueueSubmit. NOTHING ELSE — this row used to '
           + 'be the whole of SubmitTick, which is why it could read 30 ms on an '
           + 'idle frame with no way to tell which of its seven parts moved.',
  physics:   'The Jolt step: rigid bodies, ragdolls, character controllers. '
           + 'Scales with awake body count — sleeping bodies are free.',
  postStep:  'Debris, mob and avatar post-step plus player push-out. Runs after '
           + 'physics because it reads the solved transforms.',
  readback:  'Snapshot map callbacks and CPU-mirror rebuild. One tick latent by '
           + 'design and NON-BLOCKING by construction: this is a fence status '
           + 'poll plus a ~0.5 MB memcpy. The blocking wait that used to hide in '
           + 'this row is `readbackStall` now.',
  readbackStall:
             'BLOCKING. The paged mirror refuses to tick on a snapshot more than '
           + '4 ticks old (its dirty set dilates a ring per tick of gap) and '
           + 'waits on the oldest in-flight readback fence instead. It fires '
           + 'when the GPU is more than a frame behind while the loop catches '
           + 'up at 4 ticks/frame, so it is almost always a relabelled GPU '
           + 'wait — the CPU would have spent the same time in `present`. Read '
           + 'the GPU bars for the cause; the `snapshot stalls` and `readback '
           + 'ring full` counters say whether the ring or the GPU was the limit.',
  audio:     'Cue dispatch and feeding the spatializer. Fixed small cost; '
           + 'silent in headless.',
  renderCpu: 'Encoding the render pass: draw calls, instance buffers and the '
           + 'overlay. This is the CPU DESCRIBING the frame — the GPU time it '
           + 'produces shows up in the GPU bars, not here.',
  present:   'AcquireFrame and Present. THIS IS WHERE VSYNC WAITS LAND, so it '
           + 'is a wait and not work: it is excluded from CPU busy and drawn '
           + 'separately. Large here means the GPU is the limit.',
  other:     'THE RESIDUAL — frame wall clock that no scope above claimed. Not a '
           + 'system, and it has no box on the Engine map on purpose. A few '
           + 'tenths is measurement overhead. A large or spiky value means real '
           + 'work is running outside every timer, and the fix is to add a span '
           + 'where it is running, not to guess which system it belongs to.',
};
function pvScopeNote(key){ return PV_SCOPE_NOTE[key] || ''; }

// The fixed stat tiles. Same reasoning as the scopes: a number with a unit is
// not self-explanatory, and "CPU waiting" in particular is routinely read as a
// cost when it is the opposite.
const PV_TILE_NOTE = {
  'Frame time (mean)':
    'Mean wall-clock ms per frame over the whole run, and the fps that implies. '
    + 'The mean is the wrong number for judging smoothness — see the percentile '
    + 'plot below, and the p99 tile beside this one.',
  'Frame time (p99)':
    'The worst 1% of frames. A p99 far above the mean is a game that feels '
    + 'janky while averaging fine, and it is almost always a different cause '
    + 'from a high mean.',
  'GPU work':
    'Summed GPU pass time per frame, from timestamps on the real dispatches. '
    + 'Compare against the frame: if this fills the frame, the GPU is the limit.',
  'CPU busy':
    'Summed CPU scope time per frame, EXCLUDING the present wait. This is work '
    + 'the CPU actually did.',
  'CPU waiting':
    'The CPU parked in front of a full frame queue. NOT a cost and never summed '
    + 'with CPU busy — it is the SHAPE of the bottleneck. Large here means the '
    + 'GPU is the limit and the CPU has headroom.',
  'Page faults':
    'A page-table store that found no page to write to. This is a correctness '
    + 'bug — voxels were dropped — not a performance number.',
  worst:
    'The single slowest frame in the run. Useful as a bound, useless as a '
    + 'summary: one frame is not a distribution, and a worst frame at startup '
    + 'usually means shader compilation rather than anything about the engine.',
};
// The p50/p95/p99 TILES and the p50/p95/p99 LINES are the same statistic over
// different domains — whole run versus trailing window — so they share one
// explanation rather than drifting into two descriptions of one number.
for (const s of PV_PCTL_SERIES) if (s.p != null) PV_TILE_NOTE[s.key] = s.tip;

// The supported window into this file's state, for the headless harness
// (assets/perfview_test.html) and for poking at it from the console.
window.pvState = () => ({ PERF, scenario: pvScenario, live: pvLive,
                          connected: pvLive.connected,
                          pctlOn: pvPctlOn, pctlWin: pvPctlWin,
                          winSec: pvWinSec, stat: pvStat,
                          tipVisible: !!(pvTipEl && pvTipEl.style.display === 'block') });
// Harness/console hook: switch the component breakdown's statistic and report
// what it now reads, so a test can assert that p99 actually differs from the
// mean rather than merely that the button exists.
window.pvSetStat = k => {
  if (!PV_STATS.some(s => s.key === k)) return null;
  pvStat = k;
  renderPerformance();
  const V = pvView();
  if (!V) return null;
  const W = pvRecent(V, pvWinSec);
  const T = pvTotals(W, pvStat);
  return { stat: pvStat, wallMs: T.wallMs, gpuMs: T.gpuMs,
           cpuBusyMs: T.cpuBusyMs, waitMs: T.waitMs,
           cpu: T.cpuBusy.map(c => ({ scope:c.scope, ms:c.ms })),
           gpu: T.gpu.map(g => ({ id:g.id, ms:g.ms })) };
};
window.pvSetScenario = id => { pvScenario = id; renderPerformance(); };
// Harness/console hook: flip a percentile series without synthesising a click.
window.pvTogglePctl = k => { pvPctlOn[k] = !pvPctlOn[k]; renderPerformance(); };
// Harness/console hook: set the averaging window and report what the numbers
// were actually computed from. Returns the window's own description of itself
// plus the frame count, so a test can assert that a 0.5 s window really did
// average fewer frames than the whole ring rather than merely not crashing.
window.pvSetWindow = sec => {
  pvWinSec = sec;
  renderPerformance();
  const V = pvView();
  if (!V) return null;
  const W = pvRecent(V, pvWinSec);
  return { winSec: pvWinSec, label: pvWinLabel(W),
           frames: (W.wall||[]).length, ringFrames: (V.wall||[]).length,
           windowed: !!W.windowed, wallMs: pvMean(W.wall),
           gpuMs: pvTotals(W).gpuMs };
};

/* ================= tiny DOM + SVG helpers ================= */

function pvEl(tag, attrs, ...kids){
  const e = document.createElement(tag);
  for (const k in (attrs||{})){
    if (k === 'class') e.className = attrs[k];
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), attrs[k]);
    else if (attrs[k] != null) e.setAttribute(k, attrs[k]);
  }
  for (const c of kids) if (c != null) e.append(c);
  return e;
}
const PV_NS = 'http://www.w3.org/2000/svg';
function pvS(tag, attrs, ...kids){
  const e = document.createElementNS(PV_NS, tag);
  for (const k in (attrs||{})) if (attrs[k] != null) e.setAttribute(k, attrs[k]);
  for (const c of kids) if (c != null) e.append(c);
  return e;
}
function pvNum(v, dp){
  if (!isFinite(v)) return '—';
  const d = dp != null ? dp : (v >= 100 ? 0 : v >= 10 ? 1 : v >= 1 ? 2 : 3);
  return v.toFixed(d);
}
function pvInt(v){
  if (!isFinite(v)) return '—';
  return Math.round(v).toLocaleString('en-US');
}
function pvPct(v){ return (v*100).toFixed(v >= 0.1 ? 0 : 1) + '%'; }
// Magnitude with a suffix, for COUNTS rather than durations. The raymarch's
// step counters run to tens of millions per frame, and pvInt renders that as
// eleven characters of digits and commas in a column that has to be readable
// at a glance -- "12.4 M" is the same number and is one token.
function pvMag(v){
  if (!isFinite(v)) return '—';
  const a = Math.abs(v);
  if (a >= 1e6) return pvNum(v/1e6, a/1e6 >= 100 ? 0 : 1) + ' M';
  if (a >= 1e3) return pvNum(v/1e3, a/1e3 >= 100 ? 0 : 1) + ' k';
  return pvInt(v);
}

/* ================= data model =================
 *
 * ONE shape for both sources. `pvView()` returns the currently-selected
 * scenario or the live ring, already normalised to:
 *
 *   { id, label, desc, note, frames, wall[], cpu{key:[]}, gpu{node:[]},
 *     counters{key:[]}, gpuValid[], live }
 *
 * so nothing below this line knows or cares which one it is drawing.
 */

function pvNodes(){ return (PERF && PERF.nodes) || []; }
function pvNodeById(id){ return pvNodes().find(n => n.id === id) || null; }
// Labels for the scopes that deliberately have NO architecture node — right
// now just the residual. `other` is not a system, so giving it an ARCH_NODES
// entry would put an orphan box on the Engine map; it still needs a bar label
// that reads as English rather than as a JSON key.
const PV_NODELESS_LABEL = { other: 'Unattributed' };
// Labels for nodes that DO exist in perfnodes.h but may be missing from the
// `nodes` table this page was handed. That table is baked into build/perf.json
// at record time, so a perf.json recorded before a node was split out -- or a
// live session attached while perf.json is stale -- arrives with ids the table
// has never heard of, and the bar renders with a camelCase key for a label.
// These are a FALLBACK only: the recorded table always wins, so this map can
// never fight the source of truth, it can only cover for its absence.
const PV_NODE_FALLBACK = {
  raymarch:      'World raymarch',
  drawParticles: 'Particles + fluid cubes',
  drawBodies:    'Debris bodies',
  drawMicro:     'Micro bodies',
  drawSprites:   'Sprites',
  drawDebug:     'Debug draws',
  uiOverlay:     'Overlay + swapchain wait',
  readbackStall: 'Snapshot Stall',
};
function pvNodeLabel(id){
  const n = pvNodeById(id);
  return n ? n.label
           : (PV_NODELESS_LABEL[id] || PV_NODE_FALLBACK[id] || id);
}
function pvScopeNode(key){
  const s = ((PERF && PERF.scopes) || []).find(x => x.key === key);
  return s ? s.node : null;
}
function pvCounterDef(key){
  return ((PERF && PERF.counters) || []).find(c => c.key === key) || null;
}

// The live ring, reshaped into the recorded form. Built fresh per redraw: at
// 1,200 samples x ~20 keys this is a few hundred microseconds and it keeps the
// draw path identical between live and recorded, which is worth far more than
// the microseconds.
function pvLiveView(){
  const S = pvLive.samples;
  const cpu = {}, gpu = {}, counters = {};
  const wall = [], gpuValid = [];
  for (const s of S){
    wall.push(s.wallMs || 0);
    gpuValid.push(s.gpuValid ? 1 : 0);
    for (const k in (s.cpu||{})) (cpu[k] = cpu[k] || []);
    for (const k in (s.gpu||{})) (gpu[k] = gpu[k] || []);
    for (const k in (s.counters||{})) (counters[k] = counters[k] || []);
  }
  // Second pass so every array is the same length — a key that appears on
  // frame 400 must still be index-aligned with `wall`.
  for (let i = 0; i < S.length; i++){
    for (const k in cpu) cpu[k][i] = (S[i].cpu && S[i].cpu[k]) || 0;
    for (const k in gpu) gpu[k][i] = (S[i].gpu && S[i].gpu[k]) || 0;
    for (const k in counters) counters[k][i] = (S[i].counters && S[i].counters[k]) || 0;
  }
  return {
    id: '__live__', label: 'Live session', live: true,
    desc: 'The running game, one sample per rendered frame. The x axis is the '
        + 'last ' + S.length + ' frames of wall clock, not sim time.',
    note: pvLive.status,
    frames: S.length, wall, cpu, gpu, counters, gpuValid,
  };
}

function pvView(){
  if (pvScenario === '__live__') return pvLiveView();
  if (!PERF) return null;
  const sc = PERF.scenarios.find(s => s.id === pvScenario);
  if (!sc || sc.skipped) return sc ? { ...sc, frames: 0, skipped: true } : null;
  const S = sc.series;
  return {
    id: sc.id, label: sc.label, desc: sc.desc, note: sc.note,
    frames: sc.frames, live: false,
    wall: S.wallMs, cpu: S.cpu, gpu: S.gpu, counters: S.counters,
    gpuValid: S.gpuValid, passes: sc.passes, unattributed: sc.unattributed,
    worldHash: sc.worldHash, stresses: sc.stresses,
  };
}

const pvMean = a => (a && a.length) ? a.reduce((x,y)=>x+y,0)/a.length : 0;
const pvSum  = a => (a && a.length) ? a.reduce((x,y)=>x+y,0) : 0;
function pvPctl(a, p){
  if (!a || !a.length) return 0;
  const v = a.slice().sort((x,y)=>x-y);
  const i = p * (v.length - 1), lo = Math.floor(i), hi = Math.min(lo+1, v.length-1);
  return v[lo] * (1 - (i-lo)) + v[hi] * (i-lo);
}

/* ---- the rolling window --------------------------------------------------
 *
 * WHY EVERY NUMBER ON THE PAGE IS A TRAILING MEAN AND NOT A RUNNING ONE.
 *
 * The live ring holds PV_LIVE_CAP frames — about 20 seconds. Every ms figure
 * on this page used to be the mean over ALL of it, which makes the numbers
 * useless for the thing you use a live view for: you walk into a fire, the
 * raymarch doubles, and the tile creeps up over the next twenty seconds and
 * never actually reaches the new value. A twenty-second mean of a world that
 * changed four seconds ago is a number about neither world.
 *
 * So the SUMMARY numbers (tiles, verdict, component bars, counter means) are a
 * mean over the last `pvWinSec` seconds, and the CHARTS still draw the whole
 * ring. That split is the point: a chart is history and wants every frame, a
 * tile answers "what is it doing NOW" and wants a short window.
 *
 * THE WINDOW IS WALL CLOCK, NOT A FRAME COUNT. A fixed 60 frames is one second
 * at 60 fps, three at 20, and half at 120 — so the window would silently get
 * longer exactly when the game got slower, which is when you are watching. It
 * walks back summing `wallMs` instead, so "1 second" means one second at any
 * frame rate.
 */
const PV_WIN_SECS = [0.5, 1, 2, 5, 0];   // 0 = the whole ring
let pvWinSec = 1;

// The trailing slice of a view covering ~`seconds` of wall clock. Returns the
// view unchanged for a recorded scenario (the whole run IS the measurement —
// there is no "now" to be near) or when the ring is shorter than the window.
function pvRecent(V, seconds){
  if (!V || !V.live || !(seconds > 0)) return V;
  const wall = V.wall || [];
  const n = wall.length;
  if (!n) return V;
  let ms = 0, from = n - 1;
  for (; from > 0; from--){
    ms += wall[from] || 0;
    if (ms >= seconds * 1000) break;
  }
  // A GPU timestamp rides a fence ring and lands two or three frames late, so
  // the newest frames routinely carry the CPU half only. A window that happens
  // to contain no resolved frame would report "the GPU cost nothing", which is
  // the one thing perfnodes.h says this page must never draw. Widen until it
  // holds a few real ones — better a slightly longer window than a false zero.
  const gv = V.gpuValid || [];
  if (gv.length){
    let valid = 0;
    for (let i = from; i < n; i++) if (gv[i]) valid++;
    while (from > 0 && valid < 4){ from--; if (gv[from]) valid++; ms += wall[from] || 0; }
  }
  if (from <= 0) return V;
  const cut = o => { const r = {}; for (const k in o) r[k] = o[k].slice(from); return r; };
  return { ...V, wall: wall.slice(from), cpu: cut(V.cpu||{}), gpu: cut(V.gpu||{}),
           counters: cut(V.counters||{}), gpuValid: gv.slice(from),
           windowed: true, windowMs: ms, windowFrames: n - from };
}

// How the window describes itself, for the labels that must not lie about
// which frames they averaged.
function pvWinLabel(V){
  // Phrased to read after "a mean over the ...", which is the only place it
  // appears in prose. "over the all 33 frames" is what the obvious wording
  // produced.
  if (!V || !V.live) return 'whole run';
  if (!V.windowed) return 'whole ring (' + (V.wall||[]).length + ' frames)';
  return 'last ' + pvNum(V.windowMs/1000, 1) + ' s · ' + V.windowFrames + ' frames';
}

// Mean over the frames whose GPU queries actually resolved. Averaging a
// pending frame in as a zero drags every GPU bar down, and the shorter the
// window the worse it gets — at one second the two or three unresolved frames
// at the head are 5% of the sample instead of 0.2%.
function pvGpuMean(V, arr){
  if (!arr || !arr.length) return 0;
  const gv = V.gpuValid || [];
  if (!gv.length) return pvMean(arr);
  let s = 0, n = 0;
  for (let i = 0; i < arr.length; i++){ if (!gv[i]) continue; s += arr[i]; n++; }
  return n ? s / n : 0;
}

// Per-frame ms for every GPU node and every CPU scope under the chosen
// statistic, plus the totals. `V` is already windowed by the caller.
//
// `stat` defaults to 'mean' rather than to the selected one ON PURPOSE: the
// VERDICT panel calls this too, and a bottleneck judgement must rest on typical
// behaviour, not on the tail. Reading "the GPU is the bottleneck" off a p99
// would let one hitch a second relabel a CPU-bound engine.
function pvTotals(V, stat){
  stat = stat || 'mean';
  const gpu = [], cpuBusy = [];
  let waitMs = 0;
  const gpuKeys = [], cpuKeys = [];
  for (const k in (V.gpu||{})){
    const m = pvGpuStatOf(V, V.gpu[k], stat);
    if (m <= 0) continue;
    gpu.push({ id: k, label: pvNodeLabel(k), ms: m });
    gpuKeys.push(k);
  }
  for (const k in (V.cpu||{})){
    const m = pvStatOf(V.cpu[k], stat);
    if (m <= 0) continue;
    // `present` is the wait, not work. Kept out of the busy total on purpose —
    // see the header comment.
    if (k === 'present'){ waitMs += m; continue; }
    cpuBusy.push({ id: pvScopeNode(k) || k, scope: k,
                   label: pvNodeLabel(pvScopeNode(k) || k), ms: m });
    cpuKeys.push(k);
  }
  gpu.sort((a,b)=>b.ms-a.ms);
  cpuBusy.sort((a,b)=>b.ms-a.ms);
  // Statistic of the per-frame SUM, never the sum of the statistics — see
  // pvSumSeries. Identical to the old behaviour under 'mean'.
  const gpuMs = pvStatOf(pvSumSeries(V.gpu||{}, gpuKeys, true, V), stat);
  const cpuBusyMs = pvStatOf(pvSumSeries(V.cpu||{}, cpuKeys, false, V), stat);
  return { gpu, cpuBusy, gpuMs, cpuBusyMs, waitMs, stat,
           wallMs: pvStatOf(V.wall, stat) };
}

// THE VERDICT. Deliberately a rule with a stated threshold rather than a feel:
// the GPU is the bottleneck when the CPU spends more of the frame waiting than
// working, and the margin has to be real (20% of the frame) before the page
// commits to saying so.
function pvVerdict(T){
  const frame = T.wallMs || 1;
  if (T.waitMs > T.cpuBusyMs && T.waitMs / frame > 0.2)
    return { bound: 'GPU', tone: 'amber',
             why: 'the CPU spends ' + pvPct(T.waitMs/frame) + ' of the frame '
                + 'waiting on the GPU' };
  if (T.cpuBusyMs / frame > 0.6)
    return { bound: 'CPU', tone: 'red',
             why: 'CPU work fills ' + pvPct(T.cpuBusyMs/frame) + ' of the frame '
                + 'with the GPU idle for part of it' };
  return { bound: 'BALANCED', tone: 'green',
           why: 'neither side dominates: ' + pvNum(T.cpuBusyMs) + ' ms CPU work, '
              + pvNum(T.gpuMs) + ' ms GPU work' };
}

/* ================= chart primitives =================
 *
 * Plain SVG, no dependencies. The mark specs are fixed here rather than at each
 * call site so every chart on the page looks like the same instrument:
 * 2px lines, 4px rounded data-ends, hairline recessive axes, a 2px surface gap
 * between touching fills, >=8px hover targets.
 */

const PV_INK   = 'var(--fg)';
const PV_INK2  = 'var(--dim)';
const PV_INK3  = 'var(--faint)';
const PV_LINE  = 'var(--line)';
const PV_SURF  = 'var(--card)';
const PV_GAP   = 2;      // the surface gap between touching fills

function pvChartFrame(w, h, pad){
  const svg = pvS('svg', { width:'100%', height:h, viewBox:`0 0 ${w} ${h}`,
                           preserveAspectRatio:'none', class:'pv-svg' });
  return svg;
}

// Y gridlines at clean values, drawn one step off the surface and hairline.
function pvGridY(svg, x0, x1, y, ticks, fmt){
  for (const t of ticks){
    const yy = y(t.v);
    svg.append(pvS('line', { x1:x0, x2:x1, y1:yy, y2:yy, stroke:PV_LINE,
                             'stroke-width':1, 'shape-rendering':'crispEdges' }));
    svg.append(pvS('text', { x:x0-6, y:yy+3.5, 'text-anchor':'end',
                             fill:PV_INK3, 'font-size':10,
                             'font-family':'var(--mono)' }, fmt(t.v)));
  }
}
function pvTicks(max, n){
  if (!(max > 0)) return [{v:0}];
  const raw = max / n;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const step = [1,2,2.5,5,10].find(s => s*mag >= raw) * mag;
  const out = [];
  for (let v = 0; v <= max*1.0001; v += step) out.push({ v });
  return out;
}

/* ---- ROLLING PERCENTILES over time -----------------------------------------
 *
 * One point every `stride` frames, each the percentile of the trailing
 * `win` frames. Strided because the chart is 1000 px wide and a sort per frame
 * over a 600-frame window, redrawn at 10 Hz in live mode, would make the tuner
 * the most expensive process on the machine — which for a page whose whole job
 * is measuring cost is not an acceptable way to be wrong.
 */
function pvRollingPctl(arr, win, p, stride){
  const n = arr.length, out = [];
  if (!n) return out;
  for (let i = 0; i < n; i += stride){
    const lo = Math.max(0, i - win + 1);
    out.push([i, pvPctl(arr.slice(lo, i + 1), p)]);
  }
  // Always land exactly on the last frame: in live mode the right-hand edge is
  // "now", and a chart whose newest point is up to `stride` frames stale reads
  // as the game having just got faster.
  if (out.length && out[out.length-1][0] !== n-1)
    out.push([n-1, pvPctl(arr.slice(Math.max(0, n-win)), p)]);
  return out;
}

/* ---- MULTI-LINE chart ------------------------------------------------------
 * Series are [frameIndex, value] pairs so every line shares the timeline
 * chart's x domain (0..frames-1) regardless of its own sampling stride.
 */
function pvLineChart(V, opts){
  const w = 1000, h = opts.height || 220, padL = 52, padR = 12, padT = 12, padB = 24;
  const n = V.frames;
  const svg = pvChartFrame(w, h);
  if (!n) return svg;

  let maxY = 0;
  for (const s of opts.lines) for (const pt of s.pts) if (pt[1] > maxY) maxY = pt[1];
  maxY = Math.max(maxY, 20);            // keep the 30 fps line on screen
  const X = i => padL + (n<=1 ? 0 : i*(w-padL-padR)/(n-1));
  const Y = v => h - padB - (Math.min(v,maxY)/maxY)*(h-padT-padB);

  pvGridY(svg, padL, w-padR, Y, pvTicks(maxY, 4), v => pvNum(v,0));

  for (const b of [{ms:16.67,l:'60 fps'},{ms:33.33,l:'30 fps'}]){
    if (b.ms > maxY) continue;
    const yy = Y(b.ms);
    svg.append(pvS('line', { x1:padL, x2:w-padR, y1:yy, y2:yy, stroke:PV_INK2,
                             'stroke-width':1, 'stroke-dasharray':'4 4' }));
    svg.append(pvS('text', { x:w-padR-2, y:yy-4, 'text-anchor':'end',
                             fill:PV_INK2, 'font-size':10,
                             'font-family':'var(--mono)' }, b.l));
  }

  for (const s of opts.lines){
    if (!s.pts.length) continue;
    let d = 'M' + X(s.pts[0][0]) + ' ' + Y(s.pts[0][1]);
    for (let i=1;i<s.pts.length;i++) d += ' L' + X(s.pts[i][0]) + ' ' + Y(s.pts[i][1]);
    const path = pvS('path', { d, fill:'none', stroke:s.col,
                               'stroke-width': s.thin ? 1 : 2,
                               'stroke-opacity': s.thin ? 0.45 : 0.95,
                               'stroke-linejoin':'round', 'stroke-linecap':'round',
                               class:'pv-pctl-line' });
    // A <title> child is the SVG tooltip; the legend below carries the numbers.
    path.append(pvS('title', {}, s.label + (s.tip ? ' — ' + s.tip : '')));
    svg.append(path);
  }

  const xlab = V.live ? 'frames ago' : 'sim time (s)';
  svg.append(pvS('text', { x:padL, y:h-6, fill:PV_INK3, 'font-size':10,
                           'font-family':'var(--mono)' },
                 V.live ? '-' + n : '0'));
  svg.append(pvS('text', { x:(padL+w-padR)/2, y:h-6, 'text-anchor':'middle',
                           fill:PV_INK3, 'font-size':10 }, xlab));
  svg.append(pvS('text', { x:w-padR, y:h-6, 'text-anchor':'end', fill:PV_INK3,
                           'font-size':10, 'font-family':'var(--mono)' },
                 V.live ? 'now' : pvNum(n/30, 1)));
  return svg;
}

/* ---- STACKED AREA over time ------------------------------------------------
 *
 * The frame timeline. Series are the top-7 components by mean plus "Other", so
 * the colour count never exceeds the palette (a generated 9th hue is
 * indistinguishable under CVD, so the tail folds instead).
 *
 * The budget lines at 16.7 and 33.3 ms are the point of the chart: a stacked
 * area that crosses 33.3 is a frame that missed 30 fps, and that is readable at
 * a glance in a way a number is not.
 */
function pvStackedArea(V, opts){
  const w = 1000, h = opts.height || 240, padL = 52, padR = 12, padT = 12, padB = 24;
  const n = V.frames;
  const svg = pvChartFrame(w, h);
  if (!n) return svg;

  // SERIES SELECTION IS BY IDENTITY, NOT BY RANK. A series is drawn as its own
  // band only if it has a hue; everything else sums into "Other". Selecting the
  // top-N by cost instead would put two hueless components on screen as two
  // separate grey bands with two legend rows — indistinguishable, and worse
  // than honestly labelling them as a tail.
  const side = opts.side || 'gpu';
  const idOf = k => opts.nodeOf ? opts.nodeOf(k) : k;
  const keys = Object.keys(opts.series)
                     .filter(k => pvMean(opts.series[k]) > 0);
  const ranked = keys.map(k => ({ k, m: pvMean(opts.series[k]) }))
                     .sort((a,b)=>b.m-a.m);
  const shown = ranked.filter(x => pvHasHue(idOf(x.k), side)).map(x => x.k);
  const tail  = ranked.filter(x => !pvHasHue(idOf(x.k), side)).map(x => x.k);

  // Stack, bottom-up, biggest first so the noisy small stuff rides on top where
  // its wobble does not displace everything below it.
  const stack = shown.map(k => ({ k, id: idOf(k), v: opts.series[k] }));
  if (tail.length){
    const t = new Array(n).fill(0);
    for (const k of tail) for (let i=0;i<n;i++) t[i] += opts.series[k][i]||0;
    stack.push({ k:'__other__', id:'__other__', v:t,
                 label:'Other: ' + tail.map(k => pvNodeLabel(idOf(k))).join(', ') });
  }
  // NOT STACKED: the CPU's wait and the GPU's work are CONCURRENT -- the CPU
  // waits WHILE the GPU runs -- so stacking them adds a frame to itself. The
  // first version did, and drew a 71 ms axis for a 36.7 ms frame: a chart that
  // was wrong by a factor of two and looked entirely plausible. The frame time
  // rides as an overlay LINE instead, which is the honest comparison: "here is
  // the GPU work, and here is the frame it has to fit inside."

  let maxY = 0;
  const cum = new Array(n).fill(0);
  for (const s of stack) for (let i=0;i<n;i++){ cum[i]+=s.v[i]||0; if(cum[i]>maxY)maxY=cum[i]; }
  const wallLine = opts.wall || null;
  if (wallLine) for (const v of wallLine) if (v > maxY) maxY = v;
  // The 20 ms floor exists so the 30 fps budget line is always on screen, and
  // it belongs ONLY to the chart that draws those lines. Applied to the CPU
  // chart it flattened a 0.5 ms trace against a 20 ms axis into a straight line
  // at zero -- a chart that renders, passes every DOM assertion, and shows
  // nothing.
  if (opts.budgets !== false) maxY = Math.max(maxY, 20);
  maxY = Math.max(maxY, 1e-3);
  const X = i => padL + (n<=1 ? 0 : i*(w-padL-padR)/(n-1));
  const Y = v => h - padB - (v/maxY)*(h-padT-padB);

  pvGridY(svg, padL, w-padR, Y, pvTicks(maxY, 4), v => pvNum(v,0));

  // Bands, drawn bottom-up. The 2px surface gap between them is the separator —
  // no strokes, which would add ink that is not data.
  const base = new Array(n).fill(0);
  for (const s of stack){
    const top = base.map((b,i) => b + (s.v[i]||0));
    let d = 'M' + X(0) + ' ' + Y(top[0]);
    for (let i=1;i<n;i++) d += ' L' + X(i) + ' ' + Y(top[i]);
    for (let i=n-1;i>=0;i--) d += ' L' + X(i) + ' ' + Y(base[i]);
    d += ' Z';
    const col = s.k==='__other__' ? PV_OTHER : s.k==='__wait__' ? PV_WAIT
                                             : pvHue(s.id, side);
    svg.append(pvS('path', { d, fill:col, 'fill-opacity': s.k==='__wait__'?0.5:0.85,
                             stroke:PV_SURF, 'stroke-width':PV_GAP,
                             'stroke-linejoin':'round' }));
    for (let i=0;i<n;i++) base[i] = top[i];
  }

  // Frame time, as a 2px line over the bands. Ink, not a hue: it is the same
  // measure the bands sum toward, not another series competing with them.
  if (wallLine){
    let wd = 'M' + X(0) + ' ' + Y(Math.min(wallLine[0], maxY));
    for (let i=1;i<n;i++) wd += ' L' + X(i) + ' ' + Y(Math.min(wallLine[i], maxY));
    svg.append(pvS('path', { d:wd, fill:'none', stroke:PV_INK, 'stroke-width':2,
                             'stroke-linejoin':'round', 'stroke-opacity':0.9 }));
  }

  // Frame budgets. Solid hairlines in ink, labelled at the right — they are
  // reference, not data, so they get no hue.
  for (const b of (opts.budgets === false ? []
                   : [{ms:16.67,l:'60 fps'},{ms:33.33,l:'30 fps'}])){
    if (b.ms > maxY) continue;
    const yy = Y(b.ms);
    svg.append(pvS('line', { x1:padL, x2:w-padR, y1:yy, y2:yy, stroke:PV_INK2,
                             'stroke-width':1, 'stroke-dasharray':'4 4' }));
    svg.append(pvS('text', { x:w-padR-2, y:yy-4, 'text-anchor':'end',
                             fill:PV_INK2, 'font-size':10,
                             'font-family':'var(--mono)' }, b.l));
  }

  // X axis: sim seconds for a recorded run (one frame = one 30 Hz tick), frame
  // index for live (where a frame is whatever the machine managed).
  const xlab = V.live ? 'frames ago' : 'sim time (s)';
  svg.append(pvS('text', { x:padL, y:h-6, fill:PV_INK3, 'font-size':10,
                           'font-family':'var(--mono)' },
                 V.live ? '-' + n : '0'));
  svg.append(pvS('text', { x:(padL+w-padR)/2, y:h-6, 'text-anchor':'middle',
                           fill:PV_INK3, 'font-size':10 }, xlab));
  svg.append(pvS('text', { x:w-padR, y:h-6, 'text-anchor':'end', fill:PV_INK3,
                           'font-size':10, 'font-family':'var(--mono)' },
                 V.live ? 'now' : pvNum(n/30, 1)));

  pvAttachCrosshair(svg, { w, h, padL, padR, padT, padB, n, X, stack, V, side });
  return svg;
}

/* ---- crosshair + tooltip ---------------------------------------------------
 * An HTML chart IS interactive; the hover layer ships by default. The hit
 * target is the whole plot rect, not the marks, so there is nothing to miss.
 */
function pvAttachCrosshair(svg, C){
  const line = pvS('line', { y1:C.padT, y2:C.h-C.padB, stroke:PV_INK2,
                             'stroke-width':1, visibility:'hidden' });
  svg.append(line);
  const hit = pvS('rect', { x:C.padL, y:C.padT, width:C.w-C.padL-C.padR,
                            height:C.h-C.padT-C.padB, fill:'transparent' });
  svg.append(hit);
  const tip = pvTooltip();
  hit.addEventListener('mousemove', ev => {
    const r = svg.getBoundingClientRect();
    const fx = (ev.clientX - r.left) / r.width * C.w;   // viewBox is 0..w
    let i = Math.round((fx - C.padL) / Math.max(1e-6, (C.w-C.padL-C.padR)) * (C.n-1));
    i = Math.max(0, Math.min(C.n-1, i));
    line.setAttribute('x1', C.X(i)); line.setAttribute('x2', C.X(i));
    line.setAttribute('visibility','visible');
    const rows = C.stack.map(s => ({
      label: s.label || pvNodeLabel(s.id),
      col: s.k==='__other__' ? PV_OTHER : s.k==='__wait__' ? PV_WAIT
                                        : pvHue(s.id, C.side),
      v: s.v[i]||0 })).filter(r => r.v > 0.001).reverse();
    const head = C.V.live ? 'frame -' + (C.n-1-i)
                          : 't = ' + pvNum(i/30, 2) + ' s';
    // The tooltip's total is the WALL clock for that frame, not the sum of the
    // bands: the bands are GPU work, and GPU work is not the frame.
    const total = C.V.wall ? C.V.wall[i] : rows.reduce((a,b)=>a+b.v,0);
    pvTipShow(tip, ev, head, rows, total,
              C.V.gpuValid && C.V.gpuValid[i] === 0
                ? 'GPU timings for this frame had not landed yet' : null);
  });
  hit.addEventListener('mouseleave', () => {
    line.setAttribute('visibility','hidden'); tip.style.display='none';
  });
}

let pvTipEl = null;
function pvTooltip(){
  if (!pvTipEl){
    pvTipEl = pvEl('div', { class:'pv-tip' });
    document.body.appendChild(pvTipEl);
  }
  return pvTipEl;
}
// THE ORPHANED TOOLTIP, and why it needed a second exit.
//
// The tip is ONE element parented to document.body, deliberately — it has to
// escape the chart's clip and stacking context. But it is only ever hidden by
// the `mouseleave` of the SVG hit rect that showed it, and in live mode the
// whole panel is rebuilt several times a second. Rebuild the DOM while the
// pointer is inside a chart and that hit rect is discarded with its listener
// still armed: the mouseleave never fires, the tip is left parented to a body
// that no longer contains the chart, and it hangs at the last mouse position
// until you happen to enter and leave a chart again. That is exactly the
// reported "box won't go away until I move back over the source window and off
// it slowly".
//
// So: an explicit hide at the top of every render (the rebuild is the event
// that orphans it), plus a document-level fallback for the other route to the
// same state — the pointer leaving the window, or the chart scrolling out from
// under a stationary cursor. `pv-tip` is already pointer-events:none, so the
// tip can never be the thing under the mouse and the fallback cannot fight it.
function pvTipHide(){ if (pvTipEl) pvTipEl.style.display = 'none'; }
if (typeof document !== 'undefined' && !window.__pvTipGuard){
  window.__pvTipGuard = true;
  // Capture phase: the chart's own mousemove runs after this and re-shows the
  // tip, so a move INSIDE a chart is unaffected and a move anywhere else hides.
  document.addEventListener('mousemove', ev => {
    if (!pvTipEl || pvTipEl.style.display === 'none') return;
    const overChart = ev.target && ev.target.closest
                      && ev.target.closest('.pv-sec svg');
    if (!overChart) pvTipHide();
  }, true);
  document.addEventListener('mouseleave', pvTipHide);
  window.addEventListener('blur', pvTipHide);
  window.addEventListener('scroll', pvTipHide, true);
}
function pvTipShow(tip, ev, head, rows, total, warn){
  tip.innerHTML = '';
  tip.append(pvEl('div', { class:'pv-tip-head' }, head));
  for (const r of rows){
    const line = pvEl('div', { class:'pv-tip-row' });
    line.append(pvEl('span', { class:'pv-sw', style:'background:'+r.col }));
    line.append(pvEl('span', { class:'pv-tip-label' }, r.label));
    line.append(pvEl('span', { class:'pv-tip-val' }, pvNum(r.v) + ' ms'));
    tip.append(line);
  }
  if (total != null){
    const line = pvEl('div', { class:'pv-tip-row pv-tip-total' });
    line.append(pvEl('span', { class:'pv-sw', style:'background:transparent' }));
    line.append(pvEl('span', { class:'pv-tip-label' }, 'frame (wall)'));
    line.append(pvEl('span', { class:'pv-tip-val' }, pvNum(total) + ' ms'));
    tip.append(line);
  }
  if (warn) tip.append(pvEl('div', { class:'pv-tip-warn' }, warn));
  tip.style.display = 'block';
  const pad = 14;
  const bw = tip.offsetWidth, bh = tip.offsetHeight;
  let x = ev.clientX + pad, y = ev.clientY + pad;
  if (x + bw > window.innerWidth - 8) x = ev.clientX - bw - pad;
  if (y + bh > window.innerHeight - 8) y = ev.clientY - bh - pad;
  tip.style.left = x + 'px'; tip.style.top = y + 'px';
}

/* ---- horizontal magnitude bars --------------------------------------------
 * The component breakdown. ONE hue for every bar (sequential slot), because the
 * categories are nominal and a value-ramp would re-encode the bar length in
 * colour — burning the only free channel on information the bar already shows.
 * The counter that explains each bar rides at the right as text.
 */
function pvBars(rows, opts){
  const wrap = pvEl('div', { class:'pv-bars' });
  const max = Math.max(...rows.map(r => r.ms), 1e-9);
  const totalMs = opts.total || rows.reduce((a,b)=>a+b.ms,0);
  for (const r of rows){
    const row = pvEl('div', { class:'pv-bar-row' + (r.dim?' dim':'')
                                                 + (r.total?' total':'') });
    const lab = pvEl('div', { class:'pv-bar-label' + (r.child?' child':'') });
    lab.append(pvEl('span', { class:'pv-sw', style:'background:'+(r.col||PV_SEQ[3]) }));
    lab.append(pvEl('span', {}, r.label));
    // The scope key is the thing being timed; showing it makes the hover
    // explanation findable instead of something you have to guess is there.
    if (r.scope) lab.append(pvEl('code', { class:'pv-scope-key' }, r.scope));
    if (r.side) lab.append(pvEl('span', { class:'pv-side pv-side-'+r.side }, r.side));
    row.append(lab);
    const track = pvEl('div', { class:'pv-bar-track' });
    const fill = pvEl('div', { class:'pv-bar-fill' });
    fill.style.width = (100 * r.ms / max) + '%';
    fill.style.background = r.col || PV_SEQ[3];
    track.append(fill);
    row.append(track);
    row.append(pvEl('div', { class:'pv-bar-val' }, pvNum(r.ms) + ' ms'));
    row.append(pvEl('div', { class:'pv-bar-pct' },
                    totalMs > 0 ? pvPct(r.ms/totalMs) : '—'));
    row.append(pvEl('div', { class:'pv-bar-why' }, r.why || ''));
    if (r.tip){
      row.title = r.tip;
    }
    wrap.append(row);
  }
  return wrap;
}

/* ---- sparkline (small multiple) -------------------------------------------
 * One counter, one hue, no axis furniture. These are context for the bars, so
 * they are deliberately quiet: the peak and the current value are labelled and
 * nothing else is.
 */
function pvSpark(values, opts){
  const w = 300, h = 54, pad = 4;
  const svg = pvChartFrame(w, h);
  const n = values.length;
  if (!n) return svg;
  const max = Math.max(...values, 1e-9);
  const X = i => pad + (n<=1?0:i*(w-2*pad)/(n-1));
  const Y = v => h - pad - (v/max)*(h-2*pad-10);
  let d = 'M' + X(0) + ' ' + Y(values[0]);
  for (let i=1;i<n;i++) d += ' L' + X(i) + ' ' + Y(values[i]);
  const col = opts.bug ? 'var(--red)' : PV_SEQ[3];
  svg.append(pvS('path', { d: d + ' L'+X(n-1)+' '+(h-pad)+' L'+X(0)+' '+(h-pad)+' Z',
                           fill:col, 'fill-opacity':0.10 }));
  svg.append(pvS('path', { d, fill:'none', stroke:col, 'stroke-width':2,
                           'stroke-linejoin':'round', 'stroke-linecap':'round' }));
  // End marker: >=8px, with the 2px surface ring so it stays legible on the line.
  svg.append(pvS('circle', { cx:X(n-1), cy:Y(values[n-1]), r:4, fill:col,
                             stroke:PV_SURF, 'stroke-width':2 }));
  return svg;
}

/* ---- distribution ---------------------------------------------------------
 * p50/p95/p99 as a compact meter row plus a frame-time histogram. The
 * percentiles are the honest headline: a mean hides exactly the hitches this
 * page exists to find.
 */
function pvHistogram(values, opts){
  const w = 1000, h = 150, padL = 52, padR = 12, padT = 10, padB = 30;
  const svg = pvChartFrame(w, h);
  if (!values.length) return svg;
  const lo = 0, hi = Math.max(pvPctl(values, 0.995) * 1.15, 1);
  const BINS = 48;
  const bins = new Array(BINS).fill(0);
  for (const v of values){
    let b = Math.floor((v - lo) / (hi - lo) * BINS);
    b = Math.max(0, Math.min(BINS-1, b));
    bins[b]++;
  }
  const maxC = Math.max(...bins, 1);
  const bw = (w - padL - padR) / BINS;
  const Y = c => h - padB - (c/maxC)*(h-padT-padB);
  pvGridY(svg, padL, w-padR, Y, pvTicks(maxC, 3), pvInt);
  for (let i=0;i<BINS;i++){
    if (!bins[i]) continue;
    const x = padL + i*bw, y = Y(bins[i]);
    // 4px rounded data-end, square at the baseline; 2px surface gap to the next
    // bar (bw-2), which is what separates touching bars.
    const bh = h - padB - y;
    const r = Math.min(4, bh, (bw-PV_GAP)/2);
    svg.append(pvS('path', {
      d:`M${x} ${h-padB} L${x} ${y+r} Q${x} ${y} ${x+r} ${y} L${x+bw-PV_GAP-r} ${y}`
       +` Q${x+bw-PV_GAP} ${y} ${x+bw-PV_GAP} ${y+r} L${x+bw-PV_GAP} ${h-padB} Z`,
      fill: PV_SEQ[3] }));
  }
  // The budget markers, again as reference not data.
  for (const b of [{ms:16.67,l:'60 fps'},{ms:33.33,l:'30 fps'}]){
    if (b.ms > hi) continue;
    const x = padL + (b.ms-lo)/(hi-lo)*(w-padL-padR);
    svg.append(pvS('line', { x1:x, x2:x, y1:padT, y2:h-padB, stroke:PV_INK2,
                             'stroke-width':1, 'stroke-dasharray':'4 4' }));
    svg.append(pvS('text', { x:x+3, y:padT+10, fill:PV_INK2, 'font-size':10,
                             'font-family':'var(--mono)' }, b.l));
  }
  for (const t of [0, 0.25, 0.5, 0.75, 1]){
    const v = lo + t*(hi-lo);
    const x = padL + t*(w-padL-padR);
    svg.append(pvS('text', { x, y:h-12, 'text-anchor':'middle', fill:PV_INK3,
                             'font-size':10, 'font-family':'var(--mono)' },
                   pvNum(v,1)));
  }
  svg.append(pvS('text', { x:(padL+w-padR)/2, y:h-1, 'text-anchor':'middle',
                           fill:PV_INK3, 'font-size':10 }, 'frame time (ms)'));
  return svg;
}

/* ---- heatmap: component x scenario ----------------------------------------
 * Magnitude across a grid, so: sequential, one hue, with a scale legend. Cells
 * carry their number as well as their shade — the shade is for scanning, the
 * number is for reading, and a heatmap with only shade cannot be read at all.
 */
function pvHeatmap(){
  const scen = PERF.scenarios.filter(s => !s.skipped);
  if (!scen.length) return pvEl('div', { class:'pv-empty' }, 'No scenarios recorded.');
  // Rows are every node that any scenario spent time in, ordered by their worst
  // scenario — so the component that is ever the problem sorts to the top.
  const rowIds = [];
  for (const s of scen) for (const k in s.series.gpu)
    if (!rowIds.includes(k)) rowIds.push(k);
  for (const s of scen) for (const k in s.series.cpu){
    const nid = pvScopeNode(k);
    if (k !== 'present' && nid && !rowIds.includes(nid)) rowIds.push(nid);
  }
  const cell = (s, id) => {
    let v = 0;
    if (s.series.gpu[id]) v += pvMean(s.series.gpu[id]);
    for (const k in s.series.cpu)
      if (k !== 'present' && pvScopeNode(k) === id) v += pvMean(s.series.cpu[k]);
    return v;
  };
  const rows = rowIds.map(id => ({ id, vals: scen.map(s => cell(s, id)) }));
  rows.forEach(r => r.max = Math.max(...r.vals));
  rows.sort((a,b) => b.max - a.max);
  const gmax = Math.max(...rows.map(r => r.max), 1e-9);

  const tbl = pvEl('table', { class:'pv-heat' });
  const thead = pvEl('tr', {}, pvEl('th', {}, 'Component'));
  for (const s of scen) thead.append(pvEl('th', {}, s.label));
  tbl.append(pvEl('thead', {}, thead));
  const tb = pvEl('tbody');
  for (const r of rows){
    const tr = pvEl('tr', {}, pvEl('td', { class:'pv-heat-name' }, pvNodeLabel(r.id)));
    r.vals.forEach((v, i) => {
      // Sequential step by magnitude. Below 1% of the page maximum a cell reads
      // as "nothing happened here" and gets the surface, not the palest step —
      // a faint tint on a zero is the heatmap version of a lie.
      const t = v / gmax;
      const step = t < 0.01 ? -1 : Math.min(PV_SEQ.length-1, Math.floor(t*PV_SEQ.length));
      const td = pvEl('td', { class:'pv-heat-cell' },
                      v > 0 ? pvNum(v) : '·');
      if (step >= 0){
        td.style.background = PV_SEQ[step];
        // White or ink by the fill's luminance, so a label inside a fill always
        // clears contrast.
        td.style.color = step >= 3 ? '#0b0f14' : '#e8eef7';
      }
      td.title = pvNodeLabel(r.id) + ' — ' + scen[i].label + ': ' + pvNum(v) + ' ms/frame';
      tr.append(td);
    });
    tb.append(tr);
  }
  tbl.append(tb);

  const legend = pvEl('div', { class:'pv-heat-legend' },
    pvEl('span', { class:'pv-heat-legend-lab' }, '0 ms'));
  for (const c of PV_SEQ) legend.append(pvEl('span', { class:'pv-heat-chip',
                                                       style:'background:'+c }));
  legend.append(pvEl('span', { class:'pv-heat-legend-lab' }, pvNum(gmax) + ' ms/frame'));
  return pvEl('div', {}, tbl, legend);
}

/* ---- legend ---------------------------------------------------------------
 * Always present for two or more series. Identity is never colour alone.
 */
function pvLegend(items){
  const l = pvEl('div', { class:'pv-legend' });
  for (const it of items){
    l.append(pvEl('span', { class:'pv-legend-item' + (it.tip ? ' pv-help' : ''),
                            title: it.tip || null },
      pvEl('span', { class:'pv-sw' + (it.line ? ' line' : ''),
                     style:'background:'+it.col }),
      pvEl('span', {}, it.label),
      it.ms != null ? pvEl('span', { class:'pv-legend-ms' }, pvNum(it.ms)+' ms') : null));
  }
  return l;
}

/* ---- stat tile ------------------------------------------------------------ */
function pvTile(label, value, unit, sub, tone){
  // Every tile carries its own explanation; PV_TILE_NOTE is keyed by the label
  // so a new tile without a note is visible as a missing tooltip rather than
  // silently unexplained.
  const tip = PV_TILE_NOTE[label] || '';
  return pvEl('div', { class:'pv-tile' + (tone ? ' tone-'+tone : '')
                                       + (tip ? ' pv-help' : ''),
                       title: tip || null },
    pvEl('div', { class:'pv-tile-label' }, label),
    pvEl('div', { class:'pv-tile-value' }, value,
         unit ? pvEl('span', { class:'pv-tile-unit' }, unit) : null),
    sub ? pvEl('div', { class:'pv-tile-sub' }, sub) : null);
}

/* ================= layout ================= */

// The one entry point. Called by renderAll() on tab activation; safe to call
// repeatedly (it rebuilds from state rather than accumulating listeners).
function renderPerformance(){
  const root = document.getElementById('view-performance');
  if (!root) return;
  // Before the rebuild, not after: every chart's mouseleave listener is about
  // to be thrown away with the node that owns it. See pvTipHide.
  pvTipHide();
  root.innerHTML = '';
  root.append(pvHeader());

  if (!PERF && pvScenario !== '__live__'){
    root.append(pvEl('div', { class:'pv-empty' },
      pvEl('p', {}, 'No recorded run loaded.'),
      pvEl('p', { class:'note' },
        'Press ', pvEl('b', {}, 'Run performance tests'), ' above to record one '
        + '(five scenarios, each with its own worldgen and settle), or ',
        pvEl('b', {}, 'Watch live'),
        ' to launch the game with telemetry and read the graphs as you play.')));
    return;
  }
  root.append(pvScenarioBar());

  const V = pvView();
  if (!V){ root.append(pvEl('div', { class:'pv-empty' }, 'Select a scenario.')); return; }
  if (V.skipped){
    root.append(pvEl('div', { class:'pv-skip' },
      pvEl('h3', {}, V.label + ' — not recorded'),
      pvEl('p', {}, V.skipWhy || 'The scenario refused to run.'),
      pvEl('p', { class:'note' },
        'A scenario that cannot find its fixture SKIPS rather than quietly '
        + 'measuring something else. The line above is the harness reporting '
        + 'what it looked for and what it found.')));
    return;
  }
  if (!V.frames){
    root.append(pvEl('div', { class:'pv-empty' },
      pvLive.connected ? 'Connected — waiting for the first frame.'
                       : 'No samples yet.'));
    return;
  }

  // W is the trailing window the NUMBERS are means of; V stays the whole ring,
  // which is what the CHARTS draw. Every section takes both and is explicit
  // about which one it read — see the pvRecent header.
  const W = pvRecent(V, pvWinSec);
  const T = pvTotals(W);
  root.append(pvVerdictPanel(V, T, W));
  root.append(pvTimelineSection(V, T));
  root.append(pvPercentileSection(V, T));
  root.append(pvBreakdownSection(V, T, W));
  root.append(pvCountersSection(V, W));
  root.append(pvDistributionSection(V));
  if (!V.live){
    root.append(pvComparisonSection());
    root.append(pvPassSection(V));
  }
  root.append(pvMethodSection(V));
}
window.renderPerformance = renderPerformance;

/* ---- header --------------------------------------------------------------- */
function pvHeader(){
  const b = (PERF && PERF.build) || null;
  const bar = pvEl('div', { class:'pv-bar' + (pvLive.connected ? ' live' : '') });
  bar.append(pvEl('span', { class:'dot' }));
  bar.append(pvEl('span', { class:'lbl', id:'pv-status' },
    pvLive.connected ? pvLive.status
      : b ? ('recorded on ' + b.adapter + ' — ' + b.renderW + '×' + b.renderH
             + ', ' + b.residency + ' residency, ' + b.worldN + '³ window')
          : 'no run loaded'));
  bar.append(pvEl('span', { class:'spacer' }));
  bar.append(pvEl('button', { id:'pv-run', onclick:pvRunTests },
                  'Run performance tests'));
  bar.append(pvEl('button', { id:'pv-watch', onclick:pvWatchLive },
                  pvLive.connected ? 'Stop watching' : 'Watch live'));
  bar.append(pvEl('button', { onclick:() => pvLoad(true) }, 'Reload perf.json'));
  return bar;
}

/* ---- scenario selector ---------------------------------------------------- */
function pvScenarioBar(){
  const row = pvEl('div', { class:'pv-scenarios' });
  const pick = id => { pvScenario = id; renderPerformance(); };
  if (PERF) for (const s of PERF.scenarios){
    row.append(pvEl('button', {
      class:'pv-chip' + (pvScenario===s.id?' on':'') + (s.skipped?' skipped':''),
      onclick:() => pick(s.id) },
      pvEl('b', {}, s.label),
      pvEl('span', {}, s.skipped ? 'skipped'
                                 : pvNum(s.frames/30,0) + ' s · ' + s.frames + ' frames')));
  }
  row.append(pvEl('button', {
    class:'pv-chip pv-chip-live' + (pvScenario==='__live__'?' on':''),
    onclick:() => pick('__live__') },
    pvEl('b', {}, 'Live session'),
    pvEl('span', {}, pvLive.connected ? pvLive.samples.length + ' frames'
                                      : 'not connected')));

  // THE AVERAGING WINDOW, only on live — a recorded scenario has no "now" for
  // a trailing window to trail behind, and offering the control there would
  // imply otherwise. One second is the default because it is short enough to
  // track what you just walked into and long enough that a 60 fps sample is 60
  // frames, which is a steady number rather than a flicker.
  if (pvScenario === '__live__'){
    const w = pvEl('div', { class:'pv-winpick',
      title:'The trailing wall-clock window every millisecond figure and every '
          + 'counter headline is averaged over. Charts always draw the whole '
          + 'ring; this only moves the numbers.' });
    w.append(pvEl('span', { class:'pv-winpick-lbl' }, 'average over'));
    for (const s of PV_WIN_SECS)
      w.append(pvEl('button', {
        class:'pv-chip pv-chip-win' + (pvWinSec===s?' on':''),
        onclick:() => { pvWinSec = s; renderPerformance(); } },
        s > 0 ? (s < 1 ? (s*1000)+' ms' : s+' s') : 'all'));
    row.append(w);
  }
  return row;
}

/* ---- verdict -------------------------------------------------------------- */
function pvVerdictPanel(V, T, W){
  W = W || V;
  const v = pvVerdict(T);
  const fps = T.wallMs > 0 ? 1000/T.wallMs : 0;
  const worst = (T.gpu[0] && (!T.cpuBusy[0] || T.gpu[0].ms >= T.cpuBusy[0].ms))
                ? T.gpu[0] : T.cpuBusy[0];
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('div', { class:'pv-scen-head' },
    pvEl('h2', {}, V.label),
    pvEl('p', { class:'note' }, V.desc || ''),
    V.note ? pvEl('p', { class:'pv-fixture' }, V.note) : null));

  sec.append(pvEl('div', { class:'pv-hero tone-'+v.tone },
    pvEl('div', { class:'pv-hero-badge' }, v.bound + '-BOUND'),
    pvEl('div', { class:'pv-hero-why' }, v.why),
    worst ? pvEl('div', { class:'pv-hero-worst' },
      'Largest single component: ', pvEl('b', {}, worst.label),
      ' at ' + pvNum(worst.ms) + ' ms/frame (' + pvPct(worst.ms/(T.wallMs||1))
      + ' of the frame)') : null));

  // WHICH FRAMES THESE TILES AVERAGED, said out loud. A mean whose window is
  // not stated is the same trap as a duration with no denominator.
  const win = pvWinLabel(W);
  sec.append(pvEl('p', { class:'note pv-winnote' },
    'Every millisecond figure below is a mean over the ', pvEl('b', {}, win),
    V.live ? '. The charts further down draw the whole ring.'
           : '. Recorded runs have no “now”, so the window is the run.'));

  const tiles = pvEl('div', { class:'pv-tiles' });
  tiles.append(pvTile('Frame time (mean)', pvNum(T.wallMs,1), 'ms',
                      pvNum(fps,0) + ' fps · ' + win));
  // p99 and the page-fault counter deliberately stay on the FULL ring. A
  // percentile over one second of frames is just the maximum wearing a
  // percentile's name, and a hitch that scrolls out of view in a second is a
  // hitch you cannot read. The same goes for a bug counter: page faults must
  // not age out of the display before you have seen them.
  tiles.append(pvTile('Frame time (p99)', pvNum(pvPctl(V.wall,0.99),1), 'ms',
                      'the hitch, over all ' + (V.wall||[]).length + ' frames'));
  tiles.append(pvTile('GPU work', pvNum(T.gpuMs,1), 'ms',
                      pvPct(T.gpuMs/(T.wallMs||1)) + ' of the frame'));
  tiles.append(pvTile('CPU busy', pvNum(T.cpuBusyMs,1), 'ms',
                      pvPct(T.cpuBusyMs/(T.wallMs||1)) + ' of the frame'));
  tiles.append(pvTile('CPU waiting', pvNum(T.waitMs,1), 'ms',
                      'parked on a full frame queue'));
  const pf = (V.counters && V.counters.pageFaults)
             ? Math.max(...V.counters.pageFaults) : 0;
  if (pf > 0)
    tiles.append(pvTile('Page faults', pvInt(pf), '',
                        'this is a BUG, not a cost', 'red'));
  sec.append(tiles);
  return sec;
}

/* ---- timeline ------------------------------------------------------------- */
function pvTimelineSection(V, T){
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'Where the frame goes, over time'));
  sec.append(pvEl('p', { class:'note' },
    'Stacked GPU time per component, with the whole frame drawn over it as a '
    + 'line. The gap between the two is the CPU: if the line sits well above the '
    + 'stack, something on the CPU is holding the frame. The dashed lines are '
    + 'the 60 fps and 30 fps budgets. Hover anywhere for the exact split.'));

  const gpuSeries = {};
  for (const k in V.gpu) if (pvSum(V.gpu[k]) > 0) gpuSeries[k] = V.gpu[k];
  sec.append(pvStackedArea(V, { series:gpuSeries, height:250, side:'gpu',
                                wall: V.wall }));
  // The legend is built from the SAME identity split the stack draws, so a row
  // and a band are never two different things wearing one colour.
  const gHued = T.gpu.filter(g => pvHasHue(g.id, 'gpu'));
  const gTail = T.gpu.filter(g => !pvHasHue(g.id, 'gpu'));
  const legItems = gHued.map(g => ({ col:pvHue(g.id,'gpu'), label:g.label, ms:g.ms,
                                     tip:(pvNodeById(g.id)||{}).note || '' }));
  if (gTail.length)
    legItems.push({ col:PV_OTHER,
                    label:'Other: ' + gTail.map(g=>g.label).join(', '),
                    ms: gTail.reduce((a,b)=>a+b.ms,0) });
  legItems.push({ col:PV_INK, label:'frame time (wall clock)', ms:T.wallMs,
                  line:true });
  sec.append(pvLegend(legItems));

  if (T.cpuBusy.length){
    sec.append(pvEl('h3', { class:'pv-h3b' }, 'CPU work, over time'));
    sec.append(pvEl('p', { class:'note' },
      'The same frames, CPU side only — a separate chart rather than a '
      + 'second axis on the one above, because two y-scales on one plot invent a '
      + 'correlation that is not in the data.'));
    const cpuSeries = {};
    for (const k in V.cpu)
      if (k !== 'present' && pvSum(V.cpu[k]) > 0) cpuSeries[k] = V.cpu[k];
    // budgets:false -- the frame-budget lines belong to the chart that plots
    // the whole frame. On a 0.5 ms CPU trace a 20 ms axis is a flat line at
    // zero, which renders, asserts clean, and shows nothing.
    sec.append(pvStackedArea(V, { series:cpuSeries, height:170, side:'cpu',
                                  budgets:false,
                                  nodeOf: k => pvScopeNode(k) || k }));
    const cHued = T.cpuBusy.filter(c => pvHasHue(c.id, 'cpu'));
    const cTail = T.cpuBusy.filter(c => !pvHasHue(c.id, 'cpu'));
    const cLeg = cHued.map(c => ({ col:pvHue(c.id,'cpu'), label:c.label, ms:c.ms,
                                   tip: pvScopeNote(c.scope)
                                     || (pvNodeById(c.id)||{}).note || '' }));
    if (cTail.length)
      cLeg.push({ col:PV_OTHER, label:'Other: ' + cTail.map(c=>c.label).join(', '),
                  ms: cTail.reduce((a,b)=>a+b.ms,0) });
    sec.append(pvLegend(cLeg));
  }
  return sec;
}

/* ---- rolling percentile plot ----------------------------------------------
 * The "is it smooth" chart, as opposed to the "where did it go" chart above.
 */
function pvPercentileSection(V, T){
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'Frame time distribution, over time'));
  sec.append(pvEl('p', { class:'note' },
    'Each line is a percentile of the trailing ' + pvPctlWin + ' frames, so the '
    + 'value at any point is the distribution of the recent past rather than one '
    + 'frame. Percentiles rather than a smoothed average on purpose: averaging '
    + 'hides exactly the hitch you are looking for, while p95 and p99 are made '
    + 'of it. Toggle any series; the numbers in the legend are for the whole '
    + 'run, not the window.'));

  // ---- toggles ----
  const chips = pvEl('div', { class:'pv-toggles' });
  for (const s of PV_PCTL_SERIES){
    const on = !!pvPctlOn[s.key];
    const b = pvEl('button', {
      class:'pv-toggle' + (on ? ' on' : ''),
      title: s.tip,
      'data-pctl': s.key,
      'aria-pressed': on ? 'true' : 'false',
      onclick: () => { pvPctlOn[s.key] = !pvPctlOn[s.key]; renderPerformance(); } },
      pvEl('span', { class:'pv-sw', style:'background:'+s.col }),
      pvEl('span', {}, s.label));
    chips.append(b);
  }
  // The window is part of what a percentile MEANS, so it is a control and not a
  // constant — see the p99 tip: below ~100 frames p99 degenerates to the max.
  chips.append(pvEl('span', { class:'pv-toggle-gap' }));
  chips.append(pvEl('button', {
    class:'pv-toggle pv-toggle-win',
    id:'pv-pctl-win',
    title:'The trailing window each percentile is computed over. A p99 needs at '
        + 'least ~100 frames to be a percentile rather than just the maximum of '
        + 'the window, so 60 is offered but flagged.',
    onclick: () => {
      const i = PV_PCTL_WINS.indexOf(pvPctlWin);
      pvPctlWin = PV_PCTL_WINS[(i + 1) % PV_PCTL_WINS.length];
      renderPerformance();
    } },
    'window: ' + pvPctlWin + ' frames'));
  sec.append(chips);

  // ---- the chart ----
  const wall = V.wall || [];
  const stride = Math.max(1, Math.floor(wall.length / 400));
  const lines = [];
  for (const s of PV_PCTL_SERIES){
    if (!pvPctlOn[s.key]) continue;
    if (s.key === 'raw'){
      lines.push({ label:s.label, col:s.col, thin:true, tip:s.tip,
                   pts: wall.map((v,i) => [i,v]) });
    } else {
      lines.push({ label:s.label, col:s.col, tip:s.tip,
                   pts: pvRollingPctl(wall, pvPctlWin, s.p, stride) });
    }
  }
  if (!lines.length){
    sec.append(pvEl('div', { class:'pv-empty' },
      'Every series is hidden — turn one on above.'));
    return sec;
  }
  sec.append(pvLineChart(V, { lines, height:230 }));

  // Legend carries the WHOLE-RUN value, which is the number you quote. The
  // chart shows how it moved; the legend says where it ended up.
  sec.append(pvLegend(PV_PCTL_SERIES.filter(s => pvPctlOn[s.key] && s.p != null)
    .map(s => ({ col:s.col, label:s.label, ms: pvPctl(wall, s.p) }))));

  if (pvPctlWin < 100 && pvPctlOn.p99)
    sec.append(pvEl('p', { class:'pv-warn-line' },
      'At a ' + pvPctlWin + '-frame window the p99 line is the window maximum: '
      + '1% of 60 frames is less than one frame. Widen the window to read it as '
      + 'a percentile.'));
  return sec;
}

/* ---- the raymarch's step census -------------------------------------------
 *
 * WHY THIS IS NOT JUST MORE BARS. Every other row on this page is a timestamp
 * around a dispatch. The raymarch cannot be: the primary march, the sun shadow
 * ray, the far cascade, the micro bricks and the reflections all run inside ONE
 * fragment shader invocation, and there is nowhere to put a query between them.
 * So the shader counts STEPS instead -- one DDA cell advance -- over a 1-in-16
 * pixel sample, and this section divides the single span it does have by that
 * census.
 *
 * Every row is therefore an ESTIMATE and says so on screen. The known error is
 * in one direction: the shader's fixed per-pixel cost -- the register footprint
 * every trace() call site carries whether or not it ever steps -- is not a
 * step, so it is spread across these rows in proportion to steps rather than
 * sitting in one honest row of its own.
 *
 * The tips answer "so what do I do about it" rather than restating the label:
 * what the trace is, and what makes it get expensive.
 */
const PV_RM_STEPS = [
  { key:'rmPrimarySteps', short:'primary', label:'Primary DDA',
    tip:'The camera ray walking the voxel grid one cell at a time until '
      + 'something opaque stops it. Scales with how far the eye can see before '
      + 'it hits: an open view across a valley is the expensive case, a wall in '
      + 'your face is free. Every other row below is a ray that starts where '
      + 'this one stopped, so shortening this shortens all of them.' },
  { key:'rmMediaCells', short:'media', label:'Media (fire/smoke/water)',
    tip:'Cells that accumulated optical depth for fire, smoke or water instead '
      + 'of terminating the ray. These MULTIPLY the march rather than adding to '
      + 'it -- a translucent cell is a step the primary ray did not get to stop '
      + 'on -- which is why a screen full of smoke costs far more than a screen '
      + 'full of stone at the same depth.' },
  { key:'rmShadowSteps', short:'shadow', label:'Sun shadow rays',
    tip:'A second full trace from the hit point toward the sun, per lit pixel. '
      + 'This is the row that DOUBLES the shader: it costs roughly what the '
      + 'primary march cost, over the same geometry. The shadow cache in the '
      + 'denominators below exists to stop most pixels ever issuing one.' },
  { key:'rmFarSteps', short:'far', label:'Far cascade',
    tip:'The coarse cascade march that takes over where the near window runs '
      + 'out. Scales with horizon pixels -- ridge lines, distant ground, '
      + 'anything the near grid cannot reach -- so it moves with where the '
      + 'camera is LOOKING far more than with what is in the world.' },
  { key:'rmFarShadowSteps', short:'far shadow', label:'Far cascade shadows',
    tip:'Shadow rays marched through the coarse cascade. Watch this one: the '
      + 'far cascade has no early-out, so a NEAR shadow that ends up routed '
      + 'through here is dramatically dearer per step than the same shadow '
      + 'taken in the near grid.' },
  { key:'rmMicroSteps', short:'micro', label:'Micro bricks / grass',
    tip:'Sub-voxel detail -- micro-brick bodies and grass strand lattices -- '
      + 'marched at a finer step INSIDE a single voxel. Costs per screen area '
      + 'of grass rather than per blade, so it is a function of how much of the '
      + 'view is near ground, not of how much grass the world holds.' },
  { key:'rmReflectSteps', short:'reflect', label:'Reflections + refraction',
    tip:'Secondary rays off water and other reflective surfaces, plus the '
      + 'refracted ray continuing under a surface. Each is another trace from a '
      + 'hit point, so this row is close to "how much of the screen is water '
      + 'you can see across or into".' },
  { key:'rmFluidSteps', short:'MPM water', label:'MPM water surface',
    tip:'The march over the MLS-MPM fluid surface. It is zero while no fluid '
      + 'particles are live, so it appears and disappears with the water rather '
      + 'than scaling smoothly -- a jump in this row is a splash starting, not '
      + 'the renderer getting slower.' },
  { key:'rmGodraySteps', short:'god rays', label:'God rays / silt',
    tip:'Sun-shaft samples along the view ray, and the underwater silt samples '
      + 'that share the loop. A fixed sample count per qualifying pixel, so '
      + 'this scales with RESOLUTION and with how much of the screen qualifies '
      + '-- never with world content.' },
];

// The step census over the window, biggest first, or null when this run
// predates the counters. Gating on the WINDOW rather than the whole ring is
// deliberate: if the last second of a live session took no steps there is no
// split to draw, and a section of empty bars is worse than no section.
//
// THE SHARE IS BUILT FROM THE MEAN even when the bars are showing p99, because
// a share is a ratio of parts to a whole and percentiles do not partition. The
// breakdown above already says this one level up about summing components:
// each counter's worst frame is a DIFFERENT frame, so dividing p99s by the sum
// of the p99s would describe a frame on which every trace peaked at once.
function pvRmSteps(W){
  const c = (W && W.counters) || {};
  const rows = [];
  let total = 0;
  for (const d of PV_RM_STEPS){
    const n = pvMean(c[d.key]);
    if (!(n > 0)) continue;        // a trace that never ran is not a 0% row
    rows.push({ key:d.key, short:d.short, label:d.label, tip:d.tip, n });
    total += n;
  }
  if (!rows.length || !(total > 0)) return null;
  for (const r of rows) r.share = r.n / total;
  rows.sort((a,b) => b.n - a.n);
  return { rows, total };
}

// The denominators line: what those steps were spent ON. Same shape as the
// counters panel's foot -- one quiet mono line, no chart -- because these are
// context for the bars above rather than series in their own right.
function pvRmDenoms(W){
  const c = (W && W.counters) || {};
  const px = pvMean(c.rmPixels);
  const cls = [['rmPxSky','sky'], ['rmPxFar','far'], ['rmPxWater','water'],
               ['rmPxSubmerged','submerged']];
  const have = cls.some(k => pvMean(c[k[0]]) > 0);
  const taps = pvMean(c.rmShadowCacheTaps), reqs = pvMean(c.rmShadowCacheReqs);
  if (!(px > 0) && !have && !(taps > 0) && !(reqs > 0)) return null;

  const parts = [];
  if (px > 0) parts.push(pvMag(px) + ' pixels shaded/frame');
  if (have){
    // Percentages of the pixels actually shaded when there IS a denominator,
    // raw counts when there is not. A percentage with no denominator is the
    // trap this whole page is written against, so it is not offered.
    parts.push(cls.map(k => k[1] + ' '
                            + (px > 0 ? pvPct(pvMean(c[k[0]]) / px)
                                      : pvMag(pvMean(c[k[0]])))).join(', '));
  }
  if (taps > 0){
    // Hit rate, not miss count: a request IS a miss (it appends work for the
    // cache to fill later), so the number worth reading is the fraction of taps
    // that never had to ask. Clamped because taps and requests are two counters
    // and a frame boundary can land between them.
    const hit = Math.max(0, Math.min(1, 1 - reqs / taps));
    parts.push('shadow cache ' + pvMag(taps) + ' taps/frame, ' + pvPct(hit)
               + ' hit rate');
  } else if (reqs > 0){
    parts.push('shadow cache ' + pvMag(reqs) + ' requests/frame, no taps');
  }
  return parts.join(' - ');
}

// Appended INTO the breakdown's GPU half rather than made a section of its own:
// this is a drill-down under one bar, and hoisting it to the page's top level
// would imply it is a peer of the CPU/GPU split.
function pvRmSection(W, rm, rmMs){
  const box = pvEl('div', { class:'pv-rm' });
  box.append(pvEl('h4', { class:'pv-h4' }, 'Raymarch breakdown (estimated)'));
  box.append(pvEl('p', { class:'note' },
    'The raymarch bar above is ONE fragment shader -- the shadow ray, the far '
    + 'cascade and the reflections all run inside it, and no timestamp can be '
    + 'put between them. These rows split that single span by the STEPS each '
    + 'trace took, counted in the shader over a 1-in-16 pixel sample and scaled '
    + 'back up. The millisecond column is therefore an ESTIMATE -- raymarch ms '
    + 'times that trace’s share of all steps -- and it is wrong in a known '
    + 'direction, because the shader’s fixed per-pixel cost is not a step '
    + 'and so gets spread across every row. For the exact per-feature '
    + 'millisecond, use --render-budget, which removes one feature at a time '
    + 'and measures the whole frame.'));
  if (pvStat !== 'mean')
    box.append(pvEl('p', { class:'note' },
      'The shares below stay on the MEAN while the bars show '
      + pvStatDef().label + ': a share is a ratio of parts to a whole, and '
      + 'percentiles do not partition. Only the raymarch millisecond they are '
      + 'multiplied by follows the selected statistic.'));

  const rows = rm.rows.map(r => ({
    label: r.label, ms: rmMs * r.share, col: PV_SEQ[3], tip: r.tip,
    why: pvMag(r.n) + ' steps/frame - ' + pvPct(r.share) + ' of steps' }));
  box.append(pvBars(rows, { total: rmMs }));

  const den = pvRmDenoms(W);
  if (den) box.append(pvEl('div', { class:'pv-spark-foot' }, den));
  return box;
}

/* ---- component breakdown --------------------------------------------------
 * The list the page exists for. Sorted by cost, GPU and CPU kept apart, each
 * bar carrying the counter that explains it.
 */
function pvBreakdownSection(V, Tmean, W){
  W = W || V;
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'Cost by engine component'));

  // The breakdown reads whichever statistic is selected; the verdict panel
  // above keeps the mean (see pvTotals).
  const T = pvStat === 'mean' ? Tmean : pvTotals(W, pvStat);
  const sd = pvStatDef();

  const chips = pvEl('div', { class:'pv-toggles' });
  for (const s of PV_STATS){
    const on = pvStat === s.key;
    chips.append(pvEl('button', {
      class:'pv-toggle pv-toggle-win' + (on ? ' on' : ''),
      type:'button', title:s.tip, 'data-stat':s.key,
      'aria-pressed': on ? 'true' : 'false',
      onclick: () => { pvStat = s.key; renderPerformance(); } }, s.label));
  }
  sec.append(chips);

  sec.append(pvEl('p', { class:'note' },
    (pvStat === 'mean' ? 'Mean' : sd.label) + ' milliseconds per frame over the '
    + pvWinLabel(W) + '. ' + sd.tip + ' The right-hand column is the work that '
    + 'produced it — a duration with no denominator says a component is slow '
    + 'without saying why. Component names are the same boxes as the Engine '
    + 'tab’s architecture map.'));
  if (pvStat !== 'mean')
    sec.append(pvEl('p', { class:'note' },
      'THE PARTS DO NOT ADD TO THE TOTAL under a percentile, and that is '
      + 'arithmetic rather than a bug: each component’s worst frame is a '
      + 'DIFFERENT frame, so adding their ' + sd.label + 's would describe a '
      + 'frame on which everything went wrong at once. The summed rows are the '
      + sd.label + ' of the per-frame sum, which is the real one.'));
  if (pvStat === 'p99' && (W.wall||[]).length < 100)
    sec.append(pvEl('p', { class:'pv-warn-line' },
      'Only ' + (W.wall||[]).length + ' frames in this window: 1% of that is '
      + 'less than one frame, so p99 here is simply the maximum. Widen the '
      + 'averaging window to read it as a percentile.'));

  // The counters come from the SAME window as the ms figures beside them. A
  // one-second raymarch mean explained by a twenty-second chunk count is two
  // different measurements sharing a sentence.
  //
  // They follow the SELECTED statistic too: explaining a p99 raymarch with a
  // mean chunk count is the same category error one level down — the frame
  // that cost 42 ms is the frame that had the chunks.
  const cmean = {};
  for (const k in (W.counters||{})) cmean[k] = pvStatOf(W.counters[k]);
  // The raymarch's step census, built once: the GPU row below reads it for a
  // one-line hint and the breakdown under the bars reads it for the split.
  // Null on any run recorded before the counters existed, and every use of it
  // has to survive that.
  const rm = pvRmSteps(W);
  const whyFor = id => {
    const parts = [];
    for (const c of ((PERF && PERF.counters) || [])){
      if (c.node !== id) continue;
      // The sixteen rm* counters are the raymarch breakdown's own denominators,
      // spread over six nodes. Dumped raw into a `why` column they push the
      // number that matters off the end of the line, so once the section that
      // owns them is rendering, it owns them here too.
      if (rm && c.key.startsWith('rm')) continue;
      const m = cmean[c.key];
      if (m == null || m <= 0) continue;
      parts.push(pvInt(m) + ' ' + c.label);
    }
    // One span, many traces: the raymarch's useful `why` is not a counter but
    // WHICH TRACE ate the steps. Two names, because a third does not change
    // what you would go and look at and the column is one line.
    if (id === 'raymarch' && rm)
      parts.push(rm.rows.slice(0, 2)
                        .map(r => r.short + ' ' + pvPct(r.share)).join(' / ')
                 + ' of steps');
    // The per-chunk number is what the compute budget is denominated in, so it
    // is derived here rather than left for the reader to divide.
    if (id === 'caLoop' && cmean.activeChunks > 0){
      const per = (pvGpuStatOf(W, W.gpu.caLoop||[]) * 1000) / cmean.activeChunks;
      parts.push(pvNum(per,2) + ' µs/chunk');
    }
    return parts.join(' · ');
  };

  sec.append(pvEl('h4', { class:'pv-h4' }, 'GPU'));
  const gpuRows = T.gpu.map(g => ({
    label:g.label, ms:g.ms, side:'gpu', col:PV_SEQ[3], why: whyFor(g.id),
    tip: (pvNodeById(g.id)||{}).note || '' }));
  // SUMMED. The rows above are the parts; this is what they come to. Worth
  // showing because the eye cannot add ten bars, and because the sum against
  // the frame is the whole verdict: 12 ms of GPU in a 33 ms frame is a
  // different engine from 12 ms in a 13 ms frame.
  gpuRows.push({
    label:'Summed GPU', ms:T.gpuMs, col:PV_SEQ[0], total:true,
    why:'all GPU passes, ' + pvPct(T.gpuMs/(T.wallMs||1)) + ' of the frame',
    tip:'Every timed GPU pass added together. Passes are sequential on one '
      + 'queue, so this sum IS the GPU time in the frame. Compare it with the '
      + 'frame time: if it fills the frame, the GPU is the limit.' });
  sec.append(pvBars(gpuRows, { total:T.wallMs }));

  // The raymarch drill-down sits under the bar it explains, not at the bottom
  // of the page: it is the only row up there whose cost cannot be subdivided by
  // a timestamp, so the split has to be adjacent to the number it splits.
  // Absent entirely on a perf.json recorded before the counters landed.
  if (rm){
    const rmGpu = T.gpu.find(g => g.id === 'raymarch');
    sec.append(pvRmSection(W, rm, rmGpu ? rmGpu.ms : 0));
  }

  sec.append(pvEl('h4', { class:'pv-h4' }, 'CPU'));
  const cpuRows = T.cpuBusy.map(c => ({
    label:c.label, ms:c.ms, side:'cpu', col:PV_SEQ[2], why: whyFor(c.id),
    // The SCOPE note first: the bar is labelled with the architecture node
    // ("Render Pass") but what was timed is the frame-loop span (`renderCpu`),
    // and the scope is the more specific answer to "what am I looking at".
    tip: pvScopeNote(c.scope)
       || (pvNodeById(c.id)||{}).note || '',
    scope: c.scope }));
  cpuRows.push({
    label:'Summed CPU (busy)', ms:T.cpuBusyMs, col:PV_SEQ[0], total:true,
    why:'all CPU scopes except the present wait, '
      + pvPct(T.cpuBusyMs/(T.wallMs||1)) + ' of the frame',
    tip:'Every CPU scope added together, DELIBERATELY EXCLUDING the present '
      + 'wait below. Busy and waiting are concurrent with the GPU, not '
      + 'sequential with each other, so summing them would add a frame to '
      + 'itself — the same mistake that once drew a 71 ms axis for a 36.7 ms '
      + 'frame.' });
  if (T.waitMs > 0) cpuRows.push({
    label:'waiting on the GPU', ms:T.waitMs, col:PV_WAIT, dim:true,
    why:'not work — the shape of the bottleneck',
    tip: pvScopeNote('present') });
  sec.append(pvBars(cpuRows, { total:T.wallMs }));

  if (V.unattributed && V.unattributed.ns > 0){
    sec.append(pvEl('p', { class:'pv-warn' },
      'Unattributed GPU time: ' + pvNum(V.unattributed.ns/1e6/V.frames)
      + ' ms/frame across ' + V.unattributed.names.length + ' pass(es) ('
      + V.unattributed.names.join(', ') + '). These are dispatches no component '
      + 'in perfnodes.h claims — the bars above do not add up to the GPU '
      + 'total until this is zero.'));
  }
  return sec;
}

/* ---- counters -------------------------------------------------------------
 * Small multiples. Each counter is its own chart with its own scale — never two
 * counters on one plot, which is the dual-axis mistake wearing a sparkline.
 */
function pvCountersSection(V, W){
  W = W || V;
  const keys = Object.keys(V.counters||{}).filter(k => pvSum(V.counters[k]) > 0);
  if (!keys.length) return pvEl('span');
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'What the engine was doing'));
  sec.append(pvEl('p', { class:'note' },
    'The denominators. Each is its own chart with its own scale — two '
    + 'counters sharing one plot would imply a relationship the numbers do not '
    + 'have. The big figure is the mean over the ' + pvWinLabel(W)
    + '; the sparkline is the whole ring.'));
  const grid = pvEl('div', { class:'pv-spark-grid' });
  for (const k of keys){
    const def = pvCounterDef(k) || { label:k, node:null, bug:false };
    const v = V.counters[k];
    // THE HEADLINE WAS THE LAST SAMPLE, which is one frame of a counter that
    // moves every frame — the particle count flickering through four digits is
    // not a reading, it is noise with a font. Same window as the ms figures it
    // is there to explain.
    const w = (W.counters && W.counters[k]) || v;
    const card = pvEl('div', { class:'pv-spark' + (def.bug ? ' bug' : '') });
    card.append(pvEl('div', { class:'pv-spark-head' },
      pvEl('span', { class:'pv-spark-label' }, def.label),
      pvEl('span', { class:'pv-spark-val' }, pvInt(pvMean(w)))));
    card.append(pvSpark(v, { bug: def.bug }));
    // Peak stays over the WHOLE ring: a spike is the thing you are looking for
    // and a windowed peak forgets it a second later. Doubly so for a bug
    // counter, where the peak is the entire point.
    card.append(pvEl('div', { class:'pv-spark-foot' },
      'peak ' + pvInt(Math.max(...v)) + ' over ' + v.length + ' frames'
      + (def.node ? ' · explains ' + pvNodeLabel(def.node) : '')));
    grid.append(card);
  }
  sec.append(grid);
  return sec;
}

/* ---- distribution --------------------------------------------------------- */
function pvDistributionSection(V){
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'Frame time distribution'));
  sec.append(pvEl('p', { class:'note' },
    'Percentiles, not the mean: the mean hides exactly the hitches this page '
    + 'exists to find. These are over the WHOLE run — the plot above is the '
    + 'same statistics over a trailing window, which is where you see when '
    + 'they moved.'));
  const tiles = pvEl('div', { class:'pv-tiles' });
  for (const p of [['p50',0.5],['p95',0.95],['p99',0.99],['worst',1.0]]){
    const ms = pvPctl(V.wall, p[1]);
    tiles.append(pvTile(p[0], pvNum(ms,1), 'ms', pvNum(1000/ms,0) + ' fps',
                        ms > 33.34 ? 'amber' : null));
  }
  sec.append(tiles);
  sec.append(pvHistogram(V.wall, {}));
  return sec;
}

/* ---- scenario comparison -------------------------------------------------- */
function pvComparisonSection(){
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'Every component, every scenario'));
  sec.append(pvEl('p', { class:'note' },
    'Mean ms per frame. Each scenario runs on its own freshly generated and '
    + 're-settled world, so a row compares like with like. Read across a row to '
    + 'see what wakes a component up; read down a column to see what a scenario '
    + 'costs.'));
  sec.append(pvHeatmap());
  return sec;
}

/* ---- pass drill-down ------------------------------------------------------ */
function pvPassSection(V){
  if (!V.passes || !V.passes.length) return pvEl('span');
  const sec = pvEl('section', { class:'pv-sec' });
  sec.append(pvEl('h3', {}, 'Individual GPU passes'));
  sec.append(pvEl('p', { class:'note' },
    'One row per PASS() in src/sim/pass_table.def, timed at ROW granularity — '
    + 'finer than the pass GROUPS --measure reports, which is what lets the '
    + 'mutation queue, the explosion and the compaction be told apart inside one '
    + 'recorded pass. This is the drill-down under the component bars.'));
  const rows = V.passes.slice().sort((a,b) => b.usPerFrame - a.usPerFrame);
  const tot = rows.reduce((a,b) => a + b.usPerFrame, 0);
  const tbl = pvEl('table', { class:'pv-table' });
  tbl.append(pvEl('thead', {}, pvEl('tr', {},
    pvEl('th', {}, 'Pass'), pvEl('th', {}, 'Component'),
    pvEl('th', { class:'r' }, 'µs/frame'),
    pvEl('th', { class:'r' }, '% of GPU'),
    pvEl('th', { class:'r' }, 'times timed'))));
  const tb = pvEl('tbody');
  for (const p of rows){
    if (p.usPerFrame < 0.05) continue;
    tb.append(pvEl('tr', {},
      pvEl('td', { class:'mono' }, p.name),
      pvEl('td', {}, p.node ? pvNodeLabel(p.node) : '—'),
      pvEl('td', { class:'r mono' }, pvNum(p.usPerFrame,1)),
      pvEl('td', { class:'r mono' }, tot > 0 ? pvPct(p.usPerFrame/tot) : '—'),
      pvEl('td', { class:'r mono' }, pvInt(p.samples))));
  }
  tbl.append(tb);
  sec.append(pvEl('div', { class:'pv-table-wrap' }, tbl));
  return sec;
}

/* ---- method note ---------------------------------------------------------- */
function pvMethodSection(V){
  const b = (PERF && PERF.build) || {};
  const sec = pvEl('section', { class:'pv-sec pv-method' });
  sec.append(pvEl('h3', {}, 'How these numbers were taken'));
  const ul = pvEl('ul');
  const li = t => ul.append(pvEl('li', {}, t));
  if (V.live){
    li('Live: one sample per rendered frame from the running game, over the '
     + '--telemetry WebSocket. GPU pass times come back through a fence ring, so '
     + 'they arrive two or three frames late and are matched to the frame that '
     + 'produced them; the frame path never blocks on them.');
    li('A frame whose GPU numbers had not landed is marked in the tooltip rather '
     + 'than drawn as a GPU that cost nothing.');
    li('The x axis is wall-clock frames, not sim ticks: a frame may run zero or '
     + 'several 30 Hz ticks.');
  } else {
    li('Recorded headless by `sandvox --perf` at ' + (b.renderW||'?') + '×'
     + (b.renderH||'?') + ' on ' + (b.adapter||'an unknown device')
     + ', ' + (b.residency||'?') + ' residency.');
    li('Every scenario regenerates and re-settles the world (300 ticks) before '
     + 'recording, so any subset of scenarios produces the same numbers as the '
     + 'full run.');
    li('Frames in flight are bounded to three, the way a swapchain bounds them. '
     + 'Without that bound the CPU races ahead of the GPU and the measured '
     + 'distribution is a queue depth, not a frame time.');
    li('GPU time is per PASS() row from src/sim/pass_table.def, attributed to '
     + 'components by src/measure/perfnodes.h. The harness refuses to run at all '
     + 'if attaching the timer moves the world hash.');
    li('One frame here is one 30 Hz sim tick plus one render, so the x axis is '
     + 'sim seconds and the y axis is wall clock.');
  }
  li('"CPU busy" is work; "CPU waiting" is the frame-queue wait. They are never '
   + 'summed — a large wait means the GPU is the limit, not that the CPU is '
   + 'expensive.');
  sec.append(ul);
  return sec;
}

/* ================= loading, running, live =================
 *
 * Three ways data arrives, and the page treats all three as the same product.
 */

function pvLoad(explicit){
  return fetch('perf.json?t=' + Date.now())
    .then(r => r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)))
    .then(j => {
      PERF = j;
      if (!pvScenario || (pvScenario !== '__live__' &&
                          !PERF.scenarios.some(s => s.id === pvScenario))){
        // ?scenario=<id> deep-links a specific run, so a finding can be shared
        // as a URL instead of "open the tab and click the third chip". Falls
        // back to the first scenario that actually recorded frames.
        const want = new URLSearchParams(location.search).get('scenario');
        const named = want && PERF.scenarios.find(s => s.id === want);
        const first = named || PERF.scenarios.find(s => !s.skipped)
                            || PERF.scenarios[0];
        pvScenario = first ? first.id : null;
      }
      if (typeof activeTab !== 'undefined' && activeTab === 'performance')
        renderPerformance();
      if (explicit && typeof toast === 'function')
        toast('perf.json loaded (' + PERF.scenarios.length + ' scenarios)');
    })
    .catch(e => {
      if (explicit && typeof toast === 'function')
        toast('no perf.json yet — run the tests (' + e.message + ')', true);
    });
}
window.pvLoad = pvLoad;

// Kick off `sandvox --perf` through the tuner server, which takes the SAME run
// mutex every other sandvox invocation takes (scripts/run.sh). Concurrent runs
// saturate the GPU and make every number on this page garbage, so the button
// cannot start a second one.
function pvRunTests(){
  const btn = document.getElementById('pv-run');
  const st = document.getElementById('pv-status');
  if (btn){ btn.disabled = true; btn.textContent = 'Running…'; }
  if (st) st.textContent = 'running the performance suite — this takes a '
                         + 'few minutes; the run mutex is held for its duration';
  fetch('/api/perf/run', { method:'POST' })
    .then(r => r.json())
    .then(j => {
      if (st) st.textContent = j.ok ? 'run complete' : ('run failed: ' + (j.error||''));
      if (j.log && typeof toast === 'function' && !j.ok) toast('perf run failed', true);
      return pvLoad(false);
    })
    .catch(e => { if (st) st.textContent = 'run failed: ' + e.message; })
    .finally(() => {
      if (btn){ btn.disabled = false; btn.textContent = 'Run performance tests'; }
      renderPerformance();
    });
}

// Launch the game with --telemetry and attach. The button does both because
// doing one without the other is never what anyone wants.
function pvWatchLive(){
  if (pvSock){ pvSock.close(); return; }
  pvScenario = '__live__';
  pvLive.status = 'launching the game with --telemetry…';
  renderPerformance();
  // The tuner already has a launcher, and it already passes --telemetry. Reusing
  // it means one place decides how the game is started; a second launch route
  // would be a second argv to keep in step.
  fetch('/api/play', { method:'POST',
                       headers:{'Content-Type':'application/json'},
                       body: JSON.stringify({ mode:'game' }) })
    .then(r => r.json())
    .then(j => {
      if (!j.ok) throw new Error(j.error || 'launch failed');
      // The device + SPIR-V boot is a few seconds; retry the socket rather than
      // failing on the first refused connection.
      pvLive.status = 'game launched — connecting…';
      renderPerformance();
      pvConnect(0);
    })
    .catch(e => {
      // Still try to connect: the user may already have a game running with
      // --telemetry, which is the whole point of the flag existing separately.
      pvLive.status = 'could not launch (' + e.message + ') — trying to '
                    + 'attach to a game already running with --telemetry';
      renderPerformance();
      pvConnect(0);
    });
}

// The game's boot is 20-90 s with a COLD SPIR-V cache — Tint compiles every
// shader before the first frame. A budget shorter than that reports "is the game
// running with --telemetry?" about a game that was always going to come up, and
// the user's only clue is a message telling them to do the thing they just did.
const PV_RETRY_MAX = 120;      // ~2 min at 1 s, comfortably past a cold boot
// How long a CONNECTED but v2-silent peer is given before the page stops saying
// "waiting" and starts saying what is actually wrong.
const PV_V2_GRACE_MS = 6000;

window.pvConnect = pvConnect;
function pvConnect(attempt){
  try { pvSock = new WebSocket('ws://localhost:8080'); }
  catch(e){ pvLive.status = 'bad socket: ' + e.message; renderPerformance(); return; }
  pvSock.onopen = () => {
    pvLive.connected = true;
    pvLive.samples = [];
    pvLive.raw = 0; pvLive.nonV2 = 0; pvLive.sawV2 = false;
    pvLive.status = 'live — waiting for the first frame';
    renderPerformance();
    // "Connected" and "receiving" are different facts, and the page used to
    // show only the first. Whatever is true at the grace deadline, SAY it —
    // a view that waits forever with one message cannot be diagnosed from the
    // screenshot someone sends you.
    clearTimeout(pvLive.probe);
    pvLive.probe = setTimeout(() => {
      if (pvLive.sawV2 || !pvLive.connected) return;
      pvLive.status = pvLive.raw === 0
        ? 'connected, but this peer has sent nothing in '
          + (PV_V2_GRACE_MS / 1000) + 's — it is listening but not broadcasting. '
          + 'A game still compiling shaders looks like this; so does one whose '
          + 'frame loop has stalled.'
        : 'connected, but this peer is not sending v2 samples (' + pvLive.nonV2
          + ' non-v2 messages, 0 v2) — an OLDER sandvox is probably still '
          + 'holding port 8080. Close it and press Watch live again.';
      renderPerformance();
    }, PV_V2_GRACE_MS);
  };
  pvSock.onmessage = ev => {
    pvLive.raw++;
    let m; try { m = JSON.parse(ev.data); } catch(e){ pvLive.nonV2++; return; }
    if (m.v !== 2){                     // v1 stage messages belong to the Engine tab
      pvLive.nonV2++;
      return;
    }
    pvLive.sawV2 = true;
    pvLive.samples.push(m);
    if (pvLive.samples.length > PV_LIVE_CAP) pvLive.samples.shift();
    pvLive.dirty = true;
    pvScheduleRedraw();
  };
  pvSock.onerror = () => {};
  pvSock.onclose = () => {
    pvSock = null;
    pvLive.connected = false;
    clearTimeout(pvLive.probe);
    if (attempt < PV_RETRY_MAX){
      // Say what it is waiting FOR and for how long. "(3/120)" with no unit
      // reads as a failure count; the elapsed seconds read as progress.
      pvLive.status = 'waiting for the game to reach its first frame — '
                    + (attempt + 1) + 's elapsed, giving up at '
                    + PV_RETRY_MAX + 's. A cold shader cache takes 20-90s.';
      setTimeout(() => pvConnect(attempt+1), 1000);
    } else {
      pvLive.status = 'gave up after ' + PV_RETRY_MAX + 's — nothing is '
                    + 'listening on port 8080. Is the game running with '
                    + '--telemetry? Check its console for "telemetry: bind".';
    }
    renderPerformance();
  };
}

// REDRAW AT 10 Hz, NOT PER MESSAGE. Samples arrive at the game's frame rate;
// rebuilding a page of SVG per sample would make the tuner the slowest thing on
// the machine and change the numbers it is displaying.
function pvScheduleRedraw(){
  if (pvLive.raf) return;
  pvLive.raf = setTimeout(() => {
    pvLive.raf = 0;
    if (!pvLive.dirty) return;
    pvLive.dirty = false;
    if (typeof activeTab !== 'undefined' && activeTab === 'performance'
        && pvScenario === '__live__') renderPerformance();
  }, 100);
}
