/* map.js — the World map page of the Environment tab: paint WHERE the biomes
 * are, shape the landform, and place the harness pad, on the map worldgen
 * reads (assets/worldmap/<name>/{map.json,map.svmap}; src/sim/worldmap.h is
 * the format's authority).
 *
 * WHAT THIS IS. Tier A of docs/PLAN_world_map.md — the seed-INDEPENDENT
 * layout. A cell is 2^cellLog2 voxels (1024 = 102.4 m by default); the plane
 * is `size` cells with `originCell` at world (0,0). The biome plane holds
 * indices into `biomes[]` (the palette, by NAME); the landform plane is 0..255
 * (0 = ocean floor, 255 = alpine) and owns the height from P4; moisture is
 * reserved. Sites are a list; until P5's site table the engine reads one
 * `kind: "pad"` box — the selftest harness region.
 *
 * WHAT IT IS NOT. Not a preview of the generated world: the Worldgen tab's
 * heightmap and voxel views are. This page draws the PLANES, plainly, so you
 * see what you painted and not what noise made of it. Every save moves the
 * world hash (the map is an input beside the seed); the engine reads the map
 * at boot, so regenerate the world (or restart) to see a change.
 *
 * Painting model: a circular brush in CELL space, left-drag paints, right- or
 * middle-drag pans, wheel zooms about the cursor, Shift+drag with the pad tool
 * draws the harness box in world voxels. Undo is a stroke stack of plane
 * snapshots (115 KB each, capped) — the same coalescing-per-drag rule
 * envui.makeUndo uses for rows, done by hand because the target is a
 * Uint8Array rather than a JSON path.
 */

const MAGIC = 0x504D5653;   // 'SVMP'
const VERSION = 1;

// Display colours by biome NAME; anything unknown gets a stable hash colour.
const COLOR = {
  forest: '#2f7a3a', meadow: '#8cc95a', pine: '#1f5a3a', desert: '#d9c27a',
  tundra: '#d8e6f0', swamp: '#6a7a2f', alpine: '#8f959c', ocean: '#2b5d9e'
};
function colorOf(name) {
  if (COLOR[name]) return COLOR[name];
  let h = 0;
  for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return `hsl(${h % 360} 45% 55%)`;
}

let H = null;
let els = {};
let map = null;            // {name, json, biome:Uint8Array, landform:Uint8Array, moisture:Uint8Array}
let dirty = false;
let tool = 'biome';        // biome | landform | pad
let brush = {index: 0, radius: 2, landform: 128, soft: true};
let view = {cx: 0, cz: 0, scale: 4};   // cell-space centre + pixels per cell
let showLandform = true;
let undo = [];
let redo = [];
let stroke = null;         // snapshot taken at pointerdown, pushed at pointerup if anything changed
let padDrag = null;
let img = null;            // ImageData of the planes

/* ---- helpers ---------------------------------------------------------------- */
function toast(m, bad) { if (H && H.toast) H.toast(m, bad); }
function markDirty(d = true) {
  dirty = d;
  if (H && H.onDirty) H.onDirty(d);
  if (els.save) els.save.disabled = !d;
}
function cellOfWorld(x, z) {
  const j = map.json;
  return [(x >> j.cellLog2) + j.originCell[0], (z >> j.cellLog2) + j.originCell[1]];
}
function worldOfCell(cx, cz) {
  const j = map.json;
  return [(cx - j.originCell[0]) << j.cellLog2, (cz - j.originCell[1]) << j.cellLog2];
}
function snapshot() {
  return {biome: new Uint8Array(map.biome), landform: new Uint8Array(map.landform),
          sites: JSON.parse(JSON.stringify(map.json.sites || []))};
}
function restore(s) {
  map.biome.set(s.biome); map.landform.set(s.landform);
  map.json.sites = JSON.parse(JSON.stringify(s.sites));
}

