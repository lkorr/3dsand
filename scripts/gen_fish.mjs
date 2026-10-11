#!/usr/bin/env node
/* gen_fish.mjs — author-side bake of a FISH from its sidecar's `fishgen` block.
 *
 *   node scripts/gen_fish.mjs shark            # write shark.vox + the rig in shark.json
 *   node scripts/gen_fish.mjs piranha --dry    # report only
 *   node scripts/gen_fish.mjs --all            # every assets/mobs/*.json with a fishgen block
 *
 * A FISH IS A TABLE, NOT A SCULPT (the snake's rule, gen_snake.mjs, grown a
 * set of fins). The body is a cross-section profile along its length --
 * half-width, half-height and centre-line height at stations from the
 * peduncle (t = 0) to the snout (t = 1), as shares of the BODY length -- swept
 * into a superellipse tube (a flatter belly than back), cut into segments by
 * station, with a lower JAW split off the head along a mouth line, and FINS
 * rasterised as tapering plates: dorsal and anal keels, pelvic pairs, a forked
 * or heterocercal caudal, and pectorals that are their own limbs (they flap
 * and they come off). The coat is countershading (a dark back meeting a pale
 * belly along a wavy line), darker fin tips, eyes, gill slits, teeth along the
 * mouth line, and optional spots and a coloured throat.
 *
 * Laid out STRAIGHT ALONG +Z, snout forward, +X the fish's LEFT -- the layout
 * the wave (MobDef::SlitherDef, the swimmer's `swim.wave`) measures arc length
 * in and every rig in the engine faces.
 *
 * WHAT IT WRITES BACK INTO THE SIDECAR, so art and rig cannot drift:
 *   limbs[]        names, parents, joints, hp, tags, anchors -- the WHOLE list
 *   root           the segment the yaw and the pitch pivot about
 *   natural[jaws]  the bite edge, from the gape to the snout, in the head's frame
 *   swim.wave.parts  head first, tail last
 * Everything else in the sidecar (swim numbers, anatomy, clips, behaviour) is
 * authored by hand and left alone.
 *
 * Writing a fish does not move the world hash: nothing spawns it until a
 * spawner names it.
 */
import { readFileSync, writeFileSync, readdirSync } from 'fs';
import { fileURLToPath, pathToFileURL } from 'url';
import { dirname, join } from 'path';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const load = rel => import(pathToFileURL(join(ROOT, rel)).href);
const VOX = await load('assets/editor/vox.js');
const ANA = await load('assets/editor/anatomy.js');

const args = process.argv.slice(2);
const dry = args.includes('--dry');
const names = args.filter(a => !a.startsWith('--'));
if (args.includes('--all')) {
  for (const f of readdirSync(join(ROOT, 'assets/mobs')))
    if (f.endsWith('.json')) {
      try {
        const j = JSON.parse(readFileSync(join(ROOT, 'assets/mobs', f), 'utf8'));
        if (j && j.fishgen) names.push(f.slice(0, -5));
      } catch { /* not a sidecar */ }
    }
}
if (!names.length) {
  console.error('usage: node scripts/gen_fish.mjs <name>... | --all [--dry]');
  process.exit(2);
}

const materials = JSON.parse(
  readFileSync(join(ROOT, 'assets/materials/materials.json'), 'utf8')).materials;
const matId = ANA.materialIds(materials);
if (!matId.skin) throw new Error('materials.json has no `skin`');

for (const name of names) bake(name);

