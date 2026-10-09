/* terrainjobs.js — the WHOLE-WINDOW jobs of the World map page's terrain
 * tools (docs/PLAN_worldbuilder.md K1 item 6), one entry point for the
 * worker (terrain_worker.js), the page's no-worker fallback and the Node
 * tests. Pure: a job is plain data in, typed arrays out.
 *
 * A job carries a GRID of the height plane's posts: {w, hh, x0, z0, cell, h}
 * (h = absolute voxels; x0 / z0 = the world column of post (0, 0) of the
 * window; cell = the post spacing). Jobs:
 *
 *   {job: 'range', mode: 'preview' | 'bake', range, grid, seaY}
 *       preview: the range on a ~51.2 m proxy, quick -> a coarse grid;
 *       bake: terraingen.rangeBake -> new posts.
 *   {job: 'template', ops, seed, grid, seaY, vpm, preview}
 *       the op list on a proxy (~51.2 m; ~102 m for a preview), brought to
 *       the posts (bicubic + relief detail). grid.h is not read (a template
 *       makes the ground) unless an op needs it -- it starts from it.
 *   {job: 'primitive', kind, params, grid, seaY, preview}
 *       crater / volcano / canyon / glacial / fjord / mesas / dunes on the
 *       posts (preview: on a 4x proxy).
 *   {job: 'erode', method: 'land' | 'incise' | 'droplet', strength, seed,
 *    grid, seaY, iters, talusDeg, preview}
 *       land = the landscape-evolution pass (terraingen.erodeLand), incise =
 *       stream power on the ground as it is (terraingen.erodeGrid), on a proxy
 *       with the change brought back (hydrology.viaProxy); droplet = the old
 *       particle erosion (erosion.js) on the posts.
 *   {job: 'climate', grid (one sample per BIOME cell), seaY, vpm, params, biomes, current, locks, rect, rivers, lakes, classify}
 *       climate.js climateJob: temperature, precipitation, moisture, wetness
 *       and (classify) the auto-biome of every cell of the rect.
 *   {job: 'rivers', grid, seaY, threshold (cells), minLength}
 *       the channel network for the River tool's "derive" (hydrology.rivers).
 *   {job: 'flow', grid, seaY}
 *       drainage area (D8 on the epsilon-filled surface) and lake depth
 *       (Priority-Flood fill minus the ground) for the overlays.
 *   {job: 'rivers', grid, seaY, threshold (cells), minLength}
 *       the channel network for the River tool's "derive" (hydrology.rivers).
 *
 * Results: {h (Float32Array, same w x hh as the grid unless `coarse`), coarse:
 * {w, hh, x0, z0, cell} when the result is a proxy, ...extras}. */

import * as TG from './terraingen.js';
import * as HY from './hydrology.js';
import * as ER from './erosion.js';
import * as CL from './climate.js';

const coarseOf = (G, f) => TG.proxyGrid(G, f);
const meta = P => ({w: P.w, hh: P.hh, x0: P.x0, z0: P.z0, cell: P.cell});

