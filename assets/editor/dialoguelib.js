// dialoguelib.js - the conversation files' data layer for the tuner's Dialogue
// tab (docs/PLAN_world_editor.md P3). PURE and Node-runnable: parse, validate,
// write. No DOM. scripts/test_dialogue.mjs runs it over assets/dialogue/.
//
// THE SCHEMA IS game/dialogue.h's, and the checks mirror dialogue.cpp's
// Library::Validate so the tab and the game say the same thing about a file:
//   errors   - dangling goto / else / entry, duplicate node id, unknown
//              condition or action key, unknown item, bad time, empty choice
//   warnings - unreachable node, flag written but never read (and read but
//              never written; flags are world-wide so this is over ALL files),
//              unknown activity, `met` naming no file, more than 9 choices
//
// THE WRITER is the one canonical layout every saved file has: stable key
// order, one entry / choice per line, one key per line inside a node. A file
// written here and a file typed by hand in that layout diff cleanly.

export const COND_KINDS = ['flag', '!flag', 'time', 'activity', '!activity',
                           'has', '!has', 'met', '!met'];
// grant (a glyph by name) and learn (a demon's name -> flag name:<x>): demons D2,
// game/dialogue.h. The tuner has no glyph list, so a grant is checked by the engine.
// present_contract / release / dismiss (value true): demons D5, a DEMON's
// conversation only (game/demon_talk.h).
export const ACT_KINDS = ['set', 'add', 'clear', 'give', 'take', 'end', 'grant', 'learn',
                          'present_contract', 'release', 'dismiss'];
const TRUE_ACTS = ['end', 'present_contract', 'release', 'dismiss'];
export const ACTIVITIES = ['sleep', 'work', 'wander', 'socialize', 'eat', 'goto'];

const FILE_KEYS = ['name', 'speaker', 'canLeave', 'notes', 'entry', 'nodes'];
const NODE_KEYS = ['id', 'speaker', 'canLeave', 'if', 'else', 'text', 'do', 'goto',
                   'choices', 'notes', 'pos'];
const CHOICE_KEYS = ['text', 'goto', 'if', 'do', 'notes'];
const ENTRY_KEYS = ['if', 'goto'];

// ---- reading a condition / action -------------------------------------------

// The one key of a condition or action object that is not a modifier.
export function condKind(c) {
  if (!c || typeof c !== 'object') return null;
  const ks = Object.keys(c).filter(k => k !== 'value' && k !== 'count');
  return ks.length === 1 ? ks[0] : null;
}
export const actKind = condKind;

function parseClock(s) {
  const m = /^(\d{1,2}):(\d{2})$/.exec(String(s || ''));
  if (!m) return -1;
  const h = +m[1], mi = +m[2];
  if (h > 24 || mi > 59 || (h === 24 && mi)) return -1;
  return h * 60 + mi;
}

// ---- validation --------------------------------------------------------------

