#!/usr/bin/env node
// tint_coverage.mjs -- how well each MATF_TINTED material's authored `tints`
// cover the colours actually WORN on it, and (with --fit) the entries to
// APPEND so they do.
//
// WHY THIS EXISTS. A body voxel is (material, 8-bit art colour). When its
// matter enters the world grid (settle-back, cut spray, a severed garment that
// lands) the art colour cannot follow -- the 32-bit world word has no room --
// so DebrisSystem::DepositState snaps it to the material's NEAREST authored
// tint (materials.json `tints`, at most 16, entry 0 = the natural colour) and
// stores that index in the state nibble (sim/materials.h kMatFlagTinted). A
// garment painted in a colour none of its material's tints is near lands as
// whatever tint happens to be least far.
//
// WHAT IT READS. Every .vox under assets/items and assets/mobs (recursively:
// prototypes, variants, the generated pool), each painted voxel's art slot
// resolved to RGB through that file's own RGBA chunk -- exactly what
// sim/voxload.cpp does before the merge -- tallied per (material, colour).
// Unpainted voxels are the material's natural colour and are not counted.
//
// DYED GARMENTS (game/dye.h) are their own case: a dyeable item's art is a
// GREY weave the runtime dye multiplies, and DepositState quantizes the
// product, so their per-file greys are excluded and the material is instead
// scored against a sample of the dyes NPCs are actually rolled
// (session.cpp: hue 0..1, saturation 0.45..0.95, value 0.35..0.85).
//
// Usage:
//   node scripts/tint_coverage.mjs                 report
//   node scripts/tint_coverage.mjs --fit           + propose entries to APPEND
//   node scripts/tint_coverage.mjs --fit --write   + write them to materials.json
//   --only cloth,linen                             restrict --fit to these
//
// APPEND-ONLY, ON PURPOSE. A tint index is stored in saved world cells, so
// re-ordering or replacing an entry would recolour every cell already in a
// save; appending cannot. Entry 0 (the natural colour) is never touched.
// By default --fit only proposes for materials worn by ITEMS (garments,
// armour, hafts), not for body materials: skin's list is mirrored by
// flesh_cooked / skin_bruised entry for entry (a grid skin cell that cooks
// keeps its nibble), so growing skin alone would cook into an unauthored
// entry. Name a body material with --only to fit it anyway.
//
// Re-run after adding outfits or characters, then rebaseline: the nibble a
// deposit writes is hashed.
//
// Distance is plain squared RGB, the same metric NearestTint uses, reported as
// its square root (0..441).

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { readVox, isArtIndex } from '../assets/editor/vox.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const argv = process.argv.slice(2);
const FIT = argv.includes('--fit');
const WRITE = argv.includes('--write');
const onlyArg = argv.find(a => a.startsWith('--only'));
const ONLY = onlyArg
  ? new Set((onlyArg.includes('=') ? onlyArg.split('=')[1]
                                   : argv[argv.indexOf(onlyArg) + 1] || '').split(','))
  : null;
const MAX_TINTS = 16;  // sim/materials.h kMatTintsMax
const DYE_REF_GREY = 179;  // round(255 * kDyeRef), game/dye.h

const matPath = path.join(root, 'assets/materials/materials.json');
const matText = fs.readFileSync(matPath, 'utf8');
const mats = JSON.parse(matText).materials;
// .vox palette index == material id == array index + 1 (id 0 is air).
const nameOfId = id => (id >= 1 && id <= mats.length ? mats[id - 1].id : `#${id}`);
const hexToRgb = h => parseInt(h.replace('#', ''), 16) & 0xFFFFFF;
const rgbToHex = c => '#' + (c & 0xFFFFFF).toString(16).padStart(6, '0');
const d2 = (a, b) => {
  const dr = ((a >> 16) & 255) - ((b >> 16) & 255);
  const dg = ((a >> 8) & 255) - ((b >> 8) & 255);
  const db = (a & 255) - (b & 255);
  return dr * dr + dg * dg + db * db;
};
const nearest = (tints, c) => {
  let best = 0, bd = Infinity;
  for (let i = 0; i < tints.length && i < MAX_TINTS; i++) {
    const d = d2(tints[i], c);
    if (d < bd) { bd = d; best = i; }
  }
  return { idx: best, d2: bd, dist: Math.sqrt(bd) };
};

function walk(dir, out) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) walk(p, out);
    else if (e.name.endsWith('.vox')) out.push(p);
  }
  return out;
}
const itemFiles = walk(path.join(root, 'assets/items'), []);
const files = [...itemFiles, ...walk(path.join(root, 'assets/mobs'), [])];

