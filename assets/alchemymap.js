/* alchemymap.js — the tuner's Alchemy tab: a reaction map of every substance.
 *
 * WHAT IT IS. A directed graph with an arrow for every way one substance turns
 * into another, each arrow labelled and coloured by what makes it happen
 * (heat, electrify, douse, corrode, sunlight, night, contact with X, ...).
 * Hover anything to isolate it, click a substance for its rules, double-click
 * to focus the map on its neighbourhood.
 *
 * WHY IT CANNOT GO STALE. Nothing here is authored. The graph is rebuilt on
 * every visit from the SAME data the engine compiles at load:
 *
 *   reactions.json   every rule (pair / decay / emit), the tuner's live `rx` —
 *                    so an unsaved edit on the Reactions tab shows up here too;
 *   materials.json   names, class, colours, tags (the tuner's live `mat`), plus
 *                    the `molten` (heat brush) and `rubble` (shatter) products;
 *   solutes.json     dissolving and the `converts` rows (fetched read-only from
 *                    the static root; the map degrades without it).
 *
 * The world grid and the alchemy bench run that same table (benchchem.h:
 * "SEMANTICS ARE THE WORLD'S"), so this is the chemistry of both. The one bench
 * difference — a `drying` rule never fires inside a vessel — is in the label.
 *
 * Edge kinds are decided from the OTHER party of a rule, by tag, never by a
 * material name: a partner tagged `hot` is heat, `electric` is electrify,
 * `caustic` is corrode, an `extinguisher` on a burning (hot) self is douse, an
 * `infectious` partner (or `infectSpread`) is infect, anything else is contact
 * with that partner. Light gates turn a decay into sunlight / night, `rain`
 * into rain. A new material or rule needs nothing added here.
 *
 * LAYOUT. The first version drew this with Mermaid (dagre + spline curves) and
 * it was unreadable: labels floated between edges, and every back-edge looped
 * around the whole diagram. It is now laid out by ELK's layered algorithm
 * (assets/vendor/elk-*.js, elkjs 0.12.0, EPL-2.0, run in a web worker) and
 * drawn here as SVG, because that buys the four things readability needed:
 *
 *   GROUPS.  Substances are clustered by who reacts with whom (Louvain
 *            modularity over the arrows, hubs down-weighted so fire does not
 *            glue everything into one lump). A group is a box, named by its
 *            best-connected members; it is DERIVED, so a new reaction regroups.
 *   COPIES.  An arrow never leaves its group: when its far end lives in
 *            another group it points at a dashed copy of that substance inside
 *            this one (a recipe pulls in copies of outside ingredients). So each
 *            group is laid out on its own and the boxes are packed; hovering a
 *            copy lights the original, clicking it jumps there. In isolate mode
 *            the focused substance gets one copy per reaction — a recipe book.
 *   RECIPES. Two substances that COMBINE (water + fairy dust -> enchanted
 *            water) meet at a junction pill; an unconsumed catalyst enters it
 *            on a dashed line.
 *   LABELS ON THEIR EDGE. ELK reserves space for every label as part of the
 *            layout, so no label sits on a node or another label; each label is
 *            then snapped onto its own line and outlined in its arrow's colour.
 *   STRAIGHT ROUTES. Network-simplex node placement minimises edge length, and
 *            routing is orthogonal (default), straight polyline, or splines.
 *
 * Plus level of detail (labels hide when zoomed far out unless lit), a
 * minimap, and "copy mermaid" still exports the graph as Mermaid source.
 */
'use strict';

const ALCH_KINDS = {
  heat:     {name: 'heat',                 icon: '🔥', color: '#ff7a3d'},
  electric: {name: 'electrify',            icon: '⚡', color: '#ffe14d'},
  douse:    {name: 'douse',                icon: '💧', color: '#5aa9ff'},
  corrode:  {name: 'corrode',              icon: '🧪', color: '#9be15d'},
  infect:   {name: 'infect',               icon: '☣', color: '#c58cff'},
  contact:  {name: 'contact / mix',        icon: '✚', color: '#d7dde6'},
  sun:      {name: 'sunlight / day',       icon: '☀', color: '#ffc861'},
  night:    {name: 'night',                icon: '☾', color: '#8f9cff'},
  rain:     {name: 'rain',                 icon: '☔', color: '#4fd1c5'},
  time:     {name: 'decay over time',      icon: '⏳', color: '#8a94a6'},
  emit:     {name: 'emits (source stays)', icon: '↗', color: '#f08fd6'},
  dissolve: {name: 'dissolves into',       icon: '◌', color: '#62d0ff'},
  saturate: {name: 'saturated solution',   icon: '◉', color: '#2ee6a8'},
  melt:     {name: 'melt (heat brush)',    icon: '♨', color: '#ff4d4d'},
  shatter:  {name: 'shatter (rubble)',     icon: '✱', color: '#b39a7a'},
};
// Kinds that are TOOL mechanics rather than chemistry start hidden.
const ALCH_DEFAULT_OFF = new Set(['melt', 'shatter']);
const ALCH_CLASS_GLYPH = {solid: '■', powder: '⁖', liquid: '≈', gas: '☁'};

let alchState = null;        // UI state, survives tab switches
let alchSolutes = undefined; // undefined = not fetched, null = unavailable
let alchElkP = null;
let alchRenderSeq = 0;
let alchLastSrc = '';        // mermaid export of what is on screen
let alchLastKey = '';        // layout cache key
let alchGraph = null;        // the last built graph, for the side panel
let alchScene = null;        // what is drawn: nodes/edges with geometry + indices

function alchInitState() {
  if (alchState) return alchState;
  const kinds = {};
  for (const k in ALCH_KINDS) kinds[k] = !ALCH_DEFAULT_OFF.has(k);
  alchState = {
    kinds, showAir: false, showInert: false, expandTags: false, dir: 'LR',
    focus: '', depth: 2, focusDir: 'both', selected: '',
    groups: true, routing: 'ORTHOGONAL',
    zoom: 1, panX: 0, panY: 0,
  };
  // Deep link: ?tab=alchemy&focus=water&depth=1&dir=TB&route=SPLINES — the
  // view is reachable (and checkable headless) without a click.
  const q = new URLSearchParams(typeof location !== "undefined" ? location.search : "");
  if (q.get('focus')) alchState.focus = alchState.selected = q.get('focus');
  if (q.get('depth')) alchState.depth = +q.get('depth') || 2;
  if (q.get('dir') === 'TB' || q.get('dir') === 'LR') alchState.dir = q.get('dir');
  if (['ORTHOGONAL', 'POLYLINE', 'SPLINES'].includes(q.get('route'))) alchState.routing = q.get('route');
  if (q.get('groups') === '0') alchState.groups = false;
  if (q.get('select')) alchState.selected = q.get('select');
  return alchState;
}

function alchLoadScript(src) {
  return new Promise((res, rej) => {
    const s = document.createElement('script');
    s.src = src; s.onload = res; s.onerror = () => rej(new Error('could not load ' + src));
    document.head.appendChild(s);
  });
}

// ELK runs in a web worker so a big layout never freezes the page.
function alchLoadElk() {
  if (alchElkP) return alchElkP;
  alchElkP = (window.ELK ? Promise.resolve() : alchLoadScript('vendor/elk-api.js'))
    .then(() => new window.ELK({workerUrl: 'vendor/elk-worker.min.js'}))
    .catch(e => { alchElkP = null; throw e; });
  return alchElkP;
}

async function alchLoadSolutes() {
  if (alchSolutes !== undefined) return alchSolutes;
  try {
    const r = await fetch('materials/solutes.json', {cache: 'no-store'});
    alchSolutes = r.ok ? await r.json() : null;
  } catch { alchSolutes = null; }
  return alchSolutes;
}

/* ---------------- graph extraction (pure: data in, nodes + edges out) ------- */

// The graph has two kinds of arrow:
//
//   LINK   one substance becomes another under a CONDITION: time, heat,
//          electrify, douse, sunlight, night, rain, or contact with a whole
//          TAG of materials (seed + any soil -> sprout). One labelled arrow.
//   RECIPE two named substances COMBINE: water + fairy dust -> enchanted
//          water, sodium + water -> hydrogen + lye. Drawn as a junction node
//          both ingredients flow into and the products flow out of. An
//          ingredient the rule leaves unchanged (silver turning water into
//          moonwater) is a CATALYST and enters on a dashed line.
//
// The split is by rule shape, not by name: a pair rule is a recipe unless the
// unchanged partner is a tag / `any`, or acts only as a condition (heat,
// electrify, douse, light, rain). Solute `converts` rows are recipes (solvent
// + dissolved powder at a concentration).
const ALCH_COND_KINDS = new Set(['heat', 'electric', 'douse', 'sun', 'night', 'rain', 'time']);