// files: [{name, data}] where data is the parsed JSON (or {__error})
// items: array of item names (null = do not check items)
// Returns [{error, file, node, field, msg}], `node` is a node id or '' and
// `where` is the display string ("node 'x'" / "entry[1]").
export function validateAll(files, items) {
  const out = [];
  const itemSet = items ? new Set(items) : null;
  const names = new Set(files.map(f => f.name));
  const written = new Map(), read = new Map(), metRefs = [];
  for (const f of files) {
    const file = f.name + '.json';
    const P = (error, where, field, msg, node = '') =>
      out.push({error, file, where, node, field, msg});
    const d = f.data;
    if (!d || d.__error) { P(true, '', '', 'not valid JSON: ' + (d && d.__error)); continue; }
    if (typeof d !== 'object' || Array.isArray(d)) { P(true, '', '', 'the file must be one JSON object'); continue; }
    if (d.name !== undefined && d.name !== f.name)
      P(false, '', 'name', `"${d.name}" differs from the file name "${f.name}"; the FILE name is what everything refers to it by`);
    for (const k of Object.keys(d)) if (!FILE_KEYS.includes(k)) P(false, '', k, 'unknown key (kept, ignored)');
    if (d.canLeave !== undefined && typeof d.canLeave !== 'boolean') P(true, '', 'canLeave', 'is true or false');
    const nodes = Array.isArray(d.nodes) ? d.nodes : [];
    const ids = new Map();
    nodes.forEach((n, i) => {
      if (n && typeof n.id === 'string' && n.id) {
        if (ids.has(n.id)) P(true, `node '${n.id}'`, 'id', 'is used by two nodes', n.id);
        else ids.set(n.id, n);
      } else P(true, `nodes[${i}]`, 'id', 'every node needs an id');
    });
    const target = (where, field, id, node) => {
      if (id !== undefined && id !== '' && !ids.has(id))
        P(true, where, field, `goes to '${id}', which is not a node in this file`, node);
    };
    const conds = (where, field, cs, node) => {
      if (cs === undefined) return;
      if (!Array.isArray(cs)) { P(true, where, field, 'must be a list of conditions', node); return; }
      cs.forEach((c, i) => {
        const fi = `${field}[${i}]`;
        const k = condKind(c);
        if (!k) { P(true, where, fi, 'a condition has exactly one key, e.g. {"flag": "x"}', node); return; }
        const base = k.replace(/^!/, ''), v = c[k];
        if (!['flag', 'time', 'activity', 'has', 'met'].includes(base)) {
          P(true, where, fi + '.' + k, 'unknown condition (flag, !flag, time, activity, has, met)', node); return;
        }
        if (base === 'flag') {
          if (typeof v !== 'string' || !v) P(true, where, fi + '.' + k, 'names a flag', node);
          else if (!read.has(v)) read.set(v, `${file} ${where}`);
        } else if (base === 'time') {
          const ok = Array.isArray(v) && v.length === 2 && parseClock(v[0]) >= 0 && parseClock(v[1]) >= 0;
          if (!ok) P(true, where, fi + '.time', 'is ["HH:MM", "HH:MM"] (from, to; wraps midnight)', node);
        } else if (base === 'activity') {
          if (typeof v !== 'string' || !v) P(true, where, fi + '.' + k, 'names an activity', node);
          else if (!ACTIVITIES.includes(v)) P(false, where, fi + '.activity', `'${v}' is not a known activity (${ACTIVITIES.join(', ')})`, node);
        } else if (base === 'has') {
          if (typeof v !== 'string' || !v) P(true, where, fi + '.' + k, 'names an item', node);
          else if (itemSet && !itemSet.has(v)) P(true, where, fi + '.has', `'${v}' is not an item (assets/items/items.json)`, node);
        } else if (base === 'met') {
          if (typeof v === 'string' && v) metRefs.push([v, `${file} ${where} ${fi}`]);
          else if (typeof v !== 'boolean') P(true, where, fi + '.' + k, 'is true (this dialogue) or a dialogue name', node);
        }
        if (c.value !== undefined && (base !== 'flag' || !Number.isInteger(c.value)))
          P(true, where, fi + '.value', 'is an integer, and only on a flag condition', node);
        if (c.count !== undefined && (base !== 'has' || !Number.isInteger(c.count) || c.count < 1))
          P(true, where, fi + '.count', 'is a positive integer, and only on a has condition', node);
      });
    };
    const acts = (where, field, as, node) => {
      if (as === undefined) return;
      if (!Array.isArray(as)) { P(true, where, field, 'must be a list of actions', node); return; }
      as.forEach((a, i) => {
        const fi = `${field}[${i}]`;
        const k = actKind(a);
        if (!k || !ACT_KINDS.includes(k)) {
          P(true, where, fi + (k ? '.' + k : ''), 'unknown action (set, add, clear, give, take, end, grant, learn, present_contract, release, dismiss)', node); return;
        }
        const v = a[k];
        if (TRUE_ACTS.includes(k)) { if (v !== true) P(true, where, fi + '.' + k, 'is true', node); return; }
        if (typeof v !== 'string' || !v) { P(true, where, fi + '.' + k, (k === 'give' || k === 'take') ? 'names an item' : k === 'grant' ? 'names a glyph' : k === 'learn' ? 'names a name' : 'names a flag', node); return; }
        if (k === 'grant' || k === 'learn') {
          if (k === 'learn' && !written.has('name:' + v)) written.set('name:' + v, `${file} ${where}`);
          if (a.value !== undefined || a.count !== undefined) P(true, where, fi, k + ' takes no value or count', node);
        } else if (k === 'give' || k === 'take') {
          if (itemSet && !itemSet.has(v)) P(true, where, fi, `'${v}' is not an item (assets/items/items.json)`, node);
          if (a.value !== undefined) P(true, where, fi + '.value', 'give/take use "count"', node);
          if (a.count !== undefined && (!Number.isInteger(a.count) || a.count < 1)) P(true, where, fi + '.count', 'is a positive integer', node);
        } else {
          if (!written.has(v)) written.set(v, `${file} ${where}`);
          if (a.count !== undefined) P(true, where, fi + '.count', 'set/add use "value"', node);
          if (a.value !== undefined && (k === 'clear' || !Number.isInteger(a.value))) P(true, where, fi + '.value', 'is an integer, not on clear', node);
        }
      });
    };
    const entry = Array.isArray(d.entry) ? d.entry : null;
    if (!entry || !entry.length) P(true, '', 'entry', 'needs at least one entry, e.g. [ {"goto": "start"} ]');
    (entry || []).forEach((e, i) => {
      const where = `entry[${i}]`;
      if (!e || !e.goto) P(true, where, 'goto', 'names the node this entry starts at');
      target(where, 'goto', e && e.goto);
      conds(where, 'if', e && e.if);
    });
    if (!nodes.length) P(true, '', 'nodes', 'needs at least one node');
    // Reachability over goto / else / choice.goto from every entry.
    const reach = new Set(), todo = (entry || []).map(e => e && e.goto);
    while (todo.length) {
      const id = todo.pop();
      const n = ids.get(id);
      if (!n || reach.has(id)) continue;
      reach.add(id);
      todo.push(n.goto, n.else);
      for (const c of (Array.isArray(n.choices) ? n.choices : [])) todo.push(c && c.goto);
    }
    for (const n of nodes) {
      if (!n || !n.id) continue;
      const where = `node '${n.id}'`, id = n.id;
      for (const k of Object.keys(n)) if (!NODE_KEYS.includes(k)) P(false, where, k, 'unknown key (kept, ignored)', id);
      if (!n.text) P(false, where, 'text', 'is empty', id);
      target(where, 'goto', n.goto, id);
      target(where, 'else', n.else, id);
      if (n.else && !(Array.isArray(n.if) && n.if.length)) P(false, where, 'else', 'has no "if" to fail, so it is never taken', id);
      if (n.canLeave !== undefined && typeof n.canLeave !== 'boolean') P(true, where, 'canLeave', 'is true or false', id);
      conds(where, 'if', n.if, id);
      acts(where, 'do', n.do, id);
      const cs = n.choices === undefined ? [] : n.choices;
      if (!Array.isArray(cs)) P(true, where, 'choices', 'is a list', id);
      else {
        cs.forEach((c, k) => {
          const f = `choices[${k}]`;
          if (!c || typeof c !== 'object') { P(true, where, f, 'a choice is an object with "text"', id); return; }
          for (const key of Object.keys(c)) if (!CHOICE_KEYS.includes(key)) P(false, where, f + '.' + key, 'unknown key (kept, ignored)', id);
          if (!c.text) P(true, where, f + '.text', 'a choice needs text', id);
          target(where, f + '.goto', c.goto, id);
          conds(where, f + '.if', c.if, id);
          acts(where, f + '.do', c.do, id);
        });
        if (cs.length > 9) P(false, where, 'choices', 'has more than 9; only 1-9 have keys (the rest are reachable by mouse)', id);
      }
      if (!reach.has(n.id)) P(false, where, '', 'cannot be reached from any entry', id);
    }
  }
  for (const [f, at] of written)
    if (!read.has(f)) out.push({error: false, file: '(all dialogue)', where: '', node: '', field: `flag '${f}'`, msg: `is set (first in ${at}) but no condition reads it`});
  for (const [f, at] of read)
    // demon:* flags are set by the engine before a demon's conversation (demons D5).
    if (!written.has(f) && !f.startsWith('demon:')) out.push({error: false, file: '(all dialogue)', where: '', node: '', field: `flag '${f}'`, msg: `is read (first in ${at}) but nothing sets it`});
  for (const [n, at] of metRefs)
    if (!names.has(n)) out.push({error: false, file: '(all dialogue)', where: '', node: '', field: `met '${n}'`, msg: `names no dialogue file (${at})`});
  return out;
}

