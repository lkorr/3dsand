/* schedules.js — the Schedules page of the Environment tab (PLAN_world_editor P7).
 *
 * A villager's day: assets/schedules/<name>.json (game/schedule.h is the
 * format's truth). The page lists the files, shows the selected one as a
 * TABLE — from / to time pickers, the activity dropdown, `at` (a role, a tag or
 * a ref id; free text with suggestions), radius, note — with the same gap and
 * overlap checks the engine runs, a raw-JSON view for anything the table does
 * not show, and Save / Save as / New. Files go through /api/model (the server
 * allowlists schedules/). Press R in the game to reload; the in-game References
 * page (npc selected) edits the same files and shows the villager living it.
 *
 * WRITER. `writeSchedule` produces the canonical layout the C++ writer
 * (schedule::Write) produces — extra top-level keys first, then one row per
 * line, keys from, to, do, at, radius, note — so saving here and saving in the
 * game give byte-identical files.
 */

const ACTS = ['sleep', 'work', 'wander', 'socialize', 'eat', 'goto'];
const ACT_HELP = {
  sleep: 'walk to the anchor (a bed) and lie down on it',
  work: 'stand at the anchor facing its yaw',
  wander: 'stroll between random points within `radius` metres of the anchor',
  socialize: 'stand at the anchor facing the nearest other villager',
  eat: 'stand at the anchor (home: its hearth) facing its yaw',
  goto: 'go there and stand',
};

let H = null;
let els = {};
let list = [];          // names
let name = '';          // open file ('' = new, unsaved)
let doc = null;         // {extra: {...}, rows: [...]}
let dirty = false;

export function parseClock(s) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(String(s || '').trim());
  if (!m) return -1;
  const h = +m[1], mi = +m[2];
  if (h > 24 || mi > 59 || (h === 24 && mi !== 0)) return -1;
  return (h * 60 + mi) % 1440;
}
export function clockText(min) {
  min = ((min % 1440) + 1440) % 1440;
  return String(Math.floor(min / 60)).padStart(2, '0') + ':' + String(min % 60).padStart(2, '0');
}
function covers(r, m) {
  const f = parseClock(r.from), t = parseClock(r.to);
  if (f < 0 || t < 0 || f === t) return false;
  return f < t ? (m >= f && m < t) : (m >= f || m < t);
}
/** The engine's checks (schedule::Validate + the parser's row checks). */
export function validate(d) {
  const out = [];
  d.rows.forEach((r, i) => {
    if (parseClock(r.from) < 0) out.push(`rows[${i}].from: want a time like "06:30"`);
    if (parseClock(r.to) < 0) out.push(`rows[${i}].to: want a time like "06:30"`);
    if (!ACTS.includes(r.do)) out.push(`rows[${i}].do: '${r.do}' is not an activity`);
  });
  let gap = -1;
  for (let m = 0; m <= 1440; m++) {
    const c = m < 1440 && d.rows.some(r => covers(r, m));
    if (!c && m < 1440 && gap < 0) gap = m;
    if ((c || m === 1440) && gap >= 0) {
      out.push(`gap ${clockText(gap)}-${m === 1440 ? '24:00' : clockText(m)}: no row covers it, so the villager idles at home`);
      gap = -1;
    }
  }
  for (let a = 0; a < d.rows.length; a++)
    for (let b = a + 1; b < d.rows.length; b++)
      for (let m = 0; m < 1440; m++)
        if (covers(d.rows[a], m) && covers(d.rows[b], m)) {
          out.push(`rows[${b}] overlaps rows[${a}] (from ${clockText(m)}): the earlier row wins there`);
          break;
        }
  return out;
}

