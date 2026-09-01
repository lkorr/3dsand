/* ============================================================================
   test_melee.mjs — headless checks on the browser port of the stroke driver
   (assets/editor/melee.js) and the two stages assets/editor/anim.js gained
   alongside it.

     node scripts/test_melee.mjs

   WHY THIS EXISTS. melee.js is a PORT, and a port's failure mode is not a
   crash — it is agreeing with itself while disagreeing with the engine. The
   Attacks lane would look perfectly plausible with a sign flipped, a clamp
   missing or a hash that wrapped differently in JS than in C++, and the only
   witness would be the author wondering why the game does not match. So the
   checks below are all things that can be stated WITHOUT running the engine:
   arithmetic identities against an independent oracle, invariants the C++
   states in its own comments, and the shipped asset itself.

   Follows scripts/test_limblib.mjs: plain Node, no deps, no browser, exit 1 on
   the first failure with the numbers printed.
   ========================================================================== */

import { readFileSync } from 'node:fs';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
// pathToFileURL, not a bare path: on Windows an absolute path starts with a
// drive letter and the ESM loader reads "c:" as an unsupported URL scheme.
// Same note as scripts/test_limblib.mjs.
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);

const MELEE = await load('assets/editor/melee.js');
const AN = await load('assets/editor/anim.js');

let failures = 0, checks = 0;
function check(ok, what, detail) {
  checks++;
  if (ok) return;
  failures++;
  console.error(`  FAIL  ${what}${detail ? '\n        ' + detail : ''}`);
}
function section(n) { console.log('\n== ' + n + ' =='); }

const near = (a, b, eps = 1e-6) => Math.abs(a - b) <= eps;

/* ==========================================================================
   1. rng — the counter-based hash the jitter draws from.

   JS has no u32: `*` is float and `<<`/`^` are SIGNED 32-bit, so a naive
   transcription of Pcg silently loses the high bits. These five vectors were
   produced by an INDEPENDENT re-implementation in Python with explicit
   & 0xFFFFFFFF masking, not by this file, so they check the arithmetic rather
   than the transcription's self-consistency.
   ========================================================================== */
section('rng::Hash3 / SignedUnit (sim/rng.h)');
const HASH_VECTORS = [
  [[0, 0, 0], 2145236065],
  [[1, 2, 3], 1493219802],
  [[0xDEADBEEF, 7, 0], 832200866],
  [[12345, 1, 0], 3002418398],
  [[0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF], 3078439099],
];
for (const [args, want] of HASH_VECTORS)
  check(MELEE.hash3(...args) >>> 0 === want,
    `Hash3(${args.join(', ')}) === ${want}`,
    `got ${MELEE.hash3(...args) >>> 0}`);
// SignedUnit is uniform in [-1, 1): the top of the range is never reached.
let lo = 1, hi = -1;
for (let i = 0; i < 20000; i++) {
  const u = MELEE.signedUnit(MELEE.hash3(i, 0, 0));
  lo = Math.min(lo, u); hi = Math.max(hi, u);
}
check(lo >= -1 && hi < 1, 'SignedUnit stays in [-1, 1)', `range [${lo}, ${hi}]`);

/* ==========================================================================
   2. the shipped style library
   ========================================================================== */
section('assets/mobs/attack_styles.json');
const rawStyles = JSON.parse(
  readFileSync(join(ROOT, 'assets/mobs/attack_styles.json'), 'utf8'));
const lib = MELEE.parseStyleLibrary(rawStyles);

check(lib.log.length === 0, 'the shipped file loads with no loader complaints',
  lib.log.join(' | '));
check(lib.styles.length > 0, 'it defines at least one style');
check(MELEE.playerMapUsable(lib), 'it ships a usable player flick map');

// Every style a sector or the neutral pair names must resolve — the engine
// skips an unknown one LOUDLY and falls back, which is a silent behaviour
// change rather than an error, so it has to be caught here.
for (const s of (rawStyles.player?.sectors || []))
  check(lib.styles.some(x => x.name === s.style),
    `player sector "${s.style}" resolves`);