export function problemLine(p) {
  let s = p.file;
  if (p.where) s += ': ' + p.where;
  if (p.field) s += (p.where ? ' ' : ': ') + p.field;
  return (p.error ? 'error: ' : 'warning: ') + s + ': ' + p.msg;
}

// Every flag named anywhere (written or read), for the tab's dropdowns.
export function knownFlags(files) {
  const s = new Set();
  const scanC = cs => (Array.isArray(cs) ? cs : []).forEach(c => {
    const k = condKind(c); if (k && k.replace(/^!/, '') === 'flag' && typeof c[k] === 'string') s.add(c[k]);
  });
  const scanA = as => (Array.isArray(as) ? as : []).forEach(a => {
    const k = actKind(a); if (['set', 'add', 'clear'].includes(k) && typeof a[k] === 'string') s.add(a[k]);
    if (k === 'learn' && typeof a[k] === 'string') s.add('name:' + a[k]);
  });
  for (const f of files) {
    const d = f.data; if (!d || d.__error) continue;
    (d.entry || []).forEach(e => scanC(e && e.if));
    (d.nodes || []).forEach(n => {
      if (!n) return;
      scanC(n.if); scanA(n.do);
      (n.choices || []).forEach(c => { if (c) { scanC(c.if); scanA(c.do); } });
    });
  }
  return [...s].sort();
}

