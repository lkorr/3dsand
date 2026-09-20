/* sidecar.js — THE SIDECAR RESOLVER, in JavaScript.
 *
 * src/game/sidecar.cpp is this file, rule for rule, and its header carries the
 * argument for each rule and for why they are the only three. Read that first;
 * what follows is the mirror, not a second design.
 *
 *   1. RFC 7396 merge-patch: objects merge, scalars and arrays replace, an
 *      explicit `null` deletes.
 *   2. NAMED-ARRAY MERGE: an array whose elements ALL carry a string `name`
 *      merges by name, base order preserved, unmatched names appended.
 *   3. CLIP RETIME: a clip patch with `durationMs` and no `tracks` scales the
 *      inherited key times by new/old.
 *
 * Plus `effects`: `assets/mobs/effects/<name>.json`, `{patch, scale}`, applied
 * after the `extends` chain resolves, accumulating down the chain.
 *
 * PURE AND NODE-RUNNABLE. It takes a `read(path) -> object` callback rather
 * than fetching or reading files itself, because the browser has `/api/model`
 * and the scripts have `fs` and neither belongs in a merge rule.
 *
 * `--gate sidecar-resolve` dumps what the C++ resolved; test_mobgen.mjs
 * section N diffs it against what this file resolves for the same sidecars,
 * which is what keeps the two implementations one implementation.
 */

export const MAX_EXTENDS = 8;

/** An array whose EVERY element is an object with a string `name`. One unnamed
 *  element opts the whole array back out to the RFC replace rule — the
 *  conservative direction, since a replace where a merge was wanted shows up
 *  in the file and a merge where a replace was wanted does not. */
export function allNamed(a) {
  return Array.isArray(a) && a.length > 0 &&
    a.every(e => e && typeof e === 'object' && !Array.isArray(e) &&
                 typeof e.name === 'string');
}

const findNamed = (arr, name) =>
  arr.find(e => e && typeof e === 'object' && e.name === name);

/** Member of the top-level `clips` object — the one place the retime fires.
 *  Asked by PATH because there is no structural test that would not also fire
 *  on a limb that happened to carry a `durationMs`. */
const isClipSlot = p => p.startsWith('/clips/') && p.indexOf('/', 7) === -1;

/** JS Math.round on a non-negative value is C++'s floor(v + 0.5). Key times
 *  are never negative, so the two languages land on the same millisecond. */
const roundHalfUp = v => Math.floor(v + 0.5);

/** Scale a clip's key times for a new duration. The BLENDS are left alone: a
 *  blend is how long the transition takes, not how long the stride does. */
export function retimeClip(clip, oldMs, newMs) {
  if (!(oldMs > 0) || !(newMs > 0) || oldMs === newMs) return clip;
  if (!clip.tracks || typeof clip.tracks !== 'object') return clip;
  const ratio = newMs / oldMs;
  for (const track of Object.values(clip.tracks)) {
    if (!track || typeof track !== 'object') continue;
    for (const chan of ['rot', 'pos']) {
      if (!Array.isArray(track[chan])) continue;
      for (const key of track[chan])
        if (key && typeof key.t === 'number') key.t = roundHalfUp(key.t * ratio);
    }
  }
  return clip;
}

function mergeNamedArray(target, patch, path) {
  const out = [];
  for (const base of target) {
    const over = findNamed(patch, base.name);
    out.push(over ? mergePatch(base, over, `${path}/${base.name}`) : base);
  }
  // Unmatched names APPEND, after the base's own order — never displacing a
  // limb the save file counts on finding where it was.
  for (const p of patch)
    if (!findNamed(target, p.name)) out.push(JSON.parse(JSON.stringify(p)));
  return out;
}

/** Merge `patch` into `target`, returning the result. `target` is mutated the
 *  way nlohmann's merge does; callers that care pass a clone. */
export function mergePatch(target, patch, path = '') {
  if (patch === null || typeof patch !== 'object' || Array.isArray(patch))
    return patch;
  const out = (target && typeof target === 'object' && !Array.isArray(target))
    ? target : {};
  for (const k of Object.keys(patch)) {
    const v = patch[k];
    if (v === null) { delete out[k]; continue; }
    const sub = `${path}/${k}`;
    // Cloned on insert, so a def never ends up holding a reference into the
    // patch document — an effect applied to two bodies would otherwise give
    // them one shared object to mutate.
    if (!(k in out)) { out[k] = JSON.parse(JSON.stringify(v)); continue; }
    // THE RETIME, BEFORE the duration is overwritten: the factor is new/old.
    if (isClipSlot(sub) && v && typeof v === 'object' && !Array.isArray(v) &&
        typeof v.durationMs === 'number' && !('tracks' in v) &&
        out[k] && typeof out[k].durationMs === 'number')
      retimeClip(out[k], out[k].durationMs, v.durationMs);
    if (allNamed(out[k]) && allNamed(v)) {
      out[k] = mergeNamedArray(out[k], v, sub);
      continue;
    }
    out[k] = mergePatch(out[k], v, sub);
  }
  return out;
}