function alchBuildGraph(matJson, rxJson, solJson, st) {
  const mats = (matJson?.materials || []).filter(m => m && typeof m.id === 'string');
  const byId = new Map(mats.map(m => [m.id, m]));
  const isAbstract = ref => ref === 'any' || ref.startsWith('tag:');
  const tagsOf = ref => {
    if (!ref) return [];
    if (ref.startsWith('tag:')) return [ref.slice(4)];
    return byId.get(ref)?.tags || [];
  };
  const membersOf = tag => mats.filter(m => (m.tags || []).includes(tag)).map(m => m.id);
  const unknown = new Set();
  const noteRef = ref => {
    if (ref && ref !== 'air' && !isAbstract(ref) && !byId.has(ref)) unknown.add(ref);
  };
  const nice = ref => ref === 'any' ? 'anything' : ref.replace(/_/g, ' ');

  // LINKS keyed by from|to|kind; each carries the labels of every rule it merges.
  const links = new Map();
  const addLink = (from, to, kind, label, rule, extra = {}) => {
    if (!from || !to || from === to) return;
    noteRef(from); noteRef(to);
    const key = from + '|' + to + '|' + kind;
    let e = links.get(key);
    if (!e) { e = {type: 'link', from, to, kind, labels: [], rules: [], dotted: false, thick: false}; links.set(key, e); }
    if (!e.labels.includes(label)) e.labels.push(label);
    if (rule) e.rules.push(rule);
    if (extra.dotted) e.dotted = true;
    if (extra.thick) e.thick = true;
  };
  // RECIPES keyed by kind + ingredients + products, so the same combination
  // written as several rules (different chances, gates) is one junction.
  const joins = new Map();
  const addJoin = (kind, inputs, outputs, gate, rule, extra = {}) => {
    outputs = [...new Set(outputs.filter(Boolean))];
    inputs = inputs.filter(i => i.id);
    if (!outputs.length || inputs.length < 2) return;
    for (const i of inputs) noteRef(i.id);
    for (const o of outputs) noteRef(o);
    const key = kind + '|' + inputs.map(i => i.id + (i.consumed ? '' : '*')).sort().join('+') + '>' + [...outputs].sort().join('+');
    let j = joins.get(key);
    if (!j) { j = {type: 'join', key, kind, inputs, outputs, gates: [], rules: [], thick: false}; joins.set(key, j); }
    if (gate && !j.gates.includes(gate)) j.gates.push(gate);
    if (rule) j.rules.push(rule);
    if (extra.thick) j.thick = true;
  };

  // What kind of change does the partner `agent` cause in `subject`?
  const kindFrom = (agent, subject, rule) => {
    const at = tagsOf(agent), st2 = tagsOf(subject);
    if (rule.infectSpread || at.includes('infectious')) return 'infect';
    if (at.includes('hot')) return 'heat';
    if (at.includes('electric')) return 'electric';
    if (at.includes('caustic')) return 'corrode';
    if (at.includes('extinguisher') && st2.includes('hot')) return 'douse';
    return 'contact';
  };
  // `kind` already says sunlight / night, so the gate list does not repeat it.
  const gateList = (r, kind) => {
    const g = [];
    if (r.when === 'day' && kind !== 'sun') g.push(r.minLight ? 'bright day' : 'day');
    else if (r.when === 'day' && r.minLight >= 200) g.push('bright');
    if (r.when === 'night' && kind !== 'night') g.push('night');
    if (r.needsSky) g.push('open sky');
    if (r.rain && kind !== 'rain') g.push('rain');
    if (r.requires) g.push(r.requires);
    if (r.solute) g.push(r.solute + (r.cMin != null ? ' ≥' + r.cMin : '') + (r.cMax != null ? ' ≤' + r.cMax : ''));
    if (r.drying) g.push('world only');
    for (const fx of r.effects || []) g.push(fx.kind === 'explode' ? '💥 explodes' : fx.kind === 'flash' ? '✴ flash' : fx.kind);
    return g.join(', ');
  };
  const withGates = (text, gates) => gates ? text + ' · ' + gates : text;
  const lightKind = (r, base) => {
    if (base !== 'contact' && base !== 'time') return base;
    if (r.rain) return 'rain';
    if (r.when === 'day') return 'sun';
    if (r.when === 'night') return 'night';
    return base;
  };
  const kindLabel = (kind, agent) => {
    switch (kind) {
      case 'heat': return agent && !isAbstract(agent) ? 'heat (' + nice(agent) + ')' : 'heat';
      case 'electric': return 'electrify';
      case 'douse': return agent ? 'douse (' + nice(agent) + ')' : 'douse';
      case 'corrode': return 'corrode (' + nice(agent) + ')';
      case 'infect': return 'infect (' + nice(agent) + ')';
      case 'sun': return agent ? 'sunlight + ' + nice(agent) : 'sunlight';
      case 'night': return agent ? 'night + ' + nice(agent) : 'night';
      case 'rain': return 'rain';
      default: return 'with ' + nice(agent);
    }
  };
  const expand = ref => st.expandTags && ref.startsWith('tag:') ? membersOf(ref.slice(4)) : [ref];
  const fx = r => ({thick: !!(r.effects || []).length});

  for (const r of rxJson?.reactions || []) {
    if (!r || typeof r.self !== 'string') continue;
    const S = r.self;
    if (r.emit) {
      addLink(S, r.emit, 'emit', withGates('emits' + (r.dir && r.dir !== 'any' ? ' ' + ({up: '↑', down: '↓', side: '↔'}[r.dir] || r.dir) : ''), gateList(r, 'emit')), r, {dotted: true});
      continue;
    }
    if (r.decay) {
      const kind = lightKind(r, 'time');
      const lbl = kind === 'time' ? (r.drying ? 'dries' : r.burnDuration ? 'burns out' : 'decays') : kindLabel(kind, null);
      addLink(S, r.becomes, kind, withGates(lbl, gateList(r, kind)), r, fx(r));
      continue;
    }
    if (r.neighbor == null) continue;
    const N = r.neighbor, sb = r.selfBecomes, nb = r.neighborBecomes;
    if (sb && nb) {
      // Both parties change: always a recipe (lava + water -> stone + steam).
      for (const n of expand(N)) {
        const kind = lightKind(r, kindFrom(n, S, r));
        addJoin(kind, [{id: S, consumed: true}, {id: n, consumed: true}], [sb, nb], gateList(r, kind), r, fx(r));
      }
    } else if (sb) {
      // S changes, N is unchanged: a condition, or a catalyst.
      const kind = lightKind(r, kindFrom(N, S, r));
      if (isAbstract(N) || ALCH_COND_KINDS.has(kind)) addLink(S, sb, kind, withGates(kindLabel(kind, N), gateList(r, kind)), r, fx(r));
      else addJoin(kind, [{id: S, consumed: true}, {id: N, consumed: false}], [sb], gateList(r, kind), r, fx(r));
    } else if (nb) {
      // N changes, S is the unchanged agent.
      for (const n of expand(N)) {
        if (n === nb) continue;
        const kind = lightKind(r, kindFrom(S, n, r));
        if (ALCH_COND_KINDS.has(kind)) addLink(n, nb, kind, withGates(kindLabel(kind, S), gateList(r, kind)), r, fx(r));
        else addJoin(kind, [{id: n, consumed: true}, {id: S, consumed: false}], [nb], gateList(r, kind), r, fx(r));
      }
    }
  }

  for (const m of mats) {
    if (m.molten) addLink(m.id, m.molten, 'melt', 'melt', null);
    if (m.rubble) addLink(m.id, m.rubble, 'shatter', 'shatter', null);
  }

  for (const sp of solJson?.species || []) {
    if (!sp || !sp.from) continue;
    const converted = new Set((sp.converts || []).map(c => c.solvent));
    for (const solvent of sp.solvents || [])
      if (!converted.has(solvent))
        addLink(sp.from, solvent, 'dissolve', 'dissolves in (≤' + sp.saturation + ')', {solute: sp.name, kind: 'dissolve', sp}, {dotted: true});
    for (const c of sp.converts || [])
      addJoin('saturate', [{id: c.solvent, consumed: true}, {id: sp.from, consumed: true}], [c.into],
              'dissolved ≥' + c.cMin, {solute: sp.name, kind: 'converts', sp, c});
  }

  const allLinks = [...links.values()], allJoins = [...joins.values()];

  // Filter: kind toggles, then `air` (consumed) — a recipe keeps its other
  // products and disappears only if air was all it made.
  let L = allLinks.filter(e => st.kinds[e.kind]);
  let J = allJoins.filter(j => st.kinds[j.kind]);
  if (!st.showAir) {
    L = L.filter(e => e.to !== 'air' && e.from !== 'air');
    J = J.map(j => ({...j, outputs: j.outputs.filter(o => o !== 'air')})).filter(j => j.outputs.length);
  }
  // Focus: the focused substance's LINEAGE. Downstream = what it becomes,
  // then what those become; upstream = what makes it, then what makes those.
  // The two walks never mix, so "gunpowder, 2 steps" is gunpowder -> fire ->
  // smoke, not gunpowder -> fire <- every other fuel in the game. Only the
  // reactions a walk actually USED are kept, and a kept recipe keeps all its
  // ingredients (half a recipe is misinformation) — but a co-ingredient is
  // not walked on from.
  if (st.focus) {
    const steps = [];
    for (const e of L) steps.push({ins: [e.from], outs: [e.to], ref: e});
    for (const j of J) steps.push({ins: j.inputs.map(i => i.id), outs: j.outputs, ref: j});
    const used = new Set();
    const walk = down => {
      const seen = new Set([st.focus]);
      let frontier = [st.focus];
      for (let d = 0; d < st.depth && frontier.length; d++) {
        const f = new Set(frontier), next = [];
        for (const s of steps) {
          if (!(down ? s.ins : s.outs).some(x => f.has(x))) continue;
          used.add(s.ref);
          // "salt dissolves in water" / "a vine emits a flower": the far end is
          // not MADE from this one, so it is shown but not walked on from —
          // otherwise salt's lineage is all of water's chemistry.
          const through = !(s.ref.kind === 'dissolve' || s.ref.kind === 'emit');
          for (const x of down ? s.outs : s.ins) if (!seen.has(x)) { seen.add(x); if (through) next.push(x); }
        }
        frontier = next;
      }
    };
    if (st.focusDir !== 'up') walk(true);
    if (st.focusDir !== 'down') walk(false);
    L = L.filter(e => used.has(e));
    J = J.filter(j => used.has(j));
  }

  const nodeIds = new Set();
  for (const e of L) { nodeIds.add(e.from); nodeIds.add(e.to); }
  for (const j of J) { j.inputs.forEach(i => nodeIds.add(i.id)); j.outputs.forEach(o => nodeIds.add(o)); }
  if (st.focus && byId.has(st.focus)) nodeIds.add(st.focus);
  const touched = new Set();
  for (const e of allLinks) { touched.add(e.from); touched.add(e.to); }
  for (const j of allJoins) { j.inputs.forEach(i => touched.add(i.id)); j.outputs.forEach(o => touched.add(o)); }
  const inert = mats.map(m => m.id).filter(id => !touched.has(id));
  if (st.showInert && !st.focus) for (const id of inert) nodeIds.add(id);

  return {links: L, joins: J, allLinks, allJoins, nodeIds: [...nodeIds], byId, membersOf,
          inert, unknown: [...unknown], nice};
}

/* ---------------- grouping: Louvain modularity, deterministic ---------------- */

