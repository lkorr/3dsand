/* ui_main.js — the DUNGEONS top-level tab of the tuner (docs/PLAN_dungeons.md
 * §5, package D4): two pages over the D2 generator library and the D1 voxel
 * server protocol.
 *
 *   Kit      the piece designer (ui_kit.js): one piece type alone, its sliders,
 *            presets, seed, sockets, props
 *   Dungeon  the stitcher (ui_stitch.js): pieces, layout assist, entities,
 *            validation, the graph, placement preview, playtest
 *
 * tuner.html loads this as its own module (own try/catch, like the Dialogue
 * tab), calls attach(section) once and activate() on every tab click.
 * window.__tunerDungeons is the seam the World map page's dungeons layer
 * ("Open in Dungeons tab") and scripts/check_dungeons.sh use.
 */
'use strict';
import {h, CSS} from './ui_core.js';
import {createKitPage} from './ui_kit.js';
import {createStitchPage} from './ui_stitch.js';

let root = null, kit = null, stitch = null, page = 'dungeon', statusEl = null, active = false;
const pages = {};

export function attach(section) {
  root = section;
  const style = h('style', {}, CSS + '\n.dg-mini{border-radius:0;font-size:10.5px;padding:1px 5px}\n.dg-field > span input{width:100%}');
  statusEl = h('span', {class: 'st'}, '');
  const btn = (k, label, title) => h('button', {'data-page': k, title, onclick: () => show(k)}, label);
  const tabs = h('div', {class: 'dg-pages'},
    btn('kit', 'Kit', 'the piece designer: one piece type alone in rock, its sliders, sockets and props'),
    btn('dungeon', 'Dungeon', 'the stitcher: pieces joined at sockets, entities, validation, placement, playtest'),
    h('span', {class: 'grow'}), statusEl);
  pages.kit = h('div', {class: 'dg-page'});
  pages.dungeon = h('div', {class: 'dg-page'});
  const box = h('div', {class: 'dg-root'}, tabs, pages.kit, pages.dungeon);
  root.replaceChildren(style, box);
  let dirtyFlag = false;
  const T = (name) => ({
    page: pages[name],
    isActive: () => active && page === name,
    setStatus: (msg, bad) => { statusEl.textContent = msg || ''; statusEl.style.color = bad ? 'var(--red)' : ''; },
    setDirty: (d) => { dirtyFlag = d; },
    presets: (k, t) => kit.presets(k, t),
    presetsChanged: (p) => { stitch.presetsChanged(p); },
    userPresets: (p) => { kit.setUserPresets(p); }
  });
  kit = createKitPage(T('kit'));
  stitch = createStitchPage(T('dungeon'));
  window.addEventListener('beforeunload', (e) => { if (dirtyFlag) { e.preventDefault(); e.returnValue = ''; } });
  // Ctrl+S on this tab saves the dungeon, not the tuner's JSON files (the
  // Models tab's bridge does the same, capture phase, before tuner.html's).
  document.addEventListener('keydown', (e) => {
    if (!(e.ctrlKey || e.metaKey) || e.key.toLowerCase() !== 's' || root.offsetParent === null) return;
    e.preventDefault(); e.stopImmediatePropagation();
    if (page === 'dungeon') stitch.save();
  }, true);
  window.__tunerDungeons = {
    activate, show, open: (id, opts) => openDungeon(id, opts), kit, stitch,
    page: () => page
  };
}

function show(k, opts) {
  page = k;
  for (const b of root.querySelectorAll('.dg-pages button[data-page]')) b.classList.toggle('on', b.dataset.page === k);
  for (const [n, el] of Object.entries(pages)) el.classList.toggle('on', n === k);
  if (k === 'kit') { stitch.deactivate(); kit.activate(); }
  else { kit.deactivate(); return stitch.activate(opts); }
}

let firstActivation = true;
export function activate() {
  active = true;
  if (!firstActivation) return show(page);
  firstActivation = false;
  const q = new URLSearchParams(location.search);
  const want = q.get('dgpage');
  const id = q.get('dungeon');
  if (want === 'kit') return show('kit');
  return show('dungeon', id ? {open: id} : undefined);
}
export function deactivate() { active = false; if (kit) kit.deactivate(); if (stitch) stitch.deactivate(); }

async function openDungeon(id, opts) {
  const b = document.querySelector('nav.tabs button[data-tab="dungeons"]');
  if (b && !active) b.click();
  active = true;
  await show('dungeon', Object.assign({open: id}, opts || {}));
}
