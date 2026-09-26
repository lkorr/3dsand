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

/* --------------------------------------------------------------------------
   THE UNARMED SCHEMA (docs/PLAN_impact_unarmed.md §4/§5)

   Four fields and a second compass, each of which defaults to the behaviour a
   style authored before them already had — so the interesting assertions are
   that the defaults really are inert and that the new keys really do reach the
   parsed style. A key that silently failed to parse would read exactly like a
   style nobody had got round to authoring.
   ------------------------------------------------------------------------ */
{
  const by = n => lib.styles.find(s => s.name === n);
  for (const s of lib.styles) {
    if (!s.name.startsWith('player_') && !s.raw.weapon)
      check(s.weapon === 'held' && !s.fallback && s.reach === 0
            && !MELEE.lungeAny(s.lunge) && s.target.length === 0,
        `style "${s.name}" authored before the unarmed schema is unchanged`);
  }
  const punch = by('punch_r'), bite = by('bite_lunge');
  // REACH 0 IS THE ANSWER NOW, not a missing number: a natural style asks the
  // BODY how far it reaches (MobSystem::StyleReachOn), because every authored
  // one was a lie the rig could not keep -- punch_r claimed 9 on an arm that
  // reaches 5 and --shot-strike measured the fist stopping 6.7 voxels short.
  // So the assertion is the opposite of what it was: a natural style must NOT
  // carry a hand-guessed reach.
  check(!!punch && punch.weapon === 'fist.R' && punch.fallback
        && punch.reach === 0,
    'punch_r names a natural weapon, is a fallback, and derives its reach');
  for (const s2 of lib.styles)
    if (s2.weapon !== 'held')
      check(s2.reach === 0,
        `natural style "${s2.name}" derives its reach from the body`);
  check(!!bite && bite.weapon === 'jaws' && !bite.fallback,
    'bite_lunge names the jaws and is NOT a fallback');
  check(!!bite && MELEE.lungeAny(bite.lunge) && bite.lunge.at === 'windup'
        && bite.lunge.rise > 0,
    'bite_lunge has a lunge that fires at the windup and leaves the ground',
    bite ? JSON.stringify(bite.lunge) : '(missing)');
  // A LUNGE WITH NO RISE LANDS ON THE TICK AFTER IT LEFT (mob.cpp UpdateFall's
  // launch latch buys exactly one tick), so every authored one must climb.
  for (const s of lib.styles)
    if (MELEE.lungeAny(s.lunge))
      check(s.lunge.rise > 0, `style "${s.name}" lunges with a non-zero rise`);
  // The target table is SORTED BY TAG, because the draw walks it and JSON
  // object order is the library's business rather than the author's.
  check(!!bite && bite.target.length >= 3
        && bite.target.every((t, i) => i === 0 || bite.target[i - 1].tag < t.tag),
    'bite_lunge has a target table, sorted by tag for a reproducible draw');

  // The fallback filter: an armed creature never draws a punch, a disarmed one
  // draws nothing else. `usable` stands in for the rig (see pickAttackStyle).
  const listed = ['horizontal_r', 'overhead', 'thrust', 'punch_r', 'punch_l'];
  const armed = new Set(), unarmed = new Set();
  for (let t = 0; t < 400; t++) {
    armed.add(MELEE.pickAttackStyle(lib, listed, 7, t, () => true));
    unarmed.add(MELEE.pickAttackStyle(lib, listed, 7, t,
      s => s.weapon !== 'held'));
  }
  check([...armed].every(i => i >= 0 && !lib.styles[i].fallback),
    'an armed creature never draws a fallback style over 400 ticks');
  check([...unarmed].every(i => i >= 0 && lib.styles[i].weapon !== 'held')
        && unarmed.size >= 2,
    'a disarmed one draws only its natural weapons, and varies over them');
  check(MELEE.pickAttackStyle(lib, listed, 7, 0, () => false) === -1,
    'nothing usable draws -1 rather than a style with no weapon behind it');

  // ...and the second compass.
  check(MELEE.playerMapUsable(lib.playerUnarmed),
    'attack_styles.json ships a usable playerUnarmed flick map');
  const fistName = (x, y) => {
    const i = MELEE.quantizeStrike(lib.playerUnarmed, x, y);
    return i >= 0 ? lib.styles[i].name : '(none)';
  };
  check(fistName(1, 0) === 'player_punch_r', 'fists: flick RIGHT -> punch_r',
    `got ${fistName(1, 0)}`);
  check(fistName(-1, 0) === 'player_punch_l', 'fists: flick LEFT -> punch_l',
    `got ${fistName(-1, 0)}`);
  check(fistName(0, 1) === 'player_punch_thrust',
    'fists: flick DOWN (+y) -> the two-handed drive', `got ${fistName(0, 1)}`);
  for (const sec of lib.playerUnarmed.sectors)
    check(lib.styles[sec.style].weapon !== 'held',
      `playerUnarmed sector -> "${lib.styles[sec.style].name}" is a fist style`);
}

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
  return runParsed(lib.styles[si], si, aimAz, aimEl, seed);
}

/** ...and the same for a style that is not in the shipped library. */
function runParsed(sty, si = 0, aimAz = 0, aimEl = 0, seed = 1) {
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
  // Over the WHOLE path (strokes.h "A CUT IS A PATH"): the dominant channel
  // is a fact about the total travel, not about the opening leg.
  const tot = MELEE.cutTravel(r.sty);
  const dom = Math.abs(tot.az) >= Math.abs(tot.el) ? 'az' : 'el';
  const arr = dom === 'az' ? azs : cutTicks.map(t => t.el);
  const aimV = dom === 'az' ? 0.25 : 0.10;
  check(spans(aimV, arr),
    `${nm}: the cut's ${dom} sweeps THROUGH the aim (${aimV})`,
    `range [${Math.min(...arr).toFixed(2)}, ${Math.max(...arr).toFixed(2)}]`);
}

/* --------------------------------------------------------------------------
   A CUT IS A PATH (strokes.h; 2026-09-21)

   `cut` may be a LIST OF LEGS run back to back inside the one Cut phase. The
   three things worth stating without the engine are the three that would fail
   silently: that the old spelling still produces the OLD program, that a path
   really does bend (and not merely arrive), and that the aim marker moves
   where the target sits along it. All of them are arithmetic on the trace.
   ------------------------------------------------------------------------ */
