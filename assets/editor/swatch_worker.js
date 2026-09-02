/* swatch_worker.js — composes the biome page's swatch off the main thread.
 *
 * WHY A WORKER. generateSwatch is 0.3–1.2 s of pure JS for a 20–35M-cell
 * volume, and the first swatch at a new bake scale bakes every tree species
 * through treegen first (1–3 s). On the main thread that is a page that stops
 * answering the slider that caused it. Here the page keeps painting, the
 * viewer keeps orbiting the previous swatch, and the result arrives as one
 * transferred buffer.
 *
 * WHAT LIVES HERE. The component libraries (posted once by biome.js after
 * loadLibs) and the tree cache keyed by species/variant/vpm — the expensive
 * state, kept where it is used. The biome itself travels with every request:
 * it is a few KB of JSON and the page edits it between requests.
 *
 * PROTOCOL
 *   -> {cmd:'libs', libs}                       replace the libraries, drop the tree cache
 *   -> {cmd:'swatch', id, biome, seed, opts, matIds}
 *   <- {cmd:'progress', id, text}               a tree bake the cache did not have
 *   <- {cmd:'swatch', id, res, missing, ms}     res.cells already remapped to engine
 *                                               material ids (biomegen.remapToMaterials),
 *                                               transferred, ready for setLocalRegions
 *   <- {cmd:'error', id, error}
 *
 * The page discards any reply whose id is not the latest request, so a stale
 * result from a slider mid-drag costs nothing but the time it took.
 */

import * as BG from './biomegen.js';

let libs = {water: {}, trees: {}};
const treeCache = new Map();

self.onmessage = (e) => {
  const m = e.data;
  if (m.cmd === 'libs') { libs = m.libs || {water: {}, trees: {}}; treeCache.clear(); return; }
  if (m.cmd !== 'swatch') return;
  try {
    const t0 = performance.now();
    let bakes = 0;
    const onTreeBake = (species, variant, vpm) => {
      self.postMessage({cmd: 'progress', id: m.id, text: `baking ${species} #${variant} at ${vpm} vpm (${++bakes})…`});
    };
    const res = BG.generateSwatch(m.biome, libs, m.seed, Object.assign({treeCache, onTreeBake}, m.opts));
    const missing = BG.remapToMaterials(res, m.matIds);
    self.postMessage({cmd: 'swatch', id: m.id, res, missing, ms: performance.now() - t0}, [res.cells.buffer]);
  } catch (err) {
    self.postMessage({cmd: 'error', id: m.id, error: String(err && err.stack || err)});
  }
};
