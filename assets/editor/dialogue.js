// dialogue.js - the tuner's DIALOGUE tab (docs/PLAN_world_editor.md P3).
//
// Three columns over assets/dialogue/*.json:
//   left    the conversation files, then the selected file's nodes
//   middle  the graph: nodes as boxes in columns by distance from the entries,
//           choices as numbered edges, [continue] as ">", else as "else".
//           Drag to pan, wheel to zoom, click a box to edit it.
//   right   the editor for the selected node (or the file's settings):
//           text inline, conditions and actions as rows of dropdowns (known
//           flags, the item list, the schedule activities, dialogue names),
//           add choice / add node / jump to a choice's target.
// and underneath, the VALIDATION panel: the same checks game/dialogue.cpp
// runs (dialoguelib.js), each naming file, node and field; click one to go
// there. Save writes the file in the canonical layout (Ctrl+S); press R in
// the game to hot-reload.
//
// All data logic is in dialoguelib.js (pure, Node-tested by
// scripts/test_dialogue.mjs); this file is only the page.

import * as dl from './dialoguelib.js';

const st = {
  files: [],        // {name, data, saved (text), dirty, isNew, renamedFrom}
  items: [],
  sel: -1,          // file index
  node: '#file',    // node id, or '#file' for the file's settings
  view: {x: 30, y: 30, k: 1},
  root: null,
  loaded: false,
};

// ---- tiny DOM helpers ------------------------------------------------------
function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (k === 'value') e.value = v;
    else if (k === 'checked') e.checked = !!v;
    else e.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false)
    e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return e;
}
const svgNS = 'http://www.w3.org/2000/svg';
function s(tag, attrs, ...kids) {
  const e = document.createElementNS(svgNS, tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v);
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined)
    e.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return e;
}
function toast(msg, bad) {
  if (typeof window.toast === 'function') window.toast(msg, bad);
  else console.log(msg);
}

const CSS = `
#view-dialogue{padding:0}
.dlg-bar{display:flex;gap:8px;align-items:center;padding:8px 14px;border-bottom:1px solid var(--line);background:var(--bg2)}
.dlg-bar .grow{flex:1}
.dlg-bar .st{font-family:var(--mono);font-size:12px;color:var(--dim)}
.dlg-bar .st.dirty{color:var(--amber)}
.dlg-wrap{display:grid;grid-template-columns:230px 1fr 480px;height:calc(100vh - 250px);min-height:420px}
.dlg-side{border-right:1px solid var(--line);overflow:auto;padding:8px}
.dlg-side h4,.dlg-edit h4{margin:10px 0 6px;font-size:11px;letter-spacing:1px;text-transform:uppercase;color:var(--faint)}
.dlg-item{padding:4px 8px;border-radius:4px;cursor:pointer;font-family:var(--mono);font-size:12.5px;display:flex;gap:6px;align-items:center}
.dlg-item:hover{background:var(--card2)}
.dlg-item.sel{background:#1d3a6e}
.dlg-item .tag{font-size:10px;color:var(--faint)}
.dlg-item .bad{color:var(--red)}
.dlg-item .warn{color:var(--amber)}
.dlg-graph{position:relative;overflow:hidden;background:#0b0e13;cursor:grab}
.dlg-graph svg{width:100%;height:100%;display:block;user-select:none}
.dlg-graph .hint{position:absolute;left:10px;bottom:8px;font-size:11px;color:var(--faint)}
.dlg-edit{border-left:1px solid var(--line);overflow:auto;padding:10px 12px}
.dlg-edit label{display:block;font-size:11px;color:var(--dim);margin:8px 0 3px}
.dlg-edit input[type=text],.dlg-edit textarea,.dlg-edit select,.dlg-edit input[type=number],.dlg-edit input[type=time]{
  background:var(--card);color:var(--fg);border:1px solid var(--line2);border-radius:4px;padding:4px 6px;font-size:13px;font-family:var(--sans)}
.dlg-edit input[type=text],.dlg-edit textarea{width:100%}
.dlg-edit textarea{min-height:72px;resize:vertical;line-height:1.35}
.dlg-card{border:1px solid var(--line2);border-radius:6px;padding:8px;margin:8px 0;background:var(--card)}
.dlg-card .head{display:flex;gap:6px;align-items:center}
.dlg-card .num{font-family:var(--mono);color:var(--amber);min-width:18px}
.dlg-rows{margin:4px 0 0 0}
.dlg-row{display:flex;gap:4px;align-items:center;margin:3px 0;flex-wrap:wrap}
.dlg-row select,.dlg-row input{font-size:12px!important;padding:2px 4px!important}
.dlg-row input[type=number]{width:60px}
.dlg-row .lbl{font-size:11px;color:var(--faint)}
.dlg-sub{font-size:11px;color:var(--faint);margin:6px 0 2px;display:flex;gap:6px;align-items:center}
.dlg-probs{border-top:1px solid var(--line);max-height:150px;overflow:auto;padding:6px 14px;font-family:var(--mono);font-size:12px}
.dlg-probs div{cursor:pointer;padding:1px 0}
.dlg-probs .e{color:var(--red)}
.dlg-probs .w{color:var(--amber)}
.dlg-probs .ok{color:var(--green);cursor:default}
`;