/** Run one job; `progress(fraction)` is called along the way. */
export function runJob(J, progress = null) {
  const G = J.grid, sea = J.seaY == null ? 0 : J.seaY;
  switch (J.job) {
    case 'range': {
      if (J.mode === 'preview') {
        const f = TG.proxyFactor(G.w, G.hh, G.cell, 512, J.maxCells || 90000);
        const P = coarseOf(G, f);
        const R = TG.rangeField(J.range, P, {seaY: sea, progress, iters: J.iters || null});
        return {h: R.h, base: P.h, coarse: meta(P)};
      }
      const B = TG.rangeBake(J.range, G, {seaY: sea, progress});
      return {h: B.h, changed: B.changed, factor: B.factor};
    }
    case 'template': {
      const f = TG.proxyFactor(G.w, G.hh, G.cell, J.preview ? 1024 : 512, J.preview ? 40000 : 200000);
      const P = G.h ? coarseOf(G, f) : {w: Math.ceil(G.w / f), hh: Math.ceil(G.hh / f), x0: G.x0 + (f - 1) / 2 * G.cell, z0: G.z0 + (f - 1) / 2 * G.cell,
                                         cell: G.cell * f, h: new Float32Array(Math.ceil(G.w / f) * Math.ceil(G.hh / f)).fill(sea)};
      TG.runTemplate(P, J.ops, {seed: J.seed | 0, seaY: sea, vpm: J.vpm || 10, progress});
      if (J.preview) return {h: P.h, coarse: meta(P)};
      // to the posts: bicubic, plus fine relief detail on the land
      const up = f > 1 ? HY.upsample(P.h, P.w, P.hh, f, G.w, G.hh) : Float32Array.from(P.h);
      const sd = (J.seed | 0) * 7 + 3;
      for (let j = 0; j < G.hh; j++)
        for (let i = 0; i < G.w; i++) {
          const n = j * G.w + i, rel = up[n] - sea;
          if (rel > 1) up[n] += TG.reliefDetail(G.x0 + i * G.cell, G.z0 + j * G.cell, Math.min(rel, 4000), sd, 0.6);
        }
      return {h: up, factor: f};
    }
    case 'primitive': {
      const f = J.preview ? TG.proxyFactor(G.w, G.hh, G.cell, 256, 90000) : 1;
      const P = f > 1 ? coarseOf(G, f) : {...G, h: Float32Array.from(G.h)};
      const p = {...J.params, seed: (J.params && J.params.seed) | 0, seaY: sea};
      const fn = {crater: TG.crater, volcano: TG.volcano, canyon: TG.canyon, mesas: TG.mesaField, dunes: TG.dunes,
                  glacial: TG.uValley, fjord: (g, o) => TG.uValley(g, {...o, fjord: true})}[J.kind];
      if (!fn) throw new Error('unknown primitive ' + J.kind);
      fn(P, p);
      if (progress) progress(1);
      return f > 1 ? {h: P.h, base: coarseOf(G, f).h, coarse: meta(P)} : {h: P.h};
    }
    case 'erode': {
      const s = J.strength == null ? 0.5 : +J.strength, seed = J.seed | 0;
      if (J.method === 'droplet') {
        const out = ER.erode(G.h, G.w, G.hh, G.cell, {seed, iterations: J.iters || 6, strength: s, talusDeg: J.talusDeg || 38,
                                                       hydraulic: J.hydraulic !== false, thermal: J.thermal !== false}, progress);
        return {h: out};
      }
      // stream power on a proxy (~25.6 m for a box, ~51.2 m for the map), the change brought back
      const f = J.factor || TG.proxyFactor(G.w, G.hh, G.cell, 256, J.preview ? 60000 : 300000);
      const out = HY.viaProxy(G.h, G.w, G.hh, f, (c, cw, ch) => {
        const P = {w: cw, hh: ch, x0: G.x0 + (f - 1) / 2 * G.cell, z0: G.z0 + (f - 1) / 2 * G.cell, cell: G.cell * f, h: c};
        if (J.method === 'incise') TG.erodeGrid(P, {strength: s, seaY: sea, seed, iters: J.iters, talusDeg: J.talusDeg, progress});
        else TG.erodeLand(P, {strength: s, seaY: sea, seed, progress});
        if (P.h !== c) c.set(P.h);
      });
      // the window's edge is held so the pass meets the land around it
      const fe = Math.max(2, Math.min(G.w, G.hh) >> 5);
      for (let j = 0; j < G.hh; j++)
        for (let i = 0; i < G.w; i++) {
          const e = Math.min(i, j, G.w - 1 - i, G.hh - 1 - j);
          if (e >= fe || J.noFeather) continue;
          const n = j * G.w + i, t = e / fe;
          out[n] = G.h[n] + (out[n] - G.h[n]) * t * t * (3 - 2 * t);
        }
      return {h: out, factor: f};
    }
    case 'flow': {
      const F = HY.flow(G.h, G.w, G.hh, G.cell, {seaY: sea});
      const lakeF = HY.priorityFlood(G.h, G.w, G.hh, {seaY: sea, epsilon: 0});
      const A = new Float32Array(G.w * G.hh), depth = new Float32Array(G.w * G.hh);
      for (let n = 0; n < A.length; n++) { A[n] = F.A[n]; depth[n] = Math.max(0, lakeF[n] - G.h[n]); }
      if (progress) progress(1);
      return {A, depth};
    }
    // E2: climate fields + auto-biomes (climate.js climateJob; the Climate panel)
    case 'climate': return CL.climateJob(J, progress);
    case 'rivers': {
      // E2 "derive rivers": the D8 channel network on the grid (usually a
      // proxy), source -> mouth polylines in GRID units with A per point;
      // riverbake.derivedToRecords turns them into rivers[] records.
      const F = HY.flow(G.h, G.w, G.hh, G.cell, {seaY: sea});
      const list = HY.rivers(G.h, G.w, G.hh, G.cell, {flow: F, threshold: Math.max(2, J.threshold | 0), minLength: J.minLength || 4});
      if (progress) progress(1);
      return {list: list.map(r => ({id: r.id, joins: r.joins, mouth: r.mouth, A: r.A, line: r.line}))};
    }
    default: throw new Error('unknown job ' + J.job);
  }
}
/** The typed arrays of a result, for postMessage's transfer list. */
export function transferables(r) {
  const out = [];
  for (const v of Object.values(r)) if (v && v.buffer instanceof ArrayBuffer && !out.includes(v.buffer)) out.push(v.buffer);
  return out;
}