section('the cut path (strokes.h "A CUT IS A PATH")');
{
  const AIM_AZ = 0.25;
  const probe = (cut) => {
    const l = MELEE.parseStyleLibrary({ styles: [{
      name: 'probe', windup: { ticks: 6, az: 0.2, el: 0.1, reach: -0.05 },
      cut, recover: { ticks: 4 }, jitter: { az: 0, el: 0, tempo: 0 },
    }] });
    return { sty: l.styles[0], log: l.log };
  };
  const cutOf = r => r.trace.filter(t => t.phase === MELEE.STROKE_PHASE.Cut);
  const ONE = { ticks: 8, az: -2.0, el: 0.0, reach: 0.1 };

  // 1. THE OLD SPELLING IS THE OLD PROGRAM. A bare object and a one-element
  //    list have to be the same stroke, tick for tick — that is the whole
  //    compatibility story, and it is the claim that lets the shipped library
  //    stay unedited.
  const obj = probe(ONE), lst = probe([{ ...ONE }]);
  const ra = runParsed(obj.sty, 0, AIM_AZ, 0), rb = runParsed(lst.sty, 0, AIM_AZ, 0);
  check(obj.sty.cut.length === 1 && lst.sty.cut.length === 1,
    'both spellings parse to exactly one leg');
  check(ra.trace.length === rb.trace.length &&
        ra.trace.every((t, i) => near(t.az, rb.trace[i].az, 1e-9) &&
                                 near(t.el, rb.trace[i].el, 1e-9) &&
                                 near(t.radius, rb.trace[i].radius, 1e-9)),
    'a one-leg LIST and a bare cut OBJECT are the same stroke, tick for tick');

  // 2. A PATH BENDS. Two legs whose az sums to the single cut's, but which
  //    dive and then climb in elevation, must END where the straight cut ends
  //    and pass somewhere ELSE on the way. Arriving in the right place is not
  //    the claim — a straight line does that — so the assertion is on the
  //    elevation at the corner.
  const dog = probe([
    { ticks: 4, az: -1.0, el: -0.40, reach: 0.05 },
    { ticks: 4, az: -1.0, el: 0.40, reach: 0.05 },
  ]);
  const rd = runParsed(dog.sty, 0, AIM_AZ, 0);
  check(rd.cur.cutLegs === 2 && rd.cur.legTicks.join(',') === '4,4' &&
        rd.cur.cutTicks === 8,
    'the path resolves to two legs of four ticks inside one 8-tick Cut phase',
    `legs ${rd.cur.cutLegs}, ticks [${rd.cur.legTicks}], total ${rd.cur.cutTicks}`);
  check(cutOf(rd).length === cutOf(ra).length,
    'a two-leg path spends the same ticks cutting as the straight cut it sums to');
  const endStraight = cutOf(ra)[cutOf(ra).length - 1];
  const endDog = cutOf(rd)[cutOf(rd).length - 1];
  check(near(endDog.az, endStraight.az, 0.06),
    'the path ENDS where the straight cut of the same total travel ends',
    `${endDog.az.toFixed(3)} vs ${endStraight.az.toFixed(3)}`);
  const dipDog = Math.min(...cutOf(rd).map(t => t.el));
  const dipStraight = Math.min(...cutOf(ra).map(t => t.el));
  check(dipDog < dipStraight - 0.15,
    'and it goes somewhere else on the way — the corner is really driven',
    `dipped to ${dipDog.toFixed(3)} vs the straight cut's ${dipStraight.toFixed(3)}`);
  // A leg is its own clock: the elevation floor has to happen at the CORNER,
  // not at the end, or what was measured is a slow arc rather than a dogleg.
  const dipAt = cutOf(rd).findIndex(t => near(t.el, dipDog, 1e-9));
  check(dipAt >= 2 && dipAt <= 5,
    'the corner lands at the leg boundary, not at either end of the cut',
    `tick ${dipAt} of ${cutOf(rd).length}`);

  // 3. THE AIM MARKER MOVES THE TARGET ALONG THE PATH. Unmarked, the aim sits
  //    at the midpoint of the whole travel; marking leg 2 puts it in the
  //    middle of leg 2, which means the stroke STARTS further back and crosses
  //    the aim later. A marker that parsed but did nothing would look exactly
  //    like one that worked, from anywhere but here.
  const marked = probe([
    { ticks: 4, az: -1.0, el: -0.40, reach: 0.05 },
    { ticks: 4, az: -1.0, el: 0.40, reach: 0.05, aim: true },
  ]);
  check(marked.sty.aimLeg === 1, 'the "aim" flag names leg 2');
  check(dog.sty.aimLeg === -1, 'and an unmarked path reports no aiming leg');
  const rm = runParsed(marked.sty, 0, AIM_AZ, 0);
  const crossAt = (r) => cutOf(r).findIndex(t => t.az <= AIM_AZ);
  check(crossAt(rd) >= 0 && crossAt(rm) > crossAt(rd),
    'marking the second leg makes the cut meet the target LATER in the path',
    `unmarked at tick ${crossAt(rd)}, marked at ${crossAt(rm)}`);
  check(cutOf(rm).some(t => t.az <= AIM_AZ) && cutOf(rm)[0].az > AIM_AZ,
    'and it still crosses the aim rather than starting past it');

  // 4. The loader's own refusals, which are the ones an author meets.
  const many = probe(Array.from({ length: MELEE.MAX_CUT_LEGS + 2 },
                                () => ({ ticks: 2, az: -0.2 })));
  check(many.sty.cut.length === MELEE.MAX_CUT_LEGS &&
        many.log.some(l => l.includes('more than')),
    `more than ${MELEE.MAX_CUT_LEGS} legs is dropped LOUDLY`);
  const empty = probe([]);
  check(empty.sty.cut.length === 1 && empty.log.some(l => l.includes('empty')),
    'an empty cut list falls back to one default leg, loudly');
  // The path arithmetic, on the legs AS AUTHORED: a target-frame parse, since
  // an old-frame one moves leg 1 by the windup (strokes.h THE STROKE FRAME).
  const dogT = MELEE.parseStyleLibrary({ strokeFrame: 'target', styles: [{
    name: 'd', cut: [{ ticks: 4, az: -1.0, el: -0.40, reach: 0.05 },
                     { ticks: 4, az: -1.0, el: 0.40, reach: 0.05 }] }] }).styles[0];
  const tot2 = MELEE.cutTravel(dogT);
  check(near(tot2.az, -2.0, 1e-6) && near(tot2.el, 0, 1e-6) &&
        near(tot2.reach, 0.1, 1e-6) && tot2.ticks === 8,
    'cutTravel sums the legs');
  const thr = MELEE.cutThrough(dogT, 0);
  check(near(thr.az, -1.0, 1e-6) && near(thr.el, -0.40, 1e-6),
    'cutThrough(0) is where the point stands when the first leg ends');
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
  // THE DRIVER HAS NO CUT OF ITS OWN (2026-09-25): its phase names are
  // idle / guard / wind / recover and nothing a program does can add one.
  check(!MELEE.PHASE_NAME.includes('slash') && !('Slash' in MELEE.PHASE),
    'the driver has no speed-triggered Slash phase',
    MELEE.PHASE_NAME.join(', '));
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
    // by design: the controller's switches and the swing-basis cone (main.cpp
    // ResolveSwingBasis), read off the tuning document, never by the driver
    if (k === 'pickMinSpeed' ||
        k === 'aimYaw') return false;
    const base = k.replace(/(M|Mps)$/, '');
    return !(base in t);
  });
  check(missing.length === 0,
    'every tuning.json melee key reaches the ported driver',
    'unmapped: ' + missing.join(', '));
}