// ---- the graph ------------------------------------------------------------------

// Edges for the graph view: {from, to, label, kind: 'goto'|'else'|'choice'|'entry'}
export function edges(d) {
  const out = [];
  (d.entry || []).forEach((e, i) => { if (e && e.goto) out.push({from: '#entry', to: e.goto, label: 'e' + (i + 1), kind: 'entry'}); });
  for (const n of (d.nodes || [])) {
    if (!n || !n.id) continue;
    (n.choices || []).forEach((c, k) => { if (c && c.goto) out.push({from: n.id, to: c.goto, label: String(k + 1), kind: 'choice'}); });
    if (n.goto) out.push({from: n.id, to: n.goto, label: '>', kind: 'goto'});
    if (n.else) out.push({from: n.id, to: n.else, label: 'else', kind: 'else'});
  }
  return out;
}

// Columns by breadth-first depth from the entries; nodes nothing reaches go in
// a column of their own at the end. Returns Map id -> {col, row}.
export function layout(d) {
  const nodes = (d.nodes || []).filter(n => n && n.id);
  const ids = new Set(nodes.map(n => n.id));
  const depth = new Map();
  let frontier = [];
  for (const e of (d.entry || [])) if (e && ids.has(e.goto) && !depth.has(e.goto)) { depth.set(e.goto, 0); frontier.push(e.goto); }
  const byId = new Map(nodes.map(n => [n.id, n]));
  while (frontier.length) {
    const next = [];
    for (const id of frontier) {
      const n = byId.get(id);
      const outs = [...(n.choices || []).map(c => c && c.goto), n.goto, n.else];
      for (const t of outs) if (t && ids.has(t) && !depth.has(t)) { depth.set(t, depth.get(id) + 1); next.push(t); }
    }
    frontier = next;
  }
  let maxD = 0;
  for (const v of depth.values()) maxD = Math.max(maxD, v);
  const pos = new Map(), rows = [];
  for (const n of nodes) {
    const col = depth.has(n.id) ? depth.get(n.id) : maxD + 1;
    rows[col] = (rows[col] || 0);
    pos.set(n.id, {col, row: rows[col]++, unreachable: !depth.has(n.id)});
  }
  return pos;
}