for (const n of (rawStyles.player?.neutralAlternate || []))
  check(lib.styles.some(x => x.name === n),
    `neutralAlternate "${n}" resolves`);

// EVERY SECTOR MUST BE REACHABLE. Sectors do not tile the circle — they
// compete for it by max dot — so adding one can silently swallow another, and
// nothing in the engine would report that. This is the check the Attacks
// panel's compass pad draws, run as an assertion.
{
  const won = new Set();
  for (let k = 0; k < 3600; k++) {
    const a = k / 3600 * Math.PI * 2;
    const i = MELEE.quantizeStrike(lib, Math.cos(a), Math.sin(a));
    if (i >= 0) won.add(i);
  }
  for (const sec of lib.player.sectors)
    check(won.has(sec.style),
      `sector -> "${lib.styles[sec.style].name}" is reachable by some flick`);
}

// The compass as attack_styles.json's own `player_notes` describes it:
// "right flick = cut from the right, up = overhead, down = thrust". +y is
// DOWN (raw screen mouse), which is the sign everybody gets wrong.
const flickName = (x, y) => {
  const i = MELEE.quantizeStrike(lib, x, y);
  return i >= 0 ? lib.styles[i].name : '(none)';
};
check(flickName(1, 0) === 'player_horizontal_r', 'flick RIGHT -> horizontal_r',
  `got ${flickName(1, 0)}`);
check(flickName(-1, 0) === 'player_horizontal_l', 'flick LEFT -> horizontal_l',
  `got ${flickName(-1, 0)}`);
check(flickName(0, -1) === 'player_overhead', 'flick UP (-y) -> overhead',
  `got ${flickName(0, -1)}`);
check(flickName(0, 1) === 'player_thrust', 'flick DOWN (+y) -> thrust',
  `got ${flickName(0, 1)}`);

// NeutralStrike alternates, and returns the OTHER one when a side is unset.
check(MELEE.neutralStrike(lib, true) !== MELEE.neutralStrike(lib, false),
  'the two neutral entries differ');

/* ==========================================================================
   3. the driver, on a synthetic arm.

   A 7-voxel arm holding an 11-voxel blade, which is roughly the human rig
   (strokes.cpp:160 quotes "the arm is ~7 voxels and the band the driver can
   serve is [3.8, 6.5]").
   ========================================================================== */
section('MeleeState + the stroke program');

const RIGHT = { x: -1, y: 0, z: 0 }, UP = { x: 0, y: 1, z: 0 }, FWD = { x: 0, y: 0, z: 1 };
const DT = 1 / 30;

function freshDriver() {
  const m = new MELEE.MeleeState(MELEE.defaultMeleeTuning());
  m.setHandSign(1);
  // Seeded as the rig would: hand a little forward of the shoulder, blade out
  // along it. The exact values do not matter; that SetStroke measures the
  // blade rather than reading a tuning row is the point.
  m.setStroke({ x: 0.5, y: -2.0, z: 3.0 }, { x: 0.5, y: -2.0, z: 14.0 },
              { x: 0, y: 1, z: 0 }, 7.0);
  return m;
}

// The reach band is an ANNULUS, and an authored offset of 0 must land inside
// it — that is what kNeutralReach means and what "authored against the ARM
// landed outside the band and was clamped" was the bug.
{
  const m = freshDriver();
  const { lo: bl, hi: bh } = m.reachBand();
  check(bh > bl && bl > 0, 'the reach band is a non-degenerate annulus',
    `[${bl.toFixed(2)}, ${bh.toFixed(2)}]`);
  const at0 = MELEE.strokeReachIn(m, 0);
  check(at0 > bl && at0 < bh, 'reach offset 0 lands strictly inside the band',
    `${at0.toFixed(2)} in [${bl.toFixed(2)}, ${bh.toFixed(2)}]`);
  check(near(at0, bl + MELEE.kNeutralReach * (bh - bl), 1e-4),
    'reach 0 is exactly kNeutralReach of the band');
  check(MELEE.strokeReachIn(m, -10) === bl && MELEE.strokeReachIn(m, 10) === bh,
    'reach offsets clamp to the band ends rather than escaping it');
}