/* ==========================================================================
   3b. HOW A CUT IS PACED (strokes.h StrokeEase; 2026-09-22)

   Three claims, and the first is the one the whole design rests on: `linear`
   must be the drive that shipped, or every authored style in the game silently
   changes shape. It is stated as the ALGEBRAIC identity the engine relies on
   to keep spelling that case out longhand — strokeEaseStep('linear', k/N,
   (k+1)/N) === 1/(N-k) — because that identity is what makes the longhand safe
   rather than merely conservative.

   The other two are the shapes: every curve has to ARRIVE (or a leg leaks its
   residue into the next one), and exp/log have to be front- and back-loaded
   respectively rather than just "different".
   ------------------------------------------------------------------------ */
section('cut pacing (strokes.h StrokeEase)');
{
  // 1. LINEAR IS THE OLD DIVISOR, exactly, at every leg length and every tick
  //    within it.
  let worst = 0, worstAt = '';
  for (const N of [1, 2, 3, 5, 7, 8, 12, 30, 41]) {
    for (let k = 0; k < N; k++) {
      const got = MELEE.strokeEaseStep('linear', k / N, (k + 1) / N);
      const want = 1 / (N - k);
      const err = Math.abs(got - want);
      if (err > worst) { worst = err; worstAt = `N=${N} k=${k}`; }
    }
  }
  check(worst < 1e-12,
    'linear pacing IS the historical `gap / ticksLeft` divisor',
    `worst |share - 1/(N-k)| = ${worst.toExponential(2)} at ${worstAt}`);

  // 2. EVERY CURVE ARRIVES. Walk the gap down tick by tick the way the Cut
  //    phase does — gap *= (1 - share) — and the last tick must close it.
  for (const ease of MELEE.EASES) {
    for (const N of [2, 5, 8, 30]) {
      let gap = 1;
      for (let k = 0; k < N; k++)
        gap *= 1 - MELEE.strokeEaseStep(ease, k / N, (k + 1) / N);
      check(Math.abs(gap) < 1e-9,
        `${ease} over ${N} ticks lands on the leg's end exactly`,
        `residual gap ${gap.toExponential(2)} — a leg that does not arrive `
        + 'displaces every leg after it');
    }
  }

  // 3. IT IS THE ENGINE'S OWN VOCABULARY, not a second one. This is the claim
  //    that stops `exp`/`log` being reinvented next to quadOut/quadIn again.
  check(MELEE.EASES === AN.EASES || MELEE.EASES.join() === AN.EASES.join(),
    'cut pacing uses anim.js\'s ease list, not a private one',
    'melee: ' + MELEE.EASES.join(',') + ' | anim: ' + AN.EASES.join(','));
  check(MELEE.EASES.includes('quadIn') && MELEE.EASES.includes('cubicInOut'),
    'so every clip curve is available to a cut');

  // 4. THE SHAPES ARE THE ADVERTISED ONES. Measured as the fraction of the
  //    travel BEHIND the point at half time: quadOut is ahead of linear there
  //    and quadIn is behind it, which is what "front-loaded" and
  //    "back-loaded" mean and the whole of what an author is choosing.
  const half = e => AN.applyEase(e, 0.5);
  check(near(half('linear'), 0.5, 1e-9),
    'linear is half-travelled at half time');
  check(half('quadOut') > 0.7,
    'quadOut is FRONT-loaded (most of the travel spent early)',
    `f(0.5) = ${half('quadOut').toFixed(3)}, wanted > 0.7`);
  check(half('quadIn') < 0.3,
    'quadIn is BACK-loaded (most of the travel spent late)',
    `f(0.5) = ${half('quadIn').toFixed(3)}, wanted < 0.3`);
  check(half('quadOut') > half('linear') && half('linear') > half('quadIn'),
    'the curves are ordered quadOut > linear > quadIn at half time');

  // 5. AND THE PARSER TAKES THEM, defaulting to the shipped pacing and
  //    refusing a name it does not know LOUDLY rather than silently — which
  //    ParseEase alone would not do (anim.cpp returns Linear for a typo).
  const lib = MELEE.parseStyleLibrary({ styles: [
    { name: 'a', cut: { ticks: 6, az: -1 } },
    { name: 'b', ease: 'quadIn', cut: { ticks: 6, az: -1 } },
    { name: 'c', ease: 'sproing', cut: { ticks: 6, az: -1 } },
  ] });
  check(lib.styles[0].ease === 'linear', 'a style stating no ease is linear');
  check(lib.styles[1].ease === 'quadIn', 'an authored ease is read');
  check(lib.styles[2].ease === 'linear' &&
        lib.log.some(l => l.includes('sproing')),
    'an unknown ease falls back to linear and says so',
    'log: ' + JSON.stringify(lib.log));

  // 6. THE PACING REALLY REACHES THE DRIVE. Same style, same ticks, same
  //    travel: only the distribution over the cut may differ — and it must,
  //    or the knob is decoration. Compared at the cut's MIDPOINT tick.
  const mk = ease => MELEE.parseStyleLibrary({ styles: [{
    name: 'p', ease, windup: { ticks: 4, az: 0, el: 0, reach: 0 },
    cut: { ticks: 10, az: -1.2, el: 0, reach: 0 },
    recover: { ticks: 3 }, jitter: { az: 0, el: 0, tempo: 0 },
  }] }).styles[0];
  const cutAzAt = (ease, frac) => {
    const r = runParsed(mk(ease), 0, 0, 0);
    const cut = r.trace.filter(t => t.phase === MELEE.STROKE_PHASE.Cut);
    return cut[Math.floor(cut.length * frac)]?.az ?? NaN;
  };
  const [lin, ex, lg] =
    ['linear', 'quadOut', 'quadIn'].map(e => cutAzAt(e, 0.5));
  // The cut travels NEGATIVE in azimuth here, so "further along" is smaller.
  check(ex < lin - 1e-4,
    'quadOut is further through the travel than linear at mid-cut',
    `quadOut ${ex.toFixed(4)} vs linear ${lin.toFixed(4)}`);
  check(lg > lin + 1e-4,
    'quadIn is less far through the travel than linear at mid-cut',
    `quadIn ${lg.toFixed(4)} vs linear ${lin.toFixed(4)}`);

  // 7. `instant` NEVER REACHES 1 (ApplyEase returns 0 at t=1), so without the
  //    last-tick force it would freeze the blade instead of pacing it. This is
  //    the degenerate case reusing a foreign vocabulary brought with it.
  check(near(AN.applyEase('instant', 1), 0),
    'instant\'s curve really is 0 even at t=1 (the case being guarded)');
  check(MELEE.strokeEaseStep('instant', 0.9, 1.0) === 1,
    'so the LAST tick of any curve spends the whole remaining gap');
  {
    let gap = 1;
    for (let k = 0; k < 8; k++)
      gap *= 1 - MELEE.strokeEaseStep('instant', k / 8, (k + 1) / 8);
    check(Math.abs(gap) < 1e-9,
      'an instant cut holds, then arrives exactly — it does not stall',
      `residual ${gap.toExponential(2)}`);
  }
}

