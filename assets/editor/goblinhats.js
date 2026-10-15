/* goblinhats.js — THE GOBLIN'S HATS (goblin.js extras).
 *
 * EVERY HAT IS WORN, NOT BALANCED. The first hats were shapes stood on top of
 * the skull from half way up it, so the painted hair showed as a dark band
 * between the hat and the brow and every hat read as a lid. Now each one
 * starts from a FITTED CROWN: the skull's own surface above the brow, dilated
 * outward by the hat's thickness (`fitShell`), so it follows the head's real
 * shape -- a goblin skull is wide and low -- and comes down to the brow. Its
 * character is built on that: a stocking that flops, a mushroom's dome, a
 * pot's belly, a top hat's crown.
 *
 * THE HATS (HATS), each with what its variation genes do:
 *   redcap     a knitted stocking cap: rolled band, a wrinkled tube that
 *              flops back (height = length, flop = how far it hangs, tilt =
 *              to which side, detail = the bobble, wear = patches and holes)
 *   hood       a pointed hood round the face, ear slits where the ears come
 *              through, a long tail of point down the back (height = the
 *              point's length, flop = its hang, detail = the face's trim)
 *   mushroom   a toadstool cap: a dome wider than the head, gill grooves
 *              under the rim, raised warts on top (size = spread, height =
 *              dome, flop = the rim's droop, detail = warts, wear = nibbles)
 *   morel      a morel's honeycomb cone (height, flop = lean, detail = the
 *              cells' density)
 *   pot        an upturned cooking pot: a bellied wall, a rolled lip, a foot
 *              ring on top, two loop handles or (detail) a saucepan's long
 *              handle (height = depth, wear = dents)
 *   bonecrown  a leather band round the brow ringed with bone spikes, the
 *              longest at the front (height = spike length, detail = how
 *              many, flop = how far they curl out, wear = chipped)
 *   skull      a beast's skull worn as a helm: a bone dome with sutures and
 *              empty sockets, the upper jaw and teeth jutting over the brow,
 *              and (height) horns curling from the temples
 *   tophat     a stolen top hat, battered: a tall crown cocked and crumpled
 *              (flop), a brim curled at the sides, a band and (detail) a
 *              feather in it
 *   witch      a wide drooping brim and a tall cone bent over at the top,
 *              wrinkled, a band and (detail) a buckle
 *
 * Slots: the body of a hat is HAT, its trim / band / bobble / gills / warts /
 * ridges HAT_TRIM, its metal RING, its bone TUSK, dark holes PUPIL (goblin.js
 * SLOTS). What each slot is MADE of depends on the hat (goblin.js HAT_MATS).
 * PURE AND NODE-RUNNABLE; scene coordinates (x right, y BACK, z up).
 */

'use strict';

import { rnd, vnoise, clamp } from './robotkit.js';

export const HATS = ['none', 'redcap', 'hood', 'mushroom', 'morel', 'pot',
                     'bonecrown', 'skull', 'tophat', 'witch'];
/** Cloth hats wear into patches and holes; hard ones into dents and chips. */
const CLOTH = new Set(['redcap', 'hood', 'tophat', 'witch']);

const norm3 = a => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };

/**
 * Build the hat `s.hat` into `env.out` (the head extras map, K -> cell).
 * env: { K, body, out, mass, hc (head scene cells), eyeZ, mouthZ, SLOTS }.
 */