function inline(v) {
  if (Array.isArray(v)) return '[' + v.map(inline).join(', ') + ']';
  if (v && typeof v === 'object') {
    const ks = Object.keys(v);
    return ks.length ? '{ ' + ks.map(k => JSON.stringify(k) + ': ' + inline(v[k])).join(', ') + ' }' : '{}';
  }
  return JSON.stringify(v);
}
/** Canonical text (mirrors schedule::Write). */
export function writeSchedule(d) {
  let o = '{\n';
  for (const [k, v] of Object.entries(d.extra || {})) o += '  ' + JSON.stringify(k) + ': ' + inline(v) + ',\n';
  o += '  "rows": [';
  d.rows.forEach((r, i) => {
    o += i ? ',\n    ' : '\n    ';
    const f = parseClock(r.from), t = parseClock(r.to);
    o += '{ "from": "' + clockText(f) + '", "to": "' + (t === 0 && f !== 0 ? '24:00' : clockText(t)) +
         '", "do": ' + JSON.stringify(r.do);
    if (r.at) o += ', "at": ' + JSON.stringify(r.at);
    if (r.radius !== undefined && r.radius !== '' && r.radius !== null) o += ', "radius": ' + (+r.radius);
    if (r.note) o += ', "note": ' + JSON.stringify(r.note);
    for (const [k, v] of Object.entries(r.extra || {})) o += ', ' + JSON.stringify(k) + ': ' + inline(v);
    o += ' }';
  });
  o += d.rows.length ? '\n  ]\n}\n' : ']\n}\n';
  return o;
}
export function readSchedule(text) {
  const j = JSON.parse(text);
  const d = {extra: {}, rows: []};
  for (const [k, v] of Object.entries(j)) if (k !== 'rows' && k !== 'name') d.extra[k] = v;
  for (const r of (Array.isArray(j.rows) ? j.rows : [])) {
    const row = {from: r.from || '00:00', to: r.to || '00:00', do: r.do || 'goto', at: r.at || '',
                 radius: r.radius, note: r.note || '', extra: {}};
    for (const [k, v] of Object.entries(r))
      if (!['from', 'to', 'do', 'at', 'radius', 'note'].includes(k)) row.extra[k] = v;
    d.rows.push(row);
  }
  return d;
}

function setDirty(d) {
  dirty = d;
  if (H && H.onDirty) H.onDirty(d);
  if (els.save) els.save.textContent = d ? 'Save *' : 'Save';
}

async function refreshList() {
  try {
    const r = await (await fetch('/api/models', {cache: 'no-store'})).json();
    list = (r.files || []).filter(f => f.dir === 'schedules' && f.name.endsWith('.json'))
      .map(f => f.name.slice(0, -5)).sort();
  } catch (e) { list = []; }
  paintList();
}

async function openFile(n) {
  const r = await fetch('/api/model?path=schedules/' + encodeURIComponent(n) + '.json', {cache: 'no-store'});
  if (!r.ok) { H.toast('could not read schedules/' + n + '.json', true); return; }
  try { doc = readSchedule(await r.text()); }
  catch (e) { H.toast('schedules/' + n + '.json is not valid JSON: ' + e.message, true); return; }
  name = n;
  setDirty(false);
  paintList();
  paintDoc();
}

async function save(n) {
  if (!n || !/^[a-z0-9_-]+$/.test(n)) { H.toast('a schedule name is lowercase letters, digits, _ and -', true); return; }
  const text = writeSchedule(doc);
  const r = await (await fetch('/api/model?path=schedules/' + n + '.json', {method: 'POST', body: text})).json();
  if (!r.ok) { H.toast('save failed: ' + (r.error || '?'), true); return; }
  name = n;
  setDirty(false);
  H.toast('saved schedules/' + n + '.json — press R in the game');
  await refreshList();
  paintDoc();
}

function paintList() {
  const el = H.el;
  els.list.innerHTML = '';
  for (const n of list) {
    const b = el('button', {class: n === name ? 'on' : ''}, n);
    b.addEventListener('click', () => {
      if (dirty && !confirm('Discard unsaved changes to ' + (name || 'the new schedule') + '?')) return;
      openFile(n);
    });
    els.list.append(b);
  }
  if (!list.length) els.list.append(el('div', {class: 'hint'}, 'no schedules yet'));
}

