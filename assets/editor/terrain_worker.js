/* terrain_worker.js — runs terrainjobs.js off the map page's thread (module
 * worker): ranges, templates, primitives, the stream-power passes, the flow
 * overlay. One message in: {id, job: {...}} (the job's grid.h transferred);
 * {id, progress} while it runs; {id, result} (typed arrays transferred) or
 * {id, error} at the end. A worker serves one job at a time; the page makes
 * a fresh one per job, so cancelling is terminate(). */
import {runJob, transferables} from './terrainjobs.js';

self.onmessage = (ev) => {
  const {id, job} = ev.data;
  try {
    let last = -1;
    const result = runJob(job, (p) => {
      const q = Math.round(p * 100);
      if (q !== last) { last = q; self.postMessage({id, progress: p}); }
    });
    self.postMessage({id, result}, transferables(result));
  } catch (e) {
    self.postMessage({id, error: String((e && e.stack) || e)});
  }
};