// ---- loading / saving --------------------------------------------------------
async function load() {
  try {
    const r = await fetch('/api/dialogues');
    // A 404 here is a server older than this tab, most often a frozen
    // sandvox_tuner.exe built before the route existed; say so instead of
    // failing to parse its plain-text "not found".
    if (r.status === 404) throw new Error('this tuner server is older than the Dialogue tab. ' +
      'Rebuild the app with "python scripts/build_tuner_exe.py" (close the tuner first), ' +
      'or run "python scripts/tuner_server.py" instead');
    const j = await r.json();
    st.items = j.items || [];
    st.files = (j.files || []).map(f => {
      let data;
      try { data = JSON.parse(f.text); } catch (e) { data = {__error: e.message}; }
      return {name: f.name, data, saved: f.text, dirty: false};
    });
    st.loaded = true;
    if (st.sel >= st.files.length) st.sel = -1;
    if (st.sel < 0 && st.files.length) st.sel = 0;
    // Deep link: ?tab=dialogue&dlgfile=<name>&dlgnode=<id> opens a node, so a
    // bug report (or a headless check) can point at one line of one file.
    const q = new URLSearchParams(location.search);
    const fi = st.files.findIndex(f => f.name === q.get('dlgfile'));
    if (fi >= 0 && !st.linked) { st.sel = fi; st.node = q.get('dlgnode') || '#file'; }
    st.linked = true;
  } catch (e) {
    st.loaded = false;
    st.error = 'the Dialogue tab needs the tuner server (python scripts/tuner_server.py or sandvox_tuner.exe): ' + e.message;
  }
  render();
}

function cur() { return st.sel >= 0 ? st.files[st.sel] : null; }
function touch() {
  const f = cur();
  if (f) f.dirty = true;
  refreshLive();
}

async function save() {
  const f = cur();
  if (!f || f.data.__error) return;
  const probs = dl.validateAll(st.files.map(x => ({name: x.name, data: x.data})), st.items)
    .filter(p => p.error && p.file === f.name + '.json');
  if (probs.length && !confirm(`${f.name}.json has ${probs.length} error(s); the game will skip it until they are fixed. Save anyway?`)) return;
  f.data.name = f.name;
  const text = dl.serialize(f.data);
  const body = {text};
  if (f.renamedFrom) body.rename = f.renamedFrom;
  const r = await fetch('/api/dialogue?name=' + encodeURIComponent(f.name),
                        {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)});
  const j = await r.json().catch(() => ({}));
  if (!r.ok || !j.ok) { toast('save failed: ' + (j.error || r.status), true); return; }
  f.saved = text; f.dirty = false; f.isNew = false; f.renamedFrom = null;
  toast(`saved ${f.name}.json - press R in the game to reload conversations`);
  render();
}

function newFile() {
  const name = (prompt('New conversation file name (lowercase, digits, _):', 'new_dialogue') || '').trim();
  if (!name) return;
  if (!/^[a-z0-9_]+$/.test(name)) { toast('a name is lowercase letters, digits and _', true); return; }
  if (st.files.some(f => f.name === name)) { toast(name + ' already exists', true); return; }
  st.files.push({name, data: dl.blank(name), saved: '', dirty: true, isNew: true});
  st.sel = st.files.length - 1;
  st.node = 'start';
  render();
}

function renameFile() {
  const f = cur(); if (!f) return;
  const name = (prompt('Rename the file to:', f.name) || '').trim();
  if (!name || name === f.name) return;
  if (!/^[a-z0-9_]+$/.test(name)) { toast('a name is lowercase letters, digits and _', true); return; }
  if (st.files.some(x => x.name === name)) { toast(name + ' already exists', true); return; }
  if (!f.isNew && !f.renamedFrom) f.renamedFrom = f.name;
  f.name = name; f.data.name = name; f.dirty = true;
  render();
}

async function deleteFile() {
  const f = cur(); if (!f) return;
  if (!confirm(`Delete ${f.name}.json? This cannot be undone from here (git can).`)) return;
  if (!f.isNew) {
    const r = await fetch('/api/dialogue/delete?name=' + encodeURIComponent(f.renamedFrom || f.name), {method: 'POST'});
    if (!r.ok) { toast('delete failed', true); return; }
  }
  st.files.splice(st.sel, 1);
  st.sel = st.files.length ? 0 : -1;
  st.node = '#file';
  render();
}

function revert() {
  const f = cur(); if (!f) return;
  if (f.isNew) { st.files.splice(st.sel, 1); st.sel = st.files.length ? 0 : -1; render(); return; }
  if (f.dirty && !confirm('Throw away the unsaved changes to ' + f.name + '?')) return;
  try { f.data = JSON.parse(f.saved); } catch (e) { f.data = {__error: e.message}; }
  if (f.renamedFrom) { f.name = f.renamedFrom; f.renamedFrom = null; }
  f.dirty = false;
  render();
}

