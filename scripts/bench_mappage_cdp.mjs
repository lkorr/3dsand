// bench_mappage_cdp.mjs — the Chrome DevTools Protocol half of
// scripts/bench_mappage.sh: attach to a headless Chrome started with
// --remote-debugging-port, open assets/mappage_bench.html, wait (in REAL time)
// for its title to become 'BENCH DONE' / 'BENCH FAIL', print its table, and
// optionally write window.benchResult to a JSON file.
//
//   node scripts/bench_mappage_cdp.mjs <devtools port> <bench url> [out.json] [timeout s]
//
// Node >= 22 (the global WebSocket).
import {writeFileSync} from 'node:fs';

const [, , dport, url, outFile, tmo] = process.argv;
const timeoutMs = (+tmo || 600) * 1000;
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function targets() {
  for (let i = 0; i < 100; i++) {
    try { const r = await fetch(`http://127.0.0.1:${dport}/json/list`); if (r.ok) return await r.json(); } catch (e) {}
    await sleep(200);
  }
  throw new Error('no DevTools endpoint on port ' + dport);
}
const list = await targets();
const page = list.find(t => t.type === 'page');
if (!page) throw new Error('no page target');
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
let seq = 0;
const waiting = new Map();
ws.onmessage = m => {
  const d = JSON.parse(m.data);
  if (d.method === 'Runtime.consoleAPICalled') {
    const a = d.params.args.map(x => x.value).join(' ');
    if (/^BENCH |error|Error/.test(a)) console.error('[page] ' + a.slice(0, 300));
  }
  if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
};
const send = (method, params = {}) => new Promise(res => { const id = ++seq; waiting.set(id, res); ws.send(JSON.stringify({id, method, params})); });
const evalJs = async expr => { const r = await send('Runtime.evaluate', {expression: expr, returnByValue: true}); return r.result && r.result.result ? r.result.result.value : undefined; };

await send('Runtime.enable');
await send('Page.enable');
// BENCH_PROFILE=<file>: a sampled CPU profile of the whole run (all frames of
// the page, the tuner iframe included), the top self-time functions printed.
const PROFILE = process.env.BENCH_PROFILE || '';
if (PROFILE) { await send('Profiler.enable'); await send('Profiler.setSamplingInterval', {interval: 250}); await send('Profiler.start'); }
await send('Page.navigate', {url});
const t0 = Date.now();
let title = '';
while (Date.now() - t0 < timeoutMs) {
  await sleep(1000);
  title = await evalJs('document.title');
  if (title === 'BENCH DONE' || title === 'BENCH FAIL') break;
}
const res = await evalJs('JSON.stringify(window.benchResult || null)');
const r = res ? JSON.parse(res) : null;
if (!r) { console.log('bench: no result (title "' + title + '") after ' + Math.round((Date.now() - t0) / 1000) + ' s'); process.exit(1); }
if (r.error) { console.log("bench: FAIL " + r.error); await Promise.race([send("Browser.close"), sleep(2000)]); process.exit(1); }
console.log(r.text);
if (PROFILE) {
  const p = (await send('Profiler.stop')).result.profile;
  writeFileSync(PROFILE, JSON.stringify(p));
  const dt = {}, self = new Map();
  const tot = p.timeDeltas.reduce((a, b) => a + b, 0);
  p.samples.forEach((id, i) => { self.set(id, (self.get(id) || 0) + (p.timeDeltas[i] || 0)); });
  for (const n of p.nodes) {
    const f = n.callFrame, k = (f.functionName || '(anon)') + ' ' + f.url.replace(/^.*\//, '') + ':' + (f.lineNumber + 1);
    dt[k] = (dt[k] || 0) + (self.get(n.id) || 0);
  }
  console.log('\nCPU profile, top self time (of ' + (tot / 1e6).toFixed(1) + ' s):');
  for (const [k, us] of Object.entries(dt).sort((a, b) => b[1] - a[1]).slice(0, 40)) console.log('  ' + (us / 1000).toFixed(0).padStart(7) + ' ms  ' + k);
}
if (outFile) writeFileSync(outFile, JSON.stringify(r, null, 1));
await Promise.race([send("Browser.close"), sleep(2000)]);
ws.close();
process.exit(0);