// Returns a community index per id. Hub edges are down-weighted so the few
// substances everything burns / boils / dissolves into do not glue the whole
// map into one group. Ties break to the lowest community index, and ids are
// visited in the order given, so the same data always gives the same groups.
function alchCommunities(ids, pairs, hubs, gamma = 1) {
  const ix = new Map(ids.map((id, i) => [id, i]));
  let adj = ids.map(() => new Map());
  for (const [a0, b0, w0] of pairs) {
    const a = ix.get(a0), b = ix.get(b0);
    if (a == null || b == null || a === b) continue;
    const w = (w0 || 1) * (hubs.has(a0) || hubs.has(b0) ? 0.15 : 1);
    adj[a].set(b, (adj[a].get(b) || 0) + w);
    adj[b].set(a, (adj[b].get(a) || 0) + w);
  }
  let member = ids.map((_, i) => i);
  for (let lvl = 0; lvl < 4; lvl++) {
    const n = adj.length;
    const k = adj.map(m => { let s = 0; for (const w of m.values()) s += w; return s; });
    const m2 = k.reduce((a, b) => a + b, 0);
    if (!m2) break;
    const comm = [...Array(n).keys()], tot = k.slice();
    for (let pass = 0, moved = true; moved && pass < 40; pass++) {
      moved = false;
      for (let i = 0; i < n; i++) {
        const ci = comm[i];
        tot[ci] -= k[i];
        const w = new Map();
        for (const [j, wij] of adj[i]) if (j !== i) w.set(comm[j], (w.get(comm[j]) || 0) + wij);
        let best = ci, bestGain = (w.get(ci) || 0) - gamma * tot[ci] * k[i] / m2;
        for (const [c, wc] of w) {
          const gain = wc - gamma * tot[c] * k[i] / m2;
          if (gain > bestGain + 1e-9 || (Math.abs(gain - bestGain) <= 1e-9 && c < best)) { best = c; bestGain = gain; }
        }
        comm[i] = best; tot[best] += k[i];
        if (best !== ci) moved = true;
      }
    }
    const ren = new Map();
    for (const c of comm) if (!ren.has(c)) ren.set(c, ren.size);
    if (ren.size === n) break;
    member = member.map(s => ren.get(comm[s]));
    const nadj = Array.from({length: ren.size}, () => new Map());
    for (let i = 0; i < n; i++)
      for (const [j, w] of adj[i]) {
        const a = ren.get(comm[i]), b = ren.get(comm[j]);
        nadj[a].set(b, (nadj[a].get(b) || 0) + w);
      }
    adj = nadj;
  }
  return member;
}

/* ---------------- the drawing model (pure; sizes via a measure callback) ---- */

const ALCH_HUB_DEGREE = 8;     // distinct partners before a substance gets copies
const ALCH_MAX_GROUP = 18;     // a bigger community is split again, on its own

function alchJoinText(j) {
  const k = ALCH_KINDS[j.kind];
  const head = j.kind === 'contact' ? '✚' : k.icon + ' ' + (j.kind === 'saturate' ? '' : k.name.split(' ')[0]);
  const gates = j.gates.length > 1 ? j.gates[0] + ' / …' : (j.gates[0] || '');
  return (head + (gates ? ' ' + gates : '')).trim();
}
function alchLinkText(e) {
  const k = ALCH_KINDS[e.kind];
  let t = k.icon + ' ' + e.labels[0];
  if (t.length > 34) t = t.slice(0, 33) + '…';
  return e.labels.length > 1 ? t + '  +' + (e.labels.length - 1) : t;
}

// Build the ELK input(s) + everything the renderer needs to map layout ids back.
//
// GROUPED (the default) produces one self-contained ELK graph PER GROUP: an
// arrow whose far end lives in another group points at a dashed COPY of that
// substance inside this group, and a recipe pulls copies of its outside
// ingredients / products into the group that owns it. No line ever crosses a
// group border, so each group is laid out on its own and the boxes are packed
// (alchPack). That is what killed the wandering lines: in one global layout,
// every cross-group arrow was routed around the outside of the whole map.
//
// FLAT is one layered graph of everything, no copies — the raw topology.
function alchModel(g, st, measure) {
  // material-level adjacency, for grouping and naming
  const pairs = [];
  for (const e of g.links) pairs.push([e.from, e.to, 1]);
  for (const j of g.joins) {
    for (const i of j.inputs) for (const o of j.outputs) pairs.push([i.id, o, 1]);
    if (j.inputs.length > 1) pairs.push([j.inputs[0].id, j.inputs[1].id, 0.5]);
  }
  const partners = new Map();
  for (const [a, b] of pairs) {
    if (!partners.has(a)) partners.set(a, new Set());
    if (!partners.has(b)) partners.set(b, new Set());
    partners.get(a).add(b); partners.get(b).add(a);
  }
  const degree = id => partners.get(id)?.size || 0;
  const ids = [...g.nodeIds].sort();
  const hubs = new Set(ids.filter(id => degree(id) >= ALCH_HUB_DEGREE));
  const grouped = st.groups && !st.focus;

  // groups: every community is a box (a lone substance with nothing to react
  // with goes into one shared "inert" box)
  const groupOf = new Map(), groups = [];
  if (grouped) {
    // Modularity over the whole graph leaves one giant family (everything wet
    // or burning); a family bigger than ALCH_MAX_GROUP is re-clustered on its
    // OWN subgraph, at rising resolution until it breaks, so a box stays small
    // enough to read. Its cross arrows become copies like any other.
    const split = (members, depth) => {
      if (members.length <= ALCH_MAX_GROUP || depth > 3) return [members];
      const inside = new Set(members);
      const sub = pairs.filter(([a, b]) => inside.has(a) && inside.has(b));
      for (const gamma of [1, 1.6, 2.5, 4]) {
        const c = alchCommunities(members, sub, hubs, gamma);
        const parts = new Map();
        members.forEach((id, i) => { if (!parts.has(c[i])) parts.set(c[i], []); parts.get(c[i]).push(id); });
        if (parts.size > 1) return [...parts.values()].flatMap(p => split(p, depth + 1));
      }
      return [members];
    };
    const comm = alchCommunities(ids, pairs, hubs);
    const top = new Map();
    const inertIds = [];
    ids.forEach((id, i) => {
      if (!degree(id)) { inertIds.push(id); return; }
      if (!top.has(comm[i])) top.set(comm[i], []);
      top.get(comm[i]).push(id);
    });
    const byC = new Map();
    let ci = 0;
    for (const members of top.values()) for (const part of split(members, 0)) byC.set(ci++, part);
    if (inertIds.length) byC.set('inert', inertIds);
    const cs = [...byC.entries()].sort((a, b) => (a[0] === 'inert') - (b[0] === 'inert') ||
      b[1].length - a[1].length || (a[1][0] < b[1][0] ? -1 : 1));
    cs.forEach(([c, members], gi) => {
      const inside = new Set(members);
      const score = id => [...(partners.get(id) || [])].filter(p => inside.has(p)).length;
      const top = [...members].sort((a, b) => score(b) - score(a) || (a < b ? -1 : 1)).slice(0, 3);
      groups.push({key: 'g:' + gi, index: gi, members, size: members.length,
                   name: c === 'inert' ? 'inert — reacts with nothing' : top.map(g.nice).join(' · ')});
      for (const m of members) groupOf.set(m, gi);
    });
  }

  const nodes = new Map();   // key -> drawn node
  const addNode = (key, n) => { if (!nodes.has(key)) nodes.set(key, {...n, key}); return key; };
  const textOf = id => id === 'air' ? '∅ air (gone)' : id === 'any' ? 'any loose matter' :
    id.startsWith('tag:') ? id + '  (' + g.membersOf(id.slice(4)).length + ')' : g.nice(id);
  const matKey = id => 'n:' + id;
  for (const id of ids) {
    const m = g.byId.get(id), text = textOf(id);
    const glyph = m ? (ALCH_CLASS_GLYPH[m.class] || '') : '';
    addNode(matKey(id), {kind: 'mat', id, text, glyph, group: groupOf.get(id), w: measure(text, 'node') + (glyph ? 34 : 22), h: 26});
  }
  // The node that stands for `id` inside group `gi`: the original, or a copy.
  const at = (id, gi) => {
    if (!grouped || groupOf.get(id) === gi) return matKey(id);
    const base = nodes.get(matKey(id));
    const text = base.text;
    return addNode('p:' + id + '@' + gi, {kind: 'mat', id, copy: true, text, glyph: base.glyph, group: gi,
      home: groupOf.get(id), w: measure(text, 'node') + (base.glyph ? 34 : 22), h: 24});
  };

  // ISOLATE MODE: the focused substance gets its own copy per reaction, so a
  // substance with forty reactions reads as forty rows of a recipe book
  // instead of a forty-spoke fan out of one box. Hovering lights them all.
  const focusSplit = !grouped && st.focus && g.byId.has(st.focus);
  const atF = (id, gi, tag) => {
    if (!focusSplit || id !== st.focus) return at(id, gi);
    const base = nodes.get(matKey(id));
    return addNode('f:' + tag, {...base, key: undefined, focusCopy: true});
  };
  const edges = [];   // drawn edges: {key, src, dst, kind, role, link?, join?, label?, group}
  g.links.forEach((e, i) => {
    // An arrow lives with its SOURCE: "what does this turn into" reads inside
    // the source's family; the product appears there as a copy if it lives
    // elsewhere.
    const gi = groupOf.get(e.from);
    const text = alchLinkText(e);
    edges.push({key: 'l' + i, src: atF(e.from, gi, 'l' + i), dst: atF(e.to, gi, 'l' + i), kind: e.kind, role: 'link', link: e,
                dotted: e.dotted, thick: e.thick, group: gi, label: {text, w: measure(text, 'label') + 14, h: 18}});
  });
  g.joins.forEach((j, i) => {
    // A recipe lives with its first PRODUCT: "how is this made" reads inside
    // the product's family.
    const gi = groupOf.get(j.outputs[0]);
    const text = alchJoinText(j);
    const jk = addNode('j:' + i, {kind: 'join', join: j, text, group: gi, w: measure(text, 'join') + 18, h: 22});
    j.inputs.forEach((inp, n) => edges.push({key: 'j' + i + 'i' + n, src: atF(inp.id, gi, 'j' + i), dst: jk, kind: j.kind,
      role: inp.consumed ? 'in' : 'catalyst', join: j, joinKey: jk, thick: j.thick, group: gi}));
    j.outputs.forEach((o, n) => edges.push({key: 'j' + i + 'o' + n, src: jk, dst: atF(o, gi, 'j' + i + 'o'), kind: j.kind,
      role: 'out', join: j, joinKey: jk, thick: j.thick, group: gi}));
  });

  if (focusSplit && [...nodes.values()].some(n => n.focusCopy)) nodes.delete(matKey(st.focus));

  const opts = {
    'elk.algorithm': 'layered',
    'elk.direction': st.dir === 'TB' ? 'DOWN' : 'RIGHT',
    'elk.edgeRouting': st.routing,
    // network simplex minimises total edge length: the straightest runs
    'elk.layered.nodePlacement.strategy': 'NETWORK_SIMPLEX',
    'elk.layered.crossingMinimization.strategy': 'LAYER_SWEEP',
    'elk.layered.thoroughness': '40',
    'elk.layered.cycleBreaking.strategy': 'GREEDY',
    'elk.layered.spacing.nodeNodeBetweenLayers': '40',
    'elk.spacing.nodeNode': '14',
    'elk.spacing.edgeNode': '12',
    'elk.spacing.edgeEdge': '8',
    'elk.layered.spacing.edgeNodeBetweenLayers': '14',
    'elk.layered.spacing.edgeEdgeBetweenLayers': '9',
    'elk.spacing.edgeLabel': '3',
    'elk.edgeLabels.inline': 'true',
    'elk.layered.mergeEdges': 'false',
    'elk.separateConnectedComponents': 'true',
    'elk.spacing.componentComponent': '30',
    'elk.padding': '[top=4,left=4,bottom=4,right=4]',
  };
  const graphOf = (id, keep) => {
    const G = {id, layoutOptions: opts, children: [], edges: []};
    for (const n of nodes.values()) if (keep(n)) G.children.push({id: n.key, width: n.w, height: n.h});
    for (const e of edges) if (keep(e))
      G.edges.push({id: e.key, sources: [e.src], targets: [e.dst],
        labels: e.label ? [{id: e.key + ':lb', text: e.label.text, width: e.label.w, height: e.label.h,
                            layoutOptions: {'elk.edgeLabels.placement': 'CENTER'}}] : []});
    return G;
  };
  if (grouped) for (const gr of groups) gr.elk = graphOf(gr.key, x => x.group === gr.index);
  const flat = grouped ? null : graphOf('root', () => true);
  return {grouped, nodes, edges, groups, hubs, flat};
}