// ---- the page ------------------------------------------------------------------
function render() {
  const root = st.root;
  root.innerHTML = '';
  root.append(h('style', {}, CSS));
  if (st.error) { root.append(h('div', {class: 'banner', style: 'margin:20px'}, st.error)); return; }
  const f = cur();
  root.append(h('div', {class: 'dlg-bar'},
    h('button', {class: 'primary', onclick: save, disabled: !f}, 'Save ', h('kbd', {}, 'Ctrl+S')),
    h('button', {onclick: revert, disabled: !f}, 'Revert'),
    h('button', {onclick: load}, 'Reload from disk'),
    h('button', {onclick: fitView, title: 'frame the whole graph'}, 'Fit graph'),
    h('span', {class: 'st' + (f && f.dirty ? ' dirty' : ''), id: 'dlgState'}, stateText()),
    h('span', {class: 'grow'}),
    h('span', {class: 'st'}, 'in game: F1 → Spawn → Dialogue → talk; R reloads')));
  const wrap = h('div', {class: 'dlg-wrap'});
  wrap.append(renderSide(), renderGraphHost(), renderEditor());
  root.append(wrap);
  root.append(h('div', {class: 'dlg-probs', id: 'dlgProbs'}));
  refreshLive();
  if (st.fitFor !== st.sel) { st.fitFor = st.sel; requestAnimationFrame(fitView); }
}

function stateText() {
  const f = cur();
  if (!f) return st.files.length ? '' : 'no conversations yet - "+ new file"';
  return `${f.name}.json` + (f.dirty ? ' - unsaved' : ' - saved');
}

function problems() {
  return dl.validateAll(st.files.map(x => ({name: x.name, data: x.data})), st.items);
}

// Everything that depends on the data but not on focus: the graph, the
// validation list, the status line, the file list's badges.
function refreshLive() {
  const f = cur();
  const stEl = st.root.querySelector('#dlgState');
  if (stEl) { stEl.textContent = stateText(); stEl.className = 'st' + (f && f.dirty ? ' dirty' : ''); }
  drawGraph();
  const probs = problems();
  const box = st.root.querySelector('#dlgProbs');
  if (box) {
    box.innerHTML = '';
    const mine = f ? probs.filter(p => p.file === f.name + '.json' || p.file === '(all dialogue)') : probs;
    if (!mine.length) box.append(h('div', {class: 'ok'}, f ? `${f.name}.json: no problems` : 'no problems'));
    for (const p of mine) {
      box.append(h('div', {class: p.error ? 'e' : 'w', onclick: () => {
        const i = st.files.findIndex(x => x.name + '.json' === p.file);
        if (i >= 0) st.sel = i;
        st.node = p.node || (p.where && p.where.startsWith('entry') ? '#file' : st.node);
        render();
      }}, dl.problemLine(p)));
    }
  }
  const side = st.root.querySelector('#dlgFiles');
  if (side) fillFiles(side, probs);
}

function fillFiles(box, probs) {
  box.innerHTML = '';
  st.files.forEach((f, i) => {
    const errs = probs.filter(p => p.error && p.file === f.name + '.json').length;
    const warns = probs.filter(p => !p.error && p.file === f.name + '.json').length;
    box.append(h('div', {class: 'dlg-item' + (i === st.sel ? ' sel' : ''), onclick: () => { st.sel = i; st.node = '#file'; render(); }},
      h('span', {}, f.name), f.dirty ? h('span', {class: 'warn'}, '*') : null,
      errs ? h('span', {class: 'bad', title: errs + ' error(s)'}, '!' + errs) : null,
      warns ? h('span', {class: 'tag', title: warns + ' warning(s)'}, '?' + warns) : null));
  });
}

function renderSide() {
  const side = h('aside', {class: 'dlg-side'});
  side.append(h('h4', {}, 'Conversations'));
  side.append(h('div', {id: 'dlgFiles'}));
  side.append(h('div', {class: 'dlg-row'},
    h('button', {class: 'small', onclick: newFile}, '+ new file'),
    h('button', {class: 'small', onclick: renameFile, disabled: !cur()}, 'rename'),
    h('button', {class: 'small danger', onclick: deleteFile, disabled: !cur()}, 'delete')));
  const f = cur();
  if (!f) return side;
  side.append(h('h4', {}, 'Nodes'));
  side.append(h('div', {class: 'dlg-item' + (st.node === '#file' ? ' sel' : ''), onclick: () => { st.node = '#file'; render(); }},
    h('span', {}, '⚙ file settings + entries')));
  if (f.data.__error) { side.append(h('div', {class: 'bad'}, 'not valid JSON: ' + f.data.__error)); return side; }
  for (const n of (f.data.nodes || [])) {
    if (!n) continue;
    side.append(h('div', {class: 'dlg-item' + (st.node === n.id ? ' sel' : ''), onclick: () => { st.node = n.id; render(); }},
      h('span', {}, n.id || '(no id)'),
      h('span', {class: 'tag'}, (n.choices || []).length ? (n.choices.length + ' ch') : (n.goto ? '> ' + n.goto : 'end'))));
  }
  side.append(h('div', {class: 'dlg-row'}, h('button', {class: 'small', onclick: () => addNode()}, '+ add node')));
  return side;
}