/* ==========================================================================
   3c. THE GOAL FRAMES (melee.js strokeGoals / strokeGoalPose; 2026-09-22)

   The tuner can HOLD a destination instead of watching the arm travel to it.
   The claim that makes that worth having is that a goal is the SAME point the
   program drives to — if the two ever disagree the tool is lying about the
   file, which is worse than not having it. So: run the program and check that
   each phase actually ARRIVES at the goal the panel would have shown.

   The browser wiring (the chips, the snap) is not covered here — the Attacks
   lane's own harness is broken at clean HEAD (check_attacks.sh) — but the
   arithmetic underneath it is, and that is where a wrong answer would hide.
   ------------------------------------------------------------------------ */
section('goal frames (melee.js strokeGoalPose)');
{
  const AIM_AZ = 0.2, AIM_EL = -0.1;
  const sty = MELEE.parseStyleLibrary({ styles: [{
    name: 'g',
    windup: { ticks: 10, az: 0.30, el: 0.20, reach: -0.05 },
    cut: [{ ticks: 8, az: -1.2, el: -0.2, reach: 0.10 },
          { ticks: 6, az: -0.5, el: -0.3, reach: -0.05 }],
    recover: { ticks: 10, az: 0.25, el: 0.10, reach: -0.10, settle: 6 },
    jitter: { az: 0, el: 0, tempo: 0 },
  }] }).styles[0];

  // 1. THE LIST is windup, one entry per leg, then the recover — and the
  //    recover only when it has a pose to hold.
  const goals = MELEE.strokeGoals(sty);
  check(goals.map(g => g.key).join(',') === 'windup,cut0,cut1,recover',
    'the goals are windup, each cut leg, then a posed recover',
    'got ' + goals.map(g => g.key).join(','));
  const unposed = MELEE.parseStyleLibrary({ styles: [{
    name: 'u', cut: { ticks: 5, az: -1 }, recover: { ticks: 5 },
  }] }).styles[0];
  check(MELEE.strokeGoals(unposed).every(g => g.key !== 'recover'),
    'an UNPOSED recover is not a goal (it has no pose of its own)');
  check(MELEE.strokeGoalPose(unposed, freshDriver(), 'recover', 0, 0) === null,
    'and resolving it returns null rather than a fabricated pose');

  // 2. EVERY GOAL RESOLVES, and a key this style has not got does not.
  const m0 = freshDriver();
  for (const g of goals)
    check(MELEE.strokeGoalPose(sty, m0, g.key, AIM_AZ, AIM_EL) !== null,
      `goal "${g.key}" resolves to a pose`);
  check(MELEE.strokeGoalPose(sty, m0, 'cut9', AIM_AZ, AIM_EL) === null,
    'a leg index this style has not got resolves to null');
  check(MELEE.strokeGoalPose(sty, m0, 'nonsense', AIM_AZ, AIM_EL) === null,
    'an unknown goal key resolves to null');

  // 3. THE CENTRAL CLAIM: the program ARRIVES at the goals.
  //
  //    THE WINDUP IS A CLAMPED CHASE (steerTo caps the drive at 16
  //    units/tick), so whether it arrives is a question about its TICK
  //    BUDGET, not about the goal arithmetic. Both halves are worth pinning,
  //    and the second is the one the goal frame exists to make visible:
  //
  //      given enough ticks it lands ON the goal  -> the arithmetic is right
  //      given too few it stops SHORT, never past -> a real, authored defect
  //                                                  ("the cut inherits the
  //                                                  leftover", attacks.js)
  const P = MELEE.STROKE_PHASE;
  const lastIn = (run, ph) => {
    const rows = run.trace.filter(t => t.phase === ph);
    return rows[rows.length - 1] || null;
  };
  const withWindup = ticks => MELEE.parseStyleLibrary({ styles: [{
    name: 'w', windup: { ticks, az: 0.30, el: 0.20, reach: -0.05 },
    cut: [{ ticks: 8, az: -1.2, el: -0.2, reach: 0.10 },
          { ticks: 6, az: -0.5, el: -0.3, reach: -0.05 }],
    recover: { ticks: 10, az: 0.25, el: 0.10, reach: -0.10, settle: 6 },
    jitter: { az: 0, el: 0, tempo: 0 },
  }] }).styles[0];

  const roomy = withWindup(90);
  const rr = runParsed(roomy, 0, AIM_AZ, AIM_EL);
  const grw = MELEE.strokeGoalPose(roomy, rr.m, 'windup', AIM_AZ, AIM_EL);
  const rw = lastIn(rr, P.Windup);
  check(rw && Math.abs(rw.az - grw.az) < 0.02 &&
        Math.abs(rw.el - grw.el) < 0.02,
    'a windup with ticks to spare lands ON its goal pose',
    rw ? `az ${rw.az.toFixed(4)} vs goal ${grw.az.toFixed(4)}, `
         + `el ${rw.el.toFixed(4)} vs ${grw.el.toFixed(4)}` : 'no windup ticks');

  const tight = withWindup(10);
  const rt = runParsed(tight, 0, AIM_AZ, AIM_EL);
  const gtw = MELEE.strokeGoalPose(tight, rt.m, 'windup', AIM_AZ, AIM_EL);
  const tw = lastIn(rt, P.Windup);
  // Short of the goal on the side it started from, and NOT past it. "Past"
  // would mean the clamp had banked travel, which melee.h forbids.
  check(tw && tw.az < gtw.az - 0.1,
    'a windup with too few ticks stops SHORT of its goal, never past it',
    tw ? `az ${tw.az.toFixed(3)} vs goal ${gtw.az.toFixed(3)} `
         + '(short is correct here — 10 ticks cannot cross that gap under the '
         + 'clamp; the goal frame is how an author sees it)'
       : 'no windup ticks');

  const r = rr;

  // The CUT must arrive exactly — it is an open-loop distribution of a known
  // travel, and "exactly" is the property StrokeEaseStep is built to keep.
  const cutRows = r.trace.filter(t => t.phase === P.Cut);
  const gLast = MELEE.strokeGoalPose(sty, r.m, 'cut1', AIM_AZ, AIM_EL);
  const cEnd = cutRows[cutRows.length - 1];
  check(cEnd && Math.abs(cEnd.az - gLast.az) < 2e-3 &&
        Math.abs(cEnd.el - gLast.el) < 2e-3,
    'the cut ends exactly on the last leg\'s goal',
    cEnd ? `az ${cEnd.az.toFixed(4)} vs goal ${gLast.az.toFixed(4)}, `
           + `el ${cEnd.el.toFixed(4)} vs ${gLast.el.toFixed(4)}` : 'no cut ticks');

  // ...and it must do so under EVERY pacing THAT STAYS UNDER THE COMMIT
  // EVERY CURVE ENDS EXACTLY ON ITS GOAL, however back-loaded. Until
  // 2026-09-25 an aggressive curve could spend enough in one tick to cross
  // `melee.commitSpeed`, which put the DRIVER into its own Slash and handed
  // the endpoint to a two-radian follow-through arc (the "short cut tweaks
  // out" report). The driver has no Slash now, so this is unconditional.
  for (const ease of MELEE.EASES) {
    const s2 = MELEE.parseStyleLibrary({ styles: [{
      name: 'g2', ease,
      windup: { ticks: 10, az: 0.30, el: 0.20, reach: -0.05 },
      cut: [{ ticks: 8, az: -1.2, el: -0.2, reach: 0.10 },
            { ticks: 6, az: -0.5, el: -0.3, reach: -0.05 }],
      recover: { ticks: 10, az: 0.25, el: 0.10, reach: -0.10, settle: 6 },
      jitter: { az: 0, el: 0, tempo: 0 },
    }] }).styles[0];
    const r2 = runParsed(s2, 0, AIM_AZ, AIM_EL);
    const rows2 = r2.trace.filter(t => t.phase === P.Cut);
    const g2 = MELEE.strokeGoalPose(s2, r2.m, 'cut1', AIM_AZ, AIM_EL);
    const e2 = rows2[rows2.length - 1];
    const where = `az ${e2 ? e2.az.toFixed(4) : '?'} vs goal ${g2.az.toFixed(4)}`;
    check(e2 && Math.abs(e2.az - g2.az) < 2e-3 &&
          Math.abs(e2.el - g2.el) < 2e-3,
      `a ${ease} cut ends exactly on its goal`, where);
  }

  // A SHORT FAST CUT STAYS ON ITS PATH. The reported case: a dagger-length
  // cut (6 ticks, 1.2 rad) used to commit the driver's Slash and overshoot
  // its authored end by ~1.7 rad before snapping back. Every cut tick must
  // now sit between the leg's start and end — a linear drive is monotone.
  {
    const s3 = MELEE.parseStyleLibrary({ styles: [{
      name: 'short', windup: { ticks: 10, az: 0.6, el: -0.3, reach: 0 },
      cut: { ticks: 6, az: -1.2, el: -0.2, reach: 0 },
      recover: { ticks: 12, az: 0.2, el: -0.4, reach: 0, posed: true, settle: 8 },
      jitter: { az: 0, el: 0, tempo: 0 },
    }] }).styles[0];
    const r3 = runParsed(s3, 0, AIM_AZ, AIM_EL);
    const rows3 = r3.trace.filter(t => t.phase === P.Cut);
    const endAz = AIM_AZ + 0.6 - 1.2;
    const lo = Math.min(endAz, AIM_AZ + 0.6) - 1e-3;
    const hi = Math.max(endAz, AIM_AZ + 0.6) + 1e-3;
    const worst = rows3.reduce((w, t) => Math.max(w, t.az < lo ? lo - t.az
                                                   : t.az > hi ? t.az - hi : 0), 0);
    check(worst === 0, 'a 6-tick cut never leaves its authored span',
      `worst overshoot ${worst.toFixed(3)} rad`);
  }

  // A HELD GOAL IS A PURE FUNCTION OF THE STYLE. The report: "look at
  // recover, click off, go back to recover, and it is doing something totally
  // different". The tip was always right; the hand, blade and elbow were
  // whatever the previous motion left. Two drivers with DIFFERENT histories,
  // snapped to the same goal, must build the same arm.
  {
    const probe = (history) => {
      const m = freshDriver();
      history(m);
      const g = MELEE.strokeGoalPose(sty, m, 'recover', AIM_AZ, AIM_EL)
             || MELEE.strokeGoalPose(sty, m, 'cut0', AIM_AZ, AIM_EL);
      m.snapToPose(g.az, g.el, g.reach, RIGHT, UP, FWD, g.travel);
      return m.pose();
    };
    const a = probe(() => {});
    const b = probe((m) => {                       // a swing's worth of history
      const c = MELEE.newStrokeCursor();
      MELEE.beginStrokeProgram(c, sty, 0, 7);
      for (let i = 0; i < 40; i++)
        MELEE.stepStrokeProgram(c, sty, m, 0.4, 0.3, DT, RIGHT, UP, FWD);
    });
    const d = (x, y) => Math.hypot(x.x - y.x, x.y - y.y, x.z - y.z);
    const gap = Math.max(d(a.hand, b.hand), d(a.bladeDir, b.bladeDir),
                         d(a.bladeFlat, b.bladeFlat), d(a.bendPole, b.bendPole));
    check(gap < 1e-9, 'a held goal builds the same arm whatever came before',
      `max difference ${gap.toExponential(2)}`);
  }

  // A SLOW CUT KEEPS THE BLADE ON ITS PATH. The report: "the last frame of the
  // cut is NOT where the cut is, even if I give the cut 90 ticks". The wrist's
  // alignment used to be EARNED BY TIP SPEED (the freeform mode's steer band),
  // so a 90-tick cut ran at the 0.15 floor and the blade rode the grip while
  // the hand sat where a fully aligned blade would have needed it. A program
  // step aligns in full at any speed (melee.h SetProgramDrive).
  for (const ticks of [6, 90]) {
    const sl = MELEE.parseStyleLibrary({ styles: [{
      name: 'slow', windup: { ticks: 12, az: 0.9, el: -0.4, reach: 0 },
      cut: { ticks, az: -1.6, el: -0.3, reach: 0 },
      recover: { ticks: 12, az: 0.2, el: -0.8, reach: 0, posed: true, settle: 8 },
      jitter: { az: 0, el: 0, tempo: 0 },
    }] }).styles[0];
    const m = freshDriver();
    const c = MELEE.newStrokeCursor();
    MELEE.beginStrokeProgram(c, sl, 0, 1);
    let worst = 1;
    // Over the CUT: the windup's first ticks ease the wrist in from the guard
    // on its own envelope (that is the take-over, not a speed rule).
    for (let i = 0; i < 300 && c.phase !== MELEE.STROKE_PHASE.Recover; i++) {
      const ph = c.phase;
      MELEE.stepStrokeProgram(c, sl, m, 0, 0, DT, RIGHT, UP, FWD);
      if (ph === MELEE.STROKE_PHASE.Cut)
        worst = Math.min(worst, m.pose().steerAmount);
    }
    check(worst > 0.99, `a ${ticks}-tick cut keeps the wrist fully aligned`,
      `lowest alignment ${worst.toFixed(3)}`);
  }
  // ...and the flag is one-shot: a driver stepped by hand afterwards earns
  // alignment from speed again (a standing guard rides the grip).
  {
    const m = freshDriver();
    m.update(DT, true, true, RIGHT, UP, FWD);
    for (let i = 0; i < 30; i++) m.update(DT, true, true, RIGHT, UP, FWD);
    check(m.pose().steerAmount < 0.5,
      'outside a program the wrist is speed-earned (the flag does not stick)',
      `alignment ${m.pose().steerAmount.toFixed(3)}`);
  }

  // 4. THE RECOVER GOAL IS ABSOLUTE, not aim-relative — the one place the
  //    three differ, and the easiest thing to get backwards.
  const gr1 = MELEE.strokeGoalPose(sty, m0, 'recover', 0, 0);
  const gr2 = MELEE.strokeGoalPose(sty, m0, 'recover', 1.1, -0.7);
  check(near(gr1.az, gr2.az) && near(gr1.el, gr2.el),
    'the recover goal does NOT move with the aim (it is a stance)');
  const gc1 = MELEE.strokeGoalPose(sty, m0, 'cut0', 0, 0);
  const gc2 = MELEE.strokeGoalPose(sty, m0, 'cut0', 1.1, -0.7);
  check(Math.abs(gc1.az - gc2.az) > 1.0,
    'a cut goal DOES move with the aim (it is aimed)');

  // 5. AND THE START BOW IS EXCLUDED: a goal is what was authored, not this
  //    swing's draw, or the held frame would jump on every reroll.
  const jit = MELEE.parseStyleLibrary({ styles: [{
    name: 'j', windup: { ticks: 8, az: 0.3, el: 0.1, reach: 0 },
    cut: { ticks: 6, az: -1, el: 0, reach: 0 }, recover: { ticks: 4 },
    jitter: { az: 0.4, el: 0.3, tempo: 0 },
  }] }).styles[0];
  const a = MELEE.strokeGoalPose(jit, m0, 'windup', 0, 0);
  const b = MELEE.strokeGoalPose(jit, m0, 'windup', 0, 0);
  check(near(a.az, b.az) && near(a.el, b.el),
    'a goal pose is the authored number, with no jitter draw in it');
}