// Pack laid-out group boxes into rows (largest first) at an aspect ratio near
// the viewport's. Deterministic, and a shelf packer keeps the reading order
// plain: biggest family top-left, the smallest ones at the end.
function alchPack(boxes, aspect) {
  const GAP = 36;
  const area = boxes.reduce((a, b) => a + (b.w + GAP) * (b.h + GAP), 0);
  const target = Math.max(Math.sqrt(area * aspect), ...boxes.map(b => b.w + GAP));
  let x = 0, y = 0, rowH = 0, W = 0;
  const order = [...boxes].sort((a, b) => b.h - a.h || b.w - a.w);
  for (const b of order) {
    if (x > 0 && x + b.w > target) { x = 0; y += rowH + GAP; rowH = 0; }
    b.x = x; b.y = y;
    x += b.w + GAP; rowH = Math.max(rowH, b.h); W = Math.max(W, x - GAP);
  }
  return {w: W, h: y + rowH};
}

/* ---------------- Mermaid export (the same graph, as Mermaid source) -------- */

const alchNodeKey = id => (id.startsWith('tag:') ? 't_' + id.slice(4) : id === 'air' ? 'z_air' : id === 'any' ? 'z_any' : 'n_' + id)
  .replace(/[^A-Za-z0-9_]/g, '_');
const alchEsc = s => String(s).replace(/"/g, '#quot;').replace(/</g, '#lt;').replace(/>/g, '#gt;');

function alchLuma(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
  if (!m) return 0;
  const v = parseInt(m[1], 16);
  return (0.2126 * (v >> 16 & 255) + 0.7152 * (v >> 8 & 255) + 0.0722 * (v & 255)) / 255;
}

function alchMermaidSource(g, st) {
  const L = ['flowchart ' + st.dir];
  const styles = [];
  for (const id of g.nodeIds) {
    const k = alchNodeKey(id), t = alchEsc(g.nice(id));
    const m = g.byId.get(id);
    const cls = m?.class;
    L.push(`  ${k}${cls === 'powder' ? `[/"${t}"/]` : cls === 'liquid' ? `(["${t}"])` : cls === 'gas' ? `>"${t}"]` : id.startsWith('tag:') || id === 'any' ? `{{"${t}"}}` : `["${t}"]`}`);
    const fill = (m?.colors || [])[1] || (m?.colors || [])[0] || '#333333';
    styles.push(`  style ${k} fill:${fill},color:${alchLuma(fill) > 0.5 ? '#10141a' : '#f2f5f9'}`);
  }
  const byKind = {};
  let n = 0;
  const push = (line, kind) => { L.push(line); (byKind[kind] ||= []).push(n++); };
  for (const e of g.links) {
    // Mermaid reads a quoted label as markdown: never start one with '+ '/'- '.
    const lbl = alchEsc(ALCH_KINDS[e.kind].icon + ' ' + e.labels.slice(0, 2).join(' / '));
    push(`  ${alchNodeKey(e.from)} ${e.thick ? '==>' : e.dotted ? '-.->' : '-->'}|"${lbl}"| ${alchNodeKey(e.to)}`, e.kind);
  }
  g.joins.forEach((j, i) => {
    const jk = 'j' + i;
    L.push(`  ${jk}(("${alchEsc(alchJoinText(j))}"))`);
    for (const inp of j.inputs) push(`  ${alchNodeKey(inp.id)} ${inp.consumed ? '---' : '-.-'} ${jk}`, j.kind);
    for (const o of j.outputs) push(`  ${jk} ${j.thick ? '==>' : '-->'} ${alchNodeKey(o)}`, j.kind);
  });
  for (const [k, idx] of Object.entries(byKind))
    for (let i = 0; i < idx.length; i += 200)
      L.push(`  linkStyle ${idx.slice(i, i + 200).join(',')} stroke:${ALCH_KINDS[k].color},color:${ALCH_KINDS[k].color}`);
  return L.concat(styles).join('\n');
}

/* ---------------- geometry helpers ---------------- */

function alchEdgePoints(le) {
  const pts = [];
  for (const s of le.sections || []) {
    if (!pts.length) pts.push(s.startPoint);
    for (const b of s.bendPoints || []) pts.push(b);
    pts.push(s.endPoint);
  }
  return pts;
}

// Orthogonal / polyline: straight segments with softly rounded corners.
function alchPathRounded(pts, r) {
  if (pts.length < 2) return '';
  let d = `M${pts[0].x},${pts[0].y}`;
  for (let i = 1; i < pts.length - 1; i++) {
    const p = pts[i - 1], c = pts[i], n = pts[i + 1];
    const d1 = Math.hypot(c.x - p.x, c.y - p.y), d2 = Math.hypot(n.x - c.x, n.y - c.y);
    const rr = Math.min(r, d1 / 2, d2 / 2);
    if (rr < 0.5) { d += `L${c.x},${c.y}`; continue; }
    const a = {x: c.x + (p.x - c.x) * rr / d1, y: c.y + (p.y - c.y) * rr / d1};
    const b = {x: c.x + (n.x - c.x) * rr / d2, y: c.y + (n.y - c.y) * rr / d2};
    d += `L${a.x},${a.y}Q${c.x},${c.y} ${b.x},${b.y}`;
  }
  const z = pts[pts.length - 1];
  return d + `L${z.x},${z.y}`;
}

// ELK spline sections: bend points are cubic control points in threes.
function alchPathSpline(pts) {
  if (pts.length < 2) return '';
  const inner = pts.length - 2;
  if (inner % 3 !== 2 && inner % 3 !== 0) return alchPathRounded(pts, 10);
  let d = `M${pts[0].x},${pts[0].y}`;
  let i = 1;
  for (; i + 2 < pts.length; i += 3) d += `C${pts[i].x},${pts[i].y} ${pts[i + 1].x},${pts[i + 1].y} ${pts[i + 2].x},${pts[i + 2].y}`;
  for (; i < pts.length; i++) d += `L${pts[i].x},${pts[i].y}`;
  return d;
}

// The point of polyline `pts` nearest to (x, y).
function alchNearest(pts, x, y) {
  let best = pts[0], bd = Infinity;
  for (let i = 0; i + 1 < pts.length; i++) {
    const a = pts[i], b = pts[i + 1];
    const dx = b.x - a.x, dy = b.y - a.y, L2 = dx * dx + dy * dy || 1;
    const t = Math.max(0, Math.min(1, ((x - a.x) * dx + (y - a.y) * dy) / L2));
    const p = {x: a.x + t * dx, y: a.y + t * dy};
    const d = (p.x - x) ** 2 + (p.y - y) ** 2;
    if (d < bd) { bd = d; best = p; }
  }
  return best;
}

/* ---------------- page ---------------- */

function alchEl(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'style') e.style.cssText = v;
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
    else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
  }
  for (const k of kids.flat()) if (k != null) e.append(k.nodeType ? k : String(k));
  return e;
}
const ALCH_SVGNS = 'http://www.w3.org/2000/svg';
function alchS(tag, attrs = {}, text) {
  const e = document.createElementNS(ALCH_SVGNS, tag);
  for (const [k, v] of Object.entries(attrs)) if (v != null) e.setAttribute(k, v);
  if (text != null) e.textContent = text;
  return e;
}

const ALCH_FONT = {
  node: '600 12.5px "Segoe UI",system-ui,sans-serif',
  label: '11px "Segoe UI","Segoe UI Emoji",system-ui,sans-serif',
  join: '600 11px "Segoe UI","Segoe UI Emoji",system-ui,sans-serif',
};
let alchMeasureCtx = null;
function alchMeasure(text, kind) {
  alchMeasureCtx ||= document.createElement('canvas').getContext('2d');
  alchMeasureCtx.font = ALCH_FONT[kind];
  return Math.ceil(alchMeasureCtx.measureText(text).width);
}