/** Run one style to completion, sampling the driver every tick. */
function runStyle(name, aimAz = 0, aimEl = 0, seed = 1) {
  const si = lib.styles.findIndex(s => s.name === name);
  if (si < 0) return null;
  const sty = lib.styles[si];
  const m = freshDriver();
  const cur = MELEE.newStrokeCursor();
  MELEE.beginStrokeProgram(cur, sty, si, seed);
  const trace = [];
  for (let i = 0; i < 400; i++) {
    // The phase is sampled BEFORE the step. StepStrokeProgram advances
    // `phaseTick` and transitions inside the same call, so the tick that ENDS
    // the windup already reads Cut on the way out — sampling after would
    // undercount every phase by one and is not a fact about the driver.
    const phase = cur.phase;
    const r = MELEE.stepStrokeProgram(cur, sty, m, aimAz, aimEl, DT, RIGHT, UP, FWD);
    trace.push({
      phase, az: m.strokeAz(), el: m.strokeEl(),
      radius: m.strokeRadius(), weight: m.poseWeight(),
      hand: { ...m.hand_ }, tip: { ...m.tip_ },
      handL: { ...m.handL_ },
      driver: m.phaseName(),
    });
    if (r === MELEE.STEP.Finished) break;
  }
  return { sty, cur, trace, m };
}

// ---- THE CENTRAL CLAIM: the cut passes THROUGH the aim -------------------
// strokes.h: "the windup's own target is aim - cut/2 + windup, so the MIDDLE
// of the travel passes through it rather than the beginning. A stroke aimed at
// its own start point cuts the air behind the target every time."
for (const nm of ['horizontal_r', 'horizontal_l', 'diagonal', 'overhead']) {
  const r = runStyle(nm, 0.25, 0.10);
  if (!r) { check(false, `style "${nm}" exists`); continue; }
  const cutTicks = r.trace.filter(t => t.phase === MELEE.STROKE_PHASE.Cut);
  check(cutTicks.length > 0, `${nm}: the program reaches its Cut phase`);
  if (!cutTicks.length) continue;
  const azs = cutTicks.map(t => t.az);
  const spans = (v, arr) => Math.min(...arr) <= v && v <= Math.max(...arr);
  // The dominant axis of the style is where the crossing has to happen; an
  // overhead is an ELEVATION cut and barely moves in azimuth.
  const dom = Math.abs(r.sty.cut.az) >= Math.abs(r.sty.cut.el) ? 'az' : 'el';
  const arr = dom === 'az' ? azs : cutTicks.map(t => t.el);
  const aimV = dom === 'az' ? 0.25 : 0.10;
  check(spans(aimV, arr),
    `${nm}: the cut's ${dom} sweeps THROUGH the aim (${aimV})`,
    `range [${Math.min(...arr).toFixed(2)}, ${Math.max(...arr).toFixed(2)}]`);
}

// ---- the phases run in order and for the authored number of ticks -------
{
  const r = runStyle('horizontal_r', 0, 0);
  const count = p => r.trace.filter(t => t.phase === p).length;
  // The tick counts are TEMPO-JITTERED, so the assertion is against the
  // cursor's own post-jitter values rather than the authored ones.
  check(count(MELEE.STROKE_PHASE.Windup) === r.cur.windupTicks,
    'windup runs exactly cur.windupTicks ticks',
    `${count(MELEE.STROKE_PHASE.Windup)} vs ${r.cur.windupTicks}`);
  check(count(MELEE.STROKE_PHASE.Cut) === r.cur.cutTicks,
    'cut runs exactly cur.cutTicks ticks',
    `${count(MELEE.STROKE_PHASE.Cut)} vs ${r.cur.cutTicks}`);
  check(r.cur.aimed, 'the aim was frozen at the end of the windup');
  // THE WINDUP IS A TELEGRAPH: it must stay under commitSpeed, i.e. the DRIVER
  // must remain in Guard for the whole of it and never fire its own Slash.
  const windDriver = r.trace
    .filter(t => t.phase === MELEE.STROKE_PHASE.Windup)
    .map(t => t.driver);
  check(!windDriver.includes('slash'),
    'the windup never commits the driver (it stays a telegraph)',
    `saw: ${[...new Set(windDriver)].join(', ')}`);
}