/* ==========================================================================
   4. anim.js's new stages
   ========================================================================== */
section('anim.js stage 6 — AnimClampPoseLimits');

// THE BALL JOINT'S REACH CLAMP IS CONTINUOUS (2026-09-25). The human's
// shoulder stops (50 deg back, 30 deg across). Sweeping the upper arm smoothly
// through horizontal while it presses the across-the-body stop used to flip
// the clamped bone from above horizontal to below in ONE step — the "hand
// teleports above the head for a frame" report. With one stop pressed the
// clamped direction must move no more than the input does; and whatever is
// pressed, the result is legal and never farther than it has to be.
{
  const V = (x, y, z) => ({ x, y, z });
  const nn = (a) => { const l = Math.hypot(a.x, a.y, a.z); return V(a.x / l, a.y / l, a.z / l); };
  const dt3 = (a, b) => a.x * b.x + a.y * b.y + a.z * b.z;
  const n = [V(0, 0, -1), V(1, 0, 0)];
  const s = [Math.sin(50 * Math.PI / 180), Math.sin(30 * Math.PI / 180)];
  let prevIn = null, prevOut = null, worstIn = 0, worstOut = 0;
  for (let i = 0; i <= 400; i++) {
    const a = -0.6 + 1.2 * i / 400;
    const d = nn(V(0.8, Math.sin(a) * 0.5, -0.3));
    const o = AN.clampDirHalfSpaces(d, n, s, 1);
    if (prevOut) {
      worstIn = Math.max(worstIn, Math.hypot(d.x - prevIn.x, d.y - prevIn.y, d.z - prevIn.z));
      worstOut = Math.max(worstOut, Math.hypot(o.x - prevOut.x, o.y - prevOut.y, o.z - prevOut.z));
    }
    prevIn = d; prevOut = o;
  }
  check(worstOut <= worstIn * 1.01 + 1e-6,
    'a one-stop reach clamp moves no more than its input (no flip at horizontal)',
    `input step ${worstIn.toFixed(4)}, output step ${worstOut.toFixed(4)}`);
  let seed = 7, illegal = 0, farther = 0;
  const rnd = () => { seed = (seed * 1103515245 + 12345) % 2147483648; return seed / 2147483648 * 2 - 1; };
  for (let i = 0; i < 20000; i++) {
    const d = nn(V(rnd(), rnd(), rnd()));
    for (const cnt of [1, 2]) {
      const o = AN.clampDirHalfSpaces(d, n, s, cnt);
      for (let k = 0; k < cnt; k++) if (dt3(o, n[k]) > s[k] + 1e-4) illegal++;
      // Nearer than either one-plane answer that is legal, or than the corner.
      const legal = (v) => [0, 1].slice(0, cnt).every(k => dt3(v, n[k]) <= s[k] + 1e-4);
      if (legal(d) && dt3(o, d) < 1 - 1e-6) farther++;
    }
  }
  check(illegal === 0, 'the reach clamp always returns a legal direction', `${illegal} illegal`);
  check(farther === 0, 'a legal direction passes through the reach clamp untouched', `${farther} moved`);
}

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