function alchEnsureDom(host) {
  if (host.querySelector('.alch-wrap')) return;
  const st = alchInitState();
  host.append(alchEl('style', {}, `
    #view-alchemy .alch-wrap{display:flex;gap:12px;height:calc(100vh - 150px);min-height:460px}
    #view-alchemy .alch-main{flex:1;display:flex;flex-direction:column;min-width:0}
    #view-alchemy .alch-bar{display:flex;flex-wrap:wrap;gap:6px 12px;align-items:center;margin-bottom:6px;font-size:12.5px}
    #view-alchemy .alch-bar label{display:inline-flex;gap:4px;align-items:center;cursor:pointer;color:var(--dim)}
    #view-alchemy .alch-bar .sep{width:1px;height:20px;background:var(--line)}
    #view-alchemy .alch-kinds{display:flex;flex-wrap:wrap;gap:3px 6px;margin-bottom:8px;font-size:12px}
    #view-alchemy .alch-kinds label{display:inline-flex;gap:5px;align-items:center;cursor:pointer;padding:2px 8px 2px 4px;border:1px solid var(--line);border-radius:12px;background:var(--bg2);user-select:none}
    #view-alchemy .alch-kinds label.off{opacity:.4}
    #view-alchemy .alch-kinds input{display:none}
    #view-alchemy .alch-kinds i{display:inline-block;width:16px;height:3px;border-radius:2px}
    #view-alchemy .alch-view{flex:1;position:relative;overflow:hidden;background:#0b0e13;border:1px solid var(--line);border-radius:6px;cursor:grab}
    #view-alchemy .alch-view.drag{cursor:grabbing}
    #view-alchemy .alch-canvas{position:absolute;left:0;top:0;transform-origin:0 0}
    #view-alchemy .alch-status{position:absolute;left:10px;top:8px;color:var(--dim);font-size:12px;pointer-events:none;z-index:3}
    #view-alchemy .alch-mini{position:absolute;right:10px;bottom:10px;border:1px solid var(--line2);background:rgba(14,17,22,.88);border-radius:4px;z-index:3;cursor:crosshair}
    #view-alchemy .alch-tip{position:absolute;z-index:4;pointer-events:none;background:#141922;border:1px solid var(--line2);border-radius:5px;padding:6px 8px;font-size:12px;max-width:420px;line-height:1.4;display:none;box-shadow:0 4px 16px rgba(0,0,0,.5)}
    #view-alchemy .alch-side{width:350px;flex:none;overflow:auto;background:var(--card);border:1px solid var(--line);border-radius:6px;padding:10px 12px;font-size:12.5px}
    #view-alchemy .alch-side h3{margin:0 0 4px;font-size:16px}
    #view-alchemy .alch-side h4{margin:14px 0 4px;color:var(--dim);font-weight:600;font-size:11.5px;text-transform:uppercase;letter-spacing:.05em}
    #view-alchemy .alch-rule{padding:4px 0;border-bottom:1px solid var(--line);line-height:1.4}
    #view-alchemy .alch-rule a{color:var(--acc);cursor:pointer}
    #view-alchemy .alch-rule .k{font-weight:600}
    #view-alchemy .alch-foot{color:var(--faint);font-size:11.5px;margin-top:4px;line-height:1.45}
    #view-alchemy input[type=search]{width:190px}
    /* the drawing */
    .alch-svg text{font-family:"Segoe UI","Segoe UI Emoji",system-ui,sans-serif}
    .alch-svg .grp rect{fill:rgba(110,168,254,.035);stroke:#2a3240;stroke-width:1.2}
    .alch-svg .grp text{fill:#8a94a6;font-size:13px;font-weight:600;letter-spacing:.02em}
    .alch-svg .e path.v{fill:none;stroke-width:1.7;stroke-linejoin:round}
    .alch-svg .e path.hit{fill:none;stroke:transparent;stroke-width:12;cursor:pointer}
    .alch-svg .e.thick path.v{stroke-width:3.2}
    .alch-svg .e.dotted path.v{stroke-dasharray:2 4;stroke-linecap:round}
    .alch-svg .e.catalyst path.v{stroke-dasharray:7 4}
    .alch-svg .lb rect{fill:#10141b;stroke-width:1.2}
    .alch-svg .lb text{font-size:11px;fill:#d7dde6}
    .alch-svg .lb{cursor:pointer}
    .alch-svg .n{cursor:pointer}
    .alch-svg .n rect{stroke:#05070a;stroke-width:1}
    .alch-svg .n.copy rect{stroke-dasharray:4 3;stroke-width:1.6;opacity:.8}
    .alch-svg .n.copy text{font-style:italic}
    .alch-svg .n.sel rect{stroke:#fff;stroke-width:3}
    .alch-svg .n.focus rect{stroke:#fff;stroke-width:3;stroke-dasharray:none}
    .alch-svg .n text{font-size:12.5px;font-weight:600}
    .alch-svg .j{cursor:pointer}
    .alch-svg .j rect{fill:#141922;stroke-width:1.8}
    .alch-svg .j text{font-size:11px;font-weight:600;fill:#eef2f7}
    /* hover / selection isolation */
    .alch-svg .e,.alch-svg .lb,.alch-svg .n,.alch-svg .j{transition:opacity .12s}
    .alch-svg.hl .e:not(.on),.alch-svg.hl .lb:not(.on){opacity:.07}
    .alch-svg.hl .n:not(.on),.alch-svg.hl .j:not(.on){opacity:.22}
    .alch-svg.hl .e.on path.v{stroke-width:2.6}
    .alch-svg.hl .e.on.thick path.v{stroke-width:4}
    /* level of detail: far out, the arrow labels would be unreadable noise */
    .alch-svg.far .lb:not(.on){display:none}
    .alch-svg.vfar .j:not(.on) text{display:none}
  `));

  const kindsBox = alchEl('div', {class: 'alch-kinds'});
  for (const [k, d] of Object.entries(ALCH_KINDS)) {
    const cb = alchEl('input', {type: 'checkbox'});
    cb.checked = st.kinds[k];
    const lab = alchEl('label', {title: 'show / hide ' + d.name + ' arrows'}, cb, alchEl('i', {style: 'background:' + d.color}), d.icon + ' ' + d.name);
    lab.classList.toggle('off', !cb.checked);
    cb.onchange = () => { st.kinds[k] = cb.checked; lab.classList.toggle('off', !cb.checked); alchRender(true); };
    kindsBox.append(lab);
  }

  const chk = (key, text, title) => {
    const cb = alchEl('input', {type: 'checkbox'});
    cb.checked = st[key];
    cb.onchange = () => { st[key] = cb.checked; alchRender(true); };
    return alchEl('label', {title}, cb, text);
  };
  const sel = (key, title, opts, num) => {
    const s = alchEl('select', {class: 'sortsel', title}, ...opts.map(([v, t]) => alchEl('option', {value: v}, t)));
    s.value = st[key];
    s.onchange = () => { st[key] = num ? +s.value : s.value; alchRender(true); };
    return s;
  };
  const findIn = alchEl('input', {type: 'search', id: 'alchFind', placeholder: 'find substance… (Enter)', list: 'alchMatList'});
  findIn.value = st.focus;
  findIn.onkeydown = ev => { if (ev.key === 'Enter') alchFind(findIn.value.trim()); };
  const bar = alchEl('div', {class: 'alch-bar'},
    findIn,
    alchEl('button', {title: 'show only this substance and its neighbourhood', onclick: () => {
      const v = findIn.value.trim() || st.selected;
      if (v) { st.focus = v; st.selected = v; alchRender(true); }
    }}, 'isolate'),
    sel('depth', 'steps out from the isolated substance', [1, 2, 3, 4, 6, 99].map(d => [d, d === 99 ? 'all steps' : d + ' step' + (d > 1 ? 's' : '')]), true),
    sel('focusDir', 'which way to follow', [['both', 'made from + turns into'], ['down', 'turns into'], ['up', 'made from']]),
    alchEl('button', {onclick: () => { st.focus = ''; findIn.value = ''; alchRender(true); }}, 'whole map'),
    alchEl('span', {class: 'sep'}),
    sel('routing', 'how arrows are routed', [['ORTHOGONAL', 'right-angle arrows'], ['POLYLINE', 'straight arrows'], ['SPLINES', 'curved arrows']]),
    sel('dir', 'layout direction', [['LR', 'left → right'], ['TB', 'top → bottom']]),
    chk('groups', 'groups', 'cluster substances that react with each other into self-contained boxes; an arrow to another group points at a dashed copy'),
    chk('showAir', '∅ → air', 'rules whose product is air: burning out, drying, fading'),
    chk('expandTags', 'expand tags', 'draw tag:xxx as one arrow per member material (much bigger)'),
    chk('showInert', 'inert', 'materials that take part in no transformation'),
    alchEl('span', {style: 'flex:1'}),
    alchEl('button', {onclick: alchFit}, 'fit'),
    alchEl('button', {onclick: () => { navigator.clipboard?.writeText(alchLastSrc); if (typeof toast === 'function') toast('mermaid source copied'); }}, 'copy mermaid'),
    alchEl('button', {onclick: alchDownloadSvg}, 'save SVG'));

  const view = alchEl('div', {class: 'alch-view', id: 'alchView'},
    alchEl('div', {class: 'alch-status', id: 'alchStatus'}),
    alchEl('div', {class: 'alch-canvas', id: 'alchCanvas'}),
    alchEl('canvas', {class: 'alch-mini', id: 'alchMini', width: 240, height: 150}),
    alchEl('div', {class: 'alch-tip', id: 'alchTip'}));
  alchBindPanZoom(view);

  const main = alchEl('div', {class: 'alch-main'}, bar, kindsBox, view, alchEl('div', {class: 'alch-foot', id: 'alchFoot'}));
  const side = alchEl('div', {class: 'alch-side', id: 'alchSide'});
  host.append(alchEl('div', {class: 'alch-wrap'}, main, side), alchEl('datalist', {id: 'alchMatList'}));
}

/* ---------------- pan / zoom / minimap ---------------- */

function alchApplyXform() {
  const st = alchState, c = document.getElementById('alchCanvas');
  if (!c) return;
  c.style.transform = `translate(${st.panX}px,${st.panY}px) scale(${st.zoom})`;
  const svg = c.querySelector('svg');
  if (svg) {
    svg.classList.toggle('far', st.zoom < 0.55);
    svg.classList.toggle('vfar', st.zoom < 0.3);
  }
  alchDrawMini();
}