// ---- PoseWeight: the arm is claimed through the cut and handed back -----
{
  const r = runStyle('overhead', 0, 0);
  const cut = r.trace.filter(t => t.phase === MELEE.STROKE_PHASE.Cut);
  check(cut.every(t => t.weight > 0.99),
    'the stroke owns the arm outright through the cut');
  check(r.trace[r.trace.length - 1].weight < 0.5,
    'the recover hands the arm back (PoseWeight ramps down)',
    `ended at ${r.trace[r.trace.length - 1].weight.toFixed(2)}`);
}

// ---- the hand is somewhere an arm can actually be ------------------------
//
// THE INVARIANT IS NOT "|tip - hand| == bladeLen". The hand-back plane clamp
// deliberately breaks that: it pins the hand to the plane and RE-AIMS the
// blade at the commanded tip, which melee.cpp:1157 states in as many words —
// "the rig consumes hand + direction, never the tip, so the sword stays rigid
// in the fist and only the geometry of the ask changes". Measured on a
// 11-voxel blade over a deep across-body diagonal, that clamp fires on 20 of
// 29 ticks, so a rigidity assertion would fail on correct behaviour.
//
// What must hold on EVERY tick is the pair of things the arm cannot violate:
// the hand is inside the arm's reach, and it is not behind the shoulder's
// frontal plane. And on the ticks where the plane clamp did NOT fire, the
// blade IS rigid — that is the law of cosines earning its keep.
{
  const t = MELEE.defaultMeleeTuning();
  for (const nm of ['diagonal', 'horizontal_r', 'thrust', 'overhead']) {
    const r = runStyle(nm, 0.2, 0);
    if (!r) continue;
    const L = r.m.bladeLen_;
    const handReach = r.m.armReach_ * t.reachFraction;
    const backLim = -t.handBackFrac * handReach;
    // Filtered on the DRIVER's phase, not the program's. The two run on
    // different clocks: melee.recoverTime is 0.22 s while an authored
    // `recover` is 10-12 ticks (0.33-0.40 s), so the driver returns to Idle
    // partway through the program's recover — and an Idle driver deliberately
    // DECAYS hand_/tip_ toward zero ("let the arm hang; the walk cycle owns
    // the pose", melee.cpp:1650). Those ticks describe no blade at all.
    const live = r.trace.filter(x => x.driver !== 'idle');

    const over = live.filter(x =>
      Math.hypot(x.handL.x, x.handL.y, x.handL.z) > handReach + 1e-3);
    check(over.length === 0, `${nm}: the hand never leaves the arm's reach`,
      over.length ? `${over.length} ticks, worst ${Math.max(...over.map(x =>
        Math.hypot(x.handL.x, x.handL.y, x.handL.z))).toFixed(2)} > ${
        handReach.toFixed(2)}` : '');

    const behind = live.filter(x => x.handL.z < backLim - 1e-3);
    check(behind.length === 0,
      `${nm}: the hand never goes behind the shoulder's frontal plane`,
      behind.length ? `${behind.length} ticks, worst z ${Math.min(
        ...behind.map(x => x.handL.z)).toFixed(2)} < ${backLim.toFixed(2)}` : '');

    // Unclamped ticks: the hand really is the tip minus a whole blade.
    const free = live.filter(x => x.handL.z > backLim + 1e-3);
    const errs = free.map(x => Math.abs(Math.hypot(
      x.tip.x - x.hand.x, x.tip.y - x.hand.y, x.tip.z - x.hand.z) - L));
    if (errs.length)
      check(Math.max(...errs) < 1e-3,
        `${nm}: the blade is exactly rigid on the unclamped ticks`,
        `${free.length}/${live.length} free, worst error ${
          Math.max(...errs).toFixed(5)} on a ${L.toFixed(2)} blade`);
  }
}