function addNode(linkFrom) {
  const f = cur(); if (!f) return null;
  const id = dl.freshId(f.data, 'node');
  f.data.nodes = f.data.nodes || [];
  f.data.nodes.push({id, text: ''});
  if (linkFrom) linkFrom(id);
  st.node = id;
  f.dirty = true;
  render();
  const ta = st.root.querySelector('.dlg-edit textarea');
  if (ta) ta.focus();
  return id;
}

// ---- the graph ---------------------------------------------------------------
const BW = 210, BH = 62, GX = 90, GY = 22;

function renderGraphHost() {
  const host = h('section', {class: 'dlg-graph'});
  const svg = s('svg', {id: 'dlgSvg'});
  host.append(svg, h('div', {class: 'hint'}, 'drag to pan · wheel to zoom · click a node to edit · numbers are choices, > is [continue]'));
  let drag = null;
  host.addEventListener('mousedown', e => { if (e.button === 0 && !e.target.closest('.dlg-node')) drag = {x: e.clientX, y: e.clientY, vx: st.view.x, vy: st.view.y}; });
  window.addEventListener('mousemove', e => {
    if (!drag) return;
    st.view.x = drag.vx + (e.clientX - drag.x); st.view.y = drag.vy + (e.clientY - drag.y);
    applyView();
  });
  window.addEventListener('mouseup', () => { drag = null; });
  host.addEventListener('wheel', e => {
    e.preventDefault();
    const r = host.getBoundingClientRect();
    const mx = e.clientX - r.left, my = e.clientY - r.top;
    const k0 = st.view.k, k1 = Math.min(2.5, Math.max(0.3, k0 * (e.deltaY < 0 ? 1.12 : 1 / 1.12)));
    st.view.x = mx - (mx - st.view.x) * k1 / k0; st.view.y = my - (my - st.view.y) * k1 / k0; st.view.k = k1;
    applyView();
  }, {passive: false});
  return host;
}

function applyView() {
  const g = st.root.querySelector('#dlgWorld');
  if (g) g.setAttribute('transform', `translate(${st.view.x},${st.view.y}) scale(${st.view.k})`);
}

// The first two lines of `t`, broken at words, the second ellipsised.
function twoLines(t, w) {
  const words = t.split(/\s+/), lines = [''];
  for (const wd of words) {
    const cur = lines[lines.length - 1];
    if ((cur ? cur.length + 1 : 0) + wd.length <= w) lines[lines.length - 1] = cur ? cur + ' ' + wd : wd;
    else if (lines.length < 3) lines.push(wd);
    else break;
  }
  const more = lines.length > 2;
  return [lines[0] || '', (lines[1] || '') + (more ? '…' : '')];
}

// Frame the whole graph in the pane (on opening a file, and the fit button).
function fitView() {
  const f = cur(), svg = st.root && st.root.querySelector('#dlgSvg');
  if (!f || !svg || f.data.__error) return;
  const pos = dl.layout(f.data);
  let cols = 0, rows = 0;
  for (const p of pos.values()) { cols = Math.max(cols, p.col + 1); rows = Math.max(rows, p.row + 1); }
  const W = cols * (BW + GX) + GX + 70, H = rows * (BH + GY) + 40;
  const r = svg.getBoundingClientRect();
  if (!r.width || !r.height) return;
  const k = Math.max(0.3, Math.min(1.2, Math.min(r.width / W, (r.height - 30) / H)));
  st.view = {k, x: (GX + 80) * k + Math.max(0, (r.width - W * k) / 2), y: 20};
  applyView();
}