function alchBindPanZoom(view) {
  let drag = null;
  view.addEventListener('wheel', ev => {
    ev.preventDefault();
    const st = alchState, r = view.getBoundingClientRect();
    const mx = ev.clientX - r.left, my = ev.clientY - r.top;
    const z = Math.min(4, Math.max(0.03, st.zoom * Math.exp(-ev.deltaY * 0.0015)));
    st.panX = mx - (mx - st.panX) * (z / st.zoom);
    st.panY = my - (my - st.panY) * (z / st.zoom);
    st.zoom = z; alchApplyXform();
  }, {passive: false});
  view.addEventListener('pointerdown', ev => {
    if (ev.button !== 0 || ev.target.id === 'alchMini') return;
    drag = {x: ev.clientX, y: ev.clientY, px: alchState.panX, py: alchState.panY, moved: false};
  });
  window.addEventListener('pointermove', ev => {
    if (!drag) return;
    const dx = ev.clientX - drag.x, dy = ev.clientY - drag.y;
    if (!drag.moved && Math.abs(dx) + Math.abs(dy) > 4) { drag.moved = true; view.classList.add('drag'); }
    if (drag.moved) { alchState.panX = drag.px + dx; alchState.panY = drag.py + dy; alchApplyXform(); }
  });
  window.addEventListener('pointerup', () => {
    if (!drag) return;
    view.classList.remove('drag');
    view.dataset.justDragged = drag.moved ? '1' : '';
    drag = null;
  });
  // A click on empty space clears the selection.
  view.addEventListener('click', ev => {
    if (view.dataset.justDragged) { view.dataset.justDragged = ''; return; }
    if (ev.target.closest('.n,.j,.e,.lb,.alch-mini')) return;
    if (alchState.selected) { alchState.selected = ''; alchHighlight(null); alchRenderSide(); }
  });
  // minimap: click or drag to move the view there
  const mini = view.querySelector('#alchMini');
  let mdrag = false;
  const miniGo = ev => {
    const sc = alchScene; if (!sc) return;
    const r = mini.getBoundingClientRect(), m = sc.mini;
    const wx = (ev.clientX - r.left - m.ox) / m.s, wy = (ev.clientY - r.top - m.oy) / m.s;
    const st = alchState;
    st.panX = view.clientWidth / 2 - wx * st.zoom; st.panY = view.clientHeight / 2 - wy * st.zoom;
    alchApplyXform();
  };
  mini.addEventListener('pointerdown', ev => { mdrag = true; ev.stopPropagation(); miniGo(ev); });
  window.addEventListener('pointermove', ev => { if (mdrag) miniGo(ev); });
  window.addEventListener('pointerup', () => { mdrag = false; });
}

function alchDrawMini() {
  const cv = document.getElementById('alchMini'), sc = alchScene, view = document.getElementById('alchView');
  if (!cv || !sc || !view) return;
  const W = cv.width, H = cv.height, ctx = cv.getContext('2d');
  const s = Math.min((W - 8) / sc.w, (H - 8) / sc.h);
  const ox = (W - sc.w * s) / 2, oy = (H - sc.h * s) / 2;
  sc.mini = {s, ox, oy};
  ctx.clearRect(0, 0, W, H);
  ctx.strokeStyle = '#39445a'; ctx.lineWidth = 1;
  for (const gr of sc.groupBoxes) ctx.strokeRect(ox + gr.x * s, oy + gr.y * s, gr.w * s, gr.h * s);
  for (const n of sc.nodeBoxes) {
    ctx.fillStyle = n.fill;
    ctx.fillRect(ox + n.x * s, oy + n.y * s, Math.max(1.5, n.w * s), Math.max(1.5, n.h * s));
  }
  const st = alchState;
  ctx.strokeStyle = '#6ea8fe'; ctx.lineWidth = 1.5;
  ctx.strokeRect(ox + (-st.panX / st.zoom) * s, oy + (-st.panY / st.zoom) * s,
                 view.clientWidth / st.zoom * s, view.clientHeight / st.zoom * s);
}

function alchFit() {
  const view = document.getElementById('alchView'), sc = alchScene;
  if (!view || !sc) return;
  const vw = view.clientWidth, vh = view.clientHeight;
  const z = Math.min(vw / sc.w, vh / sc.h, 1.4) * 0.97;
  Object.assign(alchState, {zoom: z, panX: (vw - sc.w * z) / 2, panY: (vh - sc.h * z) / 2});
  alchApplyXform();
}

// Centre the view on a substance (its original, not a hub copy) and select it.
function alchFind(q) {
  const sc = alchScene, st = alchState;
  if (!q || !sc) return;
  const ql = q.toLowerCase().replace(/ /g, '_');
  const hit = sc.nodesById.has(ql) ? ql : [...sc.nodesById.keys()].find(id => id.includes(ql));
  if (!hit) {
    // not on the current map (filtered out, or isolated elsewhere): isolate it
    st.focus = q; st.selected = q; alchRender(true); return;
  }
  const n = sc.nodesById.get(hit)[0];
  const view = document.getElementById('alchView');
  st.zoom = Math.max(st.zoom, 0.9);
  st.panX = view.clientWidth / 2 - (n.x + n.w / 2) * st.zoom;
  st.panY = view.clientHeight / 2 - (n.y + n.h / 2) * st.zoom;
  st.selected = hit;
  alchApplyXform(); alchHighlight(hit); alchRenderSide();
}

