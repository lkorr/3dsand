#!/usr/bin/env node
/* generator_parity.mjs — runs the generator data gates, so nobody has to remember to.
 *
 *   node scripts/generator_parity.mjs                    all of them, in parallel
 *   node scripts/generator_parity.mjs --changed <path>   only those <path> can move
 *   node scripts/generator_parity.mjs --cache <file>     reuse the recorded verdicts
 *                                                        when no input changed
 *   node scripts/generator_parity.mjs --only <name,...>  a subset by name
 *
 * WHY THIS EXISTS. scripts/test_mobgen.mjs, test_anatomy.mjs and
 * test_environment.mjs are the only things that hold the Node generators
 * (mobgen.js, anatomy.js, biomegen.js / watergen.js / treegen.js) to the assets
 * they claim to describe, and until 2026-09-24 (rule-unification W1-E) nothing
 * ran them: they were green on the day each was written and nobody knew the day
 * that stopped. This is the one place that knows WHICH test reads WHAT, and it
 * has two callers:
 *
 *   scripts/post_edit_check.sh   `--changed <file>` after every Edit/Write, so
 *                                an edit to a generator or its data is checked
 *                                in the same turn it is made;
 *   the `generator-parity` gate  (src/test/selftest_generators.cpp) with
 *                                `--cache build/generator_parity.json`, so a
 *                                full --selftest / --verify says it too. The
 *                                gate SKIPS when node is not on PATH.
 *
 * ROUTING. A test runs for a changed file when the file is the test itself, is
 * in the test's IMPORT CLOSURE (parsed out of the sources, so a new import is
 * picked up without editing this table), or sits under one of the data
 * directories the test reads. A UI module no test imports (editor.js, envui.js,
 * ...) cannot move any test's answer, so it runs nothing.
 *
 * THE CACHE (--cache). The three tests are pure functions of files on disk. The
 * key is a SHA-1 over every file under every input directory below plus the test
 * scripts, this runner and the node version — a deliberate SUPERSET of what the
 * tests read, because a stale cache is a false PASS and that is the one failure
 * this whole file exists to prevent. On a hit the recorded verdicts (red ones
 * included) are replayed in milliseconds; on a miss all three run in parallel
 * (~16 s wall, test_environment.mjs is the long pole) and are recorded.
 *
 * OUTPUT. One line per test, `<name>: PASS|FAIL (<its own summary line>)`, the
 * failing checks of any red test indented under it, and a final line
 * `generator-parity: PASS|FAIL <n>/<m> [cached]` that the C++ gate parses.
 * Exit 0 when every selected test passed, 1 otherwise.
 */
import fs from 'fs';
import path from 'path';
import crypto from 'crypto';
import { spawn } from 'child_process';
import { fileURLToPath } from 'url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const rel = p => path.relative(ROOT, path.resolve(ROOT, p)).split(path.sep).join('/');

// The table. `reads` are ROOT-relative prefixes of the DATA each test opens;
// its code inputs come from the import closure, not from here.
const TESTS = [
  { name: 'mobgen', script: 'scripts/test_mobgen.mjs',
    reads: ['assets/mobs/', 'assets/anims/', 'assets/materials/', 'src/game/pose.cpp',
            'build/sidecar_resolved.json'] },
  { name: 'anatomy', script: 'scripts/test_anatomy.mjs',
    reads: ['assets/mobs/', 'assets/materials/'] },
  { name: 'environment', script: 'scripts/test_environment.mjs',
    reads: ['assets/biomes/', 'assets/trees/', 'assets/water/', 'assets/worldmap/',
            'assets/materials/', 'tests/env_predictions.json'] },
];

