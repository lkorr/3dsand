/* cdp_shot.mjs — screenshot a page in real headless Chrome over the DevTools
 * protocol, in REAL time (no --virtual-time-budget: a page that streams
 * voxels and renders on requestAnimationFrame never goes idle, and Chrome's
 * --screenshot waits on a virtual clock that its pending fetches stop).
 *
 *   node scripts/cdp_shot.mjs <url> <out.png> [--wait '<js expr>'] [--timeout s]
 *        [--run '<js>'] [--size 1600x1000] [--chrome <path>] [--settle ms]
 *
 * --wait is polled (Runtime.evaluate, awaitPromise) until it is truthy; --run
 * is evaluated first (drive the page into the state to shoot). Several
 * --run/--wait/--shot triples may follow in order: `--shot <file>` takes a
 * picture at that point. Used by scripts/check_dungeons.sh --shots.
 */
import { spawn } from 'child_process';
import { mkdtempSync, rmSync, writeFileSync, existsSync } from 'fs';
import { tmpdir } from 'os';
import { join } from 'path';

const argv = process.argv.slice(2);
const url = argv[0];
const steps = [];
let size = [1600, 1000], timeout = 240, chrome = null, settle = 600, statusExpr = null;
for (let i = 1; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--wait') steps.push({wait: argv[++i]});
  else if (a === '--run') steps.push({run: argv[++i]});
  else if (a === '--shot') steps.push({shot: argv[++i]});
  else if (a === '--size') size = argv[++i].split('x').map(Number);
  else if (a === '--timeout') timeout = +argv[++i];
  else if (a === '--chrome') chrome = argv[++i];
  else if (a === '--settle') settle = +argv[++i];
  else if (a === '--status') statusExpr = argv[++i];
  else if (!a.startsWith('--')) steps.push({shot: a});
}
if (!url || !steps.some(s => s.shot)) { console.error('usage: node scripts/cdp_shot.mjs <url> <out.png> [--wait expr] [--run js] [--shot file]...'); process.exit(2); }
chrome = chrome || ['C:/Program Files/Google/Chrome/Application/chrome.exe', 'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
                    'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find(p => existsSync(p));
if (!chrome) { console.error('cdp_shot: no Chrome / Edge'); process.exit(2); }

const prof = mkdtempSync(join(tmpdir(), 'cdpshot-'));
const port = 9300 + Math.floor(Math.random() * 400);
const proc = spawn(chrome, ['--headless=new', '--disable-gpu', '--use-angle=swiftshader-webgl', '--enable-unsafe-swiftshader',
  '--user-data-dir=' + prof, '--no-first-run', '--remote-debugging-port=' + port, '--window-size=' + size.join(','), 'about:blank'], {stdio: 'ignore'});
const done = (code) => { try { proc.kill(); } catch (e) {} setTimeout(() => { try { rmSync(prof, {recursive: true, force: true}); } catch (e) {} process.exit(code); }, 400); };
const deadline = Date.now() + timeout * 1000;
setTimeout(() => { console.error('cdp_shot: timeout'); done(1); }, timeout * 1000);
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

let target = null;
for (let i = 0; i < 100 && !target; i++) {
  try { const j = await (await fetch('http://127.0.0.1:' + port + '/json')).json(); target = j.find(t => t.type === 'page'); } catch (e) {}
  if (!target) await sleep(150);
}
if (!target) { console.error('cdp_shot: chrome did not come up'); done(1); }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
let seq = 0;
const pend = new Map();
ws.onmessage = (m) => { const j = JSON.parse(m.data); if (j.id && pend.has(j.id)) { pend.get(j.id)(j); pend.delete(j.id); } };
const cmd = (method, params) => new Promise(r => { const id = ++seq; pend.set(id, r); ws.send(JSON.stringify({id, method, params: params || {}})); });
const evalJs = async (expr) => {
  const r = await cmd('Runtime.evaluate', {expression: expr, awaitPromise: true, returnByValue: true});
  if (r.result && r.result.exceptionDetails) return {error: r.result.exceptionDetails.exception ? r.result.exceptionDetails.exception.description : 'exception'};
  return {value: r.result && r.result.result ? r.result.result.value : undefined};
};
await cmd('Page.enable');
await cmd('Emulation.setDeviceMetricsOverride', {width: size[0], height: size[1], deviceScaleFactor: 1, mobile: false});
await cmd('Page.navigate', {url});
await sleep(1500);
for (const s of steps) {
  if (s.run) { const r = await evalJs(s.run); if (r.error) console.error('cdp_shot: --run threw: ' + r.error); }
  if (s.wait) {
    // A wait that never comes true still gets its picture: leave 20 s of the
    // budget for the shots after it.
    let ok = false, lastLog = 0;
    while (Date.now() < deadline - 20000) {
      const r = await evalJs(s.wait);
      if (r.value) { ok = true; break; }
      if (statusExpr && Date.now() - lastLog > 10000) { lastLog = Date.now(); const st = await evalJs(statusExpr); console.log('  ... ' + (st.error || st.value)); }
      await sleep(500);
    }
    if (!ok) { console.error('cdp_shot: --wait never true (shooting anyway): ' + s.wait); }
  }
  if (s.shot) {
    await sleep(settle);
    const r = await cmd('Page.captureScreenshot', {format: 'png'});
    writeFileSync(s.shot, Buffer.from(r.result.data, 'base64'));
    console.log('cdp_shot: wrote ' + s.shot);
  }
}
done(0);