function drawGraph() {
  const svg = st.root && st.root.querySelector('#dlgSvg');
  if (!svg) return;
  svg.innerHTML = '';
  const f = cur();
  if (!f || f.data.__error) return;
  const d = f.data;
  const pos = dl.layout(d);
  const probs = problems().filter(p => p.file === f.name + '.json');
  const at = id => {
    if (id === '#entry') return {x: -GX - 70, y: 0, w: 70, h: 30};
    const p = pos.get(id); if (!p) return null;
    return {x: p.col * (BW + GX), y: p.row * (BH + GY), w: BW, h: BH};
  };
  const defs = s('defs', {},
    s('marker', {id: 'dlgArrow', viewBox: '0 0 10 10', refX: 9, refY: 5, markerWidth: 7, markerHeight: 7, orient: 'auto-start-reverse'},
      s('path', {d: 'M0,0 L10,5 L0,10 z', fill: '#8a94a6'})));
  const world = s('g', {id: 'dlgWorld'});
  svg.append(defs, world);
  // Edges under boxes.
  for (const e of dl.edges(d)) {
    const a = at(e.from), b = at(e.to);
    if (!a || !b) continue;
    const col = e.kind === 'choice' ? '#6ea8fe' : e.kind === 'else' ? '#ff6b6b' : e.kind === 'entry' ? '#7cf03a' : '#8a94a6';
    let path, lx, ly;
    const x1 = a.x + a.w, y1 = a.y + a.h / 2, x2 = b.x, y2 = b.y + b.h / 2;
    if (x2 > x1 + 4) {
      const cx = (x1 + x2) / 2;
      path = `M${x1},${y1} C${cx},${y1} ${cx},${y2} ${x2},${y2}`;
      lx = cx; ly = (y1 + y2) / 2;
    } else {
      // Backward (or same column): loop under both boxes.
      const yb = Math.max(a.y + a.h, b.y + b.h) + 26;
      const bx1 = a.x + a.w / 2, bx2 = b.x + b.w / 2;
      path = `M${bx1},${a.y + a.h} C${bx1},${yb} ${bx2},${yb} ${bx2},${b.y + b.h}`;
      lx = (bx1 + bx2) / 2; ly = yb - 6;
    }
    // Back edges (a return to a hub like 'ask') are drawn faint: they are
    // most of the lines in a conversation and least of what you read.
    const back = !(x2 > x1 + 4);
    world.append(s('path', {d: path, fill: 'none', stroke: col, 'stroke-width': 1.6, 'stroke-dasharray': e.kind === 'else' ? '5 4' : '', 'marker-end': 'url(#dlgArrow)', opacity: back ? 0.35 : 0.85}));
    if (back && e.kind === 'goto') continue;  // an unlabelled return: the arrow says it
    world.append(s('rect', {x: lx - 9, y: ly - 8, width: 18 + (e.label.length > 2 ? 16 : 0), height: 15, rx: 3, fill: '#0b0e13', stroke: col, 'stroke-width': 1}));
    world.append(s('text', {x: lx, y: ly + 3.5, fill: col, 'font-size': 10, 'text-anchor': 'middle', 'font-family': 'Consolas,monospace'}, e.label));
  }
  // The start pill.
  if ((d.entry || []).length) {
    const a = at('#entry');
    world.append(s('rect', {x: a.x, y: a.y, width: a.w, height: a.h, rx: 15, fill: '#16301a', stroke: '#7cf03a'}));
    world.append(s('text', {x: a.x + a.w / 2, y: a.y + 19, fill: '#7cf03a', 'font-size': 12, 'text-anchor': 'middle'}, 'start'));
  }
  for (const n of (d.nodes || [])) {
    if (!n || !n.id) continue;
    const b = at(n.id), p = pos.get(n.id);
    const errs = probs.filter(q => q.node === n.id && q.error).length;
    const warns = probs.filter(q => q.node === n.id && !q.error).length;
    const sel = st.node === n.id;
    const g = s('g', {class: 'dlg-node', style: 'cursor:pointer', onclick: () => { st.node = n.id; render(); }});
    g.append(s('rect', {x: b.x, y: b.y, width: b.w, height: b.h, rx: 6,
      fill: sel ? '#1d3a6e' : '#1a2029', stroke: errs ? '#ff6b6b' : sel ? '#6ea8fe' : p.unreachable ? '#5c667a' : '#39445a',
      'stroke-width': sel || errs ? 2 : 1, 'stroke-dasharray': p.unreachable ? '4 3' : ''}));
    g.append(s('text', {x: b.x + 8, y: b.y + 16, fill: '#ffb454', 'font-size': 12, 'font-family': 'Consolas,monospace'},
      n.id + (errs ? '  !' + errs : warns ? '  ?' + warns : '')));
    const [line1, line2] = twoLines(String(n.text || '(no text)'), 33);
    g.append(s('text', {x: b.x + 8, y: b.y + 34, fill: '#d7dde6', 'font-size': 11}, line1));
    if (line2) g.append(s('text', {x: b.x + 8, y: b.y + 49, fill: '#d7dde6', 'font-size': 11}, line2));
    if (n.if && n.if.length) g.append(s('text', {x: b.x + b.w - 8, y: b.y + 16, fill: '#8a94a6', 'font-size': 10, 'text-anchor': 'end'}, 'if'));
    world.append(g);
  }
  applyView();
}

// ---- the editor ----------------------------------------------------------------
function names() { return st.files.map(f => f.name); }

function nodeSelect(value, onchange, {allowEnd = true, endLabel = '(end the conversation)', allowNew = true} = {}) {
  const f = cur();
  const ids = (f.data.nodes || []).filter(n => n && n.id).map(n => n.id);
  const sel = h('select', {onchange: e => {
    if (e.target.value === '#new') { addNode(id => onchange(id)); return; }
    onchange(e.target.value); touch(); drawGraph();
  }});
  if (allowEnd) sel.append(h('option', {value: ''}, endLabel));
  for (const id of ids) sel.append(h('option', {value: id}, id));
  if (value && !ids.includes(value)) sel.append(h('option', {value}, value + ' (missing!)'));
  if (allowNew) sel.append(h('option', {value: '#new'}, '+ new node…'));
  sel.value = value || '';
  return sel;
}

function jump(id) {
  return h('button', {class: 'small icon', title: 'go to ' + id, onclick: () => { if (id) { st.node = id; render(); } }, disabled: !id}, '→');
}