const dyeable = new Set();
for (const f of fs.readdirSync(path.join(root, 'assets/items'))) {
  if (!f.endsWith('.json') || f === 'items.json') continue;
  try {
    const j = JSON.parse(fs.readFileSync(path.join(root, 'assets/items', f), 'utf8'));
    if (j.dyeable) dyeable.add(f.replace(/\.json$/, ''));
  } catch { /* not an item sidecar */ }
}

// worn[matId] = Map(rgb -> {count, files:Set}); dyedMats = ids a dyeable
// piece is made of; itemMats = ids any item paints (the default fit set).
const worn = new Map();
const dyedMats = new Set();
const itemMats = new Set();
for (const file of files) {
  let v;
  try { v = readVox(fs.readFileSync(file)); } catch (e) {
    console.warn(`skip ${path.relative(root, file)}: ${e.message}`);
    continue;
  }
  if (!v.prefab) continue;
  const stem = path.basename(file, '.vox');
  const rel = path.relative(root, file).replace(/\\/g, '/');
  const isItem = rel.startsWith('assets/items/');
  const isDyed = isItem && dyeable.has(stem);
  for (const m of v.prefab.models) {
    const g = m.grid;
    for (let i = 0; i < g.data.length; i++) {
      const mat = g.data[i];
      if (!mat) continue;
      if (isDyed) { dyedMats.add(mat); continue; }
      const art = g.color ? g.color[i] : 0;
      if (!art || !isArtIndex(art) || !v.palette) continue;
      if (isItem) itemMats.add(mat);
      const o = (art - 1) * 4;  // RGBA entry i is palette index i+1
      const rgb = (v.palette[o] << 16) | (v.palette[o + 1] << 8) | v.palette[o + 2];
      if (!worn.has(mat)) worn.set(mat, new Map());
      const mm = worn.get(mat);
      if (!mm.has(rgb)) mm.set(rgb, { count: 0, files: new Set() });
      const e = mm.get(rgb);
      e.count++;
      e.files.add(rel);
    }
  }
}

// The dyes an NPC is rolled (session.cpp's AI-panel outfit roll), applied to
// a cell at the reference grey -- where the drawn colour IS the dye.
function hsv(h, s, v) {
  const i = Math.floor(h * 6), f = h * 6 - i;
  const p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
  const [r, g, b] = [[v, t, p], [q, v, p], [p, v, t], [p, q, v], [t, p, v], [v, p, q]][i % 6];
  const c = x => Math.round(Math.min(1, Math.max(0, x)) * 255);
  return (c(r) << 16) | (c(g) << 8) | c(b);
}
const dyeSamples = [];
for (let h = 0; h < 24; h++)
  for (const s of [0.45, 0.7, 0.95])
    for (const v of [0.35, 0.6, 0.85]) dyeSamples.push(hsv(h / 24, s, v));
void DYE_REF_GREY;

// Candidates and weights for one material: worn colours (by voxel count,
// normalised to total 1) plus, for a dyed material, the dye samples (total 1).
function pointsFor(mat) {
  const pts = [];
  const mm = worn.get(mat);
  if (mm) {
    const tot = [...mm.values()].reduce((s, e) => s + e.count, 0);
    for (const [c, e] of mm) pts.push({ c, w: e.count / tot, kind: 'worn' });
  }
  if (dyedMats.has(mat))
    for (const c of dyeSamples) pts.push({ c, w: 1 / dyeSamples.length, kind: 'dye' });
  return pts;
}
const costOf = (tints, pts) => pts.reduce((s, p) => s + p.w * nearest(tints, p.c).d2, 0);
const worstOf = (tints, pts, kind) => {
  const k = pts.filter(p => p.kind === kind);
  return k.length ? k.reduce((m, p) => Math.max(m, nearest(tints, p.c).dist), 0) : NaN;
};

// Greedy APPEND: at each step add the candidate colour that most lowers the
// weighted squared error, until 16 entries or a step buys < 2% of what is left.
function augment(tints, pts) {
  const out = tints.slice();
  while (out.length < MAX_TINTS) {
    const cur = costOf(out, pts);
    if (cur <= 0) break;
    let best = null, bestCost = cur;
    for (const p of pts) {
      if (out.includes(p.c)) continue;
      const c = costOf([...out, p.c], pts);
      if (c < bestCost) { bestCost = c; best = p.c; }
    }
    if (best === null || cur - bestCost < 0.02 * cur) break;
    out.push(best);
  }
  return out;
}