/* --------------------------------------------------------------------------
   stage 3.5b — animApplyAimPart (mob.cpp Mob::ApplyAimPart)

   THE BITE'S WHOLE MECHANISM, and the one stage in the preview with no IK
   behind it: a part in no chain is pointed at something by ROTATING IT ABOUT
   ITS OWN JOINT, with an authored share of the yaw taken by the spine. Three
   things can be quietly wrong and none of them is visible in a screenshot —
   the share can be added twice (the part inherits the spine's rotation through
   the flatten), the pitch sign can be inverted (a positive rotation about
   model +X pitches the nose DOWN), and the root spine part can be included
   (which yaws the entire creature, so the head reads as not turning at all).
   ------------------------------------------------------------------------ */
section('anim.js stage 3.5b — animApplyAimPart');
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
  const head = sk.findPart('head'), torso = sk.findPart('torso');
  const mk = () => {
    const st = { clips: [], local: [], model: [],
                 partAlive: sk.parts.map(() => 1), springs: [] };
    AN.animSampleAndBlend(sk, st, 0);
    return st;
  };
  // Zero in, nothing touched — the engine's own early-out, and what keeps an
  // idle rig and every legacy gate pose untouched.
  {
    const st = mk();
    const before = JSON.stringify(st.local);
    AN.animApplyAimPart(sk, st, head, 0, 0, 1, 0.35, sk.rootLimb);
    check(before === JSON.stringify(st.local), 'a zero aim is an exact no-op');
  }
  // The share splits: one non-root spine part takes `spineShare` of the yaw and
  // the PART takes the remainder, never the whole (it inherits the spine's
  // through the flatten, so adding the full yaw at both joints doubles it).
  {
    const st = mk();
    AN.animApplyAimPart(sk, st, head, 0.8, 0, 1, 0.25, sk.rootLimb);
    const spineYaw = AN.animHingeAngleAbout(st.local[torso].rot, { x: 0, y: 1, z: 0 });
    const headYaw = AN.animHingeAngleAbout(st.local[head].rot, { x: 0, y: 1, z: 0 });
    check(spineYaw !== null && near(spineYaw, 0.2, 1e-4),
      'the spine takes exactly its authored share of the yaw',
      `${spineYaw === null ? 'null' : spineYaw.toFixed(4)} of 0.8 x 0.25`);
    check(headYaw !== null && near(headYaw, 0.6, 1e-4),
      'the part takes the REMAINDER, not the whole yaw',
      `${headYaw === null ? 'null' : headYaw.toFixed(4)}`);
    check(JSON.stringify(st.local[sk.rootLimb].rot) ===
          JSON.stringify(mk().local[sk.rootLimb].rot),
      'the ROOT spine part is never yawed (it would turn the whole creature)');
  }
  // spineShare 0 puts all of it on the part — the A/B for "the chest does not
  // follow" without touching any other number.
  {
    const st = mk();
    AN.animApplyAimPart(sk, st, head, 0.8, 0, 1, 0, sk.rootLimb);
    const headYaw = AN.animHingeAngleAbout(st.local[head].rot, { x: 0, y: 1, z: 0 });
    check(headYaw !== null && near(headYaw, 0.8, 1e-4),
      'spineShare 0 leaves the whole yaw on the part');
  }
  // PITCH IS POSITIVE UP and is applied negated, because a positive rotation
  // about model +X takes +Z to -Y (scripts/geometry.py). Measured on where the
  // part's FORWARD ends up, not on a quaternion component, because the sign of
  // a component is not a pitch.
  {
    const st = mk();
    AN.animApplyAimPart(sk, st, head, 0, 0.5, 1, 0, sk.rootLimb);
    const fwd = AN.qrot(st.local[head].rot, { x: 0, y: 0, z: 1 });
    check(fwd.y > 0.4, 'a positive pitch points the part UP',
      `forward.y ${fwd.y.toFixed(3)}`);
  }
  // Weight scales the whole rotation, which is what lets a stroke fade its aim
  // in and out the way the arm claim already does.
  {
    const st = mk();
    AN.animApplyAimPart(sk, st, head, 0.8, 0, 0.5, 0, sk.rootLimb);
    const headYaw = AN.animHingeAngleAbout(st.local[head].rot, { x: 0, y: 1, z: 0 });
    check(headYaw !== null && near(headYaw, 0.4, 1e-4),
      'weight 0.5 halves the commanded yaw');
    const st2 = mk();
    const before = JSON.stringify(st2.local);
    AN.animApplyAimPart(sk, st2, head, 0.8, 0, 0, 0.35, sk.rootLimb);
    check(before === JSON.stringify(st2.local), 'weight 0 is an exact no-op');
  }
  // A DEAD PART IS NOT AIMED. `StyleUsable` refuses the style before it starts,
  // but the pose stage must not pose a limb that is gone either.
  {
    const st = mk();
    st.partAlive[head] = 0;
    const before = JSON.stringify(st.local);
    AN.animApplyAimPart(sk, st, head, 0.8, 0.3, 1, 0.35, sk.rootLimb);
    check(before === JSON.stringify(st.local), 'a severed part is not aimed');
  }
}