// A list of conditions (arr is mutated in place; `set` installs a new array).
function condRows(arr, set, title) {
  const box = h('div', {class: 'dlg-rows'});
  const flags = dl.knownFlags(st.files.map(x => ({name: x.name, data: x.data})));
  const redraw = () => { const nb = condRows(arr, set, title); box.replaceWith(nb); touch(); };
  box.append(h('div', {class: 'dlg-sub'}, title,
    h('button', {class: 'small', onclick: () => { arr = arr || []; arr.push({flag: flags[0] || 'my_flag'}); set(arr); redraw(); }}, '+ condition')));
  (arr || []).forEach((c, i) => {
    const kind = dl.condKind(c) || 'flag';
    const base = kind.replace(/^!/, ''), neg = kind.startsWith('!');
    const row = h('div', {class: 'dlg-row'});
    const kindSel = h('select', {onchange: e => {
      const k = e.target.value, b = k.replace(/^!/, '');
      const nc = {};
      nc[k] = b === 'time' ? ['20:00', '05:00'] : b === 'met' ? true : b === 'activity' ? 'work'
            : b === 'has' ? (st.items[0] || '') : (typeof c[kind] === 'string' ? c[kind] : 'my_flag');
      arr[i] = nc; redraw();
    }}, ...dl.COND_KINDS.map(k => h('option', {value: k}, k === 'met' ? 'met' : k)));
    kindSel.value = kind;
    row.append(kindSel);
    if (base === 'flag') {
      row.append(h('input', {type: 'text', list: 'dlgFlagList', value: c[kind], style: 'width:150px', oninput: e => { c[kind] = e.target.value; touch(); }}));
      if (!neg) {
        row.append(h('span', {class: 'lbl'}, '='),
          h('input', {type: 'number', placeholder: 'any', value: c.value ?? '', title: 'blank = any non-zero value', oninput: e => {
            if (e.target.value === '') delete c.value; else c.value = parseInt(e.target.value, 10) || 0; touch(); }}));
      }
    } else if (base === 'time') {
      const v = Array.isArray(c[kind]) ? c[kind] : ['00:00', '00:00'];
      row.append(h('span', {class: 'lbl'}, 'from'), h('input', {type: 'time', value: v[0], oninput: e => { v[0] = e.target.value; c[kind] = v; touch(); }}),
                 h('span', {class: 'lbl'}, 'to'), h('input', {type: 'time', value: v[1], oninput: e => { v[1] = e.target.value; c[kind] = v; touch(); }}));
    } else if (base === 'activity') {
      const sel = h('select', {onchange: e => { c[kind] = e.target.value; touch(); }}, ...dl.ACTIVITIES.map(a => h('option', {value: a}, a)));
      sel.value = c[kind]; row.append(sel);
    } else if (base === 'has') {
      const sel = h('select', {onchange: e => { c[kind] = e.target.value; touch(); }}, ...st.items.map(a => h('option', {value: a}, a)));
      if (!st.items.includes(c[kind])) sel.append(h('option', {value: c[kind]}, c[kind] + ' (unknown!)'));
      sel.value = c[kind]; row.append(sel);
      if (!neg) row.append(h('span', {class: 'lbl'}, 'x'), h('input', {type: 'number', min: 1, value: c.count ?? 1, oninput: e => {
        const n = parseInt(e.target.value, 10) || 1; if (n === 1) delete c.count; else c.count = n; touch(); }}));
    } else if (base === 'met') {
      const sel = h('select', {onchange: e => { c[kind] = e.target.value === '#self' ? true : e.target.value; touch(); }},
        h('option', {value: '#self'}, '(this conversation)'), ...names().map(n => h('option', {value: n}, n)));
      sel.value = c[kind] === true ? '#self' : c[kind]; row.append(sel);
    }
    row.append(h('button', {class: 'small icon', title: 'remove', onclick: () => { arr.splice(i, 1); if (!arr.length) set(undefined); redraw(); }}, '✕'));
    box.append(row);
  });
  return box;
}