/* ---- planes <-> bytes -------------------------------------------------------- */
export function parsePlanes(buf, w, h) {
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== MAGIC) throw new Error('not an SVMP file');
  if (dv.getUint32(4, true) !== VERSION) throw new Error('unknown SVMP version');
  const fw = dv.getUint32(8, true), fh = dv.getUint32(12, true);
  if (fw !== w || fh !== h) throw new Error(`planes are ${fw}x${fh}, map.json says ${w}x${h}`);
  const n = w * h;
  if (buf.byteLength < 16 + n * 3) throw new Error('SVMP truncated');
  const u8 = new Uint8Array(buf);
  return {biome: u8.slice(16, 16 + n), landform: u8.slice(16 + n, 16 + 2 * n), moisture: u8.slice(16 + 2 * n, 16 + 3 * n)};
}
export function serializePlanes(w, h, biome, landform, moisture) {
  const n = w * h;
  const out = new Uint8Array(16 + n * 3);
  const dv = new DataView(out.buffer);
  dv.setUint32(0, MAGIC, true); dv.setUint32(4, VERSION, true);
  dv.setUint32(8, w, true); dv.setUint32(12, h, true);
  out.set(biome, 16); out.set(landform, 16 + n); out.set(moisture, 16 + 2 * n);
  return out;
}

/* ---- server ------------------------------------------------------------------ */
async function listMaps() {
  const r = await fetch('/api/worldmaps', {cache: 'no-store'});
  const j = await r.json();
  return (j.maps || []).map(m => m.name);
}
async function loadMap(name) {
  const rj = await fetch('/api/worldmap?name=' + encodeURIComponent(name), {cache: 'no-store'});
  if (!rj.ok) throw new Error('map.json: HTTP ' + rj.status);
  const json = await rj.json();
  const rp = await fetch('/api/worldmap/planes?name=' + encodeURIComponent(name), {cache: 'no-store'});
  if (!rp.ok) throw new Error('map.svmap: HTTP ' + rp.status);
  const planes = parsePlanes(await rp.arrayBuffer(), json.size[0], json.size[1]);
  map = {name, json, ...planes};
  undo = []; redo = [];
  markDirty(false);
  rebuildPalette();
  fit();
  paint();
  status(`loaded worldmap/${name}: ${json.size[0]}x${json.size[1]} cells of ${1 << json.cellLog2} vox (${(json.size[0] * (1 << json.cellLog2) / 10 / 1000).toFixed(1)} km)`);
}
async function saveMap() {
  if (!map) return;
  const name = map.name;
  const body = JSON.stringify(map.json, null, 2) + '\n';
  let r = await fetch('/api/worldmap?name=' + encodeURIComponent(name), {method: 'POST', body});
  let j = await r.json();
  if (!j.ok) { toast('save failed: ' + (j.error || '?'), true); return; }
  const blob = serializePlanes(map.json.size[0], map.json.size[1], map.biome, map.landform, map.moisture);
  r = await fetch('/api/worldmap/planes?name=' + encodeURIComponent(name), {method: 'POST', body: blob});
  j = await r.json();
  if (!j.ok) { toast('planes save failed: ' + (j.error || '?'), true); return; }
  markDirty(false);
  toast(`saved worldmap/${name} — regenerate the world to see it (every map edit moves the world hash)`);
}