/** Multiply the number at a dotted path. Returns a note if it reached nothing,
 *  so the caller can log it the way the C++ does. */
function scaleAt(def, dotted, factor) {
  const segs = dotted.split('.');
  let node = def;
  for (let i = 0; i < segs.length - 1; i++) {
    if (!node || typeof node !== 'object' || !(segs[i] in node))
      return `scale.${dotted} names nothing this body has a number at`;
    node = node[segs[i]];
  }
  const last = segs[segs.length - 1];
  if (!node || typeof node !== 'object' || typeof node[last] !== 'number')
    return `scale.${dotted} names nothing this body has a number at`;
  // ALWAYS A FLOAT, even where the base wrote an integer: `cadence: 8` scaled
  // by 0.8 is 6.4, and rounding it back because the author omitted a decimal
  // point would quantise every ratio an effect is made of.
  node[last] = node[last] * factor;
  return null;
}

/** Apply one `{patch, scale}` effect document to a resolved def, in place. */
export function applyEffect(def, effect, where, log = []) {
  if (!effect || typeof effect !== 'object') {
    log.push(`${where}: effect is not a JSON object — ignored`);
    return def;
  }
  if (effect.patch) {
    if (typeof effect.patch === 'object' && !Array.isArray(effect.patch))
      mergePatch(def, effect.patch, '');
    else log.push(`${where}: effect \`patch\` is not an object — ignored`);
  }
  if (!effect.scale) return def;
  if (typeof effect.scale !== 'object' || Array.isArray(effect.scale)) {
    log.push(`${where}: effect \`scale\` is not an object — ignored`);
    return def;
  }
  for (const [p, factor] of Object.entries(effect.scale)) {
    if (typeof factor !== 'number') {
      log.push(`${where}: scale.${p} is not a number — ignored`);
      continue;
    }
    const note = scaleAt(def, p, factor);
    if (note) log.push(`${where}: ${note} — ignored`);
  }
  return def;
}

/** Resolve an `extends` chain. `read(stem)` returns the parsed sidecar for a
 *  stem in the mob directory. `effects` accumulates base-first and deduped:
 *  a creature that extends a zombie is a zombie, and the plain replace rule
 *  would have made "inherit the zombie and change one thing" drop the zombie.
 *  An explicit `null` still clears it — the RFC rule, the deliberate way off. */
export function resolveExtends(read, stem, depth = 0) {
  if (depth > MAX_EXTENDS)
    throw new Error('`extends` nested more than 8 deep (a cycle?)');
  const child = JSON.parse(JSON.stringify(read(stem)));
  const base = typeof child.extends === 'string' ? child.extends : '';
  if (!base) return child;
  const parent = resolveExtends(read, base, depth + 1);
  if (Array.isArray(child.effects) && Array.isArray(parent.effects)) {
    const combined = [...parent.effects];
    for (const e of child.effects)
      if (!combined.some(c => JSON.stringify(c) === JSON.stringify(e)))
        combined.push(e);
    child.effects = combined;
  }
  // Not inherited: the chain is resolved here, once.
  delete child.extends;
  return mergePatch(parent, child, '');
}

/** THE ONE ENTRY POINT: `extends`, then the accumulated `effects`.
 *  `readEffect(name)` returns a parsed effect document, or null if there is no
 *  such file — a missing effect is loud but not fatal, because the def is the
 *  creature and the effect is a coat of paint on it. */
export function resolveSidecar(read, stem, readEffect = () => null, log = []) {
  const doc = resolveExtends(read, stem);
  if (!doc.effects) return doc;
  if (!Array.isArray(doc.effects)) {
    log.push(`${stem}: \`effects\` is not an array of names — ignored`);
    delete doc.effects;
    return doc;
  }
  for (const name of doc.effects) {
    if (typeof name !== 'string') {
      log.push(`${stem}: an \`effects\` entry is not a name — ignored`);
      continue;
    }
    let fx = null;
    try { fx = readEffect(name); } catch { fx = null; }
    if (!fx) {
      log.push(`${stem}: effect "${name}" did not load — this body is ` +
               'unmodified');
      continue;
    }
    applyEffect(doc, fx, `effects/${name}.json`, log);
  }
  return doc;
}

/* ===========================================================================
 * THE OTHER DIRECTION: what does this body say that its base does not?
 *
 * A thin character is not written by picking the keys somebody believed were
 * per-body. That list goes stale the first commit a derived field is added to
 * the generator, and "the generator forgot to emit a field, so the character
 * silently inherited the human's" is the exact failure this whole seam exists
 * to end. So the emitter DIFFS: generate the whole body, diff it against the
 * resolved base, write the difference. Whatever the generator derives that
 * equals the base is dropped — including the fields nobody has thought about
 * yet.
 *
 * The diff is the inverse of the merge above, rule for rule, and it CHECKS
 * ITSELF: `diffAgainst` re-applies its own output to the base and throws if
 * the result is not the document it was given. That is what makes the thin
 * file safe to write without a second gate watching it.
 * ======================================================================== */