const tintedIds = mats.map((m, i) => (m.tints && m.tints.length ? i + 1 : 0)).filter(Boolean);
const ids = [...new Set([...worn.keys(), ...dyedMats])].sort((a, b) => a - b);
console.log('material          id  tinted  colours   voxels   worst(dist colour)   mean  ->tint0   dyes worst');
const proposals = [];
for (const mat of ids) {
  const def = mats[mat - 1];
  if (!def) continue;
  const tints = (def.tints || []).map(hexToRgb);
  const mm = worn.get(mat) || new Map();
  const total = [...mm.values()].reduce((s, e) => s + e.count, 0);
  let worst = 0, mean = 0, worstC = 0, tint0 = 0;
  for (const [c, e] of mm) {
    const n = nearest(tints, c);
    mean += n.dist * e.count;
    if (n.dist > worst) { worst = n.dist; worstC = c; }
    if (n.idx === 0) tint0 += e.count;
  }
  mean = total ? mean / total : 0;
  const pts = pointsFor(mat);
  const dyeWorst = dyedMats.has(mat) && tints.length ? worstOf(tints, pts, 'dye') : NaN;
  const t = tints.length > 0;
  console.log(
    `${def.id.padEnd(16)} ${String(mat).padStart(4)}  ${t ? 'yes' : ' NO'}   ` +
    `${String(mm.size).padStart(5)} ${String(total).padStart(9)}   ` +
    `${t && total ? worst.toFixed(1).padStart(6) + ' ' + rgbToHex(worstC) : '     -        '}   ` +
    `${t && total ? mean.toFixed(1).padStart(5) : '    -'}  ` +
    `${t && total ? (100 * tint0 / total).toFixed(0).padStart(3) + '%' : '   -'}   ` +
    `${Number.isNaN(dyeWorst) ? '' : dyeWorst.toFixed(1)}`);
  const wanted = ONLY ? ONLY.has(def.id) : itemMats.has(mat) || dyedMats.has(mat);
  if (FIT && t && wanted && tints.length < MAX_TINTS) {
    const aug = augment(tints, pts);
    if (aug.length > tints.length)
      proposals.push({ name: def.id, mat, before: tints, after: aug, pts });
  }
}
const untinted = ids.filter(i => !tintedIds.includes(i) && worn.has(i));
if (untinted.length)
  console.log('\nNOT TINTED (painted colour cannot survive into the grid): ' +
              untinted.map(i => `${nameOfId(i)}(${[...worn.get(i).values()].reduce((s, e) => s + e.count, 0)})`).join(', '));
const dyedUntinted = [...dyedMats].filter(i => !tintedIds.includes(i));
if (dyedUntinted.length)
  console.log('DYEABLE BUT NOT TINTED (a dyed piece of this lands undyed): ' +
              dyedUntinted.map(nameOfId).join(', '));

if (FIT) {
  console.log('\nproposed APPENDS (existing entries and their indices unchanged):');
  if (!proposals.length) console.log('  none -- every fitted material is covered');
  for (const p of proposals) {
    const f = (t, k) => (Number.isNaN(worstOf(t, p.pts, k)) ? '-' : worstOf(t, p.pts, k).toFixed(1));
    console.log(`  ${p.name.padEnd(12)} ${p.before.length} -> ${p.after.length} entries; ` +
                `worn worst ${f(p.before, 'worn')} -> ${f(p.after, 'worn')}, ` +
                `dye worst ${f(p.before, 'dye')} -> ${f(p.after, 'dye')}; + ` +
                p.after.slice(p.before.length).map(rgbToHex).join(' '));
  }
  if (WRITE && proposals.length) {
    // Textual edit of each material's `tints` array, so the file's comments,
    // key order and formatting survive (JSON.stringify would reflow it all).
    let text = matText;
    for (const p of proposals) {
      const at = text.indexOf(`"id": "${p.name}"`);
      if (at < 0) continue;
      const ts = text.indexOf('"tints": [', at);
      const nextId = text.indexOf('"id": "', at + 1);
      if (ts < 0 || (nextId >= 0 && ts > nextId)) continue;
      const te = text.indexOf(']', ts);
      const indent = text.slice(text.lastIndexOf('\n', ts) + 1, ts);
      const body = p.after.map(t => `${indent}  "${rgbToHex(t)}"`).join(',\n');
      text = text.slice(0, ts) + `"tints": [\n${body}\n${indent}]` + text.slice(te + 1);
    }
    JSON.parse(text);  // refuse to write a broken file
    fs.writeFileSync(matPath, text);
    console.log(`wrote ${path.relative(root, matPath)}`);
  }
}
