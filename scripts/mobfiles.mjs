/* mobfiles.mjs — where a mob's files live, for the Node scripts.
 *
 * assets/mobs/<proto>.{vox,json} is a PROTOTYPE (human, critter, dummy);
 * assets/mobs/<proto>/<name>.{vox,json} is a VARIANT of it (a generated
 * character, a recolour, a zombie of somebody). A def is named for its STEM
 * wherever it is filed, so `extends`/`model` name stems and every reader here
 * goes stem -> path through mobFile. The same rule, in the same order, as
 * src/game/sidecar.h "PROTOTYPES AND VARIANTS" (IsVariantDir / StemPath /
 * ListStems) — effects/ and pool/ are not variant folders.
 */
import { existsSync, readdirSync, statSync } from 'fs';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';

export const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
export const MOBS = join(ROOT, 'assets/mobs');

export const isVariantDir = n =>
  !!n && n !== 'effects' && n !== 'pool' && !n.startsWith('.');

/** The variant folders under `dir`, sorted. */
export function variantDirs(dir = MOBS) {
  let names = [];
  try { names = readdirSync(dir); } catch { return []; }
  return names.filter(n => isVariantDir(n) &&
                           statSync(join(dir, n)).isDirectory()).sort();
}

/** Every `<ext>` file in `dir` and its variant folders, as
 *  {stem, path, folder} sorted by stem; a stem filed twice keeps the root copy
 *  (then the first variant folder), as the engine's loader does. `folder` is ''
 *  for a prototype, else the prototype it is a variant of. */
export function listMobFiles(ext, dir = MOBS) {
  const out = new Map();
  for (const folder of ['', ...variantDirs(dir)]) {
    const d = folder ? join(dir, folder) : dir;
    for (const n of readdirSync(d).sort()) {
      if (!n.endsWith(ext)) continue;
      const stem = n.slice(0, -ext.length);
      if (!out.has(stem)) out.set(stem, { stem, path: join(d, n), folder });
    }
  }
  return [...out.values()].sort((a, b) => a.stem.localeCompare(b.stem));
}

/** `<dir>/<stem><ext>` if it exists, else the first variant folder holding it,
 *  else the root path (so a miss names where it was expected). */
export function mobFile(stem, ext, dir = MOBS) {
  const root = join(dir, stem + ext);
  if (existsSync(root)) return root;
  for (const f of variantDirs(dir)) {
    const p = join(dir, f, stem + ext);
    if (existsSync(p)) return p;
  }
  return root;
}

/** The prototype a stem belongs to: its own folder's name if it is filed as a
 *  variant, else itself. A variant of a variant files beside its base. */
export function protoOf(stem, dir = MOBS) {
  for (const f of variantDirs(dir))
    if (existsSync(join(dir, f, stem + '.json'))) return f;
  return stem;
}