// ---------------------------------------------------------------------------
function bake(name) {
  const jsonPath = join(ROOT, 'assets/mobs', name + '.json');
  const voxPath = join(ROOT, 'assets/mobs', name + '.vox');
  const side = JSON.parse(readFileSync(jsonPath, 'utf8'));
  const G = side.fishgen;
  if (!G) throw new Error(name + '.json has no fishgen block');
  const avpm = side.artVoxelsPerMetre || 40;

  // ---- lengths (art voxels) ----------------------------------------------
  const Ltot = Math.round(G.length * avpm);
  const Lc = Math.round(Ltot * (G.caudalBack ?? 0.15));   // the caudal's reach behind
  const Lb = Ltot - Lc;                                   // peduncle .. snout
  const zOfT = t => Lc + t * Lb;                          // station -> z
  const tOfZ = z => (z - Lc) / Lb;

  // ---- the profile ----------------------------------------------------------
  // Stations [t, hw, hh, yc] (shares of Lb), smoothstep between stations so
  // the tube has no creases. Out of range clamps to the end station.
  const P = G.profile.map(r => ({ t: r[0], hw: r[1] * Lb, hh: r[2] * Lb, yc: (r[3] || 0) * Lb }));
  function section(t) {
    if (t <= P[0].t) return { ...P[0] };
    for (let i = 1; i < P.length; i++) {
      if (t <= P[i].t) {
        const a = P[i - 1], b = P[i];
        let u = (t - a.t) / Math.max(1e-6, b.t - a.t);
        u = u * u * (3 - 2 * u);
        return { hw: a.hw + (b.hw - a.hw) * u, hh: a.hh + (b.hh - a.hh) * u,
                 yc: a.yc + (b.yc - a.yc) * u };
      }
    }
    return { ...P[P.length - 1] };
  }
  // The body sits with its lowest point on y = 0.
  let yLow = Infinity;
  for (let t = 0; t <= 1.0001; t += 0.01) {
    const s = section(t);
    yLow = Math.min(yLow, s.yc - s.hh);
  }
  const Y0 = -yLow;   // add to a section's yc
  const flat = G.flat ?? 2.4;   // the belly's superellipse exponent

  // ---- the voxel map ----------------------------------------------------------
  // key "x,y,z" -> { limb, c } with x centred on 0 (+X = the fish's LEFT).
  const vox = new Map();
  const key = (x, y, z) => x + ',' + y + ',' + z;
  const put = (x, y, z, limb, c, force = false) => {
    const k = key(x, y, z);
    if (!force && vox.has(k)) return;
    vox.set(k, { x, y, z, limb, c });
  };

  // ---- the coat ---------------------------------------------------------------
  const C = G.coat || {};
  const art = new VOX.ArtPalette();
  const col = h => (h ? art.alloc(h) : 0);
  const K = {
    back: col(C.back || '#5b6b78'), back2: col(C.back2 || C.back || '#526170'),
    belly: col(C.belly || '#e9e7df'), belly2: col(C.belly2 || C.belly || '#dcd9cf'),
    fin: col(C.fin || C.back || '#4a5865'), finTip: col(C.finTip || '#2f3942'),
    eye: col(C.eye || '#0a0a0c'), eyeRing: col(C.eyeRing), gill: col(C.gill || '#3c4853'),
    mouth: col(C.mouth || '#5a2a2e'), teeth: col(C.teeth || '#f3efe2'),
    spot: col(C.spots && C.spots.color), throat: col(C.throat && C.throat.color),
    nostril: col(C.nostril || '#20262c'), shine: col(C.shine),
  };
  const hash = (x, y, z) => {
    let h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791);
    h = Math.imul(h ^ (h >>> 13), 1274126177);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  };

  // ---- segments and the jaw ---------------------------------------------------
  const segs = G.segments.map(([n, to]) => ({ name: n, to }));
  const segAt = t => {
    for (const s of segs) if (t <= s.to) return s.name;
    return segs[segs.length - 1].name;
  };
  const headName = segs[segs.length - 1].name;
  const J = G.jaw;   // { gape: [t, ySh], tip: [t, ySh] } ySh = share of local hh from the centre
  // The mouth line's height (absolute y) at station t, between gape and tip.
  const mouthY = t => {
    const u = Math.min(1, Math.max(0, (t - J.gape[0]) / (J.tip[0] - J.gape[0])));
    const s = section(t);
    const sh = J.gape[1] + (J.tip[1] - J.gape[1]) * u;
    return Y0 + s.yc + sh * s.hh;
  };

  // ---- the body tube ------------------------------------------------------------
  for (let z = Lc; z < Ltot; z++) {
    const t = tOfZ(z + 0.5);
    const s = section(t);
    const cy = Y0 + s.yc;
    const xr = Math.ceil(s.hw + 1), y0 = Math.floor(cy - s.hh - 1), y1 = Math.ceil(cy + s.hh + 1);
    for (let y = y0; y <= y1; y++)
      for (let x = -xr; x <= xr; x++) {
        const ex = (x + 0.5) / Math.max(s.hw, 0.5);
        const ey = (y + 0.5 - cy) / Math.max(s.hh, 0.5);
        const p = ey < 0 ? flat : 2.0;
        const r = Math.abs(ex) ** 2 + Math.abs(ey) ** p;
        // The centre column never vanishes, so a fine snout still has a tip.
        if (r > 1.0 && !(x === 0 && Math.abs(y + 0.5 - cy) < 0.75)) continue;
        // How far in from the surface (0 at the skin): for teeth and paint.
        const rim = 1 - Math.sqrt(Math.min(1, r));
        let limb = segAt(t);
        // THE LOWER JAW: head cells below the mouth line, gape to tip.
        let c = bodyColour(x, y, z, t, s, cy, rim);
        if (J && limb === headName && t >= J.gape[0] && t <= J.tip[0] + 0.02) {
          const my = mouthY(Math.min(t, J.tip[0]));
          if (y + 0.5 < my) {
            limb = 'jaw';
            if (y + 0.5 > my - 1.2) c = rim < 0.35 ? K.teeth : K.mouth;   // its top
          } else if (y + 0.5 < my + 1.2) {
            c = rim < 0.35 ? K.teeth : K.mouth;                            // the palate
          }
        }
        put(x, y, z, limb, c);
      }
  }

  function bodyColour(x, y, z, t, s, cy, rim) {
    // COUNTERSHADING: the back colour above a wavy line, the belly below, a
    // dithered band between. `ny` 0 at the belly, 1 at the crown.
    const ny = (y + 0.5 - (cy - s.hh)) / Math.max(1, 2 * s.hh);
    const wave = 0.04 * Math.sin(z * 0.37) + 0.025 * Math.sin(z * 0.11 + 1.3);
    const line = (C.line ?? 0.45) + wave;
    const n = hash(x, y, z);
    // Eye: a disc on each flank.
    if (C.eyeT != null) {
      const ez = zOfT(C.eyeT), sE = section(C.eyeT);
      const ey = Y0 + sE.yc + (C.eyeY ?? 0.25) * sE.hh;
      const er = (C.eyeR ?? 1.0) * (avpm / 40);
      const dz = z + 0.5 - ez, dy = y + 0.5 - ey;
      const d = Math.sqrt(dz * dz + dy * dy);
      if (rim < 0.3 && Math.abs(x) >= 0 && d <= er + 0.6) {
        if (d <= er) return K.eye;
        if (K.eyeRing) return K.eyeRing;
      }
    }
    // Gill slits: thin dark arcs on the flank behind the head.
    if (C.gills && rim < 0.3) {
      const g = C.gills;
      for (let i = 0; i < g.n; i++) {
        const gt = g.t[0] + (g.t[1] - g.t[0]) * (g.n > 1 ? i / (g.n - 1) : 0);
        const sg = section(gt);
        const gy0 = Y0 + sg.yc + g.y[0] * sg.hh, gy1 = Y0 + sg.yc + g.y[1] * sg.hh;
        const yy = y + 0.5;
        if (yy < gy0 || yy > gy1) continue;
        // A slight backward curve, top behind bottom.
        const zz = zOfT(gt) - (yy - gy0) / Math.max(1, gy1 - gy0) * (g.curve ?? 1.0);
        if (Math.abs(z + 0.5 - zz) < 0.55) return K.gill;
      }
    }
    // Nostrils under the snout.
    if (C.nostrilT != null && rim < 0.3 && Math.abs(z + 0.5 - zOfT(C.nostrilT)) < 0.8 &&
        ny < 0.4 && Math.abs(x) >= Math.max(1, s.hw * 0.4) && Math.abs(x) <= s.hw * 0.75)
      return K.nostril;
    // A coloured throat / belly flash.
    if (C.throat && ny < C.throat.below && t >= C.throat.t[0] && t <= C.throat.t[1])
      return n < 0.15 ? K.belly2 : K.throat;
    // Spots on the flank.
    if (C.spots && ny > C.spots.y[0] && ny < C.spots.y[1] && t >= C.spots.t[0] &&
        t <= C.spots.t[1] && rim < 0.35 && hash(x * 3 + 7, y * 5 + 1, z * 7 + 3) < C.spots.density)
      return K.spot;
    // A glint along the flank.
    if (K.shine && ny > 0.45 && ny < 0.62 && n < 0.07) return K.shine;
    if (ny > line + 0.05) return n < (C.noise ?? 0.08) ? K.back2 : K.back;
    if (ny < line - 0.05) return n < 0.12 ? K.belly2 : K.belly;
    return ((x + y + z) & 1) ? K.back2 : K.belly;
  }

  // ---- the fins ---------------------------------------------------------------
  const finCol = (u) => (u > 0.78 ? K.finTip : K.fin);
  // A keel (dorsal / anal) on the centre line: a swept plate rising off the back
  // (dir +1) or hanging off the belly (dir -1).
  function keel(f, dir) {
    const zF = zOfT(f.t[1]), zB = zOfT(f.t[0]);
    const bLen = Math.max(1, zF - zB);
    const h = f.height * Lb;
    const uA = bLen * (0.35 + (f.sweep ?? 0.5));   // the apex, behind the front
    const thick = f.thick || 1;
    for (let z = Math.floor(zB - uA); z <= Math.ceil(zF); z++) {
      const u = zF - (z + 0.5);
      const tt = tOfZ(z + 0.5);
      const s = section(Math.min(1, Math.max(0, tt)));
      const surf = Y0 + s.yc + dir * s.hh * 0.92;
      for (let v = 0; v <= h + 1; v += 0.5) {
        const fv = v / h;
        if (fv > 1) break;
        const lead = uA * fv;
        const trail = bLen + (uA - bLen) * Math.pow(fv, 0.7);
        if (u < lead - 0.3 || u > trail + 0.3) continue;
        const y = Math.floor(surf + dir * v);
        const w = Math.max(1, Math.round(thick * (1 - fv * 0.7)));
        for (let x = -Math.floor((w - 1) / 2); x <= Math.floor(w / 2); x++)
          put(x, y, z, segAt(tt), finCol(fv));
      }
    }
  }
  // A plate swept from a root point along a direction, chord tapering to the
  // tip, thickness tapering too. `chordDir` is the way the chord runs from the
  // leading edge.
  function plate(root, dir, len, chord0, chordDir, thick0, normal, limb) {
    const step = 0.35;
    for (let sd = 0; sd <= len; sd += step) {
      const u = sd / len;
      const chord = Math.max(1, chord0 * Math.pow(1 - u, 0.75) + 0.6);
      const th = Math.max(1, thick0 * (1 - u * 0.75));
      for (let w = 0; w <= chord; w += step)
        for (let k = 0; k < th; k += 0.6) {
          const p = [root[0] + dir[0] * sd + chordDir[0] * w + normal[0] * (k - th / 2),
                     root[1] + dir[1] * sd + chordDir[1] * w + normal[1] * (k - th / 2),
                     root[2] + dir[2] * sd + chordDir[2] * w + normal[2] * (k - th / 2)];
          put(Math.floor(p[0]), Math.floor(p[1]), Math.floor(p[2]),
              typeof limb === 'function' ? limb(p) : limb, finCol(u));
        }
    }
  }
  const norm = v => { const l = Math.hypot(...v) || 1; return v.map(a => a / l); };
  for (const f of G.fins || []) {
    if (f.kind === 'dorsal') keel(f, 1);
    else if (f.kind === 'anal') keel(f, -1);
    else if (f.kind === 'caudal') {
      // Two lobes from the peduncle, up-and-back and down-and-back, joined by a
      // web for `fork` of their length.
      const s0 = section(0);
      const cy = Y0 + s0.yc;
      const tailSeg = segAt(0);
      for (const [lobe, sgn] of [[f.upper, 1], [f.lower, -1]]) {
        const len = lobe[0] * Lb, ang = lobe[1];
        const dir = norm([0, sgn * Math.sin(ang), -Math.cos(ang)]);
        const root = [0.5, cy + sgn * s0.hh * 0.3, Lc + 1.5];
        // The chord runs from the lobe's leading edge toward the fork.
        const chordDir = norm([0, -sgn * Math.cos(ang), -Math.sin(ang)]);
        plate(root, dir, len, f.chord * Lb, chordDir, f.thick || 2, [1, 0, 0], tailSeg);
      }
      // The web between the lobes near the root.
      const forkLen = (f.fork ?? 0.35) * Math.min(f.upper[0], f.lower[0]) * Lb;
      for (let d = 0; d <= forkLen; d += 0.5) {
        const z = Math.floor(Lc + 1 - d);
        const yTop = cy + s0.hh * 0.3 + Math.tan(f.upper[1]) * d;
        const yBot = cy - s0.hh * 0.3 - Math.tan(f.lower[1]) * d;
        for (let y = Math.floor(yBot); y <= Math.ceil(yTop); y++) put(0, y, z, tailSeg, K.fin);
      }
      // ...and the peduncle runs back into it.
      for (let z = Lc - 2; z < Lc; z++)
        for (let y = Math.floor(cy - s0.hh); y <= Math.ceil(cy + s0.hh); y++)
          put(0, y, z, tailSeg, K.fin);
    } else if (f.kind === 'pectoral' || f.kind === 'pelvic') {
      const tm = (f.t[0] + f.t[1]) / 2;
      const s = section(tm);
      const zr = zOfT(f.t[1]);
      const chord0 = (f.t[1] - f.t[0]) * Lb;
      const len = f.length * Lb;
      const droop = f.droop ?? 0.4, sweep = f.sweep ?? 0.5;
      for (const side of [1, -1]) {
        const root = [side * s.hw * 0.8, Y0 + s.yc - (f.rootY ?? 0.45) * s.hh, zr];
        const dir = norm([side * Math.cos(droop), -Math.sin(droop), -sweep]);
        const chordDir = [0, 0, -1];
        const normal = norm([0, 1, 0]);
        const limb = f.limbs ? (side > 0 ? 'fin.L' : 'fin.R') : segAt(tm);
        plate(root, dir, len, chord0, chordDir, f.thick || 1, normal, limb);
      }
    }
  }

  // ---- teeth that always show (a piranha's underbite) ---------------------------
  if (J && C.teethAlways) {
    for (let z = Math.floor(zOfT(J.gape[0] + 0.03)); z < Math.ceil(zOfT(J.tip[0])); z++) {
      const t = tOfZ(z + 0.5);
      const s = section(t);
      const my = mouthY(t);
      for (const side of [1, -1]) {
        const x = Math.floor(side * (s.hw * 0.75)) + (side < 0 ? 0 : 0);
        const y = Math.floor(my);
        const k = key(x, y, z);
        if (vox.has(k)) vox.get(k).c = K.teeth;
        if ((z & 1) === 0) put(x, y + 1, z, headName, K.teeth, true);
      }
    }
  }

  // ---- models per limb ------------------------------------------------------------
  const limbNames = [...new Set([...vox.values()].map(v => v.limb))];
  const models = [];
  for (const ln of limbNames) {
    const cells = [...vox.values()].filter(v => v.limb === ln);
    const mn = { x: Infinity, y: Infinity, z: Infinity }, mx = { x: -Infinity, y: -Infinity, z: -Infinity };
    for (const v of cells)
      for (const a of ['x', 'y', 'z']) { mn[a] = Math.min(mn[a], v[a]); mx[a] = Math.max(mx[a], v[a]); }
    const dim = { x: mx.x - mn.x + 1, y: mx.y - mn.y + 1, z: mx.z - mn.z + 1 };
    for (const a of ['x', 'y', 'z'])
      if (dim[a] > VOX.MAX_DIM) throw new Error(`${name}: limb ${ln} is ${dim[a]} art voxels on ${a}`);
    const g = VOX.makeGrid(dim);
    for (const v of cells) {
      VOX.gridSet(g, v.x - mn.x, v.y - mn.y, v.z - mn.z, matId.skin);
      if (v.c) VOX.gridColorSet(g, v.x - mn.x, v.y - mn.y, v.z - mn.z, v.c);
    }
    models.push({ name: ln, offset: { ...mn }, dim, grid: g, count: cells.length });
  }
  const { prefab, shift } = VOX.tightenPrefab({ size: { x: 0, y: 0, z: 0 }, models });
  const total = models.reduce((a, m) => a + m.count, 0);
  console.log(`${name}: ${models.length} limbs, ${total} art voxels, ` +
              `${Ltot} long (${(Ltot / avpm).toFixed(2)} m), body ${Lb}, caudal ${Lc}`);

  // ---- anatomy --------------------------------------------------------------------
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

  // ---- the rig, written back --------------------------------------------------------
  const A = (x, y, z) => [+(x - shift.x).toFixed(2), +(y - shift.y).toFixed(2), +(z - shift.z).toFixed(2)];
  const rootName = G.root;
  const segNames = segs.map(s => s.name);                 // tail .. head
  const rootIdx = segNames.indexOf(rootName);
  if (rootIdx < 0) throw new Error(`${name}: root ${rootName} is not a segment`);
  const segFrom = i => (i === 0 ? -0.02 : segs[i - 1].to);
  const hp = G.hp || {};
  const limbs = [];
  segNames.forEach((sn, i) => {
    const L = { name: sn };
    if (i !== rootIdx) {
      // Toward the root: a forward segment hangs from its BACK face, a
      // rearward one from its FRONT face, both on the centre line.
      const tj = i > rootIdx ? segFrom(i) : segs[i].to;
      const s = section(Math.max(0, tj));
      L.parent = i > rootIdx ? segNames[i - 1] : segNames[i + 1];
      L.joint = 'ball';
      L.anchor = A(0.5, Y0 + s.yc, zOfT(Math.max(0, tj)));
    }
    L.hp = hp[sn] ?? 40;
    L.severable = i !== rootIdx;
    if (sn === headName || i === rootIdx) L.vital = true;
    L.tag = sn === headName ? 'head' : i === 0 ? 'tail' : 'spine';
    if (L.severable) L.severImpactSpeed = sn === headName ? 18 : 14;
    limbs.push(L);
  });
  if (J && limbNames.includes('jaw')) {
    const s = section(J.gape[0]);
    limbs.push({ name: 'jaw', parent: headName, joint: 'ball', hp: hp.jaw ?? 20,
                 severable: true, tag: 'jaw', severImpactSpeed: 14,
                 anchor: A(0.5, mouthY(J.gape[0]), zOfT(J.gape[0])) });
  }
  for (const fn of ['fin.L', 'fin.R']) {
    if (!limbNames.includes(fn)) continue;
    const f = (G.fins || []).find(q => q.kind === 'pectoral' && q.limbs);
    const tm = (f.t[0] + f.t[1]) / 2;
    const s = section(tm);
    const side = fn === 'fin.L' ? 1 : -1;
    limbs.push({ name: fn, parent: segAt(tm), joint: 'ball', hp: hp.fin ?? 15,
                 severable: true, tag: 'fin', severImpactSpeed: 10,
                 anchor: A(side * s.hw * 0.8 + 0.5, Y0 + s.yc - (f.rootY ?? 0.45) * s.hh,
                           zOfT(f.t[1]) - 0.5) });
  }
  // Keep any authored extras the generator does not own (sockets, per-limb
  // overrides): a limb the generator writes REPLACES the old entry of that name.
  side.root = rootName;
  side.limbs = limbs;

  // The jaws: the head's own frame (origin = the head model's min corner), from
  // the gape back to the snout tip along the mouth line, mouth-wide.
  const headM = prefab.models.find(m => m.name === headName);
  const nw = (side.natural || []).find(n => n.part === headName);
  if (nw && headM && J) {
    const ox = headM.offset.x + shift.x, oy = headM.offset.y + shift.y, oz = headM.offset.z + shift.z;
    const sG = section(J.gape[0]);
    nw.edge = nw.edge || {};
    nw.edge.from = [+(0.5 - ox).toFixed(2), +(mouthY(J.gape[0]) - oy).toFixed(2),
                    +(zOfT(J.gape[0]) - oz).toFixed(2)];
    nw.edge.to = [+(0.5 - ox).toFixed(2), +(mouthY(J.tip[0]) - oy).toFixed(2),
                  +(Ltot - oz).toFixed(2)];
    nw.edge.halfWidth = +Math.max(1, sG.hw * 0.8).toFixed(2);
  }
  // The trunk's widest section, fins excluded, in WORLD voxels: what the swim
  // drive keeps under the waterline and over the bed, and what a body on its
  // flank rests on (MobDef::SwimDef::trunkHalfW/H).
  side.swim = side.swim || {};
  {
    let w = 0, h = 0;
    for (let t = 0; t <= 1.0001; t += 0.01) {
      const s = section(t);
      w = Math.max(w, s.hw);
      h = Math.max(h, s.hh);
    }
    const toWorld = 10 / avpm;   // art voxels -> world voxels (0.1 m each)
    side.swim.trunkHalfW = +(w * toWorld).toFixed(3);
    side.swim.trunkHalfH = +(h * toWorld).toFixed(3);
  }
  side.swim.wave = side.swim.wave || {};
  side.swim.wave.parts = [...segNames].reverse();

  if (dry) { console.log('(dry run: nothing written)'); return; }
  const pal = VOX.paletteFromMaterials(materials);
  art.writeInto(pal);
  const rt = VOX.prefabRoundTripTest(prefab, pal);
  if (!rt.ok) { console.error('round-trip failed: ' + rt.error); process.exit(1); }
  const bytes = VOX.writeVox(VOX.prefabToVoxModels(prefab), pal, { scene: true });
  writeFileSync(voxPath, bytes);
  writeFileSync(jsonPath, JSON.stringify(side, null, 2) + '\n');
  console.log(`wrote ${voxPath} (${bytes.length} bytes) and the rig in ${name}.json`);
}
