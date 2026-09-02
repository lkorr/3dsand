/* environment.js — the Environment tab: biomes, and the components a biome is
 * built from.
 *
 * WHY ONE TAB WITH NESTED PAGES. The Trees tab was one component with one
 * editor. The moment there is a second (water bodies) and a container that
 * chooses between them (a biome), a flat row of tabs stops describing the
 * structure: a biome is not a sibling of a tree species, it is the thing that
 * SELECTS tree species. So this tab has a sidebar with two groups —
 *
 *   BIOMES       one page per assets/biomes/<name>.json: the feature stacks
 *                (cover, trees, water, caves) and a composed swatch
 *   COMPONENTS   the libraries the stacks pick from: Trees (the existing
 *                editor, unchanged, mounted here), Water bodies (new), and the
 *                engine's Caves and Ground cover knobs as they exist today
 *
 * — and the pages link to each other: a biome's tree row has "edit species →",
 * a water row has "edit preset →", and the band strip on the climate section
 * opens the biome you click.
 *
 * WHAT THIS FILE OWNS: the sidebar, page switching, dirty aggregation, Ctrl+S
 * routing, and the shared hooks each page gets. It owns no parameters.
 *
 * MOUNTING. trees.js mounts into a child DIV WITH id="view-trees" — the id its
 * CSS and its render loop key on — so the Trees editor runs here byte-for-byte
 * as it did as a top-level tab. That div does NOT carry class "view": nested
 * ".view.active" would be a second visible panel and scripts/check_tabs.sh would
 * rightly fail the page. Pages toggle their own `.active`; the host tab's
 * `.active` is mirrored onto the current page so a hidden tab renders nothing.
 */

import * as Trees from './trees.js';
import * as Water from './water.js';
import * as Biome from './biome.js';

let H = null;
let root = null;
let pages = {};
let current = '';
let dirtyBy = {};
let els = {};
let envActive = false;

const PAGE_ORDER = ['biome', 'trees', 'water', 'caves', 'cover'];
const PAGE_LABEL = {trees: 'Trees', water: 'Water bodies', caves: 'Caves', cover: 'Ground cover'};

const CSS = `
#view-environment.active{display:flex;gap:10px;height:calc(100vh - 150px);min-height:520px}
#view-environment .envnav{width:190px;flex:0 0 190px;display:flex;flex-direction:column;gap:2px;overflow-y:auto;border-right:1px solid #2a3040;padding-right:8px}
#view-environment .envnav h4{margin:10px 0 4px;font:600 10px/1 monospace;letter-spacing:.08em;color:#6f7f97;text-transform:uppercase}
#view-environment .envnav button{text-align:left;padding:5px 8px;border:1px solid transparent;background:transparent;color:#c7d2e3;border-radius:5px;cursor:pointer;font-size:12px;display:flex;justify-content:space-between;align-items:center}
#view-environment .envnav button:hover{background:#1b2130}
#view-environment .envnav button.on{background:#1f2a3d;border-color:#2b4a6f;color:#fff}
#view-environment .envnav button .pill{font:9px monospace;color:#6f7f97}
#view-environment .envnav button .pill.eng{color:#7fd48a}
#view-environment .envnav button .pill.dirty{color:#ffb454}
#view-environment .envnav .envnote{font:10px/1.4 monospace;color:#6f7f97;margin-top:auto;padding-top:10px;border-top:1px solid #2a3040}
#view-environment .envmain{flex:1;min-width:0;min-height:0;height:100%;position:relative}
#view-environment .envpage{display:none;height:100%}
#view-environment .envpage.active{display:flex}
#view-environment #view-trees.active{height:100%}
#view-environment .envknobs{display:block;overflow-y:auto;padding-right:6px}
#view-environment .envknobs .envintro{max-width:820px;font-size:12px;line-height:1.5;color:#9fb0c8;margin:0 0 10px}
#view-environment .envknobs .envintro b{color:#dbe4f0}
#view-environment .envknobs .trows{max-width:900px}
`;

/* ---------------------------------------------------------------------------
 * pages
 * ------------------------------------------------------------------------- */
function showPage(id) {
  for (const k of Object.keys(pages)) pages[k].classList.toggle('active', k === id && envActive);
  current = id;
  for (const b of els.nav.querySelectorAll('button[data-page]'))
    b.classList.toggle('on', b.dataset.page === id && (id !== 'biome' || b.dataset.name === (Biome.currentName() || '')));
  const api = apiOf(id);
  if (api && api.activate) api.activate();
}

function apiOf(id) {
  return id === 'trees' ? Trees : id === 'water' ? Water : id === 'biome' ? Biome : null;
}