function actRows(arr, set, title) {
  const box = h('div', {class: 'dlg-rows'});
  const redraw = () => { const nb = actRows(arr, set, title); box.replaceWith(nb); touch(); };
  box.append(h('div', {class: 'dlg-sub'}, title,
    h('button', {class: 'small', onclick: () => { arr = arr || []; arr.push({set: 'my_flag'}); set(arr); redraw(); }}, '+ action')));
  (arr || []).forEach((a, i) => {
    const kind = dl.actKind(a) || 'set';
    const row = h('div', {class: 'dlg-row'});
    const kindSel = h('select', {onchange: e => {
      const k = e.target.value, na = {};
      na[k] = (k === 'end' || k === 'present_contract' || k === 'release' || k === 'dismiss') ? true : (k === 'give' || k === 'take') ? (st.items[0] || '') : (typeof a[kind] === 'string' && kind !== 'give' && kind !== 'take' ? a[kind] : 'my_flag');
      arr[i] = na; redraw();
    }}, ...dl.ACT_KINDS.map(k => h('option', {value: k}, k)));
    kindSel.value = kind;
    row.append(kindSel);
    if (kind === 'set' || kind === 'add' || kind === 'clear') {
      row.append(h('input', {type: 'text', list: 'dlgFlagList', value: a[kind], style: 'width:150px', oninput: e => { a[kind] = e.target.value; touch(); }}));
      if (kind !== 'clear') row.append(h('span', {class: 'lbl'}, kind === 'add' ? '+' : '='),
        h('input', {type: 'number', value: a.value ?? 1, oninput: e => { const n = parseInt(e.target.value, 10); if (n === 1 || isNaN(n)) delete a.value; else a.value = n; touch(); }}));
    } else if (kind === 'give' || kind === 'take') {
      const sel = h('select', {onchange: e => { a[kind] = e.target.value; touch(); }}, ...st.items.map(x => h('option', {value: x}, x)));
      if (!st.items.includes(a[kind])) sel.append(h('option', {value: a[kind]}, a[kind] + ' (unknown!)'));
      sel.value = a[kind]; row.append(sel);
      row.append(h('span', {class: 'lbl'}, 'x'), h('input', {type: 'number', min: 1, value: a.count ?? 1, oninput: e => {
        const n = parseInt(e.target.value, 10) || 1; a.count = n; touch(); }}));
    } else if (kind === 'grant' || kind === 'learn') {
      // demons D2: a glyph id (glyphs.json) the player now owns / a name the
      // world now knows (flag name:<x>).
      row.append(h('input', {type: 'text', value: a[kind], style: 'width:150px',
        placeholder: kind === 'grant' ? 'glyph id' : 'demon name',
        oninput: e => { a[kind] = e.target.value; touch(); }}));
      row.append(h('span', {class: 'lbl'}, kind === 'grant' ? 'the player owns this glyph' : 'sets name:<name>'));
    } else if (kind === 'present_contract' || kind === 'release' || kind === 'dismiss') {
      // demons D5 (game/demon_talk.h): a demon's conversation only.
      row.append(h('span', {class: 'lbl'}, kind === 'present_contract' ? 'opens the contract picker'
        : kind === 'release' ? 'lets the demon out (the binding is weighed)' : 'sends the demon home'));
    } else {
      row.append(h('span', {class: 'lbl'}, 'ends the conversation after this'));
    }
    row.append(h('button', {class: 'small icon', title: 'remove', onclick: () => { arr.splice(i, 1); if (!arr.length) set(undefined); redraw(); }}, '✕'));
    box.append(row);
  });
  return box;
}