/* ---- rendering --------------------------------------------------------------- */
function rebuildImage() {
  const [w, h] = map.json.size;
  if (!img || img.width !== w || img.height !== h) img = new ImageData(w, h);
  const d = img.data;
  const pal = map.json.biomes.map(n => {
    const c = colorOf(n);
    const tmp = document.createElement('canvas').getContext('2d');
    tmp.fillStyle = c; tmp.fillRect(0, 0, 1, 1);
    return tmp.getImageData(0, 0, 1, 1).data;
  });
  for (let i = 0; i < w * h; i++) {
    const p = pal[map.biome[i]] || pal[0];
    let k = 1;
    if (showLandform) k = 0.55 + 0.9 * (map.landform[i] / 255);
    d[i * 4] = Math.min(255, p[0] * k);
    d[i * 4 + 1] = Math.min(255, p[1] * k);
    d[i * 4 + 2] = Math.min(255, p[2] * k);
    d[i * 4 + 3] = 255;
  }
}
function paint() {
  if (!map || !els.canvas) return;
  const c = els.canvas, ctx = c.getContext('2d');
  const W = c.clientWidth, Hh = c.clientHeight;
  if (c.width !== W || c.height !== Hh) { c.width = W; c.height = Hh; }
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.fillStyle = '#0e1219'; ctx.fillRect(0, 0, W, Hh);
  rebuildImage();
  // planes -> screen: screen = (cell - view.c) * scale + centre
  const [w, h] = map.json.size;
  const ox = W / 2 - view.cx * view.scale, oy = Hh / 2 - view.cz * view.scale;
  const off = els.off || (els.off = document.createElement('canvas'));
  if (off.width !== w || off.height !== h) { off.width = w; off.height = h; }
  off.getContext('2d').putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(off, ox, oy, w * view.scale, h * view.scale);
  // origin crosshair
  const [ocx, ocz] = map.json.originCell;
  ctx.strokeStyle = '#ffd866'; ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(ox + ocx * view.scale - 8, oy + ocz * view.scale); ctx.lineTo(ox + ocx * view.scale + 8, oy + ocz * view.scale);
  ctx.moveTo(ox + ocx * view.scale, oy + ocz * view.scale - 8); ctx.lineTo(ox + ocx * view.scale, oy + ocz * view.scale + 8);
  ctx.stroke();
  // sites: the pad box in world voxels -> cells (fractional)
  for (const s of (map.json.sites || [])) {
    if (s.kind !== 'pad') continue;
    const l = map.json.cellLog2, cv = 1 << l;
    const x0 = (s.min[0] / cv) + ocx, z0 = (s.min[1] / cv) + ocz;
    const x1 = ((s.max[0] + 1) / cv) + ocx, z1 = ((s.max[1] + 1) / cv) + ocz;
    ctx.strokeStyle = tool === 'pad' ? '#ff7a7a' : '#ffb454'; ctx.lineWidth = 2;
    ctx.strokeRect(ox + x0 * view.scale, oy + z0 * view.scale, (x1 - x0) * view.scale, (z1 - z0) * view.scale);
    ctx.fillStyle = '#ffb454'; ctx.font = '11px monospace';
    ctx.fillText(s.id || 'pad', ox + x0 * view.scale + 3, oy + z0 * view.scale - 3);
  }
  // stamp sites: a marker at the centre with the footprint radius
  for (const s of (map.json.sites || [])) {
    if (s.kind !== 'stamp') continue;
    const l = map.json.cellLog2, cv = 1 << l;
    const sx = ox + (s.x / cv + ocx) * view.scale, sz = oy + (s.z / cv + ocz) * view.scale;
    const rr = Math.max(3, ((s.radius || 16) / cv) * view.scale);
    ctx.strokeStyle = tool === 'stamp' ? '#ff7a7a' : '#7fd4ff'; ctx.lineWidth = 2;
    ctx.strokeRect(sx - rr, sz - rr, rr * 2, rr * 2);
    ctx.fillStyle = '#7fd4ff'; ctx.font = '11px monospace';
    ctx.fillText((s.id || 'site') + ' (' + (s.template || '?') + (s.rot ? ' r' + s.rot : '') + ')', sx + rr + 3, sz + 4);
  }
  // brush cursor
  if (els.hover && tool !== 'pad' && tool !== 'stamp') {
    ctx.strokeStyle = 'rgba(255,255,255,0.7)'; ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.arc(ox + (els.hover[0] + 0.5) * view.scale, oy + (els.hover[1] + 0.5) * view.scale,
            (brush.radius + 0.5) * view.scale, 0, Math.PI * 2);
    ctx.stroke();
  }
}
function fit() {
  if (!map || !els.canvas) return;
  const [w, h] = map.json.size;
  view.cx = w / 2; view.cz = h / 2;
  view.scale = Math.max(1, Math.floor(Math.min(els.canvas.clientWidth / w, els.canvas.clientHeight / h)));
}
function status(t) { if (els.status) els.status.textContent = t; }

