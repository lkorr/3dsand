#!/usr/bin/env node
// gen_attack_clips.mjs — THE BODY THAT GOES WITH A PUNCH, A BITE AND A LEAP.
//
//   node scripts/gen_attack_clips.mjs
//
// Writes assets/anims/{punch_r,punch_l,bite,lunge}.json: the shared clip
// library entries an attack style names through `AttackStyle::clip`
// (src/game/strokes.h). `LoadMobDefs` compiles every file there onto every rig
// whose part names it fits, so one file serves the human, the zombie and
// anything else with a torso and two arms, and the `mob` gate asserts each one
// compiled onto the human under its stem.
//
// ---------------------------------------------------------------------------
// WHY A CLIP AT ALL, WHEN THE DRIVER ALREADY MOVES THE ARM
//
// The stroke driver owns exactly one thing: the part the style's `weapon`
// names, and the chain that serves it (Mob::ApplyWeaponArm). Everything else —
// the off hand coming up to guard, the shoulder dropping into the blow, the
// hips turning it over, the torso hunching behind a bite — is body language the
// driver has no opinion about, and without it a punch is an arm moving in front
// of a statue. That is what these are.
//
// ---------------------------------------------------------------------------
// THE ONE RULE THAT MATTERS: MASK OFF WHAT THE DRIVER OWNS
//
// A clip's `mask` is an ALLOWLIST (anim.cpp: "a non-empty mask is an
// allowlist"). A track for the striking arm would be sampled, blended and then
// OVERWRITTEN by the IK solve on the same tick — the arm would look right on
// the frames the driver happened not to reach, which is a worse bug than no
// clip at all because it only shows up in some poses. So:
//
//   punch_r  masks armU.R / armL.R / hand.R OUT   (the driver punches with them)
//   punch_l  masks armU.L / armL.L / hand.L OUT
//   bite     masks `head` OUT                     (the Aim effector aims it)
//   lunge    is ADDITIVE and touches no arm at all
//
// ---------------------------------------------------------------------------
// AND WHY `lunge` IS ADDITIVE
//
// It has to compose over whatever the body is already doing, and a lunging
// creature may be walking, limping or CRAWLING — the crawl clip owns the whole
// torso in `override` mode (gen_human.py's `crawl`), so an override lunge would
// fight it and win, and a zombie dragging itself would stand up to pounce. An
// additive clip is a DELTA against the rest pose composed onto whatever came
// before it (anim.h ClipMode::Additive), so the crouch-then-extend reads the
// same on two legs and on none.
//
// Angles are DEGREES here and quaternions in the file, in the sidecar's own
// X->Y->Z convention (QuatFromEulerDeg). Positive X pitches a part's nose DOWN
// (scripts/geometry.py: +90 about X takes +Z to -Y), so a forward hunch is
// POSITIVE and a chest opening up is negative. Y is the body's own yaw: positive
// turns a part toward the creature's right.
//
// There are no position keys anywhere in here on purpose. A clip's `pos` track
// is a world-voxel DELTA on `rest.pos`, so it is a length that has to scale
// with kVoxelMeters; every one of these motions is a ROTATION about a joint,
// which is scale-free and reads the same on a critter and on a giant.
import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const outDir = join(root, 'assets', 'anims');

const rad = (d) => (d * Math.PI) / 180;
const r4 = (v) => Math.round(v * 1e4) / 1e4;

// X then Y then Z, matching QuatFromEulerDeg (anim.h) and gen_human.py's qxz.
function q(dx = 0, dy = 0, dz = 0) {
  const hx = rad(dx) * 0.5, hy = rad(dy) * 0.5, hz = rad(dz) * 0.5;
  const sx = Math.sin(hx), cx = Math.cos(hx);
  const sy = Math.sin(hy), cy = Math.cos(hy);
  const sz = Math.sin(hz), cz = Math.cos(hz);
  // q = qz * qy * qx (Z outermost), the order QuatFromEulerDeg composes in.
  const x = sx * cy * cz + cx * sy * sz;
  const y = cx * sy * cz - sx * cy * sz;
  const z = cx * cy * sz + sx * sy * cz;
  const w = cx * cy * cz - sx * sy * sz;
  return [r4(x), r4(y), r4(z), r4(w)];
}

