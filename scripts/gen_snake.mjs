#!/usr/bin/env node
/* gen_snake.mjs — author-side bake of assets/mobs/snake.vox (+ the joint
 * anchors in snake.json), the snake of docs/PLAN_weapon_coats.md package D.
 *
 *   node scripts/gen_snake.mjs          # write snake.vox, refresh anchors
 *   node scripts/gen_snake.mjs --dry    # report only
 *
 * THE SHAPE IS A TABLE, NOT A SCULPT. A snake is one tapering tube, so the
 * body is described by its cross-section along its length (width, height) and
 * cut into limbs by z range; every limb is a slice of the same tube, which is
 * what makes the segments meet without a step. Laid out STRAIGHT ALONG +Z with
 * the snout at the top of z (forward, as every rig in this engine faces) and
 * the belly on y = 0 — the slither (MobDef::SlitherDef) assumes exactly that
 * layout when it measures arc length.
 *
 * WHAT IS UNDER THE SKIN IS snake.json's `anatomy` recipe, baked here through
 * assets/editor/anatomy.js — the same call scripts/anatomize_mob.mjs makes, and
 * the same recipe the engine re-resolves at load (game/anatomy_resolve.h), so
 * the `anatomy-parity` gate holds. Changing a material name in that recipe
 * (the venom gland) and re-running this script is the whole edit.
 *
 * THE JOINTS are written back into snake.json's `limbs[].anchor` from the same
 * table, so the art and the rig cannot drift apart: a forward segment hangs
 * from the middle of its BACK face (toward the root), a rearward one from the
 * middle of its FRONT face. The root's anchor is derived by the loader.
 *
 * Writing the snake does not move the world hash: nothing spawns it until a
 * spawner names it.
 */