const deepEqual = (a, b) => JSON.stringify(canonical(a)) === JSON.stringify(canonical(b));
const clone = o => JSON.parse(JSON.stringify(o));

/** `//`-prefixed keys are authoring notes, never read by any loader, and so
 *  never a difference between two bodies. */
function noNotes(o) {
  if (Array.isArray(o)) return o.map(noNotes);
  if (o && typeof o === 'object') {
    const r = {};
    for (const k of Object.keys(o)) if (!k.startsWith('//')) r[k] = noNotes(o[k]);
    return r;
  }
  return o;
}

/** Key order is JSON punctuation, not content. Sorting before comparing keeps
 *  a re-ordered emitter from looking like a changed body. */
function canonical(o) {
  if (Array.isArray(o)) return o.map(canonical);
  if (o && typeof o === 'object') {
    const r = {};
    for (const k of Object.keys(o).sort()) r[k] = canonical(o[k]);
    return r;
  }
  return o;
}

function diffNamedArray(base, target, path) {
  const baseNames = base.map(e => e.name);
  const targetNames = target.map(e => e.name);
  // The merge appends unmatched names after the base's own order and can
  // neither reorder nor remove. A body that needs either is not a variant of
  // this base, and saying so here is better than writing a file that resolves
  // into a different rig.
  for (const n of baseNames)
    if (!targetNames.includes(n))
      throw new Error(`${path}: "${n}" is in the base and not in this body; ` +
                      'the named-array merge cannot remove an element');
  const kept = targetNames.filter(n => baseNames.includes(n));
  if (kept.join(',') !== baseNames.join(','))
    throw new Error(`${path}: this body reorders the base's elements ` +
                    `(${kept.join(',')} vs ${baseNames.join(',')}); the ` +
                    'named-array merge preserves base order and cannot');
  const out = [];
  for (const el of target) {
    const b = base.find(e => e.name === el.name);
    if (!b) { out.push(clone(el)); continue; }        // appended: emit whole
    const d = diffAgainst(b, el, `${path}/${el.name}`);
    if (d !== undefined) out.push({ name: el.name, ...d });
  }
  return out.length ? out : undefined;
}

/** The merge-patch that turns `base` into `target`, or undefined if they are
 *  already the same document. Throws if the difference is not expressible. */
export function diffAgainst(base, target, path = '') {
  if (deepEqual(noNotes(base), noNotes(target))) return undefined;
  if (target === null || typeof target !== 'object' || Array.isArray(target) ||
      base === null || typeof base !== 'object' || Array.isArray(base))
    return clone(target);                             // a scalar or array replaces
  const patch = {};
  for (const k of Object.keys(target)) {
    // AUTHORING NOTES ARE NOT CONTENT. `//`-prefixed keys are for a human
    // reader and no loader reads them (mob.cpp, and test_mobgen's noNotes); a
    // derived file that restated or deleted its base's notes would be adding
    // punctuation to a document whose whole point is to be short.
    if (k.startsWith('//')) continue;
    const sub = `${path}/${k}`;
    if (!(k in base)) { patch[k] = clone(target[k]); continue; }
    if (deepEqual(noNotes(base[k]), noNotes(target[k]))) continue;
    // A CLIP THAT IS ONLY THE BASE'S CLIP ON ANOTHER CLOCK costs two words
    // instead of five kilobytes. Tried first, and only accepted if the retime
    // actually reproduces this clip key for key — a clip whose shape changed
    // as well is emitted in full.
    if (isClipSlot(sub) && typeof target[k].durationMs === 'number' &&
        typeof base[k].durationMs === 'number') {
      const trial = retimeClip(clone(base[k]), base[k].durationMs,
                               target[k].durationMs);
      trial.durationMs = target[k].durationMs;
      if (deepEqual(trial, target[k])) {
        patch[k] = { durationMs: target[k].durationMs };
        continue;
      }
    }
    if (allNamed(base[k]) && allNamed(target[k])) {
      const d = diffNamedArray(base[k], target[k], sub);
      if (d !== undefined) patch[k] = d;
      continue;
    }
    const d = diffAgainst(base[k], target[k], sub);
    if (d !== undefined) patch[k] = d;
  }
  // A key the base has and this body does not is an explicit null (RFC 7396).
  for (const k of Object.keys(base))
    if (!k.startsWith('//') && !(k in target)) patch[k] = null;
  if (!Object.keys(patch).length) return undefined;
  // SELF-CHECK. The diff is only worth writing if re-applying it reproduces
  // the body exactly, and at the root that is a claim about the whole file.
  if (path === '') {
    const back = mergePatch(clone(base), clone(patch), '');
    if (!deepEqual(noNotes(back), noNotes(target)))
      throw new Error('the diff does not resolve back to the body it came ' +
                      'from — refusing to write a thin sidecar that would ' +
                      'load as a different creature');
  }
  return patch;
}
