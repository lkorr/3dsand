/* propkit.js — the drawing kit for dungeon PROP generators (PLAN_dungeons
 * §3.5). A prop is a tiny pure module:
 *
 *   export default { name, kit, label, version, SCHEMA, PRESETS,
 *                    generate(params, seed) -> { cells: [{x, y, z, mat}], notes } }
 *
 * FRAME: the prop's origin (0,0,0) is the cell it STANDS on the floor in
 * (y = 0 is the first air row above the floor), its front faces +Z (heading
 * 0). Hanging props (lantern 'ceil', banner) hang DOWN from y = 0; wall
 * props (lantern 'wall', banner) have the wall behind them at z = -1.
 *
 * MATERIALS are names from materials.json, or '@slot' to take the dungeon
 * style's slot (so a ruins altar follows the palette), or CARVE ('#carve')
 * to clear the cell to air even if the world has rock there — distinct from
 * an absent cell, which leaves the world alone (§3.5's carve code).
 *
 * No Math.random: frand(seed, key...) from dgutil.js, keyed by what it decides.
 */
'use strict';
import { frand, normalizeParams, defaultsOf } from '../../editor/dungeon/dgutil.js';

export const CARVE = '#carve';
export { frand, normalizeParams, defaultsOf };

export class Grid {
  constructor() { this.m = new Map(); }
  key(x, y, z) { return x + ',' + y + ',' + z; }
  set(x, y, z, mat) { x = Math.round(x); y = Math.round(y); z = Math.round(z); this.m.set(this.key(x, y, z), [x, y, z, mat]); }
  get(x, y, z) { const v = this.m.get(this.key(x, y, z)); return v ? v[3] : null; }
  /** Half-open box [x0,x1) x [y0,y1) x [z0,z1). */
  box(x0, y0, z0, x1, y1, z1, mat) {
    for (let y = y0; y < y1; y++) for (let z = z0; z < z1; z++) for (let x = x0; x < x1; x++) this.set(x, y, z, mat);
  }
  /** Hollow box frame: just the 12 edges. */
  frame(x0, y0, z0, x1, y1, z1, mat) {
    for (let x = x0; x < x1; x++) for (const y of [y0, y1 - 1]) for (const z of [z0, z1 - 1]) this.set(x, y, z, mat);
    for (let y = y0; y < y1; y++) for (const x of [x0, x1 - 1]) for (const z of [z0, z1 - 1]) this.set(x, y, z, mat);
    for (let z = z0; z < z1; z++) for (const x of [x0, x1 - 1]) for (const y of [y0, y1 - 1]) this.set(x, y, z, mat);
  }
  /** Vertical cylinder centred on the cell column (cx,cz), radius r (cells). */
  cylY(cx, cz, y0, y1, r, mat) {
    const R = Math.ceil(r);
    for (let y = y0; y < y1; y++) for (let dz = -R; dz <= R; dz++) for (let dx = -R; dx <= R; dx++)
      if (dx * dx + dz * dz <= r * r + 0.25) this.set(cx + dx, y, cz + dz, mat);
  }
  /** Horizontal cylinder along X or Z through (cy, c2). */
  cylH(axis, a0, a1, cy, c2, r, mat) {
    const R = Math.ceil(r);
    for (let a = a0; a < a1; a++) for (let dy = -R; dy <= R; dy++) for (let d2 = -R; d2 <= R; d2++) {
      if (dy * dy + d2 * d2 > r * r + 0.25) continue;
      if (axis === 'x') this.set(a, cy + dy, c2 + d2, mat); else this.set(c2 + d2, cy + dy, a, mat);
    }
  }
  ball(cx, cy, cz, rx, ry, rz, mat) {
    const X = Math.ceil(rx), Y = Math.ceil(ry), Z = Math.ceil(rz);
    for (let y = -Y; y <= Y; y++) for (let z = -Z; z <= Z; z++) for (let x = -X; x <= X; x++)
      if ((x / rx) ** 2 + (y / ry) ** 2 + (z / rz) ** 2 <= 1) this.set(cx + x, cy + y, cz + z, mat);
  }
  line(a, b, mat) {
    const n = Math.max(Math.abs(b[0] - a[0]), Math.abs(b[1] - a[1]), Math.abs(b[2] - a[2]), 1);
    for (let i = 0; i <= n; i++) this.set(a[0] + ((b[0] - a[0]) * i) / n, a[1] + ((b[1] - a[1]) * i) / n, a[2] + ((b[2] - a[2]) * i) / n, mat);
  }
  /** Clear a box to CARVE where nothing else is set yet (an alcove of air). */
  clearAround(x0, y0, z0, x1, y1, z1) {
    for (let y = y0; y < y1; y++) for (let z = z0; z < z1; z++) for (let x = x0; x < x1; x++)
      if (!this.m.has(this.key(x, y, z))) this.set(x, y, z, CARVE);
  }
  /** The cells, sorted (y, z, x) so the output is byte-stable. */
  cells() {
    return [...this.m.values()].sort((a, b) => a[1] - b[1] || a[2] - b[2] || a[0] - b[0]).map(([x, y, z, mat]) => ({x, y, z, mat}));
  }
}

/** Define a prop module from {name, kit, label, version, SCHEMA, PRESETS, draw(g, P, seed)}. */
export function defineProp(def) {
  def.defaults = () => defaultsOf(def.SCHEMA);
  def.normalize = (src) => normalizeParams(def.SCHEMA, src);
  def.generate = (params, seed) => {
    const P = def.normalize(params);
    const g = new Grid();
    const notes = [];
    def.draw(g, P, (seed >>> 0), notes);
    return {cells: g.cells(), params: P, seed: seed >>> 0, notes};
  };
  def.id = def.kit + '/' + def.name;
  return def;
}

/** Common optional rows. */
export const LIT_ROW = {k: 'lit', n: 'flame', kind: 'enum', options: ['light', 'lightCool', 'none'], def: 'light', d: 'Which palette light slot burns in it (warm / cool), or unlit.'};
export const MOUNT_ROW = {k: 'mount', n: 'mount', kind: 'enum', options: ['floor', 'ceil', 'wall'], def: 'floor', d: 'floor = stands; ceil = hangs down from y 0; wall = bracket into the wall at z -1.'};