// One rotation track: a list of [timeMs, quat] pairs, eased except the last.
const track = (keys) => ({
  rot: keys.map(([t, quat], i) =>
    i === keys.length - 1
      ? { t, q: quat }
      : { t, q: quat, ease: 'quadInOut' }),
});

// ---------------------------------------------------------------------------
// A PUNCH. 500 ms: about a 15-tick stroke at 30 Hz, which is the longest of
// the authored punches (hook_r: 10 windup + 5 cut) plus its recover starting.
// The clip runs ONCE and blends out; the style's own tick counts are the
// authority on timing, and a clip that outlasted them would keep the shoulder
// dropped into the next swing.
//
// THREE THINGS HAPPEN AND THEY ARE ALL THE SAME ROTATION SEEN FROM DIFFERENT
// HEIGHTS: the hips turn into the blow, the chest follows a little less (it is
// counter-rotating against the hips for balance), and the OFF shoulder comes
// back and up as the punching one goes forward. A boxer's whole body is a
// spring that unwinds through the arm, and the arm is the part this clip does
// not touch.
function punch(side) {
  const s = side === 'R' ? 1 : -1;      // +1 = the creature's own right
  const off = side === 'R' ? 'L' : 'R';
  return {
    name: `punch_${side.toLowerCase()}`,
    sidecarVoxelsPerMetre: 10,
    durationMs: 500,
    loop: false,
    mode: 'override',
    blendInMs: 70,
    blendOutMs: 140,
    // THE STRIKING ARM IS ABSENT FROM THIS LIST ON PURPOSE (see the header).
    mask: ['hips', 'torso', 'head', `armU.${off}`, `armL.${off}`, `hand.${off}`],
    tracks: {
      // Hips turn INTO the punch: +Y is toward the creature's right, so a
      // right hand travels with a left turn of the hips winding up and a right
      // one driving through. The numbers are small because a hip turn moves
      // the whole rig and the legs are still standing on the ground.
      hips: track([[0, q(0, s * 7)], [180, q(0, -s * 4)], [300, q(0, -s * 12)],
                   [500, q(0, 0)]]),
      // The chest counter-rotates against them, then overtakes — this is the
      // shoulder "coming through", and it is what makes the arm look driven
      // rather than thrown.
      torso: track([[0, q(2, -s * 6)], [180, q(5, s * 3)], [300, q(7, s * 10)],
                    [500, q(0, 0)]]),
      // The head stays on the target while the body turns under it, which is
      // the same remainder-of-the-yaw idea Mob::ApplyAimPart uses: the spine
      // took the turn, so the neck gives some of it back.
      head: track([[0, q(0, -s * 4)], [300, q(4, -s * 8)], [500, q(0, 0)]]),
      // The OFF hand guards: it comes up and in front of the face and stays
      // there through the blow. Elbow well bent (the forearm's hinge is
      // authored [0, 130] about -X, so a negative X here folds it).
      [`armU.${off}`]: track([[0, q(-38, 0, -s * 10)], [200, q(-52, 0, -s * 16)],
                              [500, q(0, 0)]]),
      [`armL.${off}`]: track([[0, q(-62)], [200, q(-78)], [500, q(0, 0)]]),
      [`hand.${off}`]: track([[0, q(-10)], [500, q(0, 0)]]),
    },
  };
}