export function buildHat(s, env) {
  const { K, body, out, mass, hc, eyeZ, eyeTop, mouthZ, SLOTS } = env;
  const HAT = SLOTS.HAT, TRIM = SLOTS.HAT_TRIM, RING = SLOTS.RING,
        BONE = SLOTS.TUSK, DARK = SLOTS.PUPIL;
  const seed = 0x4a7;

  // ---- the head, measured --------------------------------------------------
  let skullTop = -1e9;
  for (const c of hc) skullTop = Math.max(skullTop, c[2]);
  // The brim comes down to the row over the eyes: any higher and the painted
  // hair shows as a dark band under it.
  const browZ = eyeTop !== undefined ? eyeTop + 1 : Math.round(eyeZ + 2.5);
  const top = hc.filter(c => c[2] >= skullTop - 2);
  const cx = top.reduce((a, c) => a + c[0] + 0.5, 0) / top.length;
  const cy = top.reduce((a, c) => a + c[1] + 0.5, 0) / top.length;
  let x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
  for (const [x, y, z] of hc)
    if (Math.abs(z - (browZ + 2)) < 1.5) {
      x0 = Math.min(x0, x); x1 = Math.max(x1, x);
      y0 = Math.min(y0, y); y1 = Math.max(y1, y);
    }
  if (x0 > x1) return;
  const Rx = (x1 - x0 + 1) / 2, Ry = (y1 - y0 + 1) / 2, R = Math.max(Rx, Ry);
  // The skull's WIDEST above the brow (it bulges past the brow's width): a
  // straight-sided crown must clear this or the skull pokes through it.
  let RxMax = Rx, RyMax = Ry;
  for (const [x, y, z] of hc)
    if (z >= browZ) {
      RxMax = Math.max(RxMax, Math.abs(x + 0.5 - cx) + 0.5);
      RyMax = Math.max(RyMax, Math.abs(y + 0.5 - cy) + 0.5);
    }
  const topAt = new Map();
  for (const [x, y, z] of hc) {
    const k = x * 4096 + y;
    if (!topAt.has(k) || z > topAt.get(k)) topAt.set(k, z);
  }
  const headTop = (x, y) => topAt.get(Math.floor(x) * 4096 + Math.floor(y));
  // The skull's exposed cells from a little under the brow up: what a fitted
  // crown is grown from.
  const exposed = hc.filter(([x, y, z]) => z >= browZ - 3 &&
    [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]]
      .some(([dx, dy, dz]) => !body.has(K(x + dx, y + dy, z + dz))));
  const exposedAll = (lo) => lo >= browZ - 3 ? exposed : hc.filter(([x, y, z]) => z >= lo - 3 &&
    [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]]
      .some(([dx, dy, dz]) => !body.has(K(x + dx, y + dy, z + dz))));

  // ---- writing -------------------------------------------------------------
  const hat = new Map();                          // this hat's own cells
  // A floor under which a swept part may not reach (a cone's base ball would
  // otherwise bulge down over the face).
  let zFloor = -1e9;
  const own = (X, Y, Z, c, over = false) => {
    X = Math.floor(X); Y = Math.floor(Y); Z = Math.floor(Z);
    if (Z < zFloor) return false;
    const k = K(X, Y, Z);
    if (body.has(k)) return false;
    // An ear (or a nose) already there wins: the hat wraps round it.
    if (out.has(k) && !hat.has(k)) return false;
    if (hat.has(k) && !over) return false;
    if (mass) mass.delete(k);
    const cell = [X, Y, Z, c];
    out.set(k, cell);
    hat.set(k, cell);
    return true;
  };
  const del = (X, Y, Z) => {
    const k = K(X, Y, Z);
    if (hat.has(k)) { hat.delete(k); out.delete(k); }
  };
  const ball = (p, r, c, over = false) => {
    const Rr = Math.ceil(r);
    for (let dz = -Rr; dz <= Rr; dz++)
      for (let dy = -Rr; dy <= Rr; dy++)
        for (let dx = -Rr; dx <= Rr; dx++) {
          const X = Math.floor(p[0] + dx), Y = Math.floor(p[1] + dy), Z = Math.floor(p[2] + dz);
          const ex = X + 0.5 - p[0], ey = Y + 0.5 - p[1], ez = Z + 0.5 - p[2];
          if (ex * ex + ey * ey + ez * ez <= r * r + 0.2)
            own(X, Y, Z, typeof c === 'function' ? c(X, Y, Z) : c, over);
        }
  };
  /** The FITTED CROWN: every free cell within `thick` of the skull, z in
   *  [zLo, zHi], minus what `skip` refuses. */
  const fitShell = (zLo, zHi, thick, colour, skip) => {
    const T = Math.ceil(thick);
    for (const [x, y, z] of exposedAll(zLo)) {
      if (z < zLo - T || z > zHi + T) continue;
      for (let dz = -T; dz <= T; dz++) {
        const Z = z + dz;
        if (Z < zLo || Z > zHi) continue;
        for (let dy = -T; dy <= T; dy++)
          for (let dx = -T; dx <= T; dx++) {
            if (dx * dx + dy * dy + dz * dz > (thick + 0.35) * (thick + 0.35)) continue;
            const X = x + dx, Y = y + dy;
            if (skip && skip(X, Y, Z)) continue;
            own(X, Y, Z, typeof colour === 'function' ? colour(X, Y, Z) : colour);
          }
      }
    }
  };
  /** A tube swept along a path that starts going `dir0` and turns, step by
   *  step, by `turn(t)` -> the direction at t. Radius `rad(t)`. */
  const sweep = (p0, len, dirAt, rad, colour) => {
    let p = p0.slice();
    const ds = 0.4, n = Math.max(1, Math.ceil(len / ds));
    for (let i = 0; i <= n; i++) {
      const t = i / n;
      ball(p, rad(t), colour);
      const d = norm3(dirAt(t));
      p = [p[0] + d[0] * ds, p[1] + d[1] * ds, p[2] + d[2] * ds];
    }
    return p;
  };
  // Which way a flop goes: back, and to the side the tilt says.
  const flopDir = norm3([s.hatTilt * 0.9, 1, 0]);
  const bendDir = (theta) => [flopDir[0] * Math.sin(theta), flopDir[1] * Math.sin(theta),
                               Math.cos(theta)];

  const H = s.hatHeight, D = s.hatDetail, F = s.hatFlop, Z = s.hatSize;

  // =========================================================================
  if (s.hat === 'redcap') {
    // A rolled band at the brow, the crown fitted over the skull, then a
    // knitted tube that rises, bends and hangs, wrinkled as it goes.
    fitShell(browZ, browZ + 2, 2.3, TRIM);
    fitShell(browZ + 3, skullTop + 1, 1.6, HAT);
    const r0 = Math.min(Rx, Ry) * (0.72 + 0.2 * Z);
    const len = R * (1.0 + H * 2.6);
    zFloor = browZ + 3;
    const tip = sweep([cx, cy + 0.5, skullTop - 1], len,
      t => bendDir(Math.min(Math.PI * 0.95, (0.25 + F * 2.0) * Math.pow(t, 1.25) * Math.PI / 2)),
      t => Math.max(0.9, r0 * Math.pow(1 - t, 0.85) + 0.9 * t) *
           (1 + 0.1 * Math.sin(t * len * 1.3)),
      HAT);
    if (D > 0.05) ball(tip, 1.0 + D * 1.9, TRIM, true);           // the bobble
  }

  // =========================================================================
  else if (s.hat === 'hood') {
    // A hood: the whole head from under the jaw up, open round the face, the
    // ears through slits (they are there first), a point down the back.
    const chinZ = Math.round(mouthZ - 3);
    const openX = Rx * 0.8;
    const open = (X, Y, Z2) => Y + 0.5 < cy - Ry * 0.1 && Z2 <= browZ && Z2 >= chinZ &&
                               Math.abs(X + 0.5 - cx) < openX;
    const rimW = 1 + Math.round(D * 1.5);
    const rim = (X, Y, Z2) => {
      for (let r = 1; r <= rimW; r++)
        if (open(X + r, Y, Z2) || open(X - r, Y, Z2) || open(X, Y, Z2 - r)) return true;
      return false;
    };
    fitShell(chinZ - 3, skullTop + 1, 2.2, (X, Y, Z2) => (rim(X, Y, Z2) ? TRIM : HAT), open);
    const len = R * (0.6 + H * 2.6);
    sweep([cx, cy + Ry * 0.45, skullTop - 1.5], len,
      t => bendDir(Math.min(Math.PI * 0.97, (0.9 + F * 1.2 * t) * Math.PI / 2)),
      t => Math.max(0.8, R * 0.55 * Math.pow(1 - t, 1.1) + 0.8 * t), HAT);
    // A hood hides the hair: all of it.
    if (mass) mass.clear();
  }

  // =========================================================================
  else if (s.hat === 'mushroom') {
    // A toadstool: a dome wider than the head down to just over the brow,
    // the rim drooping, gill grooves underneath, raised warts on top.
    const z0 = browZ + 1;
    const Rm = R * (1.2 + Z * 0.9);
    const Hd = Rm * (0.35 + H * 0.6);
    const ox = s.hatTilt * 2;
    const Rc = Math.ceil(Rm + 1);
    const nGill = Math.max(12, Math.round(Rm * 2.2));
    for (let y = Math.floor(cy) - Rc; y <= Math.floor(cy) + Rc; y++)
      for (let x = Math.floor(cx + ox) - Rc; x <= Math.floor(cx + ox) + Rc; x++) {
        const dd = Math.hypot(x + 0.5 - cx - ox, y + 0.5 - cy);
        const d = dd / Rm;
        if (d > 1) continue;
        const sk = headTop(x, y);
        const droop = F * 3.5 * d * d * d;
        // A full shoulder (a toadstool is a bun, not a lampshade).
        let tp = z0 + Hd * Math.pow(1 - d * d, 0.32) - droop;
        if (sk !== undefined) tp = Math.max(tp, sk + 2);
        let under = dd <= R + 0.5 ? z0 : tp - (1.6 + 2.2 * (1 - d));
        const gills = dd > R + 1;
        if (gills) {
          const a = Math.atan2(y + 0.5 - cy, x + 0.5 - cx - ox);
          if (Math.floor((a / (2 * Math.PI) + 1) * nGill) % 2) under += 1;
        }
        for (let z = Math.floor(under); z <= Math.ceil(tp); z++) {
          let c = HAT;
          if (gills && z <= Math.floor(under) + 0) c = TRIM;
          // Warts: round spots of the trim on the top, on a jittered grid,
          // each raised a cell at its middle.
          if (z >= Math.ceil(tp) - 1 && D > 0.05) {
            const sp = 6;
            const gx = Math.floor(x / sp), gy = Math.floor(y / sp);
            for (let jy = -1; jy <= 1; jy++)
              for (let jx = -1; jx <= 1; jx++) {
                const h = rnd(seed, gx + jx, gy + jy, 3);
                if (h > 0.12 + D * 0.4) continue;
                const px = (gx + jx + 0.2 + 0.6 * rnd(seed, gx + jx, gy + jy, 4)) * sp;
                const py = (gy + jy + 0.2 + 0.6 * rnd(seed, gx + jx, gy + jy, 5)) * sp;
                const rr = 1.2 + 1.0 * rnd(seed, gx + jx, gy + jy, 6);
                const dd2 = Math.hypot(x + 0.5 - px, y + 0.5 - py);
                if (dd2 <= rr) { c = TRIM; if (dd2 < 0.8 && z === Math.ceil(tp)) own(x, y, z + 1, TRIM); }
              }
          }
          own(x, y, z, c);
        }
      }
  }

  // =========================================================================
  else if (s.hat === 'morel') {
    // A morel's cone of pits and ridges: the ridges stand a cell proud of
    // the pits, in a brick-laid honeycomb.
    fitShell(browZ + 1, skullTop, 1.6, HAT);
    const Hc = R * (1.0 + H * 1.8);
    const pitch = D > 0.5 ? 3 : 4;
    const z0 = browZ + 1;
    for (let z = z0; z <= skullTop + Hc; z++) {
      const h = (z - z0) / (skullTop + Hc - z0);
      const r = (R + 1.2) * Math.pow(Math.max(0, 1 - h * h), 0.55);
      if (r < 0.6) { own(cx, cy, z, TRIM); continue; }
      const lean = F * R * 0.6 * h * h;
      const ccx = cx + s.hatTilt * lean, ccy = cy + lean;
      const row = Math.floor((z - z0) / pitch);
      const ridgeRow = (z - z0) % pitch === 0;
      const nSect = Math.max(5, Math.round(2 * Math.PI * r / (pitch + 1)));
      const Rr = Math.ceil(r + 1);
      for (let y = Math.floor(ccy) - Rr; y <= Math.floor(ccy) + Rr; y++)
        for (let x = Math.floor(ccx) - Rr; x <= Math.floor(ccx) + Rr; x++) {
          const d = Math.hypot(x + 0.5 - ccx, y + 0.5 - ccy);
          if (d > r + 0.3) continue;
          const outer = d > r - 1.0;
          if (!outer) { own(x, y, z, HAT); continue; }
          const a = (Math.atan2(y + 0.5 - ccy, x + 0.5 - ccx) / (2 * Math.PI) + 1) * nSect +
                    (row % 2) * 0.5;
          const ridge = ridgeRow || (a - Math.floor(a)) < 0.24;
          if (ridge) own(x, y, z, TRIM);            // a pit is the cell left out
        }
    }
  }

  // =========================================================================
  else if (s.hat === 'pot') {
    // An upturned pot: a bellied wall from the brow, rounded over the top,
    // a rolled lip, a foot ring on top, and handles.
    const zr = browZ;
    const zt = skullTop + 1 + Math.round(H * 5);
    const Rp = R + 1.6 + Z * 1.2;
    const rAt = z => {
      const u = (z - zr) / Math.max(1, zt - zr);
      let r = Rp + 0.9 * Math.sin(Math.PI * Math.min(1, u * 1.1));
      if (u > 0.78) r *= Math.sqrt(1 - Math.pow((u - 0.78) / 0.22, 2) * 0.45);
      return r;
    };
    for (let z = zr; z <= zt; z++) {
      const r = rAt(z), Rr = Math.ceil(r + 1);
      for (let y = Math.floor(cy) - Rr; y <= Math.floor(cy) + Rr; y++)
        for (let x = Math.floor(cx) - Rr; x <= Math.floor(cx) + Rr; x++)
          if (Math.hypot(x + 0.5 - cx, y + 0.5 - cy) <= r) own(x, y, z, RING);
    }
    // The lip: a rolled ring one cell proud at the bottom.
    for (const z of [zr, zr + 1]) {
      const r = rAt(zr) + 1.1, Rr = Math.ceil(r + 1);
      for (let y = Math.floor(cy) - Rr; y <= Math.floor(cy) + Rr; y++)
        for (let x = Math.floor(cx) - Rr; x <= Math.floor(cx) + Rr; x++) {
          const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
          if (d <= r && d > r - 1.6) own(x, y, z, RING);
        }
    }
    // The foot ring on the bottom (now the top).
    {
      const r = rAt(zt) * 0.7, Rr = Math.ceil(r + 1);
      for (let y = Math.floor(cy) - Rr; y <= Math.floor(cy) + Rr; y++)
        for (let x = Math.floor(cx) - Rr; x <= Math.floor(cx) + Rr; x++) {
          const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
          if (d <= r && d > r - 1.2) own(x, y, zt + 1, RING);
        }
    }
    const zh = Math.round(zr + (zt - zr) * 0.55);
    if (D < 0.5) {
      // Two loop handles, out of each side.
      for (const side of [-1, 1]) {
        const xw = cx + side * (rAt(zh) - 0.5);
        for (let a = 0; a <= Math.PI; a += 0.15)
          for (const dy of [0, 1])
            own(xw + side * Math.sin(a) * 2.6, cy - 0.5 + dy, zh + Math.cos(a) * 2.2, RING);
      }
    } else {
      // A saucepan's long handle, out of one side, rising a little.
      const side = s.hatTilt < 0 ? -1 : 1;
      const len = R * (0.9 + D * 0.8);
      for (let t = 0; t <= len; t += 0.4)
        for (let dy = -1; dy <= 1; dy++)
          for (const dz of [0, 1])
            own(cx + side * (rAt(zh) - 0.5 + t), cy - 0.5 + dy, zh + t * 0.25 + dz, RING);
      // ...with a hanging hole at its end.
      const ex = cx + side * (rAt(zh) + len - 1), ez = zh + len * 0.25;
      for (const dy of [0, 1]) del(Math.floor(ex), Math.floor(cy - 0.5 + dy), Math.floor(ez));
    }
  }

  // =========================================================================
  else if (s.hat === 'bonecrown') {
    // A leather band at the brow with a pale stitched middle row, ringed
    // with bone spikes: the longest at the front, alternating long and
    // short, curling outward with the flop.
    const zb0 = browZ + 1, zb1 = browZ + 3;
    fitShell(zb0, zb1, 1.6, (X, Y, Z2) => (Z2 === browZ + 2 && (X + Y) % 3 === 0 ? TRIM : HAT));
    const n = 5 + Math.round(D * 6);
    for (let i = 0; i < n; i++) {
      const u = n > 1 ? i / (n - 1) - 0.5 : 0;            // -0.5 .. 0.5
      // Round the front and the sides (front is LOW y: angle -pi/2).
      const a = -Math.PI / 2 + u * 2 * (Math.PI * 0.62);
      const ax = Math.cos(a), ay = Math.sin(a);
      // The band's outer face at this angle.
      let rr = 0;
      while (rr < R * 2 && (hat.has(K(Math.floor(cx + ax * (rr + 1)), Math.floor(cy + ay * (rr + 1)), zb1)) ||
                            body.has(K(Math.floor(cx + ax * (rr + 1)), Math.floor(cy + ay * (rr + 1)), zb1)))) rr++;
      if (rr === 0) rr = R;
      const L = (2.5 + H * 7) * (1 - 0.55 * Math.abs(u) * 2 * 0.8) * (i % 2 ? 0.7 : 1);
      const base = [cx + ax * rr, cy + ay * rr, zb1 + 0.5];
      sweep(base, L, t => [ax * (0.2 + F * 1.1 * t * t), ay * (0.2 + F * 1.1 * t * t), 1],
            t => Math.max(0.55, 1.45 * (1 - t) + 0.4 * t), BONE);
    }
  }

  // =========================================================================
  else if (s.hat === 'skull') {
    // A beast's skull worn as a helm.
    fitShell(browZ, skullTop + 1, 2.2, (X, Y, Z2) => {
      // A suture: a thin crooked crack over the top, front to back.
      const sut = Math.abs(X + 0.5 - cx - Math.sin(Y * 0.9) * 0.8) < 0.55 && Z2 >= skullTop - 1;
      return sut ? DARK : BONE;
    });
    // The sockets: on the dome's face above the goblin's eyes, empty.
    const frontY = new Map();
    for (const [k, c] of hat) {
      const kk = c[0] * 4096 + c[2];
      if (!frontY.has(kk) || c[1] < frontY.get(kk)) frontY.set(kk, c[1]);
      void k;
    }
    // Solid dark ovals on the dome's face, two cells deep, so they read
    // as holes from any angle.
    const sz = browZ + 3;
    for (const side of [-1, 1]) {
      const sx = cx + side * (Rx * 0.62 + 1.6);
      for (let z = sz - 2; z <= sz + 2; z++)
        for (let x = Math.floor(sx - 3); x <= Math.ceil(sx + 3); x++) {
          if (Math.hypot((x + 0.5 - sx) / 2.2, (z + 0.5 - sz) / 1.7) > 1) continue;
          const y = frontY.get(x * 4096 + z);
          if (y === undefined) continue;
          for (const dy of [0, 1]) {
            const c = out.get(K(x, y + dy, z));
            if (c && hat.has(K(x, y + dy, z))) c[3] = DARK;
          }
        }
    }
    // The upper jaw, jutting forward over the brow, teeth down its front.
    let fy = 1e9;
    for (const c of hat.values()) if (Math.abs(c[0] + 0.5 - cx) < 2 && c[2] === browZ + 1) fy = Math.min(fy, c[1]);
    if (fy < 1e9) {
      const Ls = R * (0.5 + Z * 0.7);
      for (let t = 0; t <= 1.001; t += 0.5 / Ls) {
        const yy = fy - t * Ls;
        const w = Rx * (0.62 - 0.32 * t);
        const zl = browZ, hgt = 4.8 - 2.6 * t;
        // A rounded muzzle: a half-ellipse across, a ridge down its middle.
        for (let z = zl; z <= zl + Math.ceil(hgt); z++)
          for (let x = Math.floor(cx - w); x < Math.ceil(cx + w); x++) {
            const ex = (x + 0.5 - cx) / w, ez = (z - zl) / hgt;
            if (ex * ex + ez * ez <= 1.05) own(x, yy, z, BONE);
          }
        // Teeth: every other column of the lower edge, a fang at the corners.
        for (let x = Math.floor(cx - w); x < Math.ceil(cx + w); x++)
          if (t > 0.25 && (x & 1)) own(x, yy, zl - 1, BONE);
      }
      for (const side of [-1, 1]) {
        const x = Math.floor(cx + side * Rx * 0.5);
        own(x, fy - Ls * 0.4, browZ - 1, BONE); own(x, fy - Ls * 0.4, browZ - 2, BONE);
      }
      // Nostrils at the end of the snout.
      for (const dx of [-1, 0]) {
        const k = K(Math.floor(cx) + dx, Math.floor(fy - Ls), browZ + 2);
        if (out.has(k) && hat.has(k)) out.get(k)[3] = DARK;
      }
    }
    // Horns from the temples, curling out and back.
    if (H > 0.08) {
      const len = 3 + H * 11;
      for (const side of [-1, 1])
        sweep([cx + side * (Rx + 0.5), cy, browZ + 4], len,
          t => [side * (1 - t * 0.6), 0.3 + F * 1.4 * t, 0.9 - F * 1.6 * t * t],
          t => Math.max(0.6, 1.6 * (1 - t) + 0.6 * t), HAT);
    }
  }

  // =========================================================================
  else if (s.hat === 'tophat') {
    // A stolen top hat, battered: crown cocked and crumpled, the brim curled
    // up at the sides, a band, a feather.
    const brimZ = browZ + 2;
    // Under the crown, a fitted shell over the whole skull, so no corner of
    // it can show through a crown that is straighter than the head.
    fitShell(browZ + 1, skullTop + 1, 1.5,
             (X, Y, Z2) => (Z2 >= brimZ + 1 && Z2 <= brimZ + 2 ? TRIM : HAT));
    const Hc = R * (1.3 + H * 1.6);
    const zTop = brimZ + Hc;
    const rx = RxMax + 1, ry = RyMax + 1;
    let crownTopX = cx;
    for (let z = brimZ; z <= zTop; z++) {
      const u = (z - brimZ) / Hc;
      const ox = s.hatTilt * (z - brimZ) * 0.3 + F * 1.6 * Math.sin(u * Math.PI * 1.4);
      const fl = 1 + 0.07 * u;
      const ccx = cx + ox;
      crownTopX = ccx;
      for (let y = Math.floor(cy - ry * fl) - 1; y <= Math.ceil(cy + ry * fl) + 1; y++)
        for (let x = Math.floor(ccx - rx * fl) - 1; x <= Math.ceil(ccx + rx * fl) + 1; x++) {
          // A rounded rectangle (a superellipse), as a top hat is.
          const e = Math.cbrt(Math.abs((x + 0.5 - ccx) / (rx * fl)) ** 3 +
                              Math.abs((y + 0.5 - cy) / (ry * fl)) ** 3);
          if (e > 1) continue;
          // The top is crushed: lower on one side, by the flop.
          const crush = F * 3 * ((x + 0.5 - ccx) / (rx * fl) + 1) / 2;
          if (z > zTop - crush) continue;
          own(x, y, z, z <= brimZ + 2 ? TRIM : HAT);
        }
    }
    void crownTopX;
    // The brim, curled up at the sides.
    const rb = 2 + Z * 3;
    const Rr = Math.ceil(Math.max(rx, ry) + rb + 1);
    for (let y = Math.floor(cy) - Rr; y <= Math.floor(cy) + Rr; y++)
      for (let x = Math.floor(cx) - Rr; x <= Math.floor(cx) + Rr; x++) {
        const e = Math.hypot((x + 0.5 - cx) / (rx + rb), (y + 0.5 - cy) / (ry + rb * 0.8));
        if (e > 1) continue;
        const side = Math.abs(x + 0.5 - cx) / (rx + rb);
        own(x, y, brimZ + Math.round(Math.pow(side, 3) * 2.5), HAT);
      }
    // A feather in the band, sweeping up and back.
    if (D > 0.3) {
      const side = s.hatTilt < 0 ? -1 : 1;
      sweep([cx + side * (rx + 0.3), cy + ry * 0.3, brimZ + 2], 4 + D * 9,
        t => [side * 0.2, 0.7 + t, 1 - 0.6 * t], t => (t < 0.7 ? 0.9 : 0.55), TRIM);
    }
  }

  // =========================================================================
  else if (s.hat === 'witch') {
    // A wide brim drooping at the edge, a tall wrinkled cone bent over near
    // the top, a band and a buckle.
    const brimZ = browZ + 2;
    fitShell(browZ + 1, skullTop, 1.5,
             (X, Y, Z2) => (Z2 >= brimZ + 1 && Z2 <= brimZ + 2 ? TRIM : HAT));
    const Rb = R * (1.7 + Z * 1.1);
    const Rr = Math.ceil(Rb + 1);
    for (let y = Math.floor(cy) - Rr; y <= Math.floor(cy) + Rr; y++)
      for (let x = Math.floor(cx) - Rr; x <= Math.floor(cx) + Rr; x++) {
        const dd = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
        const a = Math.atan2(y + 0.5 - cy, x + 0.5 - cx);
        const edge = Rb * (1 + 0.06 * Math.sin(a * 5 + 1));
        if (dd > edge || dd < R - 1) continue;
        const u = clamp((dd - R) / (edge - R), 0, 1);
        own(x, y, brimZ - Math.round(F * 3 * u * u), HAT);
      }
    const Hc = R * (1.7 + H * 2.4);
    const r0 = R + 1;
    zFloor = brimZ + 1;
    const end = sweep([cx, cy, brimZ + 1], Hc,
      t => bendDir(t < 0.45 ? 0 : Math.min(Math.PI * 0.85, F * 2.4 * Math.pow((t - 0.45) / 0.55, 1.3))),
      t => Math.max(0.6, r0 * Math.pow(1 - t, 1.15) + 0.6 * t) *
           (Math.sin(t * Hc * 0.9) > 0.8 ? 0.88 : 1),
      (X, Y, Z2) => (Z2 <= brimZ + 3 ? TRIM : HAT));
    void end;
    zFloor = -1e9;
    if (D > 0.4) {
      // A buckle on the band, at the front.
      let fy = 1e9;
      for (const c of hat.values())
        if (Math.abs(c[0] + 0.5 - cx) < 1 && c[2] === brimZ + 2) fy = Math.min(fy, c[1]);
      if (fy < 1e9)
        for (let z = brimZ + 1; z <= brimZ + 4; z++)
          for (let x = Math.floor(cx) - 2; x <= Math.floor(cx) + 1; x++)
            if (z === brimZ + 1 || z === brimZ + 4 || x === Math.floor(cx) - 2 || x === Math.floor(cx) + 1)
              own(x, fy - 1, z, RING, true);
    }
  }

  // ---- wear ------------------------------------------------------------------
  if (s.hatWear > 0.02 && hat.size) {
    const w = s.hatWear;
    const cells = [...hat.values()];
    const isOut = c => [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1]]
      .some(([dx, dy, dz]) => !hat.has(K(c[0] + dx, c[1] + dy, c[2] + dz)) &&
                              !body.has(K(c[0] + dx, c[1] + dy, c[2] + dz)));
    if (CLOTH.has(s.hat)) {
      // Patches sewn on in the trim colour, and a few holes.
      for (const c of cells) {
        if (c[3] !== HAT) continue;
        if (vnoise(seed ^ 0x55, c[0], c[1], c[2], 2.6) > 1 - w * 0.22) c[3] = TRIM;
        else if (isOut(c) && rnd(seed, c[0], c[1], c[2], 9) < w * 0.035) del(c[0], c[1], c[2]);
      }
    } else {
      // Dents, chips and nibbles: surface cells knocked out.
      for (const c of cells)
        if (isOut(c) && vnoise(seed ^ 0x77, c[0], c[1], c[2], 1.8) > 1 - w * 0.12)
          del(c[0], c[1], c[2]);
    }
  }

  // ---- hair under the hat goes ------------------------------------------------
  if (mass && s.hat !== 'hood') {
    const reach = R + 5;
    for (const [k, v] of [...mass])
      if (v[2] >= browZ - 1 && Math.hypot(v[0] + 0.5 - cx, v[1] + 0.5 - cy) <= reach)
        mass.delete(k);
  }
}