function paintDoc() {
  const el = H.el;
  const box = els.body;
  box.innerHTML = '';
  if (!doc) { box.append(el('p', {class: 'hint'}, 'Pick a schedule on the left, or make a new one.')); return; }
  box.append(el('h3', {}, name ? 'schedules/' + name + '.json' : 'new schedule (unsaved)'));
  const about = el('input', {type: 'text', value: (doc.extra.about || ''), placeholder: 'about: whose day is this?', style: 'width:100%'});
  about.addEventListener('input', () => { doc.extra.about = about.value; if (!about.value) delete doc.extra.about; setDirty(true); });
  box.append(about);
  const tbl = el('table', {class: 'schedtbl'});
  tbl.append(el('tr', {}, el('th', {}, 'from'), el('th', {}, 'to'), el('th', {}, 'do'), el('th', {}, 'at'),
                el('th', {}, 'radius m'), el('th', {}, 'note'), el('th', {}, '')));
  const dl = el('datalist', {id: 'schedAtList'});
  for (const s of ['home', 'bed', 'work', 'gather', 'hearth']) dl.append(el('option', {value: s}));
  box.append(dl);
  doc.rows.forEach((r, i) => {
    const tr = el('tr', {});
    const time = (k) => {
      const inp = el('input', {type: 'time', value: clockText(Math.max(0, parseClock(r[k])))});
      inp.addEventListener('change', () => { r[k] = inp.value || '00:00'; setDirty(true); paintChecks(); });
      return inp;
    };
    const act = el('select', {title: ACT_HELP[r.do] || ''});
    for (const a of ACTS) { const o = el('option', {value: a}, a); if (a === r.do) o.selected = true; act.append(o); }
    act.addEventListener('change', () => { r.do = act.value; act.title = ACT_HELP[r.do]; setDirty(true); paintChecks(); });
    const at = el('input', {type: 'text', value: r.at, placeholder: 'home', list: 'schedAtList',
                            title: 'WHERE: a role (a prop on the npc naming a ref: bed, work, home, or one you invent), a TAG (the nearest marker wearing it, e.g. gather), or a ref id (harrowby/green_well).'});
    at.addEventListener('input', () => { r.at = at.value.trim(); setDirty(true); });
    const rad = el('input', {type: 'number', step: '0.5', min: '0', value: r.radius ?? '', placeholder: '3', style: 'width:60px'});
    rad.addEventListener('input', () => { r.radius = rad.value === '' ? undefined : +rad.value; setDirty(true); });
    const note = el('input', {type: 'text', value: r.note, placeholder: 'note'});
    note.addEventListener('input', () => { r.note = note.value; setDirty(true); });
    const up = el('button', {title: 'move up'}, '↑');
    up.addEventListener('click', () => { if (i > 0) { [doc.rows[i - 1], doc.rows[i]] = [doc.rows[i], doc.rows[i - 1]]; setDirty(true); paintDoc(); } });
    const del = el('button', {title: 'delete row'}, '×');
    del.addEventListener('click', () => { doc.rows.splice(i, 1); setDirty(true); paintDoc(); });
    tr.append(el('td', {}, time('from')), el('td', {}, time('to')), el('td', {}, act), el('td', {}, at),
              el('td', {}, rad), el('td', {}, note), el('td', {}, up, del));
    tbl.append(tr);
  });
  box.append(tbl);
  const add = el('button', {}, '+ row');
  add.addEventListener('click', () => {
    const last = doc.rows[doc.rows.length - 1];
    const f = last ? parseClock(last.to) : 8 * 60;
    doc.rows.push({from: clockText(f), to: clockText(f + 120), do: 'goto', at: '', note: '', extra: {}});
    setDirty(true);
    paintDoc();
  });
  box.append(add);
  els.checks = el('div', {class: 'schedchecks'});
  box.append(els.checks);
  paintChecks();
  const raw = el('details', {}, el('summary', {}, 'raw JSON (what Save writes)'));
  const pre = el('pre', {class: 'schedraw'}, writeSchedule(doc));
  raw.addEventListener('toggle', () => { pre.textContent = writeSchedule(doc); });
  raw.append(pre);
  box.append(raw);
  box.append(el('p', {class: 'hint'},
    'Give a villager this schedule with props.schedule on its npc ref (F1 → World → References). ',
    'Its roles (bed, work, home, ...) are props on that ref naming other refs. See docs/EDITOR_GUIDE.md, ',
    '“How do I give an NPC a home and a day”.'));
}