// ---- import closure -----------------------------------------------------------
const SPEC = [
  /\bfrom\s*['"](\.{1,2}\/[^'"]+)['"]/g,            // import ... from './x.js'
  /\bimport\s*\(?\s*['"](\.{1,2}\/[^'"]+)['"]/g,     // import './x.js' / import('./x.js')
];
const LOAD = /\bload\(\s*['"]([^'"]+)['"]\s*\)/g;    // load('assets/editor/x.js'), ROOT-relative
function closure(script) {
  const seen = new Set();
  const walk = r => {
    if (seen.has(r)) return;
    const abs = path.join(ROOT, r);
    if (!fs.existsSync(abs)) return;
    seen.add(r);
    const src = fs.readFileSync(abs, 'utf8');
    for (const re of SPEC)
      for (const m of src.matchAll(re)) walk(rel(path.join(path.dirname(abs), m[1])));
    for (const m of src.matchAll(LOAD)) walk(rel(m[1]));
  };
  walk(script);
  return seen;
}

// ---- args ---------------------------------------------------------------------
const argv = process.argv.slice(2);
const arg = k => { const i = argv.indexOf(k); return i >= 0 ? argv[i + 1] : null; };
const changed = arg('--changed');
const cachePath = arg('--cache');
const only = arg('--only');

const self = rel(fileURLToPath(import.meta.url));
let selected = TESTS.map(t => ({ ...t, code: closure(t.script) }));
if (only) {
  const want = new Set(only.split(','));
  selected = selected.filter(t => want.has(t.name));
}
if (changed) {
  const c = rel(changed);
  selected = selected.filter(t => c === self || t.code.has(c) || t.reads.some(r => c.startsWith(r)));
  if (!selected.length) process.exit(0);   // nothing this file can move
}

// ---- cache key ----------------------------------------------------------------
function listFiles(r) {
  const abs = path.join(ROOT, r);
  if (!fs.existsSync(abs)) return [];
  if (fs.statSync(abs).isFile()) return [r];
  return fs.readdirSync(abs, { withFileTypes: true }).flatMap(d =>
    listFiles(r.replace(/\/?$/, '/') + d.name));
}
function inputKey() {
  const files = new Set([self]);
  for (const t of TESTS) {
    for (const f of closure(t.script)) files.add(f);
    for (const r of t.reads) for (const f of listFiles(r)) files.add(f);
  }
  const h = crypto.createHash('sha1').update(process.version);
  for (const f of [...files].sort()) {
    h.update(f); h.update('\0');
    // Line endings normalised: a CRLF checkout and an LF one are the same input.
    const b = fs.readFileSync(path.join(ROOT, f));
    h.update(/\.(json|js|mjs|cpp|h)$/.test(f) ? b.toString('latin1').replace(/\r\n/g, '\n') : b);
    h.update('\0');
  }
  return h.digest('hex');
}

// ---- run ----------------------------------------------------------------------
function runOne(t) {
  return new Promise(resolve => {
    const t0 = Date.now();
    const p = spawn(process.execPath, [path.join(ROOT, t.script)], { cwd: ROOT });
    let out = '';
    p.stdout.on('data', d => out += d);
    p.stderr.on('data', d => out += d);
    p.on('close', code => {
      const lines = out.split(/\r?\n/).map(l => l.trimEnd()).filter(Boolean);
      // Each test ends with its own verdict line; keep it verbatim.
      const summary = lines.length ? lines[lines.length - 1].trim() : '(no output)';
      const failing = [];
      for (let i = 0; i < lines.length; i++)
        if (/^\s*FAIL\b/.test(lines[i]) && lines[i].trim() !== summary) {
          failing.push(lines[i].trim());
          // test_mobgen puts the reason on the next, deeper-indented line.
          if (i + 1 < lines.length && /^\s{6,}\S/.test(lines[i + 1]) && !/^\s*(FAIL|ok|SKIP)\b/.test(lines[i + 1]))
            failing.push('  ' + lines[i + 1].trim());
        }
      resolve({ name: t.name, pass: code === 0, summary, failing: failing.slice(0, 24),
                seconds: (Date.now() - t0) / 1000 });
    });
  });
}

let key = null, results = null, cached = false;
if (cachePath) {
  key = inputKey();
  try {
    const c = JSON.parse(fs.readFileSync(cachePath, 'utf8'));
    if (c.key === key && selected.every(t => c.results.some(r => r.name === t.name))) {
      results = c.results; cached = true;
    }
  } catch { /* absent or unreadable: run */ }
}
if (!results) {
  // With a cache, run EVERY test so the record is complete for the next caller.
  const run = cachePath ? TESTS : selected;
  results = await Promise.all(run.map(runOne));
  if (cachePath) {
    // Re-key AFTER the run: test_environment.mjs may have (re)written
    // tests/env_predictions.json, which is one of its own inputs.
    key = inputKey();
    fs.mkdirSync(path.dirname(path.resolve(cachePath)), { recursive: true });
    fs.writeFileSync(cachePath, JSON.stringify({ key, results }, null, 1) + '\n');
  }
}

let passed = 0;
for (const t of selected) {
  const r = results.find(x => x.name === t.name);
  if (r.pass) passed++;
  console.log(`${r.name}: ${r.pass ? 'PASS' : 'FAIL'} (${r.summary}${cached ? '' : `, ${r.seconds.toFixed(1)} s`})`);
  if (!r.pass) for (const l of r.failing) console.log(`    ${l}`);
}
const ok = passed === selected.length;
console.log(`generator-parity: ${ok ? 'PASS' : 'FAIL'} ${passed}/${selected.length}` +
            (cached ? ' cached' : ''));
process.exit(ok ? 0 : 1);