import { readFileSync, writeFileSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const VOX = await load('assets/editor/vox.js');
const ANA = await load('assets/editor/anatomy.js');

const dry = process.argv.includes('--dry');
const jsonPath = join(ROOT, 'assets/mobs/snake.json');
const voxPath = join(ROOT, 'assets/mobs/snake.vox');
const side = JSON.parse(readFileSync(jsonPath, 'utf8'));
const materials = JSON.parse(
  readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const matId = ANA.materialIds(materials);
if (!matId.skin) throw new Error('materials.json has no `skin`');

// ---- the tube ---------------------------------------------------------------
// Segments TAIL FIRST in z (z = 0 is the tail tip). `len` in art voxels at
// snake.json's artVoxelsPerMetre (80 = 8 per world voxel); `w`/`h` are the
// cross-section at the segment's FRONT end, so the tube tapers linearly from
// the previous segment's front to this one's.
const SEGS = [
  { name: 'tail',   len: 18, w: 6,  h: 5 },
  { name: 'body10', len: 11, w: 8,  h: 7 },
  { name: 'body9',  len: 11, w: 9,  h: 7 },
  { name: 'body8',  len: 11, w: 10, h: 8 },
  { name: 'body7',  len: 11, w: 11, h: 8 },
  { name: 'body6',  len: 11, w: 11, h: 9 },
  { name: 'body5',  len: 11, w: 11, h: 9 },
  { name: 'body4',  len: 11, w: 11, h: 9 },
  { name: 'body3',  len: 11, w: 10, h: 8 },
  { name: 'body2',  len: 11, w: 9,  h: 8 },
  { name: 'body1',  len: 10, w: 8,  h: 7 },   // the neck: narrower than the head
  { name: 'head',   len: 16, w: 13, h: 8 },
];
const ROOT_SEG = side.root || 'body5';
const TAIL_TIP = { w: 1.2, h: 1.2 };
const totalLen = SEGS.reduce((a, s) => a + s.len, 0);
const W = 13;                       // grid width: the broadest section
const CX = W / 2;                   // the centre line, x
// z ranges
let z = 0;
for (const s of SEGS) { s.z0 = z; s.z1 = z + s.len; z = s.z1; }

// Cross-section at a z (continuous). The head is its own shape: a viper's
// flat spade, widest at the jaw hinge and narrowing to the snout, lower at
// the front.
function section(zc) {
  const head = SEGS[SEGS.length - 1];
  if (zc >= head.z0) {
    const t = (zc - head.z0) / head.len;          // 0 at the neck, 1 at the snout
    const prev = SEGS[SEGS.length - 2];
    // neck -> jaw hinge widening over the first 30%, then the spade tapers
    const wHinge = head.w, wSnout = 6;
    const w = t < 0.3 ? prev.w + (wHinge - prev.w) * (t / 0.3)
                      : wHinge + (wSnout - wHinge) * ((t - 0.3) / 0.7) ** 1.3;
    const h = t < 0.3 ? prev.h + (head.h - prev.h) * (t / 0.3)
                      : head.h + (4.5 - head.h) * ((t - 0.3) / 0.7);
    return { w, h };
  }
  for (let i = 0; i < SEGS.length; i++) {
    const s = SEGS[i];
    if (zc < s.z1 || i === SEGS.length - 2) {
      const back = i === 0 ? TAIL_TIP : SEGS[i - 1];
      const t = Math.min(1, Math.max(0, (zc - s.z0) / s.len));
      return { w: back.w + (s.w - back.w) * t, h: back.h + (s.h - back.h) * t };
    }
  }
  return { w: 1, h: 1 };
}

// ---- the art: an adder's coat ------------------------------------------------
const art = new VOX.ArtPalette();
const C = {
  belly: art.alloc('#cfc39a'), bellyB: art.alloc('#b9ad84'),
  flank: art.alloc('#7e6c3f'), flankB: art.alloc('#6c5c34'),
  back: art.alloc('#8d7a48'), backB: art.alloc('#7a6a3d'),
  zig: art.alloc('#2b2318'), zigEdge: art.alloc('#3f3322'),
  spot: art.alloc('#3a2f1f'),
  eye: art.alloc('#d6a128'), pupil: art.alloc('#0e0c0a'),
  mouth: art.alloc('#2a1f17'), lip: art.alloc('#d8cfae'),
  tailTip: art.alloc('#a39a72'),
};

const cellHash = (x, y, zz) => {
  let h = (x * 73856093) ^ (y * 19349663) ^ (zz * 83492791);
  h = Math.imul(h ^ (h >>> 13), 1274126177);
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
};

function colourAt(x, y, zc, w, h) {
  const head = SEGS[SEGS.length - 1];
  const dx = x + 0.5 - CX;                  // + = the snake's left (+X)
  const ny = (y + 0.5) / Math.max(h, 1);    // 0 belly .. 1 crown
  // Scales: a diamond lattice two voxels across, alternating two tones, so
  // the coat reads as scales and not as a flat paint at body scale.
  const scale = ((Math.floor((zc + y) / 2) + Math.floor((dx + 8 + zc) / 2)) & 1) === 0;
  if (zc >= head.z0) {
    const t = (zc - head.z0) / head.len;
    // eyes: a pair at the sides of the crown, a third of the way from the snout
    if (t > 0.6 && t < 0.78 && ny > 0.55 && ny < 0.9 && Math.abs(dx) > w / 2 - 1.6)
      return (t > 0.66 && t < 0.72 && ny > 0.65 && ny < 0.82) ? C.pupil : C.eye;
    // the mouth line along each side, pale lips under it
    if (ny > 0.2 && ny < 0.36 && Math.abs(dx) > w / 2 - 1.5 && t > 0.3) return C.mouth;
    if (ny <= 0.2) return C.lip;
    // the V on the crown, opening toward the neck
    if (ny > 0.7) {
      const arm = (1 - t) * 4.2;
      if (Math.abs(Math.abs(dx) - arm) < 1.0 && t < 0.75) return C.zig;
      return scale ? C.back : C.backB;
    }
    return scale ? C.flank : C.flankB;
  }
  if (zc < 4) return C.tailTip;
  // belly scutes: broad bands, one every two voxels
  if (ny < 0.28) return (Math.floor(zc / 2) & 1) ? C.belly : C.bellyB;
  // the dorsal zigzag of an adder, period 8 along the body
  const period = 8, amp = Math.max(1.0, w * 0.22);
  const ph = (zc % period) / period;
  const tri = (ph < 0.5 ? ph * 4 - 1 : 3 - ph * 4) * amp;
  if (ny > 0.62) {
    const d = Math.abs(dx - tri);
    if (d < 1.1) return C.zig;
    if (d < 1.8) return C.zigEdge;
    return scale ? C.back : C.backB;
  }
  // flank spots, under each apex of the zigzag
  if (ny > 0.35 && ny < 0.6 && Math.abs(ph - 0.25) < 0.08 && dx < 0) return C.spot;
  if (ny > 0.35 && ny < 0.6 && Math.abs(ph - 0.75) < 0.08 && dx > 0) return C.spot;
  // a little noise so the flanks are not a clean print
  if (cellHash(x, y, zc) < 0.06) return C.flankB;
  return scale ? C.flank : C.flankB;
}

// ---- voxelise -----------------------------------------------------------------
const H = Math.max(...SEGS.map(s => s.h)) + 1;
const models = [];
for (const s of SEGS) {
  const dim = { x: W, y: H, z: s.len };
  const g = VOX.makeGrid(dim);
  let n = 0;
  for (let lz = 0; lz < s.len; lz++) {
    const zc = s.z0 + lz + 0.5;
    const { w, h } = section(zc);
    for (let y = 0; y < H; y++)
      for (let x = 0; x < W; x++) {
        const ex = (x + 0.5 - CX) / (w / 2);
        // The belly is flattened: a superellipse below the middle, round above.
        const cyy = h / 2;
        const ey = (y + 0.5 - cyy) / (h / 2);
        const p = ey < 0 ? 3.0 : 2.0;
        const inside = Math.abs(ex) ** 2 + Math.abs(ey) ** p <= 1.0;
        // Never lose the centre column, so a 1-voxel tail tip still exists.
        const spine = Math.abs(x + 0.5 - CX) < 0.6 && y < Math.max(1, h);
        if (!inside && !(spine && y === 0)) continue;
        VOX.gridSet(g, x, y, lz, matId.skin);
        VOX.gridColorSet(g, x, y, lz, colourAt(x, y, zc, w, h));
        n++;
      }
  }
  models.push({ name: s.name, offset: { x: 0, y: 0, z: s.z0 }, dim, grid: g, count: n });
}
const prefab0 = { size: { x: W, y: H, z: totalLen }, models };
const { prefab, shift } = VOX.tightenPrefab(prefab0);
console.log(`snake: ${SEGS.length} limbs, ${totalLen} art voxels long ` +
            `(${(totalLen / (side.artVoxelsPerMetre || 80)).toFixed(2)} m), ` +
            `rebase shift ${shift.x},${shift.y},${shift.z}`);

// ---- anatomy -------------------------------------------------------------------
if (side.anatomy) {
  const plan = ANA.planAnatomy(prefab, side.anatomy, matId);
  if (plan.report.unresolved.length) {
    console.error('anatomy names unknown materials: ' + plan.report.unresolved.join(', '));
    process.exit(1);
  }
  ANA.applyPlan(prefab, plan);
  for (const [limb, hist] of Object.entries(plan.report.perLimb))
    console.log('  ' + limb.padEnd(7) +
      Object.entries(hist).map(([m, n]) => `${m} ${n}`).join(', '));
}

// ---- joints back into the sidecar ------------------------------------------------
// A rearward segment (tail side of the root) hangs from its FRONT face, a
// forward one from its BACK face; both on the centre line at the height of
// the thinner of the two sections that meet there.
const rootIdx = SEGS.findIndex(s => s.name === ROOT_SEG);
const anchors = {};
SEGS.forEach((s, i) => {
  if (i === rootIdx) return;
  const zj = i < rootIdx ? s.z1 : s.z0;
  const a = section(zj - 0.01), b = section(zj + 0.01);
  const yj = Math.min(a.h, b.h) / 2;
  anchors[s.name] = [CX - shift.x, +(yj - shift.y).toFixed(2), zj - shift.z];
});
for (const l of side.limbs || [])
  if (anchors[l.name]) l.anchor = anchors[l.name];

// The jaws ride the head's own frame (origin = the head model's min corner):
// from the jaw hinge to the snout, at mouth height.
const headM = prefab.models.find(m => m.name === 'head');
for (const nw of side.natural || [])
  if (nw.part === 'head' && headM) {
    nw.edge.from = [+(CX - headM.offset.x).toFixed(2), 3, 5];
    nw.edge.to = [+(CX - headM.offset.x).toFixed(2), 3, headM.dim.z];
  }

if (dry) { console.log('(dry run: nothing written)'); process.exit(0); }
const pal = VOX.paletteFromMaterials(materials);
art.writeInto(pal);
const rt = VOX.prefabRoundTripTest(prefab, pal);
if (!rt.ok) { console.error('round-trip failed: ' + rt.error); process.exit(1); }
const bytes = VOX.writeVox(VOX.prefabToVoxModels(prefab), pal, { scene: true });
writeFileSync(voxPath, bytes);
writeFileSync(jsonPath, JSON.stringify(side, null, 2) + '\n');
console.log(`wrote ${voxPath} (${bytes.length} bytes) and the anchors in snake.json`);