/* --------------------------------------------------------------------------
   AnimStateRule::lungeScale (mob.cpp:1532)

   A loco state scales a leap the way it scales a walk, and it DEFAULTS TO
   speedScale — the two answer the same question about the same body, so a
   crawler that walks at 0.3 of its own speed pounces at 0.3 of its own leap.
   The Attacks lane reads it to time the arc it draws, so a default that
   silently came out 1 would draw every crawler's lunge as an upright one.
   ------------------------------------------------------------------------ */
section('anim.js — AnimStateRule::lungeScale');
{
  const sk = AN.buildSkeleton({
    root: 'hips',
    limbs: [{ name: 'hips' }, { name: 'legU.L', parent: 'hips', anchor: [0, 0, 0] },
            { name: 'legU.R', parent: 'hips', anchor: [1, 0, 0] }],
    states: [
      { name: 'crawl', missing: ['legU.L', 'legU.R'], speedScale: 0.3 },
      { name: 'limp', missingAny: ['legU.L'], speedScale: 0.6, lungeScale: 0.1 },
    ],
  }, ['hips', 'legU.L', 'legU.R'].map(n => ({
    name: n, offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 },
  })));
  const crawl = sk.states.find(s => s.name === 'crawl');
  const limp = sk.states.find(s => s.name === 'limp');
  check(!!crawl && crawl.lungeScale === 0.3,
    'lungeScale defaults to speedScale when the state does not state one',
    String(crawl && crawl.lungeScale));
  check(!!limp && limp.lungeScale === 0.1,
    'an authored lungeScale overrides it', String(limp && limp.lungeScale));
  // ...and the shipped human, which is what the lane actually previews.
  const human = JSON.parse(readFileSync(join(ROOT, 'assets/mobs/human.json'), 'utf8'));
  const hsk = AN.buildSkeleton(human, human.limbs.map(l => ({
    name: l.name, offset: { x: 0, y: 0, z: 0 }, dim: { x: 2, y: 2, z: 2 },
  })));
  check(hsk.states.every(s => Number.isFinite(s.lungeScale) && s.lungeScale >= 0),
    'every state on the shipped human rig resolves a usable lungeScale',
    hsk.states.map(s => `${s.name}=${s.lungeScale}`).join(' '));
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

/* ==========================================================================
   PER-SEGMENT PACING + JOINT BRAKES (strokes.h "PER-SEGMENT PACING",
   melee.h ArmSmooth). The port's half; the engine loader reads the same keys.
   ========================================================================== */