// ---- the azimuth window is respected ------------------------------------
{
  const t = MELEE.defaultMeleeTuning();
  for (const nm of lib.styles.map(s => s.name)) {
    const r = runStyle(nm, 1.2, 0.6);              // aim hard into the stop
    if (!r) continue;
    const bad = r.trace.filter(x => x.az > t.azOut + 1e-4 || x.az < -t.azAcross - 1e-4);
    check(bad.length === 0, `${nm}: the stroke never leaves the azimuth window`,
      bad.length ? `${bad.length} ticks, worst az ${
        bad.reduce((a, b) => Math.abs(b.az) > Math.abs(a.az) ? b : a).az.toFixed(2)}` : '');
    const badEl = r.trace.filter(x => x.el > t.elMax + 1e-4 || x.el < t.elMin - 1e-4);
    check(badEl.length === 0, `${nm}: the stroke never leaves the elevation window`);
  }
}

// ---- DETERMINISM (CLAUDE.md rule 1) -------------------------------------
// Same seed -> the same swing, bit for bit. This is the property the jitter
// exists to have and the one a Math.random() would quietly destroy.
{
  const a = runStyle('horizontal_r', 0.3, 0.1, 0x51ED);
  const b = runStyle('horizontal_r', 0.3, 0.1, 0x51ED);
  const same = JSON.stringify(a.trace) === JSON.stringify(b.trace);
  check(same, 'the same seed replays the same swing exactly');
  const c = runStyle('horizontal_r', 0.3, 0.1, 0x51EE);
  check(JSON.stringify(a.trace) !== JSON.stringify(c.trace),
    'a different seed produces a different swing (the jitter is live)');
}

// ---- tempo jitter actually moves the tick counts ------------------------
{
  const si = lib.styles.findIndex(s => s.name === 'horizontal_r');
  const sty = lib.styles[si];
  const seen = new Set();
  for (let s = 0; s < 200; s++) {
    const cur = MELEE.newStrokeCursor();
    MELEE.beginStrokeProgram(cur, sty, si, s);
    seen.add(cur.windupTicks);
    check(cur.windupTicks >= 2 && cur.cutTicks >= 2,
      'jittered tick counts never fall below the floor of 2');
  }
  check(seen.size > 1, 'jitter.tempo varies the windup length across swings',
    `saw ${[...seen].sort((a, b) => a - b).join(', ')}`);
  // ...and the player styles, authored at jitter 0, must NOT vary: a strike
  // has to go exactly where it was flicked.
  const pi = lib.styles.findIndex(s => s.name === 'player_horizontal_r');
  if (pi >= 0) {
    const p = lib.styles[pi];
    const pseen = new Set();
    for (let s = 0; s < 50; s++) {
      const cur = MELEE.newStrokeCursor();
      MELEE.beginStrokeProgram(cur, p, pi, s);
      pseen.add(cur.windupTicks + ':' + cur.cutTicks);
    }
    check(pseen.size === 1,
      'the player styles are jitter-free (a strike goes where it was flicked)',
      `saw ${[...pseen].join(', ')}`);
  }
}