function renderEditor() {
  const ed = h('aside', {class: 'dlg-edit'});
  const f = cur();
  const flags = dl.knownFlags(st.files.map(x => ({name: x.name, data: x.data})));
  ed.append(h('datalist', {id: 'dlgFlagList'}, ...flags.map(x => h('option', {value: x}))));
  if (!f) { ed.append(h('p', {class: 'hint'}, 'Pick a conversation on the left, or make a new one.')); return ed; }
  if (f.data.__error) { ed.append(h('p', {}, 'This file is not valid JSON; fix it in a text editor: ' + f.data.__error)); return ed; }
  const d = f.data;
  if (st.node === '#file' || !(d.nodes || []).some(n => n && n.id === st.node)) {
    ed.append(h('h4', {}, `File: ${f.name}.json`));
    ed.append(h('label', {}, 'Speaker (header name; a node may override it)'),
      h('input', {type: 'text', value: d.speaker || '', oninput: e => { d.speaker = e.target.value; touch(); }}));
    ed.append(h('label', {}, h('input', {type: 'checkbox', checked: d.canLeave !== false, onchange: e => { d.canLeave = e.target.checked; touch(); }}),
      ' the player may leave with Esc (a node can override)'));
    ed.append(h('label', {}, 'Notes (for authors; the game ignores them)'),
      h('textarea', {value: d.notes || '', oninput: e => { if (e.target.value) d.notes = e.target.value; else delete d.notes; touch(); }}));
    ed.append(h('h4', {}, 'Entries - the FIRST whose conditions hold picks where the conversation starts'));
    d.entry = d.entry || [];
    d.entry.forEach((e, i) => {
      const card = h('div', {class: 'dlg-card'});
      card.append(h('div', {class: 'head'}, h('span', {class: 'num'}, 'e' + (i + 1)), h('span', {class: 'lbl'}, 'start at'),
        nodeSelect(e.goto, v => { e.goto = v; }, {allowEnd: false}), jump(e.goto),
        h('span', {class: 'grow', style: 'flex:1'}),
        h('button', {class: 'small icon', title: 'earlier', disabled: i === 0, onclick: () => { d.entry.splice(i - 1, 0, d.entry.splice(i, 1)[0]); touch(); render(); }}, '↑'),
        h('button', {class: 'small icon', title: 'later', disabled: i === d.entry.length - 1, onclick: () => { d.entry.splice(i + 1, 0, d.entry.splice(i, 1)[0]); touch(); render(); }}, '↓'),
        h('button', {class: 'small icon', title: 'remove', onclick: () => { d.entry.splice(i, 1); touch(); render(); }}, '✕')));
      card.append(condRows(e.if, v => { if (v) e.if = v; else delete e.if; }, 'only if (all hold):'));
      ed.append(card);
    });
    ed.append(h('button', {class: 'small', onclick: () => { d.entry.push({goto: (d.nodes[0] || {}).id || ''}); touch(); render(); }}, '+ add entry'));
    return ed;
  }
  const n = d.nodes.find(x => x && x.id === st.node);
  ed.append(h('h4', {}, 'Node'));
  ed.append(h('label', {}, 'id (renaming updates every goto that names it)'),
    h('input', {type: 'text', value: n.id, onchange: e => {
      const to = e.target.value.trim();
      if (!to || to === n.id) return;
      if (d.nodes.some(x => x && x.id === to)) { toast('a node called ' + to + ' already exists', true); e.target.value = n.id; return; }
      dl.renameNode(d, n.id, to); st.node = to; touch(); render();
    }}));
  ed.append(h('label', {}, 'Speaker (blank = the file\'s: ' + (d.speaker || 'none') + ')'),
    h('input', {type: 'text', value: n.speaker || '', oninput: e => { if (e.target.value) n.speaker = e.target.value; else delete n.speaker; touch(); }}));
  ed.append(h('label', {}, 'What is said'),
    h('textarea', {value: n.text || '', oninput: e => { n.text = e.target.value; touch(); }}));
  {
    const sel = h('select', {onchange: e => { const v = e.target.value; if (v === '') delete n.canLeave; else n.canLeave = v === 'yes'; touch(); }},
      h('option', {value: ''}, 'as the file says'), h('option', {value: 'yes'}, 'yes'), h('option', {value: 'no'}, 'no - the player must answer'));
    sel.value = n.canLeave === undefined ? '' : n.canLeave ? 'yes' : 'no';
    ed.append(h('label', {}, 'May the player leave here with Esc?'), sel);
  }
  const guard = h('div', {class: 'dlg-card'});
  guard.append(condRows(n.if, v => { if (v) n.if = v; else delete n.if; }, 'arriving here needs (all hold):'));
  guard.append(h('div', {class: 'dlg-row'}, h('span', {class: 'lbl'}, 'otherwise go to'),
    nodeSelect(n.else, v => { if (v) n.else = v; else delete n.else; }, {endLabel: '(end the conversation)'}), jump(n.else)));
  ed.append(guard);
  const doCard = h('div', {class: 'dlg-card'});
  doCard.append(actRows(n.do, v => { if (v) n.do = v; else delete n.do; }, 'on arrival, do:'));
  ed.append(doCard);
  ed.append(h('h4', {}, 'Choices (keys 1-9; hidden unless their conditions hold)'));
  n.choices = n.choices || [];
  n.choices.forEach((c, i) => {
    const card = h('div', {class: 'dlg-card'});
    card.append(h('div', {class: 'head'}, h('span', {class: 'num'}, String(i + 1)),
      h('input', {type: 'text', value: c.text || '', placeholder: 'what the player says', oninput: e => { c.text = e.target.value; touch(); }}),
      h('button', {class: 'small icon', title: 'earlier', disabled: i === 0, onclick: () => { n.choices.splice(i - 1, 0, n.choices.splice(i, 1)[0]); touch(); render(); }}, '↑'),
      h('button', {class: 'small icon', title: 'later', disabled: i === n.choices.length - 1, onclick: () => { n.choices.splice(i + 1, 0, n.choices.splice(i, 1)[0]); touch(); render(); }}, '↓'),
      h('button', {class: 'small icon', title: 'remove this choice', onclick: () => { n.choices.splice(i, 1); touch(); render(); }}, '✕')));
    card.append(h('div', {class: 'dlg-row'}, h('span', {class: 'lbl'}, 'leads to'),
      nodeSelect(c.goto, v => { if (v) c.goto = v; else delete c.goto; }), jump(c.goto)));
    card.append(condRows(c.if, v => { if (v) c.if = v; else delete c.if; }, 'shown only if:'));
    card.append(actRows(c.do, v => { if (v) c.do = v; else delete c.do; }, 'when chosen, do:'));
    ed.append(card);
  });
  ed.append(h('div', {class: 'dlg-row'},
    h('button', {class: 'small', onclick: () => { (n.choices = n.choices || []).push({text: ''}); touch(); render(); }}, '+ add choice'),
    h('button', {class: 'small', onclick: () => { addNode(id => (n.choices = n.choices || []).push({text: '', goto: id})); }}, '+ choice to a new node')));
  ed.append(h('label', {}, 'With no choice showing, [continue] goes to'),
    h('div', {class: 'dlg-row'}, nodeSelect(n.goto, v => { if (v) n.goto = v; else delete n.goto; }), jump(n.goto)));
  if (!n.choices.length) delete n.choices;
  ed.append(h('h4', {}, ''), h('button', {class: 'small danger', onclick: () => {
    if (!confirm('Delete node ' + n.id + '? Gotos that name it will show as errors.')) return;
    d.nodes.splice(d.nodes.indexOf(n), 1); st.node = '#file'; touch(); render();
  }}, 'delete this node'));
  return ed;
}

// ---- the host's hooks ------------------------------------------------------------
export function attach(section) {
  st.root = section;
  window.addEventListener('keydown', e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 's' && section.classList.contains('active')) {
      e.preventDefault(); e.stopPropagation(); save();
    }
  }, true);
}

export function activate() {
  if (!st.loaded && !st.error) load();
  else render();
}

// For a headless check: the state after load.
export function _state() { return st; }
