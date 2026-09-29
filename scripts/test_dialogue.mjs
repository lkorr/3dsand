// test_dialogue.mjs - the tuner Dialogue tab's data layer over the shipped
// conversations (docs/PLAN_world_editor.md P3). No browser, no game.
//
//   node scripts/test_dialogue.mjs
//
// Asserts: every assets/dialogue/*.json validates with no ERRORS under the
// tab's validator (the same checks game/dialogue.cpp runs), the sample has no
// warnings either, every file is already in the writer's canonical layout
// (so a save from the tab diffs as the edit and nothing else), and the
// validator catches the four §2.7 faults in a broken fixture.

import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import * as dl from '../assets/editor/dialoguelib.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const dir = path.join(root, 'assets', 'dialogue');
const itemsJson = JSON.parse(fs.readFileSync(path.join(root, 'assets', 'items', 'items.json'), 'utf8'));
const items = (Array.isArray(itemsJson) ? itemsJson : itemsJson.items || []).map(i => i.name || i.id);

let fails = 0;
const check = (ok, what) => { if (!ok) { fails++; console.log('FAIL ' + what); } else console.log('ok   ' + what); };

const files = fs.readdirSync(dir).filter(f => f.endsWith('.json')).sort().map(f => {
  const text = fs.readFileSync(path.join(dir, f), 'utf8');
  let data;
  try { data = JSON.parse(text); } catch (e) { data = {__error: e.message}; }
  return {name: f.slice(0, -5), text, data};
});
check(files.some(f => f.name === 'sample_stranger'), 'sample_stranger.json is present');
const ps = dl.validateAll(files, items);
for (const p of ps) console.log('     ' + dl.problemLine(p));
check(!ps.some(p => p.error), 'no file has an error');
check(!ps.some(p => p.file === 'sample_stranger.json'), 'the sample has no warnings');
for (const f of files) {
  if (f.data.__error) continue;
  const out = dl.serialize(f.data);
  check(out === f.text.replace(/\r\n/g, '\n'), `${f.name}.json is in the canonical layout (serialize(parse) is identity)`);
}

const broken = {name: 'broken', data: {
  entry: [{goto: 'start'}],
  nodes: [
    {id: 'start', text: 'hi', choices: [{text: 'go', goto: 'nowhere'}, {text: 'take', do: [{take: 'no_such_item'}]}]},
    {id: 'island', text: 'nobody comes here', do: [{set: 'orphan_flag'}]},
  ]}};
const bp = dl.validateAll([broken], items);
const has = (err, node, field, msg) => bp.some(p => p.error === err && p.node === node && p.field.includes(field) && p.msg.includes(msg));
check(has(true, 'start', 'choices[0].goto', 'not a node'), 'dangling goto is an error');
check(has(true, 'start', 'choices[1].do[0]', 'not an item'), 'unknown item is an error');
check(has(false, 'island', '', 'cannot be reached'), 'unreachable node is a warning');
check(bp.some(p => !p.error && p.field === "flag 'orphan_flag'" && p.msg.includes('no condition reads it')), 'flag written never read is a warning');

// The rename helper keeps every reference.
const d = JSON.parse(JSON.stringify(files.find(f => f.name === 'sample_stranger').data));
dl.renameNode(d, 'ask', 'questions');
check(!dl.validateAll([{name: 'sample_stranger', data: d}], items).some(p => p.error), 'renaming a node rewrites every goto that named it');

console.log(fails ? `test_dialogue: ${fails} FAILED` : 'test_dialogue: PASS');
process.exit(fails ? 1 : 0);