// ---------------------------------------------------------------------------
// A BITE. The head is MASKED OUT: the Aim effector rotates it toward the tip
// direction every tick (Mob::ApplyStrikeAim), and a keyed head track would be
// blended in and then written over.
//
// So what is left is everything that makes a lunge read as a lunge from the
// neck down — the shoulders hunching forward, the chest dipping, and both arms
// coming up and out the way a thing that leads with its mouth carries them.
// 600 ms: a 10-tick windup plus a 4-tick cut plus most of a 12-tick recover.
function bite() {
  const arm = (s) => ({
    [`armU.${s}`]: track([[0, q(-20, 0, -(s === 'R' ? 1 : -1) * 22)],
                          [260, q(-46, 0, -(s === 'R' ? 1 : -1) * 34)],
                          [380, q(-30, 0, -(s === 'R' ? 1 : -1) * 30)],
                          [600, q(0, 0)]]),
    [`armL.${s}`]: track([[0, q(-30)], [260, q(-58)], [600, q(0, 0)]]),
  });
  return {
    name: 'bite',
    sidecarVoxelsPerMetre: 10,
    durationMs: 600,
    loop: false,
    mode: 'override',
    blendInMs: 90,
    blendOutMs: 180,
    mask: ['hips', 'torso', 'armU.L', 'armL.L', 'armU.R', 'armL.R'],
    tracks: {
      // The chest dips into the bite and then snaps forward with it. The
      // hunch is on the TORSO and not the hips, because the hips are holding
      // the body up and a creature that pitched from the pelvis would fall
      // over — and because a crawler's own state clip already owns the hips.
      torso: track([[0, q(6)], [260, q(16)], [380, q(24)], [600, q(0)]]),
      hips: track([[0, q(2)], [380, q(6)], [600, q(0)]]),
      ...arm('L'),
      ...arm('R'),
    },
  };
}

// ---------------------------------------------------------------------------
// A LUNGE: crouch, then extend. ADDITIVE (see the header) so it composes over
// walk, limp and crawl instead of replacing them.
//
// The timing is the flight's: `bite_lunge` authors 9 ticks of air = 300 ms, so
// the gather is over by 120 ms (the body is already leaving the ground) and the
// extension runs through the landing. The legs trail: a pouncing body's knees
// come up under it at the top and reach for the ground at the bottom, and that
// is one keyframe each rather than a second state machine.
function lunge() {
  return {
    name: 'lunge',
    sidecarVoxelsPerMetre: 10,
    durationMs: 520,
    loop: false,
    mode: 'additive',
    blendInMs: 60,
    blendOutMs: 200,
    // NO ARMS AND NO HEAD. The arms belong to whichever stroke is running over
    // the top of this (a lunging punch is a real thing) and the head to the
    // Aim effector; an additive clip that touched them would bias a solve it
    // knows nothing about.
    mask: ['hips', 'torso', 'legU.L', 'legL.L', 'legU.R', 'legL.R'],
    tracks: {
      // The gather and the release, as a delta on whatever the body is doing:
      // fold forward hard, then open past neutral as the creature stretches
      // out along its own flight.
      hips: track([[0, q(0)], [120, q(14)], [300, q(-6)], [520, q(0)]]),
      torso: track([[0, q(0)], [120, q(10)], [300, q(-4)], [520, q(0)]]),
      // Knees up at the top of the arc, reaching down at the bottom. Mirrored
      // exactly, because a pounce is symmetric — the asymmetry in a real one
      // comes from the ground it left, which the gait already owns.
      'legU.L': track([[0, q(0)], [120, q(-26)], [320, q(12)], [520, q(0)]]),
      'legU.R': track([[0, q(0)], [120, q(-26)], [320, q(12)], [520, q(0)]]),
      'legL.L': track([[0, q(0)], [120, q(34)], [320, q(-8)], [520, q(0)]]),
      'legL.R': track([[0, q(0)], [120, q(34)], [320, q(-8)], [520, q(0)]]),
    },
  };
}

mkdirSync(outDir, { recursive: true });
for (const clip of [punch('R'), punch('L'), bite(), lunge()]) {
  const path = join(outDir, `${clip.name}.json`);
  writeFileSync(path, JSON.stringify(clip, null, 2) + '\n');
  const parts = Object.keys(clip.tracks).length;
  console.log(`${clip.name}.json  ${clip.durationMs} ms, ${clip.mode}, ` +
              `${parts} track(s), mask of ${clip.mask.length}`);
}