// ---- tuning.json reaches the driver -------------------------------------
// The editor's melee tuning is read off the SAME document the Tuning tab
// edits, through the metre -> voxel conversion ApplyMeleeTuning does. If that
// map is wrong the whole preview is wrong by a constant nobody would spot.
{
  const tj = JSON.parse(
    readFileSync(join(ROOT, 'assets/materials/tuning.json'), 'utf8'));
  const t = MELEE.meleeTuningFrom(tj);
  check(near(t.azOut, tj.melee.azOut), 'a bare radian passes through unscaled');
  check(near(t.fallbackReach, tj.melee.fallbackReachM / MELEE.kVoxelMeters, 1e-4),
    'a `...M` key is converted metres -> voxels',
    `${t.fallbackReach} vs ${tj.melee.fallbackReachM / MELEE.kVoxelMeters}`);
  check(near(t.minSpeed, tj.melee.minSpeedMps / MELEE.kVoxelMeters, 1e-4),
    'a `...Mps` key is converted m/s -> voxels/s');
  // Every key the C++ ApplyMeleeTuning copies must be reachable, or a knob the
  // Tuning tab shows would silently do nothing in the preview.
  const missing = Object.keys(tj.melee).filter(k => {
    if (k === 'controlMode' || k === 'pickMinSpeed') return false;   // by design
    const base = k.replace(/(M|Mps)$/, '');
    return !(base in t);
  });
  check(missing.length === 0,
    'every tuning.json melee key reaches the ported driver',
    'unmapped: ' + missing.join(', '));
}

/* ==========================================================================
   4. anim.js's new stages
   ========================================================================== */
section('anim.js stage 6 — AnimClampPoseLimits');

// A hinge authored [0, 130] degrees must never come out beyond it, and the
// off-axis swing must be DISCARDED (that is what `hinge: true` means, as
// opposed to the axis form which keeps it).
{
  const sidecar = {
    root: 'a',
    limbs: [
      { name: 'a' },
      { name: 'b', parent: 'a', anchor: [0, 0, 0],
        poseLimit: { axis: [1, 0, 0], min: 0, max: 130, hinge: true } },
    ],
  };
  const models = [
    { name: 'a', offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 } },
    { name: 'b', offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 } },
  ];
  const sk = AN.buildSkeleton(sidecar, models);
  const bi = sk.findPart('b');
  check(sk.parts[bi].hasPoseLimit, 'the axis/hinge form parses');
  check(sk.parts[bi].poseHinge, '`hinge: true` survives the parse');
  check(near(sk.parts[bi].poseMax, 130 * Math.PI / 180, 1e-6),
    'degrees are converted to radians at parse time',
    `${sk.parts[bi].poseMax}`);

  for (const [ang, want] of [[-1.0, 0], [0.5, 0.5], [3.0, 130 * Math.PI / 180]]) {
    const st = { clips: [], local: [], model: [], partAlive: [1, 1], springs: [] };
    AN.animSampleAndBlend(sk, st, 0);
    AN.animFlatten(sk, st);
    // Pose the child out of range in MODEL space, which is the frame the IK
    // writes and the frame the clamp reads.
    st.model[bi] = {
      rot: AN.qmul(st.model[sk.parts[bi].parent].rot,
                   AN.qmul(sk.parts[bi].rest.rot, AN.qaxisangle({ x: 1, y: 0, z: 0 }, ang))),
      pos: st.model[bi].pos,
    };
    AN.animClampPoseLimits(sk, st, null);
    const par = st.model[sk.parts[bi].parent];
    const delta = AN.qmul(AN.qconj(sk.parts[bi].rest.rot),
                          AN.qmul(AN.qconj(par.rot), st.model[bi].rot));
    const got = AN.animHingeAngleAbout(delta, { x: 1, y: 0, z: 0 });
    check(near(got, want, 1e-4),
      `a hinge posed to ${ang.toFixed(2)} rad clamps to ${want.toFixed(3)}`,
      `got ${got === null ? 'null' : got.toFixed(4)}`);
  }
}