function alchDownloadSvg() {
  const svg = document.querySelector('#alchCanvas svg');
  if (!svg) return;
  const clone = svg.cloneNode(true);
  clone.classList.remove('hl', 'far', 'vfar');
  const css = [...document.querySelectorAll('#view-alchemy style')].map(s => s.textContent).join('\n');
  clone.insertBefore(alchS('style', {}, css), clone.firstChild);
  clone.insertBefore(alchS('rect', {x: -20, y: -20, width: '100%', height: '100%', fill: '#0b0e13'}), clone.firstChild);
  const blob = new Blob([clone.outerHTML], {type: 'image/svg+xml'});
  const a = alchEl('a', {href: URL.createObjectURL(blob), download: 'alchemy_reaction_map.svg'});
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

/* ---------------- drawing ---------------- */

// Lay the model out: each group on its own, then pack the boxes; or one flat
// graph. Returns ONE placement in absolute coordinates for the renderer.
const ALCH_GPAD = {l: 16, r: 16, t: 40, b: 16};
async function alchLayout(model, elk, aspect, measure) {
  const placed = {nodes: new Map(), edges: new Map(), groups: [], w: 0, h: 0};
  const take = (laid, ox, oy) => {
    for (const c of laid.children || []) placed.nodes.set(c.id, {x: c.x + ox, y: c.y + oy, width: c.width, height: c.height});
    for (const e of laid.edges || []) {
      const pts = alchEdgePoints(e).map(p => ({x: p.x + ox, y: p.y + oy}));
      const L = e.labels?.[0];
      placed.edges.set(e.id, {pts, label: L && L.x != null ? {x: L.x + ox, y: L.y + oy, width: L.width, height: L.height} : null});
    }
  };
  if (!model.grouped) {
    const laid = await elk.layout(model.flat);
    take(laid, 0, 0);
    placed.w = laid.width; placed.h = laid.height;
    return placed;
  }
  const boxes = [];
  for (const gr of model.groups) {
    if (!gr.elk.children.length) continue;
    const laid = await elk.layout(gr.elk);
    const title = gr.name + (gr.size > 3 ? '  ·  ' + gr.size : '');
    const w = Math.max(laid.width, measure(title, 'node') + 12) + ALCH_GPAD.l + ALCH_GPAD.r;
    boxes.push({gr, laid, title, w, h: laid.height + ALCH_GPAD.t + ALCH_GPAD.b});
  }
  const size = alchPack(boxes, aspect);
  for (const b of boxes) {
    take(b.laid, b.x + ALCH_GPAD.l, b.y + ALCH_GPAD.t);
    placed.groups.push({gr: b.gr, title: b.title, x: b.x, y: b.y, w: b.w, h: b.h});
  }
  placed.w = size.w; placed.h = size.h;
  return placed;
}

function alchDraw(model, placed, g, st) {
  const pos = placed.nodes;
  const W = Math.ceil(placed.w) + 40, H = Math.ceil(placed.h) + 40;
  const svg = alchS('svg', {class: 'alch-svg', width: W, height: H, viewBox: `-20 -20 ${W} ${H}`});
  const defs = alchS('defs');
  for (const [k, d] of Object.entries(ALCH_KINDS)) {
    const mk = alchS('marker', {id: 'alch-ah-' + k, viewBox: '0 0 10 10', refX: 9, refY: 5, markerWidth: 7, markerHeight: 7,
                               markerUnits: 'userSpaceOnUse', orient: 'auto-start-reverse'});
    mk.setAttribute('markerWidth', 9); mk.setAttribute('markerHeight', 9);
    mk.append(alchS('path', {d: 'M0,0.5 L10,5 L0,9.5 z', fill: d.color}));
    defs.append(mk);
  }
  svg.append(defs);

  const scene = {w: W - 40, h: H - 40, groupBoxes: [], nodeBoxes: [], nodesById: new Map(),
                 elOf: new Map(), edgesOf: new Map(), edgeEls: [], labelEls: new Map(), model};
  const gLayer = alchS('g'), eLayer = alchS('g'), nLayer = alchS('g'), lLayer = alchS('g');
  svg.append(gLayer, eLayer, nLayer, lLayer);

  for (const p of placed.groups) {
    scene.groupBoxes.push({x: p.x, y: p.y, w: p.w, h: p.h});
    const gg = alchS('g', {class: 'grp'});
    gg.append(alchS('rect', {x: p.x, y: p.y, width: p.w, height: p.h, rx: 12}));
    gg.append(alchS('text', {x: p.x + 16, y: p.y + 25}, p.title));
    gLayer.append(gg);
  }

  // edges first, so the index of every node's edges exists when nodes draw
  for (const e of model.edges) {
    const le = placed.edges.get(e.key);
    if (!le) continue;
    const pts = le.pts;
    if (pts.length < 2) continue;
    const d = st.routing === 'SPLINES' ? alchPathSpline(pts) : alchPathRounded(pts, 8);
    const color = ALCH_KINDS[e.kind].color;
    const cls = ['e', e.thick ? 'thick' : '', e.dotted ? 'dotted' : '', e.role === 'catalyst' ? 'catalyst' : ''].join(' ');
    const ge = alchS('g', {class: cls});
    const arrow = e.role === 'link' || e.role === 'out';
    ge.append(alchS('path', {class: 'v', d, stroke: color, 'marker-end': arrow ? `url(#alch-ah-${e.kind})` : null}));
    ge.append(alchS('path', {class: 'hit', d}));
    eLayer.append(ge);
    const rec = {e, el: ge, lb: null};
    scene.edgeEls.push(rec);
    for (const k of [e.src, e.dst]) { if (!scene.edgesOf.has(k)) scene.edgesOf.set(k, []); scene.edgesOf.get(k).push(rec); }
    if (e.label && le.label) {
      const L = le.label;
      // ELK reserves the label's space; snap its centre onto its OWN line so
      // there is never a question which arrow a label belongs to.
      const c = alchNearest(pts, L.x + L.width / 2, L.y + L.height / 2);
      const w = e.label.w, h = e.label.h;
      const gl = alchS('g', {class: 'lb'});
      gl.append(alchS('rect', {x: c.x - w / 2, y: c.y - h / 2, width: w, height: h, rx: 9, stroke: color}));
      gl.append(alchS('text', {x: c.x, y: c.y + 3.8, 'text-anchor': 'middle'}, e.label.text));
      lLayer.append(gl);
      rec.lb = gl;
      for (const x of [ge, gl]) {
        x.addEventListener('mouseenter', ev => { alchHighlight({edge: rec}); alchTip(ev, alchEdgeTip(e)); });
        x.addEventListener('mousemove', ev => alchTip(ev));
        x.addEventListener('mouseleave', () => { alchRestore(); alchTip(null); });
        x.addEventListener('click', ev => { ev.stopPropagation(); alchState.selected = e.link.from; alchHighlight(e.link.from); alchRenderSide(); });
      }
    } else if (e.join) {
      ge.addEventListener('mouseenter', ev => { alchHighlight({join: e.joinKey}); alchTip(ev, alchJoinTip(e.join, g)); });
      ge.addEventListener('mousemove', ev => alchTip(ev));
      ge.addEventListener('mouseleave', () => { alchRestore(); alchTip(null); });
    }
  }

  for (const n of model.nodes.values()) {
    const p = pos.get(n.key);
    if (!p) continue;
    let el;
    if (n.kind === 'join') {
      const color = ALCH_KINDS[n.join.kind].color;
      el = alchS('g', {class: 'j'});
      el.append(alchS('rect', {x: p.x, y: p.y, width: p.width, height: p.height, rx: p.height / 2, stroke: color}));
      el.append(alchS('text', {x: p.x + p.width / 2, y: p.y + p.height / 2 + 4, 'text-anchor': 'middle'}, n.text));
      el.addEventListener('mouseenter', ev => { alchHighlight({join: n.key}); alchTip(ev, alchJoinTip(n.join, g)); });
      el.addEventListener('mousemove', ev => alchTip(ev));
      el.addEventListener('mouseleave', () => { alchRestore(); alchTip(null); });
      scene.nodeBoxes.push({x: p.x, y: p.y, w: p.width, h: p.height, fill: color});
    } else {
      const m = g.byId.get(n.id);
      const abstract = n.id === 'air' || n.id === 'any' || n.id.startsWith('tag:');
      const fill = abstract ? '#1f2733' : (m?.colors || [])[1] || (m?.colors || [])[0] || '#444444';
      const fg = abstract ? '#d7dde6' : alchLuma(fill) > 0.5 ? '#10141a' : '#f4f6fa';
      el = alchS('g', {class: 'n' + (n.copy ? ' copy' : '') + (n.id === st.focus ? ' focus' : '')});
      el.append(alchS('rect', {x: p.x, y: p.y, width: p.width, height: p.height, rx: abstract ? 13 : 5, fill,
                               stroke: abstract ? '#6ea8fe' : m ? null : '#ff6b6b', 'stroke-dasharray': abstract ? '5 3' : null}));
      if (n.glyph) el.append(alchS('text', {x: p.x + 9, y: p.y + p.height / 2 + 4.5, fill: fg, opacity: 0.7, 'font-size': 12}, n.glyph));
      el.append(alchS('text', {x: p.x + (n.glyph ? 24 : 11), y: p.y + p.height / 2 + 4.5, fill: fg}, n.text));
      el.addEventListener('mouseenter', ev => {
        alchHighlight(n.id);
        alchTip(ev, `<b>${g.nice(n.id)}</b>${m ? ' <span style="color:#8a94a6">' + m.class + '</span>' : ''}` +
          (n.copy ? '<br><i style="color:#8a94a6">a copy — click to jump to the original</i>' : '') +
          '<br><span style="color:#8a94a6">click for its rules · double-click to isolate</span>');
      });
      el.addEventListener('mousemove', ev => alchTip(ev));
      el.addEventListener('mouseleave', () => { alchRestore(); alchTip(null); });
      el.addEventListener('click', ev => {
        ev.stopPropagation();
        if (n.copy) { alchFind(n.id); return; }   // a copy jumps to its original
        alchState.selected = n.id; alchHighlight(n.id); alchRenderSide();
      });
      el.addEventListener('dblclick', ev => {
        ev.stopPropagation();
        Object.assign(alchState, {focus: n.id, selected: n.id});
        document.getElementById('alchFind').value = n.id; alchRender(true);
      });
      if (!n.copy || !scene.nodesById.has(n.id)) {
        if (!scene.nodesById.has(n.id)) scene.nodesById.set(n.id, []);
        scene.nodesById.get(n.id)[n.copy ? 'push' : 'unshift']({x: p.x, y: p.y, w: p.width, h: p.height});
      }
      scene.nodeBoxes.push({x: p.x, y: p.y, w: p.width, h: p.height, fill});
    }
    scene.elOf.set(n.key, el);
    nLayer.append(el);
  }
  return {svg, scene};
}

// After a hover: back to the selection's highlight — except in isolate mode
// when the selection IS the isolated substance (the whole view is its).
function alchRestore() {
  const st = alchState;
  alchHighlight(st.selected && st.selected !== st.focus ? st.selected : null);
}

// Light one thing and everything it touches; dim the rest. `what` is a
// substance id (all its copies), {join: key} (the whole recipe), {edge: rec}.
function alchHighlight(what) {
  const sc = alchScene, svg = document.querySelector('#alchCanvas svg');
  if (!sc || !svg) return;
  for (const x of svg.querySelectorAll('.on')) x.classList.remove('on');
  for (const x of svg.querySelectorAll('.n.sel')) x.classList.remove('sel');
  if (!what) { svg.classList.remove('hl'); return; }
  const litEdge = rec => {
    rec.el.classList.add('on'); rec.lb?.classList.add('on');
    for (const k of [rec.e.src, rec.e.dst]) sc.elOf.get(k)?.classList.add('on');
  };
  const litJoin = jk => {
    sc.elOf.get(jk)?.classList.add('on');
    for (const rec of sc.edgesOf.get(jk) || []) litEdge(rec);
  };
  if (typeof what === 'string') {
    const keys = [...sc.model.nodes.values()].filter(n => n.kind === 'mat' && n.id === what).map(n => n.key);
    for (const k of keys) {
      const el = sc.elOf.get(k);
      el?.classList.add('on');
      if (what === alchState.selected) el?.classList.add('sel');
      for (const rec of sc.edgesOf.get(k) || []) {
        litEdge(rec);
        if (rec.e.joinKey) litJoin(rec.e.joinKey);
      }
    }
  } else if (what.join) litJoin(what.join);
  else if (what.edge) { litEdge(what.edge); if (what.edge.e.joinKey) litJoin(what.edge.e.joinKey); }
  svg.classList.add('hl');
}

function alchTip(ev, html) {
  const tip = document.getElementById('alchTip'), view = document.getElementById('alchView');
  if (!tip) return;
  if (ev === null) { tip.style.display = 'none'; return; }
  if (html != null) { tip.innerHTML = html; tip.style.display = 'block'; }
  const r = view.getBoundingClientRect();
  let x = ev.clientX - r.left + 14, y = ev.clientY - r.top + 14;
  if (x + tip.offsetWidth > r.width - 8) x = ev.clientX - r.left - tip.offsetWidth - 14;
  if (y + tip.offsetHeight > r.height - 8) y = ev.clientY - r.top - tip.offsetHeight - 14;
  tip.style.left = x + 'px'; tip.style.top = y + 'px';
}
const alchH = s => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;');
function alchEdgeTip(e) {
  const l = e.link, k = ALCH_KINDS[l.kind];
  return `<b>${alchH(l.from.replace(/_/g, ' '))}</b> → <b>${alchH(l.to.replace(/_/g, ' '))}</b><br>` +
    l.labels.map(t => `<span style="color:${k.color}">${k.icon} ${alchH(t)}</span>`).join('<br>');
}
function alchJoinTip(j, g) {
  const k = ALCH_KINDS[j.kind];
  const ins = j.inputs.map(i => `<b>${alchH(g.nice(i.id))}</b>${i.consumed ? '' : ' <i style="color:#8a94a6">(catalyst, stays)</i>'}`).join(' + ');
  return `${ins}<br>→ <b>${j.outputs.map(o => alchH(g.nice(o))).join('</b> + <b>')}</b><br>` +
    `<span style="color:${k.color}">${k.icon} ${k.name}${j.gates.length ? ' · ' + alchH(j.gates.join(' / ')) : ''}</span>`;
}

function alchRuleText(r) {
  if (!r) return '';
  if (r.kind === 'dissolve') return `solutes.json ${r.sp.name}: ${r.sp.from} dissolves into ${r.sp.solvents.join(', ')} (≤${r.sp.saturation}, ${r.sp.dissolveChance}‰/tick)`;
  if (r.kind === 'converts') return `solutes.json ${r.sp.name}: ${r.c.solvent} at ≥${r.c.cMin} becomes ${r.c.into}`;
  const parts = [];
  if (r.emit) parts.push(`${r.self} emits ${r.emit}${r.dir ? ' ' + r.dir : ''}`);
  else if (r.decay) parts.push(`${r.self} → ${r.becomes}`);
  else parts.push(`${r.self} + ${r.neighbor}: ${r.selfBecomes ? 'self → ' + r.selfBecomes : ''}${r.selfBecomes && r.neighborBecomes ? ', ' : ''}${r.neighborBecomes ? 'neighbour → ' + r.neighborBecomes : ''}`);
  parts.push(`${r.chance}‰`);
  for (const k of ['when', 'minLight', 'needsSky', 'rain', 'requires', 'solute', 'cMin', 'cMax', 'dir', 'drying', 'burnDuration'])
    if (r[k] != null && !(k === 'dir' && r.emit)) parts.push(`${k}:${r[k]}`);
  for (const fx of r.effects || []) parts.push(`effect ${fx.kind}${fx.radius ? ' r' + fx.radius : ''}${fx.power ? ' p' + fx.power : ''}`);
  return parts.join(' · ');
}


function alchRenderSide() {
  const side = document.getElementById('alchSide'), st = alchState, g = alchGraph;
  if (!side || !g) return;
  side.innerHTML = '';
  const id = st.selected;
  const go = o => alchEl('a', {onclick: () => { alchFind(o); }}, g.nice(o));
  if (!id || !(g.byId.has(id) || g.nodeIds.includes(id))) {
    side.append(alchEl('h3', {}, 'Reaction map'));
    side.append(alchEl('div', {class: 'alch-foot'},
      'Every substance and what turns it into what, rebuilt from reactions.json, materials.json and solutes.json each time this tab opens — it is never hand-updated.'));
    side.append(alchEl('h4', {}, 'how to read it'));
    for (const t of [
      ['→ labelled arrow', 'one substance becomes another under a condition (heat, electrify, sunlight, time…). The label sits on its own arrow, outlined in its colour.'],
      ['✚ recipe pill', 'two substances COMBINE: both ingredients flow into the pill, the products flow out. Dashed ingredient = a catalyst that is not used up.'],
      ['dotted arrow', 'the source stays: it emits something, or dissolves into a liquid.'],
      ['thick arrow', 'the reaction has an effect: 💥 explosion, ✴ flash.'],
      ['boxes', 'groups of substances that mostly react with each other, found automatically. No arrow leaves its box.'],
      ['dashed italic box', 'a COPY of a substance that lives in another group, so the arrow stays inside its box. Hover it to light the original; click it to jump there.'],
      ['■ ⁖ ≈ ☁', 'solid, powder, liquid, gas.'],
    ]) side.append(alchEl('div', {class: 'alch-rule'}, alchEl('span', {class: 'k'}, t[0]), ' — ', t[1]));
    side.append(alchEl('div', {class: 'alch-foot', style: 'margin-top:8px'},
      'Hover anything to isolate it. Click a substance for its rules; double-click to show only its neighbourhood. Scroll to zoom (labels appear as you get close), drag to pan, or drag in the minimap.'));
    const counts = {};
    for (const e of g.links) counts[e.kind] = (counts[e.kind] || 0) + 1;
    for (const j of g.joins) counts[j.kind] = (counts[j.kind] || 0) + 1;
    side.append(alchEl('h4', {}, `on the map: ${g.links.length} arrows, ${g.joins.length} recipes`));
    for (const [k, n] of Object.entries(counts).sort((a, b) => b[1] - a[1]))
      side.append(alchEl('div', {class: 'alch-rule'}, alchEl('span', {class: 'k', style: 'color:' + ALCH_KINDS[k].color}, ALCH_KINDS[k].icon + ' ' + ALCH_KINDS[k].name), ' — ' + n));
    return;
  }
  const m = g.byId.get(id);
  side.append(alchEl('h3', {}, g.nice(id)));
  if (m) side.append(alchEl('div', {class: 'alch-foot'}, `${ALCH_CLASS_GLYPH[m.class] || ''} ${m.class}${m.tags?.length ? ' · tags: ' + m.tags.join(', ') : ''}`));
  if (id.startsWith('tag:')) side.append(alchEl('div', {class: 'alch-foot'}, 'members: ' + g.membersOf(id.slice(4)).join(', ')));
  side.append(alchEl('div', {style: 'margin:8px 0;display:flex;gap:6px;flex-wrap:wrap'},
    alchEl('button', {onclick: () => { st.focus = id; document.getElementById('alchFind').value = id; alchRender(true); }}, 'isolate'),
    typeof gotoReactions === 'function' ? alchEl('button', {onclick: () => gotoReactions(id)}, 'open in Reactions') : null,
    m && typeof gotoMaterial === 'function' ? alchEl('button', {onclick: () => gotoMaterial(id)}, 'open in Materials') : null));

  const airOk = x => st.showAir || x !== 'air';
  const kspan = (kind, text) => alchEl('span', {class: 'k', style: 'color:' + ALCH_KINDS[kind].color}, ALCH_KINDS[kind].icon + ' ' + text);
  const rulesOf = (row, rules) => { for (const r of rules) row.append(alchEl('div', {class: 'alch-foot', style: 'margin:1px 0 0 10px'}, alchRuleText(r))); };
  const joinHow = j => ALCH_KINDS[j.kind].name + (j.gates.length ? ' · ' + j.gates.join(' / ') : '');
  const section = (title, rows) => {
    side.append(alchEl('h4', {}, title + ' (' + rows.length + ')'));
    if (!rows.length) side.append(alchEl('div', {class: 'alch-foot'}, 'none'));
    for (const r of rows) side.append(r);
  };
  const sep = (arr, s) => arr.flatMap((x, i) => i ? [s, x] : [x]);

  const joinsIn = g.allJoins.filter(j => j.inputs.some(i => i.id === id && i.consumed) && j.outputs.some(airOk));
  const joinsCat = g.allJoins.filter(j => j.inputs.some(i => i.id === id && !i.consumed));
  const joinsOut = g.allJoins.filter(j => j.outputs.includes(id));

  section('turns into', [
    ...g.allLinks.filter(e => e.from === id && airOk(e.to)).map(e => {
      const row = alchEl('div', {class: 'alch-rule'}, kspan(e.kind, e.labels.join(' / ')), ' → ', go(e.to));
      rulesOf(row, e.rules); return row;
    }),
    ...joinsIn.map(j => {
      const others = j.inputs.filter(i => i.id !== id);
      const row = alchEl('div', {class: 'alch-rule'}, kspan(j.kind, 'with '), ...sep(others.map(i => go(i.id)), ' + '),
        ' → ', ...sep(j.outputs.filter(airOk).map(go), ' + '), alchEl('div', {class: 'alch-foot', style: 'margin-left:10px'}, joinHow(j)));
      rulesOf(row, j.rules); return row;
    })]);
  section('made from', [
    ...g.allLinks.filter(e => e.to === id && airOk(e.from)).map(e => {
      const row = alchEl('div', {class: 'alch-rule'}, go(e.from), ' → here, by ', kspan(e.kind, e.labels.join(' / ')));
      rulesOf(row, e.rules); return row;
    }),
    ...joinsOut.map(j => {
      const row = alchEl('div', {class: 'alch-rule'},
        ...sep(j.inputs.map(i => i.consumed ? go(i.id) : alchEl('span', {}, go(i.id), alchEl('i', {style: 'color:var(--faint)'}, ' (catalyst)'))), ' ✚ '),
        ' → here', alchEl('div', {class: 'alch-foot', style: 'margin-left:10px'}, kspan(j.kind, joinHow(j))));
      rulesOf(row, j.rules); return row;
    })]);
  if (joinsCat.length) section('catalyst in (not used up)', joinsCat.map(j => {
    const row = alchEl('div', {class: 'alch-rule'}, ...sep(j.inputs.filter(i => i.id !== id).map(i => go(i.id)), ' + '),
      ' → ', ...sep(j.outputs.filter(airOk).map(go), ' + '), alchEl('div', {class: 'alch-foot', style: 'margin-left:10px'}, kspan(j.kind, joinHow(j))));
    rulesOf(row, j.rules); return row;
  }));
}

// Headless checks (?wait=1): Chrome's virtual time fast-forwards past a web
// worker's real compute, so poll the server (network pauses virtual time)
// until the layout is back. Costs nothing in normal use.
async function alchKeepAlive(p) {
  if (!new URLSearchParams(location.search).get('wait')) return p;
  let done = false;
  p.then(() => { done = true; }, () => { done = true; });
  while (!done) { try { await fetch('/api/status', {cache: 'no-store'}); } catch { break; } }
  return p;
}

async function alchRender(refit) {
  const host = document.getElementById('view-alchemy');
  if (!host) return;
  alchEnsureDom(host);
  const st = alchInitState();
  const status = document.getElementById('alchStatus');
  if (!mat || !rx) { status.textContent = 'Load materials.json + reactions.json (Open assets folder, or run the tuner server).'; return; }
  const seq = ++alchRenderSeq;
  status.textContent = 'reading solutes.json…';
  const sol = await alchLoadSolutes();
  if (seq !== alchRenderSeq) return;

  const dl = document.getElementById('alchMatList');
  if (dl && dl.childElementCount !== mat.materials.length)
    dl.replaceChildren(...mat.materials.map(m => alchEl('option', {value: m.id})));

  alchGraph = alchBuildGraph(mat, rx, sol, st);
  const g = alchGraph;
  alchLastSrc = alchMermaidSource(g, st);
  const foot = document.getElementById('alchFoot');
  foot.textContent = `${g.nodeIds.length} substances, ${g.links.length} arrows, ${g.joins.length} recipes. ` +
    (sol ? '' : 'solutes.json unreachable (open the tuner through its server) — dissolving omitted. ') +
    (g.unknown.length ? `⚠ unknown names referenced: ${g.unknown.join(', ')}. ` : '') +
    (st.showInert ? '' : g.inert.length ? `${g.inert.length} inert substance(s) hidden: ${g.inert.join(', ')}.` : '');
  alchRenderSide();

  const model = alchModel(g, st, alchMeasure);
  const key = JSON.stringify(model.grouped ? model.groups.map(x => x.elk) : model.flat);
  if (key === alchLastKey && document.querySelector('#alchCanvas svg')) {
    status.textContent = '';
    alchHighlight(st.selected && st.selected !== st.focus ? st.selected : null);
    return;
  }
  status.textContent = `laying out ${model.nodes.size} boxes and ${model.edges.length} arrows…`;
  let placed;
  try {
    const elk = await alchLoadElk();
    const view = document.getElementById('alchView');
    placed = await alchKeepAlive(alchLayout(model, elk, Math.max(0.8, view.clientWidth / Math.max(1, view.clientHeight)), alchMeasure));
  } catch (e) {
    status.textContent = 'layout failed: ' + (e.message || e);
    return;
  }
  if (seq !== alchRenderSeq) return;
  alchLastKey = key;
  const {svg, scene} = alchDraw(model, placed, g, st);
  alchScene = scene;
  document.getElementById('alchCanvas').replaceChildren(svg);
  status.textContent = '';
  if (refit || !st._fitted) { alchFit(); st._fitted = true; } else alchApplyXform();
  if (st.selected) {
    // A deep-linked selection on the whole map: centre on it.
    if (!st.focus && !st._found && scene.nodesById.has(st.selected)) { st._found = true; alchFind(st.selected); }
    else if (st.selected !== st.focus) alchHighlight(st.selected);
  }
}

// Pure extraction + model, exposed for a Node-side check (no DOM needed).
if (typeof module !== 'undefined') module.exports = {alchBuildGraph, alchModel, alchMermaidSource, alchCommunities, ALCH_KINDS};