// ---- the writer -----------------------------------------------------------------

function orderKeys(o, order) {
  const ks = Object.keys(o);
  return [...order.filter(k => ks.includes(k)), ...ks.filter(k => !order.includes(k))];
}

// A value on one line: `{ "a": 1, "b": [ "x", "y" ] }`, `[]`, `"s"`.
export function inline(v) {
  if (Array.isArray(v)) return v.length ? '[ ' + v.map(inline).join(', ') + ' ]' : '[]';
  if (v && typeof v === 'object') {
    const ks = Object.keys(v);
    if (!ks.length) return '{}';
    // A condition/action: its kind key first, then value/count.
    const kind = condKind(v);
    const order = kind ? [kind, 'value', 'count'] : ENTRY_KEYS.concat(CHOICE_KEYS);
    return '{ ' + orderKeys(v, order).map(k => JSON.stringify(k) + ': ' + inline(v[k])).join(', ') + ' }';
  }
  return JSON.stringify(v);
}

function inlineOrdered(o, order) {
  return '{ ' + orderKeys(o, order).map(k => JSON.stringify(k) + ': ' + inline(o[k])).join(', ') + ' }';
}

export function serialize(d) {
  const L = ['{'];
  const top = orderKeys(d, FILE_KEYS);
  top.forEach((k, ti) => {
    const last = ti === top.length - 1, comma = last ? '' : ',';
    if (k === 'entry' && Array.isArray(d.entry)) {
      L.push('  "entry": [');
      d.entry.forEach((e, i) => L.push('    ' + inlineOrdered(e, ENTRY_KEYS) + (i < d.entry.length - 1 ? ',' : '')));
      L.push('  ]' + comma);
    } else if (k === 'nodes' && Array.isArray(d.nodes)) {
      L.push('  "nodes": [');
      d.nodes.forEach((n, i) => {
        L.push('    {');
        const nk = orderKeys(n, NODE_KEYS);
        nk.forEach((key, j) => {
          const c2 = j < nk.length - 1 ? ',' : '';
          if (key === 'choices' && Array.isArray(n.choices) && n.choices.length) {
            L.push('      "choices": [');
            n.choices.forEach((c, m) => L.push('        ' + inlineOrdered(c, CHOICE_KEYS) + (m < n.choices.length - 1 ? ',' : '')));
            L.push('      ]' + c2);
          } else {
            L.push('      ' + JSON.stringify(key) + ': ' + inline(n[key]) + c2);
          }
        });
        L.push('    }' + (i < d.nodes.length - 1 ? ',' : ''));
      });
      L.push('  ]' + comma);
    } else {
      L.push('  ' + JSON.stringify(k) + ': ' + inline(d[k]) + comma);
    }
  });
  L.push('}');
  return L.join('\n') + '\n';
}

// A blank conversation, valid as written.
export function blank(name) {
  return {name, speaker: '', canLeave: true, entry: [{goto: 'start'}],
          nodes: [{id: 'start', text: 'Hello.', choices: [{text: 'Goodbye.'}]}]};
}

// A node id not yet used in `d`, from a stem.
export function freshId(d, stem = 'node') {
  const used = new Set((d.nodes || []).map(n => n && n.id));
  if (!used.has(stem)) return stem;
  for (let i = 2; ; i++) if (!used.has(stem + '_' + i)) return stem + '_' + i;
}

// Rename a node and every reference to it in the file.
export function renameNode(d, from, to) {
  for (const e of (d.entry || [])) if (e && e.goto === from) e.goto = to;
  for (const n of (d.nodes || [])) {
    if (!n) continue;
    if (n.id === from) n.id = to;
    if (n.goto === from) n.goto = to;
    if (n.else === from) n.else = to;
    for (const c of (n.choices || [])) if (c && c.goto === from) c.goto = to;
  }
}
