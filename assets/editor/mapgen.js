/* mapgen.js — THE GENERATE PANEL of the World map page
 * (docs/PLAN_worldbuilder.md K1 item 5): whole-map or Area generators.
 *
 *   TEMPLATES   continent / island / archipelago / peninsula / highlands
 *               (terraingen.TEMPLATES, Azgaar-style op lists: fill, hill, pit,
 *               range, trough, strait, mask, noise, erode, ...) with a seed;
 *               the op list is editable JSON. Over the whole map, or the Area
 *               (blended into the land round it).
 *   LANDFORMS   crater, volcano, canyon, glacial valley, fjord, mesa field,
 *               dunes, placed by the Area (its centre and size; a canyon /
 *               valley / fjord runs along its long side).
 *
 * Preview draws a coarse run over the map (nothing written). Generate /
 * Apply runs the full job in the worker (progress bar under the toolbar) and
 * puts the result IN THE PLANE as a pending preview: Commit keeps it (one
 * undo step), Discard puts the posts back, the next edit commits it. A
 * committed template records itself in map.json `gen` (editor-only:
 * {template, seed, ops, box}) so it can be re-run. */

import * as TG from './terraingen.js';

export const LANDFORMS = {
  crater:  {label: 'crater', params: [['radiusM', 'radius', 1200, 'm'], ['depthM', 'depth', 180, 'm'], ['rimM', 'rim', 60, 'm']]},
  volcano: {label: 'volcano', params: [['radiusM', 'radius', 3000, 'm'], ['heightM', 'height', 900, 'm'], ['craterPct', 'crater', 12, '%']]},
  canyon:  {label: 'canyon', params: [['widthM', 'half-width', 500, 'm'], ['depthM', 'depth', 250, 'm'], ['steps', 'terraces', 4, ''], ['meanderPct', 'meander', 60, '%'], ['sidePct', 'side canyons', 60, '%']]},
  glacial: {label: 'glacial valley', params: [['widthM', 'half-width', 900, 'm'], ['depthM', 'depth', 350, 'm'], ['meanderPct', 'meander', 35, '%']]},
  fjord:   {label: 'fjord', params: [['widthM', 'half-width', 700, 'm'], ['depthM', 'depth below sea', 120, 'm'], ['meanderPct', 'meander', 35, '%']]},
  mesas:   {label: 'mesa field', params: [['heightM', 'height', 160, 'm'], ['sizeM', 'size', 400, 'm'], ['densityPct', 'density', 55, '%'], ['steps', 'terraces', 2, '']]},
  dunes:   {label: 'dunes', params: [['heightM', 'height', 18, 'm'], ['lengthM', 'wavelength', 220, 'm'], ['windDeg', 'wind toward', 20, '°']]},
};

