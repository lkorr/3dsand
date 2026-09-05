/* envlink.js — the Environment tab's link to the RUNNING game.
 *
 * WHAT THIS IS. docs/PLAN_environment_truth.md P-A, the browser half. The
 * engine loads the biome files, the painted world map and the tree atlas
 * ONCE and generates from those tables; an Environment save changes the
 * files, not the tables. So the tab needs two things a save button cannot
 * give it: to know whether the game is behind the disk, and to tell the game
 * to catch up. Both ride the game's telemetry WebSocket (src/telemetry.h,
 * `--telemetry`, the Play button passes it):
 *
 *   game -> page   {"v":3,"type":"environment","stamp":{map, mapHash,
 *                   biomesHash, treesHash}}   on attach and after every reload
 *   page -> game   {"cmd":"apply-environment"}  = F7: reload + regenerate
 *                  {"cmd":"env-stamp"}          = say it again
 *
 * The disk side comes from the tuner server (/api/environment/hashes), which
 * computes the same FNV-1a the engine prints at boot. Equal means the world
 * you are looking at was generated from the files on disk. Different means
 * "apply". No game means "launch one with Play, or press F7 in the one you
 * started by hand".
 */

let url = 'ws://localhost:8080';
let sock = null;
let wanted = false;
let timer = null;
let connected = false;
let stamp = null;          // the game's last environment stamp, or null
const listeners = [];

function notify() { for (const cb of listeners) { try { cb(state()); } catch (e) { console.error(e); } } }

function connect() {
  if (sock || !wanted) return;
  let s;
  try { s = new WebSocket(url); } catch (e) { schedule(); return; }
  sock = s;
  s.onopen = () => {
    connected = true;
    notify();
    try { s.send(JSON.stringify({cmd: 'env-stamp'})); } catch (e) { /* the attach message is on its way anyway */ }
  };
  s.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    if (msg && msg.type === 'environment' && msg.stamp) { stamp = msg.stamp; notify(); }
  };
  s.onerror = () => { /* onclose follows */ };
  s.onclose = () => {
    sock = null;
    const was = connected;
    connected = false;
    stamp = null;
    if (was) notify();
    schedule();
  };
}
function schedule() {
  clearTimeout(timer);
  if (wanted) timer = setTimeout(connect, 3000);
}

/** Start (and keep) trying to reach the game. Idempotent. */
export function start(wsUrl) {
  if (wsUrl) url = wsUrl;
  wanted = true;
  connect();
}
export function stop() {
  wanted = false;
  clearTimeout(timer);
  if (sock) sock.close();
}
export function state() { return {connected, stamp}; }
export function onChange(cb) { listeners.push(cb); }

/** Ask the game to reload the environment from disk and regenerate (= F7). */
export function apply() {
  if (!connected || !sock) return false;
  try { sock.send(JSON.stringify({cmd: 'apply-environment'})); } catch (e) { return false; }
  return true;
}

/** The disk side: {map, mapHash, biomesHash, treesHash} from the tuner server. */
export async function diskHashes() {
  const r = await fetch('/api/environment/hashes', {cache: 'no-store'});
  if (!r.ok) throw new Error('HTTP ' + r.status);
  return r.json();
}

/** Compare a game stamp with the disk: [] when equal, else the parts that differ. */
export function staleParts(game, disk) {
  if (!game || !disk) return [];
  const out = [];
  if (game.map !== disk.map) out.push('map name');
  if (game.mapHash !== disk.mapHash) out.push('map');
  if (game.biomesHash !== disk.biomesHash) out.push('biomes');
  if (game.treesHash !== disk.treesHash) out.push('trees');
  return out;
}