function paintChecks() {
  if (!els.checks) return;
  const probs = validate(doc);
  els.checks.innerHTML = '';
  if (!probs.length) els.checks.append(H.el('div', {class: 'ok'}, 'every minute of the day is covered, no overlaps'));
  for (const p of probs) els.checks.append(H.el('div', {class: 'warn'}, p));
}

const CSS = `
#env-schedules.active{display:flex;gap:12px;overflow:hidden}
#env-schedules .schednav{width:170px;flex:0 0 170px;display:flex;flex-direction:column;gap:3px;overflow-y:auto}
#env-schedules .schednav button{text-align:left;padding:4px 8px;border:1px solid #2a3040;background:#141922;color:#c7d2e3;border-radius:4px;cursor:pointer;font-size:12px}
#env-schedules .schednav button.on{background:#1f2a3d;border-color:#2b4a6f;color:#fff}
#env-schedules .schedbody{flex:1;overflow-y:auto;font-size:12px;color:#c7d2e3}
#env-schedules .schedtbl{border-collapse:collapse;margin:8px 0}
#env-schedules .schedtbl td,#env-schedules .schedtbl th{padding:2px 4px;border-bottom:1px solid #222a38;text-align:left}
#env-schedules .schedchecks .warn{color:#ffb454;font:11px monospace}
#env-schedules .schedchecks .ok{color:#7fd48a;font:11px monospace}
#env-schedules .schedraw{font:11px monospace;background:#10141c;padding:6px;border-radius:4px;white-space:pre}
`;

export function attach(hooks) {
  H = hooks;
  const el = H.el;
  const root = H.section;
  const style = document.createElement('style');
  style.textContent = CSS;
  document.head.append(style);
  const nav = el('div', {class: 'schednav'});
  els.list = el('div', {style: 'display:flex;flex-direction:column;gap:3px'});
  const neu = el('button', {}, '+ new schedule');
  neu.addEventListener('click', () => {
    if (dirty && !confirm('Discard unsaved changes?')) return;
    name = '';
    doc = {extra: {about: ''}, rows: [
      {from: '22:00', to: '06:00', do: 'sleep', at: 'bed', note: '', extra: {}},
      {from: '06:00', to: '22:00', do: 'work', at: 'work', note: '', extra: {}}]};
    setDirty(true);
    paintList();
    paintDoc();
  });
  els.save = el('button', {}, 'Save');
  els.save.addEventListener('click', () => (name ? save(name) : els.saveAs.click()));
  els.saveAs = el('button', {}, 'Save as…');
  els.saveAs.addEventListener('click', () => {
    if (!doc) return;
    const n = prompt('Schedule name (lowercase, e.g. smith):', name || '');
    if (n) save(n.trim());
  });
  nav.append(neu, els.save, els.saveAs, el('h4', {}, 'schedules'), els.list);
  els.body = el('div', {class: 'schedbody'});
  root.append(nav, els.body);
  paintDoc();
}

export function activate() { refreshList(); }
export function saveFromHost() { if (doc) (name ? save(name) : els.saveAs.click()); }
export function isDirty() { return dirty; }
export async function open(n) { await refreshList(); await openFile(n); }