section('per-segment pacing + joint brakes');
{
  const base = {
    name: 'p', windup: { ticks: 10, az: 0.6, el: 0.3, reach: 0 },
    cut: [{ ticks: 6, az: -1.2, el: 0, reach: 0 },
          { ticks: 6, az: -0.6, el: -0.3, reach: 0 }],
    recover: { ticks: 10 },
  };
  const parse = (extra) => {
    const j = JSON.parse(JSON.stringify(base));
    extra(j);
    const L = MELEE.parseStyleLibrary({ styles: [j] });
    return { sty: L.styles[0], log: L.log };
  };
  // Absent = the old program, tick for tick.
  const plain = runParsed(parse(() => {}).sty);
  const lin = parse(j => { j.ease = 'linear'; });
  const same = runParsed(lin.sty);
  check(plain.trace.length === same.trace.length &&
        plain.trace.every((t, i) => t.az === same.trace[i].az),
    'no per-segment ease = the historical program, bit for bit');

  // A PACED WINDUP lands on its pose by its last tick; front-loaded gets
  // further in the first half than back-loaded.
  const wOut = runParsed(parse(j => { j.windup.ease = 'quadOut'; }).sty);
  const wIn = runParsed(parse(j => { j.windup.ease = 'quadIn'; }).sty);
  const W = MELEE.STROKE_PHASE.Windup;
  const mid = (r) => r.trace.filter(t => t.phase === W)[3].az;
  const startAz = plain.trace[0].az;
  check(Math.abs(mid(wOut) - startAz) > Math.abs(mid(wIn) - startAz),
    'windup quadOut travels further by tick 4 than quadIn',
    `${mid(wOut).toFixed(3)} vs ${mid(wIn).toFixed(3)}`);
  const endW = (r) => r.trace.filter(t => t.phase === W).slice(-1)[0].az;
  check(Math.abs(endW(wOut) - endW(wIn)) < 0.05,
    'both paced windups arrive at the same pose',
    `${endW(wOut).toFixed(3)} vs ${endW(wIn).toFixed(3)}`);

  // A LEG'S OWN EASE beats the style's.
  const C = MELEE.STROKE_PHASE.Cut;
  const firstCut = (r) => r.trace.filter(t => t.phase === C)[0].az;
  const styleIn = runParsed(parse(j => { j.ease = 'cubicIn'; }).sty);
  const legOut = runParsed(parse(j => { j.ease = 'cubicIn'; j.cut[0].ease = 'cubicOut'; }).sty);
  const cutStart = (r) => r.trace.filter(t => t.phase === W).slice(-1)[0].az;
  check(Math.abs(firstCut(legOut) - cutStart(legOut)) >
        Math.abs(firstCut(styleIn) - cutStart(styleIn)),
    'cut leg 1 ease cubicOut overrides the style-wide cubicIn');
  check(MELEE.legEase(parse(j => { j.ease = 'quadIn'; }).sty, 1) === 'quadIn' &&
        MELEE.legEase(parse(j => { j.cut[1].ease = 'instant'; }).sty, 1) === 'instant',
    'legEase: a leg without its own ease inherits the style\'s');

  // Recover ease rides a posed recover's settle.
  const rec = parse(j => { j.recover = { ticks: 10, az: 0.3, el: 0.1, settle: 6,
                                         ease: 'quadInOut' }; });
  check(rec.sty.recover.ease === 'quadInOut', 'recover.ease is read');
  check(parse(j => { j.windup.ease = 'quadOOut'; }).log.some(l => /unknown ease/.test(l)),
    'an unknown segment ease is reported, not silently linear');

  // JOINTS: read, clamped, inherited by the player copy field by field.
  const jt = parse(j => {
    j.joints = { shoulder: { smooth: 2, maxDeg: 20 }, wrist: { maxDeg: -5 } };
    j.player = { joints: { shoulder: { maxDeg: 40 } } };
  });
  const L = MELEE.parseStyleLibrary({ styles: [JSON.parse(JSON.stringify({
    ...base, joints: { shoulder: { smooth: 2, maxDeg: 20 } },
    player: { joints: { shoulder: { maxDeg: 40 } } } }))] });
  check(jt.sty.joints.shoulder.smooth === 2 && jt.sty.joints.shoulder.maxDeg === 20 &&
        jt.sty.joints.wrist.maxDeg === 0 && MELEE.jointsAny(jt.sty.joints),
    'joints: read, negatives clamp to off');
  const pl = L.styles.find(x => x.name === 'p:player');
  check(pl && pl.joints.shoulder.maxDeg === 40 && pl.joints.shoulder.smooth === 2,
    'player joints override per field and inherit the rest');
  check(!MELEE.jointsAny(parse(() => {}).sty.joints), 'no joints block = no brakes');
}

/* ==========================================================================
   THE STROKE FRAME (strokes.h, 2026-09-25): windup measured from the target.
   ========================================================================== */
section('the stroke frame: windup measured from the target');
{
  const shipped = JSON.parse(JSON.stringify(rawStyles));
  check(shipped.strokeFrame === 'target',
    'assets/mobs/attack_styles.json is in the target frame');
  // 0/0 IS THE TARGET.
  const zero = MELEE.parseStyleLibrary({ strokeFrame: 'target', styles: [{
    name: 'z', windup: { ticks: 30, az: 0, el: 0, reach: 0 },
    cut: { ticks: 6, az: -1.2, el: 0, reach: 0 }, recover: { ticks: 4 } }] });
  const zs = zero.styles[0];
  const g = MELEE.strokeGoalPose(zs, freshDriver(), 'windup', 0.3, -0.1);
  check(g && near(g.az, 0.3) && near(g.el, -0.1),
    'windup 0/0 resolves to the aim itself', g && `${g.az} ${g.el}`);
  const zr = runParsed(zs, 0, 0.3, -0.1);
  const lastW = zr.trace.filter(t => t.phase === MELEE.STROKE_PHASE.Windup).slice(-1)[0];
  check(Math.abs(lastW.az - 0.3) < 0.05 && Math.abs(lastW.el + 0.1) < 0.05,
    'a long 0/0 windup arrives pointing at the target',
    `${lastW.az.toFixed(3)} ${lastW.el.toFixed(3)}`);
  const g0 = MELEE.strokeGoalPose(zs, freshDriver(), 'cut0', 0.3, -0.1);
  check(near(g0.az, 0.3 - 1.2), 'the cut travels FROM the windup pose');

  // A LEGACY FILE plays exactly as the same file migrated.
  const legacy = { styles: [{
    name: 'L', windup: { ticks: 10, az: 0.3, el: 0.1, reach: -0.1 },
    cut: [{ ticks: 4, az: -1.0, el: -0.4, reach: 0.05 },
          { ticks: 4, az: -1.0, el: 0.4, reach: 0.05, aim: true }],
    recover: { ticks: 6 },
    player: { windup: { ticks: 6, az: 0.1 }, cut: [{ ticks: 3, az: -0.8 }, { ticks: 3, az: -0.8 }] },
  }] };
  const mig = JSON.parse(JSON.stringify(legacy));
  check(MELEE.migrateRawToTargetFrame(mig) && mig.strokeFrame === 'target' &&
        !MELEE.migrateRawToTargetFrame(mig),
    'migration stamps the frame and runs once');
  const A = MELEE.parseStyleLibrary(legacy), B = MELEE.parseStyleLibrary(mig);
  for (const nm of ['L', 'L:player']) {
    const ia = A.styles.findIndex(x => x.name === nm), ib = B.styles.findIndex(x => x.name === nm);
    const ta = runParsed(A.styles[ia], ia, 0.25, 0.05).trace;
    const tb = runParsed(B.styles[ib], ib, 0.25, 0.05).trace;
    let worst = ta.length === tb.length ? 0 : Infinity;
    for (let i = 0; i < Math.min(ta.length, tb.length); i++)
      worst = Math.max(worst, Math.abs(ta[i].az - tb[i].az), Math.abs(ta[i].el - tb[i].el));
    check(worst < 1e-5, `${nm}: a legacy file and its migration are the same swing`,
      `worst ${worst}`);
  }
}

/* ========================================================================== */
console.log(`\n${checks - failures} / ${checks} checks passed`);
if (failures) {
  console.error(`${failures} FAILED`);
  process.exit(1);
}