/* ---- input --------------------------------------------------------------------- */
function cellAt(ev) {
  const r = els.canvas.getBoundingClientRect();
  const W = els.canvas.clientWidth, Hh = els.canvas.clientHeight;
  const px = ev.clientX - r.left, py = ev.clientY - r.top;
  return [Math.floor((px - W / 2) / view.scale + view.cx), Math.floor((py - Hh / 2) / view.scale + view.cz)];
}
function applyBrush(cx, cz) {
  const [w, h] = map.json.size;
  const r = brush.radius;
  let changed = false;
  for (let dz = -r; dz <= r; dz++)
    for (let dx = -r; dx <= r; dx++) {
      if (dx * dx + dz * dz > r * r + r * 0.5) continue;
      const x = cx + dx, z = cz + dz;
      if (x < 0 || z < 0 || x >= w || z >= h) continue;
      const i = z * w + x;
      if (tool === 'biome') {
        if (map.biome[i] !== brush.index) { map.biome[i] = brush.index; changed = true; }
      } else if (tool === 'landform') {
        // soft brush: move the value a third of the way per pass so a held
        // stroke ramps rather than stamps a plateau
        const cur = map.landform[i];
        const next = brush.soft ? Math.round(cur + (brush.landform - cur) * 0.34) : brush.landform;
        if (next !== cur) { map.landform[i] = next; changed = true; }
      }
    }
  return changed;
}
function wire() {
  const c = els.canvas;
  let panning = null;
  c.addEventListener('contextmenu', e => e.preventDefault());
  c.addEventListener('pointerdown', ev => {
    if (!map) return;
    c.setPointerCapture(ev.pointerId);
    if (ev.button === 1 || ev.button === 2) { panning = {x: ev.clientX, y: ev.clientY, cx: view.cx, cz: view.cz}; return; }
    if (tool === 'pad') {
      const [cx, cz] = cellAt(ev);
      const [wx, wz] = worldOfCell(cx, cz);
      stroke = snapshot();
      padDrag = {x0: wx, z0: wz};
      return;
    }
    if (tool === 'stamp') {
      // Click: a new stamp site at the cursor's world column (cell centre if
      // zoomed out, the exact column when zoomed in). Shift+click on an
      // existing marker deletes it. Sites are hand-placed here; rules
      // (seeded placement per biome) are P5b.
      const r = els.canvas.getBoundingClientRect();
      const W = els.canvas.clientWidth, Hh = els.canvas.clientHeight;
      const fx = (ev.clientX - r.left - W / 2) / view.scale + view.cx;
      const fz = (ev.clientY - r.top - Hh / 2) / view.scale + view.cz;
      const cv = 1 << map.json.cellLog2;
      const wx = Math.round((fx - map.json.originCell[0]) * cv);
      const wz = Math.round((fz - map.json.originCell[1]) * cv);
      const sites = map.json.sites || (map.json.sites = []);
      const hit = sites.find(s => s.kind === 'stamp' && Math.abs(s.x - wx) <= (s.radius || 16) && Math.abs(s.z - wz) <= (s.radius || 16));
      stroke = snapshot();
      if (ev.shiftKey && hit) {
        sites.splice(sites.indexOf(hit), 1);
        stroke.changed = true;
      } else if (!hit) {
        const t = prompt('Template: assets/prefabs/<name>.vox', els.lastTemplate || '');
        if (!t) { stroke = null; return; }
        els.lastTemplate = t;
        const rot = parseInt(prompt('Rotation (0..3 quarter turns):', '0') || '0', 10) & 3;
        sites.push({id: t + '_' + sites.length, kind: 'stamp', template: t, x: wx, z: wz, rot, padMargin: 8, salt: sites.length});
        stroke.changed = true;
      }
      paint();
      return;
    }
    stroke = snapshot();
    stroke.changed = applyBrush(...cellAt(ev));
    paint();
  });
  c.addEventListener('pointermove', ev => {
    if (!map) return;
    const cell = cellAt(ev);
    els.hover = cell;
    if (panning) {
      view.cx = panning.cx - (ev.clientX - panning.x) / view.scale;
      view.cz = panning.cz - (ev.clientY - panning.y) / view.scale;
    } else if (padDrag) {
      const [wx, wz] = worldOfCell(cell[0] + 1, cell[1] + 1);
      const s = (map.json.sites || []).find(s => s.kind === 'pad');
      const box = {min: [Math.min(padDrag.x0, wx), Math.min(padDrag.z0, wz)],
                   max: [Math.max(padDrag.x0, wx) - 1, Math.max(padDrag.z0, wz) - 1]};
      if (s) { s.min = box.min; s.max = box.max; }
      else map.json.sites = [...(map.json.sites || []), {id: 'harness', kind: 'pad', ...box}];
      stroke.changed = true;
    } else if (stroke && ev.buttons & 1) {
      if (applyBrush(...cell)) stroke.changed = true;
    }
    const [wx, wz] = worldOfCell(cell[0], cell[1]);
    const [w, h] = map.json.size;
    const inside = cell[0] >= 0 && cell[1] >= 0 && cell[0] < w && cell[1] < h;
    const i = cell[1] * w + cell[0];
    status(inside
      ? `cell (${cell[0]},${cell[1]})  world x ${wx}..${wx + (1 << map.json.cellLog2) - 1}  z ${wz}..  biome ${map.json.biomes[map.biome[i]]}  landform ${map.landform[i]}`
      : `outside the painted planes (ocean)`);
    paint();
  });
  const up = () => {
    panning = null;
    padDrag = null;
    if (stroke) {
      if (stroke.changed) { undo.push(stroke); if (undo.length > 40) undo.shift(); redo = []; markDirty(true); }
      stroke = null;
    }
    paint();
  };
  c.addEventListener('pointerup', up);
  c.addEventListener('pointercancel', up);
  c.addEventListener('pointerleave', () => { els.hover = null; paint(); });
  c.addEventListener('wheel', ev => {
    if (!map) return;
    ev.preventDefault();
    const [cx, cz] = cellAt(ev);
    const f = ev.deltaY < 0 ? 1.25 : 0.8;
    const ns = Math.max(0.5, Math.min(64, view.scale * f));
    // zoom about the cursor: keep the cell under it fixed
    view.cx = cx + (view.cx - cx) * (view.scale / ns);
    view.cz = cz + (view.cz - cz) * (view.scale / ns);
    view.scale = ns;
    paint();
  }, {passive: false});
  window.addEventListener('keydown', ev => {
    if (!H.isVisible() || !map) return;
    const inField = /input|select|textarea/i.test(document.activeElement && document.activeElement.tagName);
    if (inField) return;
    if ((ev.ctrlKey || ev.metaKey) && ev.key === 'z') { ev.preventDefault(); doUndo(); }
    else if ((ev.ctrlKey || ev.metaKey) && ev.key === 'y') { ev.preventDefault(); doRedo(); }
    else if (ev.key === '[') { brush.radius = Math.max(0, brush.radius - 1); syncControls(); paint(); }
    else if (ev.key === ']') { brush.radius = Math.min(24, brush.radius + 1); syncControls(); paint(); }
  });
}
function doUndo() {
  if (!undo.length) return;
  redo.push(snapshot());
  restore(undo.pop());
  markDirty(true); paint();
}
function doRedo() {
  if (!redo.length) return;
  undo.push(snapshot());
  restore(redo.pop());
  markDirty(true); paint();
}

