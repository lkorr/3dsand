/* maptools.js — THE TOOL REGISTRY of the World map page
 * (docs/PLAN_worldbuilder.md K1 item 1). A tool is declared in ONE place:
 *
 *   registerTool({
 *     key: 'range',              // unique; the `tool` string the page tracks
 *     label: 'Range',            // the toolbar button's text
 *     title: '...',              // its tooltip
 *     group: 'height',           // toolbar group: select | height | paint | vector | site | locale | view
 *     order: 25,                 // position inside the group (registration order breaks ties)
 *     world: true, locale: false,// which page shows it (the world map / an open locale)
 *     opts: () => Element,      // the options strip under the toolbar, built once, shown while active
 *     panel: () => Element,      // a side panel (a <details>), built once, always shown on its page
 *     down(ev, wx, wz) -> bool   // left button pressed on the canvas (true = handled)
 *     move(ev, wx, wz)           // pointer moved while the left button is held
 *     up()                       // left button released / cancelled
 *     hover(ev, wx, wz)          // pointer moved with no button (status, cursor)
 *     statusAt(wx, wz) -> string // the status line under the map while this tool is active (default: the readout)
 *     cursor(ctx, wx, wz)        // draw the tool's cursor at the hover column
 *     paint(ctx)                 // draw the tool's own overlay (every repaint, any active tool)
 *     onKey(ev) -> bool          // a key while the page is focused and this tool active
 *     anyKey(ev) -> bool         // a key whatever the active tool
 *     early(ev) -> bool          // a press any tool yields first (the minimap)
 *     dblclick(ev) -> bool
 *     resize(f)                  // [ / ] : scale the tool's size by f
 *     sync()                     // the page's controls changed (or the map loaded)
 *     guide: [title, what, how, limits],   // the tool guide box's entry
 *   })
 *
 * Every hook is optional. Tools whose pointer work lives in a layer that
 * handles it before the registry (the vector layers' road / region / marker,
 * the locales' locale / structure / waylink) register with no `down`. */

export const GROUPS = ['select', 'height', 'paint', 'vector', 'locale', 'site', 'view'];

export function createToolRegistry() {
  const tools = new Map();
  let seq = 0;
  function registerTool(t) {
    if (!t || typeof t.key !== 'string' || !t.key) throw new Error('registerTool: a tool needs a string key (the keyboard hook is onKey)');
    if (tools.has(t.key)) throw new Error('registerTool: tool ' + t.key + ' is registered twice');
    const g = GROUPS.includes(t.group) ? t.group : 'view';
    tools.set(t.key, {world: true, locale: false, ...t, group: g, order: t.order == null ? 100 : t.order, seq: seq++});
    return tools.get(t.key);
  }
  /** Every tool in toolbar order: by group, then order, then registration. */
  function list() {
    return [...tools.values()].sort((a, b) => GROUPS.indexOf(a.group) - GROUPS.indexOf(b.group) || a.order - b.order || a.seq - b.seq);
  }
  return {
    registerTool,
    get: k => tools.get(k) || null,
    has: k => tools.has(k),
    list,
    keys: () => list().map(t => t.key),
    /** Call hook `name` on tool `key` if it has it; returns its result (undefined if none). */
    call(key, name, ...args) { const t = tools.get(key); return t && typeof t[name] === 'function' ? t[name](...args) : undefined; },
    /** Call hook `name` on every tool that has it (paint, anyKey). Stops at the first truthy result when `first`. */
    each(name, args = [], first = false) {
      for (const t of list()) if (typeof t[name] === 'function') { const r = t[name](...args); if (first && r) return r; }
      return undefined;
    },
    /** The TOOL_GUIDE entries the tools carry themselves. */
    guides() { const g = {}; for (const t of tools.values()) if (t.guide) g[t.key] = t.guide; return g; },
  };
}
