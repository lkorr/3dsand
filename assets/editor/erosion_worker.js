/* erosion_worker.js — runs erosion.js off the map page's thread (module
 * worker). One message in: {id, heights (Float32Array, transferred), w, h, sp,
 * opts}; {id, progress} while it runs; {id, heights} (transferred) or
 * {id, error} at the end. */
import {erode} from './erosion.js';

self.onmessage = (ev) => {
  const {id, heights, w, h, sp, opts} = ev.data;
  try {
    const out = erode(heights, w, h, sp, opts, (p) => self.postMessage({id, progress: p}));
    self.postMessage({id, heights: out}, [out.buffer]);
  } catch (e) {
    self.postMessage({id, error: String(e && e.message || e)});
  }
};