/* ---- UI --------------------------------------------------------------------------- */
function rebuildPalette() {
  const el = H.el;
  els.palette.replaceChildren();
  map.json.biomes.forEach((name, i) => {
    const b = el('button', {class: 'mapchip' + (i === brush.index ? ' on' : ''), title: `paint ${name} (index ${i})`},
      el('span', {class: 'sw', style: `background:${colorOf(name)}`}), name);
    b.addEventListener('click', () => { brush.index = i; tool = 'biome'; syncControls(); rebuildPalette(); paint(); });
    els.palette.append(b);
  });
}
function syncControls() {
  for (const [k, b] of Object.entries(els.tools)) b.classList.toggle('on', k === tool);
  els.radius.value = brush.radius; els.radiusOut.textContent = brush.radius;
  els.land.value = brush.landform; els.landOut.textContent = brush.landform;
}
export function attach(hooks) {
  H = hooks;
  const root = H.section;
  if (!root) return;
  const el = H.el;
  const style = document.createElement('style');
  style.textContent = `
#env-map.active{display:flex;flex-direction:column;gap:6px;height:100%}
#env-map .mapbar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;font-size:12px}
#env-map .mapbar button.on{background:#1f2a3d;border-color:#2b4a6f;color:#fff}
#env-map .mapchip{display:inline-flex;align-items:center;gap:5px;padding:3px 7px;border:1px solid #2a3040;border-radius:4px;background:#12161f;color:#c7d2e3;cursor:pointer;font:11px monospace}
#env-map .mapchip.on{border-color:#ffd866;color:#fff}
#env-map .mapchip .sw{width:12px;height:12px;border-radius:2px;display:inline-block}
#env-map .mapcanvas{flex:1;min-height:320px;width:100%;background:#0e1219;border:1px solid #2a3040;border-radius:6px;cursor:crosshair;touch-action:none}
#env-map .mapstatus{font:11px monospace;color:#8fa0b8;min-height:14px}
#env-map .mapnote{font:10px/1.4 monospace;color:#6f7f97}
`;
  document.head.append(style);

  els.canvas = el('canvas', {class: 'mapcanvas'});
  els.status = el('div', {class: 'mapstatus'}, 'no map loaded');
  els.palette = el('div', {class: 'mapbar'});
  els.mapSel = el('select', {});
  els.save = el('button', {disabled: true}, 'Save map');
  els.save.addEventListener('click', saveMap);
  els.tools = {
    biome: el('button', {title: 'paint the selected biome (left-drag)'}, 'Biome brush'),
    landform: el('button', {title: 'paint landform 0..255 (left-drag)'}, 'Landform brush'),
    pad: el('button', {title: 'drag the harness pad box (world voxels)'}, 'Pad box'),
    stamp: el('button', {title: 'click: place a stamp site (a .vox from assets/prefabs/); shift+click a marker: delete it'}, 'Stamp site'),
  };
  for (const [k, b] of Object.entries(els.tools)) b.addEventListener('click', () => { tool = k; syncControls(); paint(); });
  els.radius = el('input', {type: 'range', min: 0, max: 24, value: brush.radius, style: 'width:110px'});
  els.radiusOut = el('span', {}, String(brush.radius));
  els.radius.addEventListener('input', () => { brush.radius = +els.radius.value; syncControls(); paint(); });
  els.land = el('input', {type: 'range', min: 0, max: 255, value: brush.landform, style: 'width:140px'});
  els.landOut = el('span', {}, String(brush.landform));
  els.land.addEventListener('input', () => { brush.landform = +els.land.value; tool = 'landform'; syncControls(); });
  const showLf = el('input', {type: 'checkbox', checked: true});
  showLf.addEventListener('change', () => { showLandform = showLf.checked; paint(); });
  const fitBtn = el('button', {}, 'Fit');
  fitBtn.addEventListener('click', () => { fit(); paint(); });
  const undoBtn = el('button', {title: 'Ctrl+Z'}, 'Undo');
  undoBtn.addEventListener('click', doUndo);
  const redoBtn = el('button', {title: 'Ctrl+Y'}, 'Redo');
  redoBtn.addEventListener('click', doRedo);

  const bar = el('div', {class: 'mapbar'},
    el('label', {}, 'map ', els.mapSel), els.save, undoBtn, redoBtn, fitBtn,
    el('span', {style: 'width:10px'}),
    els.tools.biome, els.tools.landform, els.tools.pad, els.tools.stamp,
    el('label', {}, ' radius ', els.radius, ' ', els.radiusOut),
    el('label', {}, ' landform ', els.land, ' ', els.landOut),
    el('label', {}, ' ', showLf, ' shade by landform'));
  const note = el('div', {class: 'mapnote'},
    'Tier A: what you paint here is where the biomes ARE on every seed; the boundary warp and everything inside a region take the seed. ' +
    'Cells are 2^cellLog2 voxels (102.4 m). Right/middle-drag pans, wheel zooms, [ ] resize the brush. ' +
    'The pad box is the selftest harness region (no trunks, crowns, tarns or cover inside). ' +
    'A stamp site places assets/prefabs/<name>.vox on a levelled pad at that column (its cells keep out trees, tarns and cover); the engine refuses to start if the .vox is missing. ' +
    'Saving writes assets/worldmap/<name>/ and moves the world hash; regenerate the world to see it.');
  root.append(bar, els.palette, els.canvas, els.status, note);
  wire();
  new ResizeObserver(() => paint()).observe(els.canvas);

  els.mapSel.addEventListener('change', () => loadMap(els.mapSel.value).catch(e => toast('load failed: ' + e.message, true)));
  listMaps().then(names => {
    els.mapSel.replaceChildren(...names.map(n => el('option', {value: n}, n)));
    const want = (H.tuning && H.tuning.worldgen && H.tuning.worldgen.mapLayer) || names[0];
    if (want && names.includes(want)) els.mapSel.value = want;
    if (els.mapSel.value) return loadMap(els.mapSel.value);
    status('no maps under assets/worldmap/ — run python scripts/seed_worldmap.py');
  }).catch(e => status('map list failed: ' + e.message));
  syncControls();
}
export function isDirty() { return dirty; }