// The ball form: the shipped human's shoulder. Its `reach` planes bound where
// the bone may POINT, and the two normals must be perpendicular — a tilted
// pair is a load error in the engine because the closed-form projection is
// only the nearest legal direction when they are square.
{
  const human = JSON.parse(readFileSync(join(ROOT, 'assets/mobs/human.json'), 'utf8'));
  const models = human.limbs.map(l => ({
    name: l.name, offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 },
  }));
  const sk = AN.buildSkeleton(human, models);
  let balls = 0, hinges = 0, warns = [];
  for (const p of sk.parts) {
    if (p.poseBall?.has) balls++;
    if (p.hasPoseLimit) hinges++;
    if (p.poseLimitWarn) warns.push(`${p.name}: ${p.poseLimitWarn}`);
  }
  check(balls >= 2, 'the human rig parses its shoulder ball limits', `${balls} found`);
  check(hinges >= 2, 'the human rig parses its elbow/knee axis limits', `${hinges} found`);
  check(warns.length === 0, 'no poseLimit in human.json is malformed',
    warns.join(' | '));

  // Stage 6 is a no-op on a rig posed inside its own limits: the rest pose.
  const st = { clips: [], local: [], model: [], partAlive: sk.parts.map(() => 1), springs: [] };
  AN.animSampleAndBlend(sk, st, 0);
  AN.animFlatten(sk, st);
  const before = JSON.stringify(st.model);
  AN.animClampPoseLimits(sk, st, null);
  check(before === JSON.stringify(st.model),
    'the clamp leaves an in-range (rest) pose bit-for-bit unchanged');
}

section('anim.js stage 3.5 — AnimApplySpineTwist');
{
  const sidecar = {
    root: 'hips',
    limbs: [
      { name: 'hips', tag: 'spine' },
      { name: 'torso', parent: 'hips', tag: 'spine', anchor: [0, 1, 0] },
      { name: 'head', parent: 'torso', tag: 'head', anchor: [0, 2, 0] },
    ],
  };
  const models = sidecar.limbs.map(l => ({
    name: l.name, offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 },
  }));
  const sk = AN.buildSkeleton(sidecar, models);
  const mk = () => {
    const st = { clips: [], local: [], model: [], partAlive: sk.parts.map(() => 1), springs: [] };
    AN.animSampleAndBlend(sk, st, 0);
    return st;
  };
  // Zero in, nothing touched — the engine's own early-out, and what keeps an
  // idle rig and every legacy gate pose untouched.
  {
    const st = mk();
    const before = JSON.stringify(st.local);
    AN.animApplySpineTwist(sk, st, 0, 0, sk.rootLimb);
    check(before === JSON.stringify(st.local),
      'a zero twist is an exact no-op');
  }
  // The ROOT spine part is excluded (it is the pelvis; twisting it would turn
  // the whole creature), and the share is split across the remaining ones.
  {
    const st = mk();
    const rootIdx = sk.rootLimb;
    const beforeRoot = JSON.stringify(st.local[rootIdx]);
    AN.animApplySpineTwist(sk, st, 0.4, 0, rootIdx);
    check(JSON.stringify(st.local[rootIdx]) === beforeRoot,
      'the ROOT spine part is never twisted');
    const ti = sk.findPart('torso');
    const ang = AN.animHingeAngleAbout(st.local[ti].rot, { x: 0, y: 1, z: 0 });
    // One non-root spine part, so it takes the whole share; NEGATIVE is
    // "toward the right" in model space (anim.cpp:410 quotes the derivation).
    check(ang !== null && near(ang, -0.4, 1e-4),
      'the twist lands on the non-root spine part with the engine\'s sign',
      `got ${ang === null ? 'null' : ang.toFixed(4)}`);
  }
}