/** Deep link from a page: open a biome / species / preset by name. */
async function openPage(id, name) {
  showPage(id);
  try {
    if (id === 'biome' && name) { await Biome.open(name); paintNav(); }
    else if (id === 'water' && name) await Water.open(name);
    else if (id === 'trees' && name && Trees.open) await Trees.open(name);
  } catch (e) { H.toast('could not open ' + id + '/' + name + ': ' + (e && e.message || e), true); }
}

/* ---------------------------------------------------------------------------
 * the engine-knob pages (Caves, Ground cover): the Worldgen tab's rows for one
 * subject, built by the same tuneRow so a knob has one definition.
 * ------------------------------------------------------------------------- */
function knobPage(id, intro, match) {
  const el = H.el;
  const page = pages[id];
  page.innerHTML = '';
  page.classList.add('envknobs');
  page.append(el('div', {class: 'envintro'}, ...intro));
  const T = H.tuning && H.tuning();
  const tab = T && T.schema ? T.schema.find(t => t.id === 'worldgen') : null;
  if (!tab || !T.tune) {
    page.append(el('div', {class: 'hint'}, 'No tuning.json loaded — run the tuner through python scripts/tuner_server.py.'));
    return;
  }
  const body = el('div', {class: 'trows'});
  let n = 0, sec = null, secHas = false;
  for (const pr of tab.params) {
    if (pr.sec) { sec = pr.sec; secHas = false; continue; }
    if (!match(pr.k)) continue;
    if (sec && !secHas) {
      body.append(el('div', {class: 'tsec'}, el('b', {}, sec.t), sec.d ? el('span', {}, sec.d) : null));
      secHas = true;
    }
    body.append(T.tuneRow(tab, pr, T.paramGroup(tab, pr), T.touchTune));
    n++;
  }
  if (!n) body.append(el('div', {class: 'hint'}, 'No worldgen knob matches this subject.'));
  page.append(body);
}

const CAVE_KEYS = /^cave|lava|magma/i;
const COVER_KEYS = /^(flower|grass|tallGrass|tussock|scrub|desertPatch|heath|alpine|cactus|undergrowth|fern|moss|mushroom|bramble|sapling|litter|meadow|sed|treeline)/i;

function paintKnobPages() {
  const el = H.el;
  knobPage('caves', [
    el('b', {}, 'Caves — the engine today. '),
    'worldgen carves two bands world-wide: a near-surface band under caveThreshold1 and a deep band under ',
    'caveThreshold2, both stone-walled, with lava below LAVA_LEVEL through the one caveFill() route. These are ',
    'the live knobs (they write tuning.json; a new world shows them). ',
    el('b', {}, 'Per-biome caves are scaffolded'), ' on each biome page as a cave stack, authored and validated but ',
    'not yet read — the plan is the 3D caves behind a province mask in docs/RESEARCH_worldgen.md stage 8.'
  ], k => CAVE_KEYS.test(k));
  knobPage('cover', [
    el('b', {}, 'Ground cover — the engine today. '),
    'The flora worldgen paints per column: meadow tall grass and flowers, desert tussock and scrub behind a ',
    'patch mask, pine heath, the alpine cushion above the treeline, and the undergrowth layer that reads ',
    'canopy shade rather than biome. These knobs are world-wide; the ',
    el('b', {}, 'per-biome version is each biome page’s Ground cover stack'), ', which the swatch composes and ',
    'the `biomes` gate validates, and which worldgen will read once the cover blocks take a biome table ',
    '(the lowest-risk seam: every one of them is outside the CPU-mirrored height code).'
  ], k => COVER_KEYS.test(k));
}

/* ---------------------------------------------------------------------------
 * sidebar
 * ------------------------------------------------------------------------- */