export function createGenLayer(C) {
  const st = {template: 'continent', seed: 1, ops: null, opsText: '', kind: 'canyon', vals: {}, busy: null};
  for (const [k, L] of Object.entries(LANDFORMS)) st.vals[k] = Object.fromEntries(L.params.map(p => [p[0], p[2]]));
  const els = {};
  const v = () => C.vpm(), sea = () => C.seaY();
  const opsOf = () => {
    if (st.ops) return st.ops;
    return TG.TEMPLATES[st.template];
  };
  /** The world box the generator writes: the Area, or the whole extent. */
  function targetBox() {
    const a = C.area();
    if (a) return a;
    const e = C.map().json.extent;
    return [e.min[0], e.min[1], e.min[0] + e.size[0], e.min[1] + e.size[1]];
  }
  /** Feather a generated window into the land round it (an Area target). */
  function feather(h, old, w, hh) {
    const fe = Math.max(3, Math.round(Math.min(w, hh) * 0.06));
    for (let j = 0; j < hh; j++)
      for (let i = 0; i < w; i++) {
        const e = Math.min(i, j, w - 1 - i, hh - 1 - j);
        if (e >= fe) continue;
        const n = j * w + i, t = e / fe, s = t * t * (3 - 2 * t);
        h[n] = old[n] + (h[n] - old[n]) * s;
      }
  }
  /* ---- templates ---- */
  async function template(preview) {
    if (!C.map()) return;
    if (C.jobBusy() && !preview) { C.toast('another terrain job is running', true); return; }
    let ops;
    try { ops = st.opsText ? JSON.parse(st.opsText) : opsOf(); if (!Array.isArray(ops)) throw new Error('the op list must be a JSON array'); }
    catch (e) { C.toast('template ops: ' + e.message, true); return; }
    const whole = !C.area(), box = targetBox();
    const g = C.postsGrid(whole ? null : box);
    if (!g) return;
    if (!preview && C.rectBlocked(g.rect)) { C.toast('tiles are still loading; try again in a moment', true); return; }
    // an Area target starts from the land there (and blends back into it); the whole map starts empty
    if (!whole) g.grid.h = C.readPosts(...g.rect);
    const job = {job: 'template', ops, seed: st.seed | 0, grid: g.grid, seaY: sea(), vpm: v(), preview};
    const old = whole ? null : Float32Array.from(g.grid.h);
    const label = `${st.ops || st.opsText ? 'edited ' : ''}${st.template} (seed ${st.seed})`;
    let run;
    try { run = C.runJob(job, preview ? {quiet: true, key: 'gen-preview'} : {label: 'generating ' + label + (whole ? ' over the whole map' : ' in the Area')}); }
    catch (e) { C.toast(e.message, true); return; }
    try {
      const res = await run;
      if (run.cancelled) return;
      if (preview) { C.setPreview(res, 'template ' + label, 'gen'); return; }
      if (old) feather(res.h, old, g.grid.w, g.grid.hh);
      const snap = C.snapshot();
      C.map().json.gen = {template: st.template, seed: st.seed | 0, ops: st.opsText ? ops : undefined, box: whole ? null : box.map(Math.round)};
      C.markDirty(true);
      if (C.preview() && C.preview().owner === 'gen') C.setPreview(null);
      const r = C.writePending(g.rect, res.h, null, 'template ' + label, snap.json);
      C.status(`generated ${label}: ${r.changed} posts (proxy ${(res.factor * C.map().H.sp / v()).toFixed(1)} m) — Commit keeps it (one undo step), Discard puts the land back`);
    } catch (e) { if (!run.cancelled) C.toast('generate: ' + e.message, true); }
    sync();
  }
  /* ---- landforms ---- */
  function landformJob() {
    const a = C.area();
    if (!a) { C.toast('draw an Area first (the Area tool): a landform is placed by it', true); return null; }
    const P = st.vals[st.kind], m = v(), cx = (a[0] + a[2]) / 2, cz = (a[1] + a[3]) / 2;
    let params, box;
    switch (st.kind) {
      case 'crater': { const R = P.radiusM * m; params = {x: cx, z: cz, radius: R, depth: P.depthM * m, rim: P.rimM * m}; box = [cx - R * 2.5, cz - R * 2.5, cx + R * 2.5, cz + R * 2.5]; break; }
      case 'volcano': { const R = P.radiusM * m; params = {x: cx, z: cz, radius: R, height: P.heightM * m, crater: P.craterPct / 100}; box = [cx - R * 1.05, cz - R * 1.05, cx + R * 1.05, cz + R * 1.05]; break; }
      case 'canyon': params = {box: a, width: P.widthM * m, depth: P.depthM * m, steps: P.steps | 0, meander: P.meanderPct / 100, side: P.sidePct / 100}; break;
      case 'glacial': params = {box: a, width: P.widthM * m, depth: P.depthM * m, meander: P.meanderPct / 100}; break;
      case 'fjord': params = {box: a, width: P.widthM * m, depth: P.depthM * m, meander: P.meanderPct / 100}; break;
      case 'mesas': params = {box: a, height: P.heightM * m, size: P.sizeM * m, density: P.densityPct / 100, steps: P.steps | 0}; break;
      case 'dunes': params = {box: a, height: P.heightM * m, wavelength: P.lengthM * m, wind: P.windDeg}; break;
      default: return null;
    }
    if (!box) { const pad = (params.width || 0) * 2; box = [a[0] - pad, a[1] - pad, a[2] + pad, a[3] + pad]; }
    params.seed = st.seed | 0;
    return {params, box};
  }
  async function landform(preview) {
    if (!C.map()) return;
    if (C.jobBusy() && !preview) { C.toast('another terrain job is running', true); return; }
    const lj = landformJob();
    if (!lj) return;
    const g = C.postsGrid(lj.box);
    if (!g) return;
    if (!preview && C.rectBlocked(g.rect)) { C.toast('tiles are still loading; try again in a moment', true); return; }
    g.grid.h = C.readPosts(...g.rect);
    const label = LANDFORMS[st.kind].label;
    let run;
    try { run = C.runJob({job: 'primitive', kind: st.kind, params: lj.params, grid: g.grid, seaY: sea(), preview}, preview ? {quiet: true, key: 'gen-preview'} : {label: 'carving a ' + label}); }
    catch (e) { C.toast(e.message, true); return; }
    try {
      const res = await run;
      if (run.cancelled) return;
      if (preview) { C.setPreview(res, label, 'gen'); return; }
      if (C.preview() && C.preview().owner === 'gen') C.setPreview(null);
      const r = C.writePending(g.rect, res.h, null, label);
      C.status(`${label}: ${r.changed} posts — Commit keeps it (one undo step), Discard puts the land back`);
    } catch (e) { if (!run.cancelled) C.toast(label + ': ' + e.message, true); }
    sync();
  }
  /* ---- the panel ---- */
  function buildPanel() {
    const el = C.el;
    const det = el('details', {class: 'maptools', open: false});
    det.append(el('summary', {}, 'Generate — continents, islands and landforms (the whole map or the Area)'));
    els.target = el('div', {class: 'mapnote'});
    // templates
    els.tpl = el('select', {title: 'an Azgaar-style op list: the shape of the land it makes'}, ...TG.TEMPLATE_NAMES.map(n => el('option', {value: n}, n)));
    els.tpl.value = st.template;
    els.tpl.addEventListener('change', () => { st.template = els.tpl.value; st.ops = null; st.opsText = ''; sync(); });
    els.seed = el('input', {type: 'number', min: 0, max: 999999, step: 1, value: st.seed, style: 'width:70px', title: 'the seed: the same template + seed = the same land'});
    els.seed.addEventListener('change', () => { st.seed = Math.max(0, Math.round(+els.seed.value || 0)); els.seed.value = st.seed; });
    const reroll = el('button', {title: 'a new seed (then Preview)'}, 'reroll');
    reroll.addEventListener('click', () => { st.seed = (st.seed * 7919 + 13) % 100000; els.seed.value = st.seed; template(true); });
    const pv = el('button', {title: 'a coarse run drawn over the map; nothing is written'}, 'Preview');
    pv.addEventListener('click', () => template(true));
    els.go = el('button', {class: 'mapgo', title: 'run it in full (a worker), then hold it in the plane: Commit / Discard below'}, 'Generate');
    els.go.addEventListener('click', () => template(false));
    els.ops = el('textarea', {rows: 7, spellcheck: false, style: 'width:100%;font:10px monospace;background:#0e1219;color:#c7d2e3;border:1px solid #2a3040;border-radius:3px',
                              title: 'the op list (JSON). Ops: ' + TG.TEMPLATE_OPS.join(', ') + '. Coordinates are fractions of the target (0..1), heights metres above sea, sizes fractions of its larger side.'});
    els.ops.addEventListener('change', () => { st.opsText = els.ops.value.trim(); });
    const resetOps = el('button', {title: 'back to the template’s own op list'}, 'reset ops');
    resetOps.addEventListener('click', () => { st.ops = null; st.opsText = ''; sync(); });
    // landforms
    els.kind = el('select', {}, ...Object.entries(LANDFORMS).map(([k, L]) => el('option', {value: k}, L.label)));
    els.kind.value = st.kind;
    els.kind.addEventListener('change', () => { st.kind = els.kind.value; sync(); });
    els.params = el('div', {class: 'mapgrid2'});
    const lpv = el('button', {title: 'a coarse run drawn over the map; nothing is written'}, 'Preview');
    lpv.addEventListener('click', () => landform(true));
    els.apply = el('button', {class: 'mapgo', title: 'carve / raise it into the plane, held as a preview: Commit / Discard below'}, 'Apply');
    els.apply.addEventListener('click', () => landform(false));
    // the pending result
    els.pend = el('span', {class: 'mapnote'});
    els.commit = el('button', {title: 'keep it (one undo step)'}, 'Commit');
    els.commit.addEventListener('click', () => { C.commitPending(); C.paint(); });
    els.discard = el('button', {title: 'put the land back as it was'}, 'Discard');
    els.discard.addEventListener('click', () => C.discardPending());
    const clr = el('button', {title: 'hide the preview drawn over the map'}, 'hide preview');
    clr.addEventListener('click', () => C.setPreview(null));
    det.append(els.target,
      el('div', {class: 'mapsub'}, 'Template (Azgaar-style op list)'),
      el('div', {class: 'mapbar'}, els.tpl, ' seed ', els.seed, reroll),
      els.ops, el('div', {class: 'mapbar'}, pv, els.go, resetOps),
      el('div', {class: 'mapsub'}, 'Landform (placed by the Area)'),
      el('div', {class: 'mapbar'}, els.kind), els.params, el('div', {class: 'mapbar'}, lpv, els.apply),
      el('div', {class: 'mapsub'}, 'Result'),
      el('div', {class: 'mapbar'}, els.commit, els.discard, clr, els.pend));
    sync();
    return det;
  }
  function sync() {
    if (!els.target || !C.map()) return;
    const el = C.el, a = C.area();
    els.target.textContent = a ? `target: the Area, ${C.fmtM((a[2] - a[0]) / v())} × ${C.fmtM((a[3] - a[1]) / v())} (a template blends back into the land round it)`
                               : 'target: the WHOLE MAP (draw an Area to generate into a box; landforms need one)';
    if (document.activeElement !== els.ops) els.ops.value = st.opsText || JSON.stringify(opsOf(), null, 0).replace(/\},\{/g, '},\n{');
    els.params.replaceChildren();
    for (const [key, label, , unit] of LANDFORMS[st.kind].params) {
      const i = el('input', {type: 'number', step: 'any', value: st.vals[st.kind][key], style: 'width:64px'});
      i.addEventListener('change', () => { st.vals[st.kind][key] = +i.value || 0; });
      els.params.append(el('label', {}, label + ' ', i, el('span', {class: 'mapunit'}, unit)));
    }
    els.apply.disabled = !a;
    const p = C.pending();
    els.commit.disabled = els.discard.disabled = !p;
    els.pend.textContent = p ? (p.label || 'result') + ' is in the plane, not yet kept' : '';
  }
  return {st, buildPanel, sync, template, landform, reset() { st.ops = null; st.opsText = ''; sync(); }};
}