section('anim.js — clip rate and the blend-in clock');
{
  const sidecar = {
    root: 'a',
    limbs: [{ name: 'a' }, { name: 'b', parent: 'a', anchor: [0, 1, 0] }],
    clips: {
      walk: {
        durationMs: 600, loop: true, blendInMs: 180,
        tracks: { b: { rot: [{ t: 0, rot: [0, 0, 0, 1] },
                             { t: 300, rot: [0.3, 0, 0, 0.954] },
                             { t: 600, rot: [0, 0, 0, 1] }] } },
      },
    },
  };
  const models = sidecar.limbs.map(l => ({
    name: l.name, offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 },
  }));
  const sk = AN.buildSkeleton(sidecar, models);
  // THE BLEND-IN MUST HAPPEN ONCE, not once per loop. Keyed on timeMs (the
  // pre-2026-09-01 port) a looping clip re-dipped to zero weight at the top of
  // every cycle, which is three times a second on a 600 ms walk.
  const st = { clips: [{ clip: 0, timeMs: 0, ageMs: 0, rate: 1, weight: 1,
                         stopping: false, fade: 1 }],
               local: [], model: [], partAlive: [1, 1], springs: [] };
  let minFadeAfterBlendIn = 1;
  for (let i = 0; i < 120; i++) {                     // 2 s at 60 fps
    AN.animSampleAndBlend(sk, st, 1 / 60);
    const inst = st.clips[0];
    if (inst.ageMs > 200)
      minFadeAfterBlendIn = Math.min(minFadeAfterBlendIn,
        AN.clipFade(sk.clips[0], inst.timeMs, inst.ageMs, false, 1));
  }
  check(near(minFadeAfterBlendIn, 1, 1e-9),
    'a looping clip blends in ONCE, not once per cycle',
    `min fade after the blend-in was ${minFadeAfterBlendIn.toFixed(3)}`);

  // `rate` multiplies the PLAYHEAD and not the AGE — that split is what lets
  // the stride lock re-rate a walk without disturbing its blend.
  const st2 = { clips: [{ clip: 0, timeMs: 0, ageMs: 0, rate: 2, weight: 1,
                          stopping: false, fade: 1 }],
                local: [], model: [], partAlive: [1, 1], springs: [] };
  AN.animSampleAndBlend(sk, st2, 0.1);
  check(near(st2.clips[0].timeMs, 200, 1e-6),
    'rate 2 advances the playhead at double speed',
    `${st2.clips[0].timeMs}`);
  check(near(st2.clips[0].ageMs, 100, 1e-6),
    'rate does NOT touch the age', `${st2.clips[0].ageMs}`);
}

section('anim.js — the locomotion clip family');
{
  const sidecar = {
    root: 'a', speed: 60,
    limbs: [{ name: 'a' }],
    clips: {
      idle: { durationMs: 900, loop: true, tracks: {} },
      walk: { durationMs: 600, loop: true, tracks: {} },
      run: { durationMs: 400, loop: true, tracks: {} },
    },
  };
  const sk = AN.buildSkeleton(sidecar,
    [{ name: 'a', offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 } }]);
  const st = {};
  const at = v => AN.pickLocoClip(sk, st, v, 60, {});
  check(at(0) === 'idle', 'standing still picks idle', at(0));
  check(at(0.2) === 'idle', 'below 0.4 vox/s is still idle', at(0.2));
  check(at(20) === 'walk', 'a plain walk picks walk', at(20));
  // THE SPLIT IS NOT AT HALF SPEED. `speed` is the SPRINT reference, so 35 of
  // 60 is a plain walk sitting at 0.58 of it — a 0.55 threshold flipped every
  // frame and neither clip got past a fraction of its 180 ms blend-in, which
  // is the "arms held out stiff" report.
  st.running = false;
  check(at(35) === 'walk', '35 of 60 vox/s is a WALK, not a run', at(35));
  check(at(50) === 'run', '50 of 60 crosses the 0.80 runOn threshold', at(50));
  // ...and hysteresis: once running, it stays running down to 0.70.
  check(at(45) === 'run', 'hysteresis keeps it running down to runOff', at(45));
  check(at(40) === 'walk', 'below runOff it drops back to walk', at(40));
  // Exclusivity is the caller's job (rig.js), but the family must never name
  // two at once.
  check(typeof at(35) === 'string', 'the family returns exactly one name');
  // A rig missing a clip falls back rather than naming one that is not there.
  const bare = AN.buildSkeleton({ root: 'a', limbs: [{ name: 'a' }] },
    [{ name: 'a', offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 } }]);
  check(AN.pickLocoClip(bare, {}, 30, 60, {}) === '',
    'a rig with no clips names none rather than a missing one');
}

/* ========================================================================== */
console.log(`\n${checks - failures} / ${checks} checks passed`);
if (failures) {
  console.error(`${failures} FAILED`);
  process.exit(1);
}