let navGen = 0;
async function paintNav() {
  const el = H.el;
  const nav = els.nav;
  // Several callers repaint at once (activate, a dirty flip, a deep link) and
  // each awaits the file list: without a generation check the second call
  // clears the sidebar and then the FIRST call's append lands on top of it,
  // and every biome is listed twice (measured: 10 for 4 files).
  const gen = ++navGen;
  let names = [];
  try { names = await Biome.listBiomes(); } catch (e) { names = []; }
  if (gen !== navGen) return;
  nav.innerHTML = '';
  nav.append(el('h4', {}, 'Biomes'));
  const cur = Biome.currentName();
  for (const n of names) {
    const eng = ['forest', 'meadow', 'pine', 'desert'].includes(n);
    const b = el('button', {'data-page': 'biome', 'data-name': n, class: (current === 'biome' && cur === n) ? 'on' : ''},
                 el('span', {}, n),
                 el('span', {class: 'pill' + (eng ? ' eng' : '')}, eng ? 'engine' : 'authored'));
    b.addEventListener('click', () => openPage('biome', n));
    nav.append(b);
  }
  // A NEW, never-saved biome gets a placeholder row; a biome that is merely
  // still LOADING must not (it showed as a fifth "(unsaved biome)" entry for
  // the first paint of every session).
  if (current === 'biome' && !cur && Biome.hasUnsaved && Biome.hasUnsaved()) {
    nav.append(el('button', {'data-page': 'biome', 'data-name': '', class: 'on'}, el('span', {}, '(unsaved biome)'),
                  el('span', {class: 'pill dirty'}, 'new')));
  }
  const add = el('button', {}, el('span', {}, '+ new biome'), el('span', {class: 'pill'}, ''));
  add.addEventListener('click', async () => {
    const from = names.length ? prompt('Copy stacks from which biome? (blank = empty)', names[0]) : '';
    showPage('biome');
    await Biome.newBiome(from && names.includes(from) ? from : '');
    paintNav();
  });
  nav.append(add);

  nav.append(el('h4', {}, 'Components'));
  for (const id of ['trees', 'water', 'caves', 'cover']) {
    const b = el('button', {'data-page': id, class: current === id ? 'on' : ''},
                 el('span', {}, PAGE_LABEL[id]),
                 el('span', {class: 'pill' + (dirtyBy[id] ? ' dirty' : '')},
                    dirtyBy[id] ? 'unsaved' : (id === 'caves' || id === 'cover' ? 'engine knobs' : 'library')));
    b.addEventListener('click', () => showPage(id));
    nav.append(b);
  }
  nav.append(el('div', {class: 'envnote'},
    'A biome SELECTS from the component libraries and says how often and where. ',
    'Species and presets are edited once, in their library; a biome edits the row.'));
}

function setDirty(page, d) {
  dirtyBy[page] = !!d;
  if (H.onDirty) H.onDirty(Object.values(dirtyBy).some(Boolean));
  paintNav();
}

/* ---------------------------------------------------------------------------
 * mount
 * ------------------------------------------------------------------------- */
export function attach(hooks) {
  H = hooks;
  root = H.section;
  if (!root) return;
  const el = H.el;
  const style = document.createElement('style');
  style.textContent = CSS;
  document.head.append(style);

  els.nav = el('div', {class: 'envnav'});
  els.main = el('div', {class: 'envmain'});
  for (const id of PAGE_ORDER) {
    const div = el('div', {class: 'envpage', id: id === 'trees' ? 'view-trees' : 'env-' + id});
    pages[id] = div;
    els.main.append(div);
  }
  root.append(els.nav, els.main);

  envActive = root.classList.contains('active');
  // Mirror the tab's active state onto the current page, so a hidden tab's
  // render loops (which key on their page's .active) go quiet.
  new MutationObserver(() => {
    const now = root.classList.contains('active');
    if (now === envActive) return;
    envActive = now;
    for (const k of Object.keys(pages)) pages[k].classList.toggle('active', now && k === current);
  }).observe(root, {attributes: true, attributeFilter: ['class']});

  const shared = (id) => ({
    el: H.el, toast: H.toast, materials: H.materials,
    onDirty: (d) => setDirty(id, d),
    section: pages[id],
    isVisible: () => envActive,
    tuning: H.tuning,
    voxelsPerMetre: H.voxelsPerMetre,
    openPage,
    onLibraryChanged: (kind) => {
      if (kind !== 'biomes') Biome.librariesChanged && Biome.librariesChanged();
      paintNav();
    }
  });

  Trees.attach(shared('trees'));
  Water.attach(shared('water'));
  Biome.attach(shared('biome'));
  paintKnobPages();
  paintNav();
  current = 'biome';
}

export function activate() {
  envActive = root.classList.contains('active');
  // tuning.json arrives asynchronously in the tuner, usually AFTER this module
  // attached: rebuild the knob pages (idempotent) and let the biome page add
  // its band strip and terrain rows once tuning is there.
  paintKnobPages();
  if (Biome.tuningAvailable) Biome.tuningAvailable();
  showPage(current || 'biome');
  paintNav();
}

/** Ctrl+S on this tab saves the page you are looking at. */
export function saveFromHost() {
  const api = apiOf(current);
  if (api && api.saveFromHost) api.saveFromHost();
  else if (H.saveTuning) H.saveTuning();     // the knob pages edit tuning.json
}
export function isDirty() { return Object.values(dirtyBy).some(Boolean); }

// test seams
export function _pages() { return pages; }
export function _current() { return current; }
export function _open(id, name) { return openPage(id, name); }
export const _modules = {Trees, Water, Biome};
