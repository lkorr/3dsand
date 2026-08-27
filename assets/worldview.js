/* worldview.js — the voxel-accurate terrain viewer and editor.
 *
 * WHAT REPLACED WHAT. The Worldgen tab used to draw a COLUMN MAP: one ground
 * height, slope, sediment depth and water depth per (x,z), from
 * `sandvox --heightmap`. Its "3D" view extruded exactly that grid into one
 * heightfield mesh, which is why it looked like a single continuous sheet and
 * could never show a cave, an overhang, a tree, the inside of a pond, or a
 * voxel. None of those are functions of (x,z).
 *
 * This draws the world's actual cells, streamed from `sandvox --voxserve` (see
 * src/tools/voxregion.h). The column map is still here, as the far layer and as
 * the 2D map — it is the right tool for "where are the mountains" and this is
 * the right tool for "what is that".
 *
 * ---------------------------------------------------------------------------
 * THE ONE IDEA THAT MAKES THIS FAST: GEOMETRY AND MATERIAL ARE SEPARATE.
 *
 * The obvious voxel mesher emits a coloured quad per exposed face and merges
 * neighbours that share a colour. That fails badly here, and the reason is
 * worldgen's palette jitter: every stone cell carries a per-cell variant in its
 * state nibble, so no two adjacent cells share a colour and NOTHING merges. A
 * 64^3 region of plain rock becomes ~25k unmergeable quads.
 *
 * So the mesh carries no colour at all. Each region uploads its cells as an
 * R16UI 3D TEXTURE (material | state << 12), and the greedy mesher merges on
 * one bit — "is this face exposed" — which merges maximally: a flat plain is a
 * handful of quads whatever it is made of. The fragment shader reads the cell
 * just inside the face out of the 3D texture and looks the colour up in a
 * palette texture. Material identity stays EXACT, per voxel, including the
 * jitter variant; the geometry is as cheap as a heightfield's.
 *
 * The same texture pays for per-fragment ambient occlusion (sampled from the
 * real neighbourhood rather than baked per vertex, so it survives the merging)
 * and for the voxel-edge lines that appear when you zoom in far enough to be
 * looking at cells rather than at terrain.
 *
 * ---------------------------------------------------------------------------
 * LOD. Four levels, each a shell of 64^3-sample regions at lod 1, 2, 4 and 8 —
 * so a level-k region covers (64 << k) voxels per axis and every level costs
 * the same to fetch, mesh and store. A level-k region is skipped when it lies
 * entirely inside level k-1's coverage, which is exact because every extent is
 * a power of two multiple of the last. Past level 3 the heightfield from
 * /api/heightmap draws the horizon, which is the one thing a column map is
 * genuinely better at.
 *
 * SEAMS. A mesher that cannot see across a region boundary must guess, and both
 * guesses are wrong: "outside is air" emits a face the neighbour also emits
 * (z-fighting), "outside is solid" leaves a hole. So the mesher is handed the
 * six neighbour FACE SLABS when they exist, and a region is re-meshed when a
 * neighbour arrives. Missing neighbours read as solid, so the transient artifact
 * is a hole that fills itself rather than a flicker that does not.
 *
 * ---------------------------------------------------------------------------
 * EDITING. Brush strokes and selection operations write into an EDIT LAYER —
 * a sparse chunk-keyed map of world cell -> voxel word — never into the
 * generated data. The layer is the authored artifact: it saves to
 * assets/worldedits/<name>.svedit and the engine applies it after worldgen and
 * on every chunk stream-in, so an edit survives flying away and coming back and
 * composes with any seed. See src/sim/worldedit.h.
 *
 * Edits are only accepted inside level 0, where cells are 1:1 with world
 * voxels. A "brush" at lod 4 would have to invent 63 of every 64 voxels it
 * touched.
 */
(function (global) {
  'use strict';

  // ---- constants ----------------------------------------------------------
  var REGION_N = 64;          // samples per axis, every level
  var LEVELS = 4;             // lod 1, 2, 4, 8
  var CHUNK = 16;             // engine chunk, in voxels — the grid overlay uses it
  var MAT_MASK = 0xFFF;

  // Material classes, mirroring MatClass in src/sim/materials.h. The mesher
  // needs them to decide which faces exist and the shader needs them to decide
  // what blends; both read the same table out of /api/voxpalette, so there is
  // no second opinion about what a liquid is.
  var CLASS_SOLID = 0, CLASS_POWDER = 1, CLASS_LIQUID = 2, CLASS_GAS = 3;

  // ---- small matrix/vector helpers ---------------------------------------
  function m4() { return new Float32Array(16); }
  function m4ident() { var m = m4(); m[0] = m[5] = m[10] = m[15] = 1; return m; }
  function m4mul(a, b) {
    var o = m4();
    for (var c = 0; c < 4; c++) for (var r = 0; r < 4; r++) {
      var s = 0;
      for (var k = 0; k < 4; k++) s += a[r + k * 4] * b[k + c * 4];
      o[r + c * 4] = s;
    }
    return o;
  }
  function m4persp(fovy, asp, zn, zf) {
    var f = 1 / Math.tan(fovy / 2), d = 1 / (zn - zf), m = m4();
    m[0] = f / asp; m[5] = f; m[10] = (zf + zn) * d; m[11] = -1; m[14] = 2 * zf * zn * d;
    return m;
  }
  function m4look(ex, ey, ez, cx, cy, cz, ux, uy, uz) {
    var zx = ex - cx, zy = ey - cy, zz = ez - cz;
    var zl = Math.hypot(zx, zy, zz) || 1; zx /= zl; zy /= zl; zz /= zl;
    var xx = uy * zz - uz * zy, xy = uz * zx - ux * zz, xz = ux * zy - uy * zx;
    var xl = Math.hypot(xx, xy, xz) || 1; xx /= xl; xy /= xl; xz /= xl;
    var yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
    var m = m4();
    m[0] = xx; m[1] = yx; m[2] = zx; m[3] = 0;
    m[4] = xy; m[5] = yy; m[6] = zy; m[7] = 0;
    m[8] = xz; m[9] = yz; m[10] = zz; m[11] = 0;
    m[12] = -(xx * ex + xy * ey + xz * ez);
    m[13] = -(yx * ex + yy * ey + yz * ez);
    m[14] = -(zx * ex + zy * ey + zz * ez);
    m[15] = 1;
    return m;
  }

  // ===========================================================================
  // THE WORKER — decode + greedy mesh, off the main thread.
  // ===========================================================================
  //
  // Inline rather than a second file so the tuner stays servable as one page,
  // and because the format table this decodes is the twin of the one in
  // src/tools/voxregion.h — the two want to be read together, not hunted for.
  var WORKER_SRC = [
    '"use strict";',
    'var CLS = null;',   // Uint8Array: material id -> class
    'var PASSABLE = null;',
    '',
    // Decode the 'SVVX' RLE into a Uint16Array of (material | state<<12).
    // 16 bits and not the engine's full 32: the viewer draws material, variant
    // and liquid fullness, and worldgen emits no stain at all. Halving the
    // resident bytes is what lets every streamed region keep its cells on the
    // main thread, which is what the neighbour-slab mesher needs.
    'function decode(buf){',
    '  var d = new DataView(buf);',
    '  if (d.getUint32(0, true) !== 0x58565653) throw new Error("not an SVVX region");',
    '  var h = {ver: d.getUint32(4,true), ox: d.getInt32(8,true), oy: d.getInt32(12,true),',
    '           oz: d.getInt32(16,true), nx: d.getUint32(20,true), ny: d.getUint32(24,true),',
    '           nz: d.getUint32(28,true), lod: d.getUint32(32,true), seed: d.getUint32(36,true),',
    '           vpm: d.getInt32(40,true), runs: d.getUint32(48,true),',
    '           solid: d.getUint32(52,true), yMin: d.getInt32(56,true), yMax: d.getInt32(60,true)};',
    '  var n = h.nx * h.ny * h.nz;',
    '  var cells = new Uint16Array(n);',
    '  var p = 64, o = 0;',
    '  for (var i = 0; i < h.runs; i++) {',
    '    var w = d.getUint32(p, true), c = d.getUint32(p + 4, true); p += 8;',
    '    var v = w & 0xFFFF;',            // material (12) | state (4)
    '    if (v) { var e = o + c; if (e > n) e = n; for (var k = o; k < e; k++) cells[k] = v; }',
    '    o += c;',
    '  }',
    '  h.cells = cells;',
    '  return h;',
    '}',
    '',
    // Occupancy classes the mesher switches on. UNKNOWN is what a missing
    // neighbour slab reads as, and it is deliberately treated as OPAQUE: a hole
    // at a seam fills itself when the neighbour lands, a double-emitted face
    // z-fights for as long as both regions are loaded.
    'var OC_EMPTY = 0, OC_OPAQUE = 1, OC_FLUID = 2;',
    'function occOf(v){',
    '  var m = v & 0xFFF;',
    '  if (!m) return OC_EMPTY;',
    '  var c = CLS[m];',
    '  return (c === 2 || c === 3) ? OC_FLUID : OC_OPAQUE;',
    '}',
    '',
    // Greedy mesh, one pass per draw layer.
    //   layer 0 (opaque):  a solid cell facing empty or fluid
    //   layer 1 (fluid):   a liquid/gas cell facing empty
    // Merging tests only "is this face exposed, and which way does it point",
    // because colour is a texture lookup in the fragment shader. That is what
    // makes a flat plain a few quads instead of one per cell.
    'function mesh(cells, nx, ny, nz, nbr, layer){',
    '  var dims = [nx, ny, nz];',
    '  var verts = [], idx = [], vcount = 0;',
    '  function at(x, y, z){',
    // Out of the region: read the neighbour slab if we have it, else OPAQUE.
    '    if (x < 0) return nbr.nx0 ? nbr.nx0[z * ny + y] : -1;',
    '    if (x >= nx) return nbr.px0 ? nbr.px0[z * ny + y] : -1;',
    '    if (y < 0) return nbr.ny0 ? nbr.ny0[z * nx + x] : -1;',
    '    if (y >= ny) return nbr.py0 ? nbr.py0[z * nx + x] : -1;',
    '    if (z < 0) return nbr.nz0 ? nbr.nz0[y * nx + x] : -1;',
    '    if (z >= nz) return nbr.pz0 ? nbr.pz0[y * nx + x] : -1;',
    '    return cells[(z * ny + y) * nx + x];',
    '  }',
    '  function occ(x, y, z){ var v = at(x, y, z); return v < 0 ? OC_OPAQUE : occOf(v); }',
    '  for (var d = 0; d < 3; d++) {',
    '    var u = (d + 1) % 3, v2 = (d + 2) % 3;',
    '    var x = [0, 0, 0], q = [0, 0, 0]; q[d] = 1;',
    '    var mw = dims[u], mh = dims[v2];',
    '    var mask = new Int8Array(mw * mh);',
    '    for (x[d] = -1; x[d] < dims[d];) {',
    '      var n = 0;',
    '      for (x[v2] = 0; x[v2] < mh; x[v2]++)',
    '        for (x[u] = 0; x[u] < mw; x[u]++) {',
    '          var a = occ(x[0], x[1], x[2]);',
    '          var b = occ(x[0] + q[0], x[1] + q[1], x[2] + q[2]);',
    '          var f = 0;',
    '          if (layer === 0) {',
    '            if (a === OC_OPAQUE && b !== OC_OPAQUE) f = 1;',
    '            else if (b === OC_OPAQUE && a !== OC_OPAQUE) f = -1;',
    '          } else {',
    '            if (a === OC_FLUID && b === OC_EMPTY) f = 1;',
    '            else if (b === OC_FLUID && a === OC_EMPTY) f = -1;',
    '          }',
    '          mask[n++] = f;',
    '        }',
    '      x[d]++;',
    '      n = 0;',
    '      for (var j = 0; j < mh; j++) {',
    '        for (var i = 0; i < mw;) {',
    '          var c = mask[n];',
    '          if (!c) { i++; n++; continue; }',
    '          var w = 1;',
    '          while (i + w < mw && mask[n + w] === c) w++;',
    '          var hgt = 1, done = false;',
    '          while (j + hgt < mh) {',
    '            for (var kk = 0; kk < w; kk++)',
    '              if (mask[n + hgt * mw + kk] !== c) { done = true; break; }',
    '            if (done) break;',
    '            hgt++;',
    '          }',
    '          var p0 = [0, 0, 0]; p0[d] = x[d]; p0[u] = i; p0[v2] = j;',
    '          var du = [0, 0, 0]; du[u] = w;',
    '          var dv = [0, 0, 0]; dv[v2] = hgt;',
    // Face index 0..5 = -x,+x,-y,+y,-z,+z, which the shader turns into the
    // normal and the two tangents. Winding flips with the sign so both sides
    // stay front-facing under back-face culling.
    '          var face = d * 2 + (c > 0 ? 1 : 0);',
    '          var v0 = vcount;',
    '          verts.push(p0[0], p0[1], p0[2], face);',
    '          verts.push(p0[0] + du[0], p0[1] + du[1], p0[2] + du[2], face);',
    '          verts.push(p0[0] + du[0] + dv[0], p0[1] + du[1] + dv[1], p0[2] + du[2] + dv[2], face);',
    '          verts.push(p0[0] + dv[0], p0[1] + dv[1], p0[2] + dv[2], face);',
    '          vcount += 4;',
    '          if (c > 0) idx.push(v0, v0 + 1, v0 + 2, v0, v0 + 2, v0 + 3);',
    '          else idx.push(v0, v0 + 2, v0 + 1, v0, v0 + 3, v0 + 2);',
    '          for (var l = 0; l < hgt; l++)',
    '            for (var kx = 0; kx < w; kx++) mask[n + l * mw + kx] = 0;',
    '          i += w; n += w;',
    '        }',
    '      }',
    '    }',
    '  }',
    '  return {verts: new Uint8Array(verts), idx: new Uint32Array(idx), quads: vcount / 4};',
    '}',
    '',
    'self.onmessage = function(e){',
    '  var m = e.data;',
    '  if (m.cmd === "classes") { CLS = m.classes; PASSABLE = m.passable; return; }',
    '  if (m.cmd === "decode") {',
    '    try {',
    '      var h = decode(m.buf);',
    '      self.postMessage({cmd: "decoded", key: m.key, head: h, cells: h.cells},',
    '                       [h.cells.buffer]);',
    '    } catch (err) { self.postMessage({cmd: "error", key: m.key, error: String(err)}); }',
    '    return;',
    '  }',
    '  if (m.cmd === "mesh") {',
    '    try {',
    '      var op = mesh(m.cells, m.nx, m.ny, m.nz, m.nbr || {}, 0);',
    '      var fl = mesh(m.cells, m.nx, m.ny, m.nz, m.nbr || {}, 1);',
    // The cells are NOT sent back: the main thread kept the original and only
    // lent a copy, so returning it would just make garbage.
    '      self.postMessage({cmd: "meshed", key: m.key, gen: m.gen, rev: m.rev,',
    '                        op: op, fl: fl},',
    '        [op.verts.buffer, op.idx.buffer, fl.verts.buffer, fl.idx.buffer]);',
    '    } catch (err) { self.postMessage({cmd: "error", key: m.key, error: String(err)}); }',
    '    return;',
    '  }',
    '};'
  ].join('\n');

  // ===========================================================================
  // SHADERS
  // ===========================================================================
  var VS_VOX = [
    '#version 300 es',
    'precision highp float; precision highp int;',
    'uniform mat4 uMVP;',
    'uniform vec3 uRegionOrigin;',   // world voxels
    'uniform float uLod;',
    'in vec4 aPacked;',              // xyz = sample-space corner, w = face 0..5
    'out vec3 vSample;',             // sample-space position (region local)
    'flat out int vFace;',
    'out vec3 vWorld;',
    'void main(){',
    '  vSample = aPacked.xyz;',
    '  vFace = int(aPacked.w + 0.5);',
    '  vWorld = uRegionOrigin + aPacked.xyz * uLod;',
    '  gl_Position = uMVP * vec4(vWorld, 1.0);',
    '}'
  ].join('\n');

  var FS_VOX = [
    '#version 300 es',
    'precision highp float; precision highp int;',
    'precision highp usampler3D; precision highp usampler2D;',
    'uniform usampler3D uVox;',
    'uniform usampler2D uPal;',       // 4096 x 4 : rows 0..2 colours, row 3 meta
    'uniform ivec3 uDims;',
    'uniform vec3 uRegionOrigin;',
    'uniform float uLod;',
    'uniform vec3 uEye;',
    'uniform vec3 uLight;',
    'uniform float uAO;',
    'uniform float uGrid;',           // voxel edge lines, 0..1
    'uniform int uMode;',             // 0 material, 1 class, 2 height, 3 depth-x-ray
    'uniform int uIsolate;',          // material id to isolate, -1 = none
    'uniform float uAlpha;',
    'uniform float uSeaY;',
    'in vec3 vSample; flat in int vFace; in vec3 vWorld;',
    'out vec4 oCol;',
    '',
    'const ivec3 NRM[6] = ivec3[6](ivec3(-1,0,0), ivec3(1,0,0), ivec3(0,-1,0),',
    '                              ivec3(0,1,0), ivec3(0,0,-1), ivec3(0,0,1));',
    'const ivec3 TAN[6] = ivec3[6](ivec3(0,1,0), ivec3(0,1,0), ivec3(1,0,0),',
    '                              ivec3(1,0,0), ivec3(1,0,0), ivec3(1,0,0));',
    'const ivec3 BIT[6] = ivec3[6](ivec3(0,0,1), ivec3(0,0,1), ivec3(0,0,1),',
    '                              ivec3(0,0,1), ivec3(0,1,0), ivec3(0,1,0));',
    '',
    'uint cellAt(ivec3 c){',
    '  if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, uDims))) return 0u;',
    '  return texelFetch(uVox, c, 0).r;',
    '}',
    'bool solidAt(ivec3 c){',
    '  uint m = cellAt(c) & 4095u;',
    '  if (m == 0u) return false;',
    '  return texelFetch(uPal, ivec2(int(m), 3), 0).r < 2u;',   // class solid|powder
    '}',
    // Classic 0..3 voxel corner AO, but evaluated per FRAGMENT. Baking it into
    // vertices is what forces one quad per cell; sampling the real
    // neighbourhood here is what lets the mesher merge and still look like
    // voxels.
    'float cornerAO(bool s1, bool s2, bool cr){',
    '  if (s1 && s2) return 0.0;',
    '  return 3.0 - (float(s1) + float(s2) + float(cr));',
    '}',
    'void main(){',
    '  ivec3 n = NRM[vFace];',
    '  vec3 fn = vec3(n);',
    '  ivec3 cell = ivec3(floor(vSample - fn * 0.5));',
    '  cell = clamp(cell, ivec3(0), uDims - 1);',
    '  uint w = cellAt(cell);',
    '  uint mat = w & 4095u;',
    '  if (mat == 0u) discard;',
    '  if (uIsolate >= 0 && int(mat) != uIsolate) discard;',
    '  uint meta = texelFetch(uPal, ivec2(int(mat), 3), 0).r;',
    '  uint state = (w >> 12) & 15u;',
    '  vec3 base;',
    '  if (uMode == 1) {',
    // Class view: one colour per class, for reading structure rather than looks.
    '    if (meta == 0u) base = vec3(0.62, 0.62, 0.66);',
    '    else if (meta == 1u) base = vec3(0.85, 0.68, 0.32);',
    '    else if (meta == 2u) base = vec3(0.24, 0.52, 0.88);',
    '    else base = vec3(0.75, 0.35, 0.80);',
    '  } else if (uMode == 2) {',
    '    float t = clamp((vWorld.y - uSeaY + 360.0) / 720.0, 0.0, 1.0);',
    '    base = mix(mix(vec3(0.13,0.22,0.42), vec3(0.40,0.55,0.33), smoothstep(0.0,0.5,t)),',
    '               vec3(0.92,0.94,0.97), smoothstep(0.5,1.0,t));',
    '  } else {',
    // The palette variant is the engine's own, out of the state nibble: this is
    // the value genCell assigned, not a dither invented here. Liquids spend the
    // nibble on fullness instead, so they take variant 0.
    '    int variant = (meta >= 2u) ? 0 : int(state % 3u);',
    '    uvec4 c = texelFetch(uPal, ivec2(int(mat), variant), 0);',
    '    base = vec3(c.rgb) / 255.0;',
    '  }',
    '  float ao = 1.0;',
    '  if (uAO > 0.0) {',
    '    ivec3 t = TAN[vFace], b = BIT[vFace];',
    '    ivec3 air = cell + n;',
    '    vec3 rel = vSample - fn * 0.5 - vec3(cell);',
    '    float fu = clamp(dot(rel, vec3(t)), 0.0, 1.0);',
    '    float fv = clamp(dot(rel, vec3(b)), 0.0, 1.0);',
    '    bool tp = solidAt(air + t), tm = solidAt(air - t);',
    '    bool bp = solidAt(air + b), bm = solidAt(air - b);',
    '    float a00 = cornerAO(tm, bm, solidAt(air - t - b));',
    '    float a10 = cornerAO(tp, bm, solidAt(air + t - b));',
    '    float a01 = cornerAO(tm, bp, solidAt(air - t + b));',
    '    float a11 = cornerAO(tp, bp, solidAt(air + t + b));',
    '    float a = mix(mix(a00, a10, fu), mix(a01, a11, fu), fv) / 3.0;',
    '    ao = mix(1.0, 0.35 + 0.65 * a, uAO);',
    '  }',
    '  float lambert = 0.42 + 0.58 * max(dot(fn, uLight), 0.0);',
    '  vec3 col = base * lambert * ao;',
    // Voxel edge lines, faded in only once a cell is worth several pixels —
    // "see the individual voxels" is a zoom-dependent request and drawing the
    // lattice at 60 m would be a grey haze.
    '  if (uGrid > 0.0) {',
    '    vec3 t3 = vec3(TAN[vFace]), b3 = vec3(BIT[vFace]);',
    '    float pu = dot(vSample, t3), pv = dot(vSample, b3);',
    '    vec2 g = abs(fract(vec2(pu, pv) - 0.5) - 0.5) / fwidth(vec2(pu, pv));',
    '    float line = 1.0 - min(min(g.x, g.y), 1.0);',
    '    col = mix(col, col * 0.55, line * uGrid);',
    '  }',
    '  float alpha = uAlpha;',
    '  if (meta == 2u) {',
    // Liquid: the state nibble is FULLNESS 1..8, so a shallow film reads as
    // one and a full cell as the other. Same field the engine renders from.
    '    float full = clamp(float(state) / 8.0, 0.125, 1.0);',
    '    alpha = uAlpha * (0.35 + 0.55 * full);',
    '  } else if (meta == 3u) { alpha = uAlpha * 0.30; }',
    '  oCol = vec4(col, alpha);',
    '}'
  ].join('\n');

  var VS_LINE = [
    '#version 300 es',
    'uniform mat4 uMVP;',
    'in vec3 aPos; in vec4 aCol;',
    'out vec4 vCol;',
    'void main(){ vCol = aCol; gl_Position = uMVP * vec4(aPos, 1.0); }'
  ].join('\n');
  var FS_LINE = [
    '#version 300 es',
    'precision mediump float;',
    'in vec4 vCol; out vec4 oCol;',
    'void main(){ oCol = vCol; }'
  ].join('\n');

  // The far horizon: the column map, extruded. Kept because it is the one thing
  // a heightfield is strictly better at — it covers kilometres for one fetch,
  // and past the last voxel level there is nothing else to draw.
  var VS_FAR = [
    '#version 300 es',
    'uniform mat4 uMVP;',
    'in vec3 aPos; in vec3 aCol; in vec3 aNrm;',
    'out vec3 vCol; out vec3 vNrm; out vec3 vW;',
    'void main(){ vCol = aCol; vNrm = aNrm; vW = aPos; gl_Position = uMVP * vec4(aPos,1.0); }'
  ].join('\n');
  var FS_FAR = [
    '#version 300 es',
    'precision mediump float;',
    'uniform vec3 uLight; uniform vec3 uEye; uniform float uFade;',
    'in vec3 vCol; in vec3 vNrm; in vec3 vW; out vec4 oCol;',
    'void main(){',
    '  float d = max(dot(normalize(vNrm), uLight), 0.0);',
    '  float dist = length(vW - uEye);',
    // Faded IN with distance, not out: the voxel levels own the near field and
    // the two would otherwise z-fight through each other across the whole
    // overlap. Near the camera this layer is invisible by construction.
    '  float a = clamp((dist - uFade) / (uFade * 0.6), 0.0, 1.0);',
    '  if (a <= 0.002) discard;',
    '  oCol = vec4(vCol * (0.40 + 0.60 * d), a);',
    '}'
  ].join('\n');

  // ===========================================================================
  // WorldView
  // ===========================================================================
  function WorldView(canvas, opts) {
    this.canvas = canvas;
    this.opts = opts || {};
    this.gl = null;
    this.regions = new Map();     // key -> region
    this.pending = new Map();     // key -> true while fetching
    this.queue = [];
    this.inflight = 0;
    this.maxInflight = 4;
    this.pal = null;              // parsed /api/voxpalette
    this.palTex = null;
    this.classes = null;
    this.seed = 1337;
    this.gen = 0;                 // bumped when the world changes; stale replies drop
    // Bumped by every voxel written. The host page uses it to decide whether a
    // frame would show anything new — an edit changes the picture without
    // moving the camera, so a camera-only signature would freeze the view mid
    // brush stroke.
    this.editRev = 0;
    this.stats = {regions: 0, quads: 0, draws: 0, fetches: 0, bytes: 0, ms: 0};

    this.view = {
      mode: 'material',           // material | class | height
      ao: 1.0,
      grid: 1.0,
      levels: LEVELS,
      showChunks: false,
      showRegions: false,
      showAxes: true,
      showFar: true,
      showFluids: true,
      isolate: -1,
      sliceEnabled: false,
      sliceAxis: 1,
      slicePos: 0,
      sliceThick: 0,
      wire: false
    };

    this.cam = {
      // World VOXEL coordinates throughout. Metres are a display unit here; the
      // engine's own unit is the voxel and every readout, brush size and grid
      // in this tab is in voxels for that reason.
      pos: [0, 1040, -60], yaw: 0, pitch: -0.35,
      mode: 'fly',                // fly | orbit
      target: [0, 1024, 0], dist: 90,
      speed: 40                   // voxels / second
    };

    this.edit = new EditLayer();
    this.tool = {
      name: 'none',               // none | brush | erase | box | picker | measure
      material: 1,
      radius: 3,
      shape: 'sphere',            // sphere | cube
      mode: 'replace',            // replace | air-only
      sel: null,                  // {a:[x,y,z], b:[x,y,z]}
      measure: null
    };
    this.hover = null;            // {vox:[x,y,z], face:[dx,dy,dz], mat, state}
    this.far = null;              // heightfield layer

    this._initGL();
    this._initWorker();
    this._bindInput();
  }

  // ---- GL setup -----------------------------------------------------------
  WorldView.prototype._initGL = function () {
    var gl = this.canvas.getContext('webgl2', {antialias: true, alpha: false,
                                               preserveDrawingBuffer: false});
    if (!gl) throw new Error('WebGL2 is required for the voxel view');
    this.gl = gl;
    this.progVox = makeProgram(gl, VS_VOX, FS_VOX, ['aPacked']);
    this.progLine = makeProgram(gl, VS_LINE, FS_LINE, ['aPos', 'aCol']);
    this.progFar = makeProgram(gl, VS_FAR, FS_FAR, ['aPos', 'aCol', 'aNrm']);
    this.uVox = uniforms(gl, this.progVox,
      ['uMVP', 'uRegionOrigin', 'uLod', 'uVox', 'uPal', 'uDims', 'uEye', 'uLight',
       'uAO', 'uGrid', 'uMode', 'uIsolate', 'uAlpha', 'uSeaY']);
    this.uLine = uniforms(gl, this.progLine, ['uMVP']);
    this.uFar = uniforms(gl, this.progFar, ['uMVP', 'uLight', 'uEye', 'uFade']);
    this.lineVBO = gl.createBuffer();
    this.lineVAO = gl.createVertexArray();
    gl.bindVertexArray(this.lineVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineVBO);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 28, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 4, gl.FLOAT, false, 28, 12);
    gl.bindVertexArray(null);
    gl.enable(gl.DEPTH_TEST);
    gl.enable(gl.CULL_FACE);
    gl.cullFace(gl.BACK);
  };

  function makeProgram(gl, vsSrc, fsSrc, attribs) {
    function sh(type, src) {
      var s = gl.createShader(type);
      gl.shaderSource(s, src); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
        throw new Error('shader: ' + gl.getShaderInfoLog(s) + '\n' + src);
      return s;
    }
    var p = gl.createProgram();
    gl.attachShader(p, sh(gl.VERTEX_SHADER, vsSrc));
    gl.attachShader(p, sh(gl.FRAGMENT_SHADER, fsSrc));
    for (var i = 0; i < attribs.length; i++) gl.bindAttribLocation(p, i, attribs[i]);
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS))
      throw new Error('link: ' + gl.getProgramInfoLog(p));
    return p;
  }
  function uniforms(gl, prog, names) {
    var o = {};
    for (var i = 0; i < names.length; i++) o[names[i]] = gl.getUniformLocation(prog, names[i]);
    return o;
  }

  WorldView.prototype._initWorker = function () {
    var self = this;
    var blob = new Blob([WORKER_SRC], {type: 'text/javascript'});
    this.worker = new Worker(URL.createObjectURL(blob));
    this.worker.onmessage = function (e) { self._onWorker(e.data); };
  };

  // ---- palette ------------------------------------------------------------
  WorldView.prototype.loadPalette = function (pal) {
    var gl = this.gl;
    this.pal = pal;
    var N = 4096;
    var data = new Uint8Array(N * 4 * 4);   // 4 rows: 3 colours + meta
    var classes = new Uint8Array(N);
    var passable = new Uint8Array(N);
    function hex(s) {
      var v = parseInt(s.replace('#', ''), 16);
      return [(v >> 16) & 255, (v >> 8) & 255, v & 255];
    }
    for (var i = 0; i < pal.materials.length; i++) {
      var m = pal.materials[i];
      for (var c = 0; c < 3; c++) {
        var rgb = hex(m.colors[c] || m.colors[0] || '#000000');
        var o = (c * N + m.id) * 4;
        data[o] = rgb[0]; data[o + 1] = rgb[1]; data[o + 2] = rgb[2]; data[o + 3] = 255;
      }
      var mo = (3 * N + m.id) * 4;
      data[mo] = m.class;                     // the shader's `meta`
      data[mo + 1] = m.emission & 255;
      data[mo + 2] = m.opacity & 255;
      data[mo + 3] = m.flags & 255;
      classes[m.id] = m.class;
      passable[m.id] = (m.flags & 8) ? 1 : 0;
    }
    this.classes = classes;
    if (!this.palTex) this.palTex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.palTex);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8UI, N, 4, 0, gl.RGBA_INTEGER,
                  gl.UNSIGNED_BYTE, data);
    this.worker.postMessage({cmd: 'classes', classes: classes, passable: passable});
  };

  // ---- region keys and streaming -----------------------------------------
  function regionKey(level, rx, ry, rz) { return level + ':' + rx + ',' + ry + ',' + rz; }

  WorldView.prototype._levelExtent = function (level) { return REGION_N << level; };

  // The set of regions that should be resident, nearest level first.
  WorldView.prototype._desired = function () {
    var want = [];
    var eye = this.camEye();
    var covered = null;   // world-voxel box already owned by a finer level
    for (var L = 0; L < this.view.levels; L++) {
      var ext = this._levelExtent(L);
      var cx = Math.floor(eye[0] / ext), cy = Math.floor(eye[1] / ext),
          cz = Math.floor(eye[2] / ext);
      var rad = 1, radY = 1;
      var box = [(cx - rad) * ext, (cy - radY) * ext, (cz - rad) * ext,
                 (cx + rad + 1) * ext, (cy + radY + 1) * ext, (cz + rad + 1) * ext];
      for (var z = cz - rad; z <= cz + rad; z++)
        for (var y = cy - radY; y <= cy + radY; y++)
          for (var x = cx - rad; x <= cx + rad; x++) {
            // Skip anything a finer level already draws. Exact, not a heuristic:
            // every level's extent is a power-of-two multiple of the last, so a
            // coarse region is either wholly inside the fine box or wholly out.
            if (covered) {
              var ox = x * ext, oy = y * ext, oz = z * ext;
              if (ox >= covered[0] && oy >= covered[1] && oz >= covered[2] &&
                  ox + ext <= covered[3] && oy + ext <= covered[4] && oz + ext <= covered[5])
                continue;
            }
            var d = Math.max(Math.abs(x - cx), Math.abs(y - cy), Math.abs(z - cz));
            want.push({level: L, rx: x, ry: y, rz: z, pri: L * 10 + d});
          }
      covered = box;
    }
    want.sort(function (a, b) { return a.pri - b.pri; });
    return want;
  };

  WorldView.prototype.update = function () {
    var want = this._desired();
    var live = new Set();
    for (var i = 0; i < want.length; i++) {
      var w = want[i];
      var key = regionKey(w.level, w.rx, w.ry, w.rz);
      live.add(key);
      if (this.regions.has(key) || this.pending.has(key)) continue;
      this.pending.set(key, true);
      this.queue.push(w);
    }
    // Evict what fell out of every level's shell. Regions are 512 KB of cells
    // plus a 3D texture; without this a long flight grows without bound.
    var self = this;
    this.regions.forEach(function (r, key) {
      if (!live.has(key)) { self._freeRegion(r); self.regions.delete(key); }
    });
    this.queue = this.queue.filter(function (w) {
      return live.has(regionKey(w.level, w.rx, w.ry, w.rz));
    });
    this._pump();
  };

  WorldView.prototype._pump = function () {
    while (this.inflight < this.maxInflight && this.queue.length) {
      var w = this.queue.shift();
      this._fetch(w);
    }
  };

  WorldView.prototype._fetch = function (w) {
    var self = this;
    var key = regionKey(w.level, w.rx, w.ry, w.rz);
    var lod = 1 << w.level, ext = REGION_N * lod;
    var q = 'ox=' + (w.rx * ext) + '&oy=' + (w.ry * ext) + '&oz=' + (w.rz * ext) +
            '&nx=' + REGION_N + '&ny=' + REGION_N + '&nz=' + REGION_N +
            '&lod=' + lod + '&seed=' + this.seed;
    this.inflight++;
    var gen = this.gen;
    var t0 = performance.now();
    fetch('/api/voxregion?' + q).then(function (r) {
      if (!r.ok) return r.json().then(function (j) { throw new Error(j.error || ('HTTP ' + r.status)); });
      return r.arrayBuffer();
    }).then(function (buf) {
      self.stats.fetches++;
      self.stats.bytes += buf.byteLength;
      self.stats.ms = performance.now() - t0;
      if (gen !== self.gen) { self.pending.delete(key); self.inflight--; self._pump(); return; }
      self._pendingMeta = self._pendingMeta || new Map();
      self._pendingMeta.set(key, {level: w.level, rx: w.rx, ry: w.ry, rz: w.rz, gen: gen});
      self.worker.postMessage({cmd: 'decode', key: key, buf: buf}, [buf]);
    }).catch(function (e) {
      self.pending.delete(key);
      self.inflight--;
      self.onError && self.onError(String(e && e.message || e));
      self._pump();
    });
  };

  WorldView.prototype._onWorker = function (m) {
    var self = this;
    if (m.cmd === 'error') {
      this.pending.delete(m.key);
      this.inflight = Math.max(0, this.inflight - 1);
      this.onError && this.onError(m.error);
      this._pump();
      return;
    }
    if (m.cmd === 'decoded') {
      var meta = this._pendingMeta && this._pendingMeta.get(m.key);
      this._pendingMeta && this._pendingMeta.delete(m.key);
      this.inflight--;
      this.pending.delete(m.key);
      if (!meta || meta.gen !== this.gen) { this._pump(); return; }
      var r = {
        key: m.key, level: meta.level, rx: meta.rx, ry: meta.ry, rz: meta.rz,
        lod: m.head.lod, nx: m.head.nx, ny: m.head.ny, nz: m.head.nz,
        origin: [m.head.ox, m.head.oy, m.head.oz],
        cells: m.cells, tex: null, op: null, fl: null, meshing: false,
        dirty: true, gen: this.gen, rev: 0
      };
      // The edit layer is authored over the GENERATED world, so it has to be
      // composited into a region the moment that region arrives — otherwise a
      // fly-away-and-back would show the terrain the edit was meant to change.
      this.edit.applyToRegion(r);
      this.regions.set(m.key, r);
      this._uploadTex(r);
      this._markNeighborsDirty(r);
      this._pump();
      this._meshPass();
      return;
    }
    if (m.cmd === 'meshed') {
      var reg = this.regions.get(m.key);
      if (!reg) return;
      reg.meshing = false;
      // A mesh built from cells that have since been edited describes a world
      // that no longer exists. `rev` is bumped by every poke; if it moved while
      // the worker was running, throw the result away and mesh again.
      if (m.gen !== reg.gen || m.rev !== reg.rev) { reg.dirty = true; this._meshPass(); return; }
      this._uploadMesh(reg, 'op', m.op);
      this._uploadMesh(reg, 'fl', m.fl);
      this._meshPass();
      return;
    }
  };

  WorldView.prototype._markNeighborsDirty = function (r) {
    var d = [[1,0,0],[-1,0,0],[0,1,0],[0,-1,0],[0,0,1],[0,0,-1]];
    for (var i = 0; i < 6; i++) {
      var n = this.regions.get(regionKey(r.level, r.rx + d[i][0], r.ry + d[i][1], r.rz + d[i][2]));
      if (n) n.dirty = true;
    }
  };

  // At most two regions meshed at a time — meshing every dirty region at once
  // on arrival is what turns a camera move into a stall.
  //
  // THE WORKER GETS A COPY, NOT THE ARRAY. Transferring `cells` is free and was
  // the obvious thing to do, and it is wrong: while the worker holds them the
  // main thread has no data for that region, so cellAtWorld returns "not
  // resident" and every read through it silently fails — picking, the hover
  // readout, and worst, apply(), which drops the voxels it was asked to write.
  // A brush stroke over a region that happened to be re-meshing lost part of
  // itself with no error anywhere. A 512 KB copy per mesh is nothing beside the
  // meshing it feeds.
  WorldView.prototype._meshPass = function () {
    var busy = 0;
    this.regions.forEach(function (r) { if (r.meshing) busy++; });
    if (busy >= 2) return;
    var pick = null, bestPri = 1e9;
    var eye = this.camEye();
    this.regions.forEach(function (r) {
      if (!r.dirty || r.meshing || !r.cells) return;
      var ext = r.nx * r.lod;
      var cx = r.origin[0] + ext / 2, cy = r.origin[1] + ext / 2, cz = r.origin[2] + ext / 2;
      var pri = r.level * 1e6 + Math.hypot(cx - eye[0], cy - eye[1], cz - eye[2]);
      if (pri < bestPri) { bestPri = pri; pick = r; }
    });
    if (!pick) return;
    pick.dirty = false;
    pick.meshing = true;
    var nbr = this._neighborSlabs(pick);
    var copy = pick.cells.slice();
    this.worker.postMessage({
      cmd: 'mesh', key: pick.key, gen: pick.gen, rev: pick.rev || 0, cells: copy,
      nx: pick.nx, ny: pick.ny, nz: pick.nz, nbr: nbr
    }, [copy.buffer]);
  };

  // The six face slabs of the neighbouring regions, so the mesher can tell a
  // real surface from a region boundary. A missing neighbour is simply absent,
  // and the worker reads absent as solid.
  WorldView.prototype._neighborSlabs = function (r) {
    var out = {};
    var nx = r.nx, ny = r.ny, nz = r.nz;
    var g = this.regions;
    function slabX(n, xi) {
      if (!n || !n.cells) return null;
      var s = new Uint16Array(n.nz * n.ny);
      for (var z = 0; z < n.nz; z++)
        for (var y = 0; y < n.ny; y++) s[z * n.ny + y] = n.cells[(z * n.ny + y) * n.nx + xi];
      return s;
    }
    function slabY(n, yi) {
      if (!n || !n.cells) return null;
      var s = new Uint16Array(n.nz * n.nx);
      for (var z = 0; z < n.nz; z++)
        for (var x = 0; x < n.nx; x++) s[z * n.nx + x] = n.cells[(z * n.ny + yi) * n.nx + x];
      return s;
    }
    function slabZ(n, zi) {
      if (!n || !n.cells) return null;
      var s = new Uint16Array(n.ny * n.nx);
      for (var y = 0; y < n.ny; y++)
        for (var x = 0; x < n.nx; x++) s[y * n.nx + x] = n.cells[(zi * n.ny + y) * n.nx + x];
      return s;
    }
    var k = regionKey;
    out.nx0 = slabX(g.get(k(r.level, r.rx - 1, r.ry, r.rz)), nx - 1);
    out.px0 = slabX(g.get(k(r.level, r.rx + 1, r.ry, r.rz)), 0);
    out.ny0 = slabY(g.get(k(r.level, r.rx, r.ry - 1, r.rz)), ny - 1);
    out.py0 = slabY(g.get(k(r.level, r.rx, r.ry + 1, r.rz)), 0);
    out.nz0 = slabZ(g.get(k(r.level, r.rx, r.ry, r.rz - 1)), nz - 1);
    out.pz0 = slabZ(g.get(k(r.level, r.rx, r.ry, r.rz + 1)), 0);
    return out;
  };

  WorldView.prototype._uploadTex = function (r) {
    var gl = this.gl;
    if (!r.tex) r.tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_3D, r.tex);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_3D, gl.TEXTURE_WRAP_R, gl.CLAMP_TO_EDGE);
    gl.texImage3D(gl.TEXTURE_3D, 0, gl.R16UI, r.nx, r.ny, r.nz, 0,
                  gl.RED_INTEGER, gl.UNSIGNED_SHORT, r.cells);
  };

  WorldView.prototype._uploadMesh = function (r, slot, m) {
    var gl = this.gl;
    var o = r[slot];
    if (!o) { o = r[slot] = {vao: gl.createVertexArray(), vbo: gl.createBuffer(),
                             ibo: gl.createBuffer(), count: 0, quads: 0}; }
    o.quads = m.quads;
    o.count = m.idx.length;
    gl.bindVertexArray(o.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, o.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, m.verts, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 4, gl.UNSIGNED_BYTE, false, 4, 0);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, o.ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, m.idx, gl.STATIC_DRAW);
    gl.bindVertexArray(null);
  };

  WorldView.prototype._freeRegion = function (r) {
    var gl = this.gl;
    if (r.tex) gl.deleteTexture(r.tex);
    ['op', 'fl'].forEach(function (s) {
      if (!r[s]) return;
      gl.deleteVertexArray(r[s].vao);
      gl.deleteBuffer(r[s].vbo);
      gl.deleteBuffer(r[s].ibo);
    });
  };

  WorldView.prototype.invalidate = function (seed) {
    // Everything resident describes the old world. Bump the generation so
    // in-flight replies are dropped rather than composited into the new one.
    if (seed !== undefined) this.seed = seed;
    this.gen++;
    var self = this;
    this.regions.forEach(function (r) { self._freeRegion(r); });
    this.regions.clear();
    this.pending.clear();
    this.queue.length = 0;
    this.inflight = 0;
    this._pendingMeta && this._pendingMeta.clear();
  };

  // ---- camera -------------------------------------------------------------
  WorldView.prototype.camEye = function () {
    if (this.cam.mode === 'orbit') {
      var c = this.cam;
      return [c.target[0] + c.dist * Math.cos(c.pitch) * Math.sin(c.yaw),
              c.target[1] - c.dist * Math.sin(c.pitch),
              c.target[2] + c.dist * Math.cos(c.pitch) * Math.cos(c.yaw)];
    }
    return this.cam.pos.slice();
  };
  WorldView.prototype.camForward = function () {
    if (this.cam.mode === 'orbit') {
      var e = this.camEye(), t = this.cam.target;
      var d = [t[0] - e[0], t[1] - e[1], t[2] - e[2]];
      var l = Math.hypot(d[0], d[1], d[2]) || 1;
      return [d[0] / l, d[1] / l, d[2] / l];
    }
    var cp = Math.cos(this.cam.pitch);
    return [cp * Math.sin(this.cam.yaw), Math.sin(this.cam.pitch), cp * Math.cos(this.cam.yaw)];
  };

  WorldView.prototype.lookAt = function (x, y, z, dist) {
    this.cam.target = [x, y, z];
    if (dist !== undefined) this.cam.dist = dist;
    if (this.cam.mode === 'fly') {
      var f = this.camForward(), d = dist === undefined ? 60 : dist;
      this.cam.pos = [x - f[0] * d, y - f[1] * d, z - f[2] * d];
    }
  };

  // ---- picking ------------------------------------------------------------
  //
  // A DDA through the level-0 cells the client already holds. Not a GPU
  // readback and not a re-raymarch: the data is right here, and a picker that
  // asked the server would make every hover a round trip.
  WorldView.prototype.cellAtWorld = function (x, y, z, level) {
    level = level || 0;
    var ext = this._levelExtent(level);
    var r = this.regions.get(regionKey(level,
      Math.floor(x / ext), Math.floor(y / ext), Math.floor(z / ext)));
    if (!r || !r.cells) return -1;
    var lx = Math.floor((x - r.origin[0]) / r.lod);
    var ly = Math.floor((y - r.origin[1]) / r.lod);
    var lz = Math.floor((z - r.origin[2]) / r.lod);
    if (lx < 0 || ly < 0 || lz < 0 || lx >= r.nx || ly >= r.ny || lz >= r.nz) return -1;
    return r.cells[(lz * r.ny + ly) * r.nx + lx];
  };

  WorldView.prototype.raycast = function (ox, oy, oz, dx, dy, dz, maxDist, wantFluid) {
    var x = Math.floor(ox), y = Math.floor(oy), z = Math.floor(oz);
    var sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
    var tdx = dx !== 0 ? Math.abs(1 / dx) : Infinity;
    var tdy = dy !== 0 ? Math.abs(1 / dy) : Infinity;
    var tdz = dz !== 0 ? Math.abs(1 / dz) : Infinity;
    var tmx = dx !== 0 ? ((dx > 0 ? x + 1 - ox : ox - x) * tdx) : Infinity;
    var tmy = dy !== 0 ? ((dy > 0 ? y + 1 - oy : oy - y) * tdy) : Infinity;
    var tmz = dz !== 0 ? ((dz > 0 ? z + 1 - oz : oz - z) * tdz) : Infinity;
    var face = [0, 0, 0], t = 0;
    for (var i = 0; i < maxDist * 3 && t <= maxDist; i++) {
      var v = this.cellAtWorld(x, y, z, 0);
      if (v > 0) {
        var mat = v & MAT_MASK;
        var cls = this.classes ? this.classes[mat] : 0;
        if (wantFluid || (cls !== CLASS_LIQUID && cls !== CLASS_GAS))
          return {vox: [x, y, z], face: face, word: v, mat: mat,
                  state: (v >> 12) & 15, t: t};
      }
      if (tmx < tmy && tmx < tmz) { x += sx; t = tmx; tmx += tdx; face = [-sx, 0, 0]; }
      else if (tmy < tmz) { y += sy; t = tmy; tmy += tdy; face = [0, -sy, 0]; }
      else { z += sz; t = tmz; tmz += tdz; face = [0, 0, -sz]; }
    }
    return null;
  };

  WorldView.prototype.pickAt = function (px, py) {
    var cv = this.canvas;
    var w = cv.clientWidth, h = cv.clientHeight;
    var ndcx = (px / w) * 2 - 1, ndcy = 1 - (py / h) * 2;
    var eye = this.camEye(), fwd = this.camForward();
    var up = [0, 1, 0];
    var right = [fwd[1] * up[2] - fwd[2] * up[1], fwd[2] * up[0] - fwd[0] * up[2],
                 fwd[0] * up[1] - fwd[1] * up[0]];
    var rl = Math.hypot(right[0], right[1], right[2]) || 1;
    right = [right[0] / rl, right[1] / rl, right[2] / rl];
    var realUp = [right[1] * fwd[2] - right[2] * fwd[1],
                  right[2] * fwd[0] - right[0] * fwd[2],
                  right[0] * fwd[1] - right[1] * fwd[0]];
    var tanF = Math.tan(this.fov() / 2), asp = w / h;
    var d = [fwd[0] + right[0] * ndcx * tanF * asp + realUp[0] * ndcy * tanF,
             fwd[1] + right[1] * ndcx * tanF * asp + realUp[1] * ndcy * tanF,
             fwd[2] + right[2] * ndcx * tanF * asp + realUp[2] * ndcy * tanF];
    var dl = Math.hypot(d[0], d[1], d[2]) || 1;
    // Fluids are transparent to the picker unless you ask otherwise: reaching
    // through a pond to the rock under it is what you want nine times in ten,
    // and "pick water" is a checkbox rather than a surprise.
    return this.raycast(eye[0], eye[1], eye[2], d[0] / dl, d[1] / dl, d[2] / dl, 400,
                        !!this.tool.pickFluids);
  };

  WorldView.prototype.fov = function () { return Math.PI / 3; };

  // ---- rendering ----------------------------------------------------------
  WorldView.prototype.render = function () {
    var gl = this.gl, cv = this.canvas;
    var dpr = Math.min(window.devicePixelRatio || 1, 2);
    var dw = Math.max(1, Math.round(cv.clientWidth * dpr));
    var dh = Math.max(1, Math.round(cv.clientHeight * dpr));
    if (cv.width !== dw || cv.height !== dh) { cv.width = dw; cv.height = dh; }
    gl.viewport(0, 0, dw, dh);
    gl.clearColor(0.055, 0.065, 0.082, 1);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    var eye = this.camEye(), fwd = this.camForward();
    // The far plane follows the coarsest resident level; a fixed one either
    // clips the horizon or wastes the whole depth range on the near field.
    var farD = this._levelExtent(this.view.levels - 1) * 3;
    var proj = m4persp(this.fov(), dw / dh, 0.4, farD);
    var viewM = m4look(eye[0], eye[1], eye[2],
                       eye[0] + fwd[0], eye[1] + fwd[1], eye[2] + fwd[2], 0, 1, 0);
    var mvp = m4mul(proj, viewM);
    this.mvp = mvp;

    var lightDir = [-0.42, 0.80, -0.43];
    var ll = Math.hypot(lightDir[0], lightDir[1], lightDir[2]);
    lightDir = [lightDir[0] / ll, lightDir[1] / ll, lightDir[2] / ll];

    // The heightfield horizon first, so the voxel levels overwrite it.
    if (this.view.showFar && this.far) this._drawFar(mvp, eye, lightDir, farD);

    gl.useProgram(this.progVox);
    gl.uniformMatrix4fv(this.uVox.uMVP, false, mvp);
    gl.uniform3fv(this.uVox.uEye, eye);
    gl.uniform3fv(this.uVox.uLight, lightDir);
    gl.uniform1f(this.uVox.uAO, this.view.ao);
    gl.uniform1i(this.uVox.uMode,
      this.view.mode === 'class' ? 1 : (this.view.mode === 'height' ? 2 : 0));
    gl.uniform1i(this.uVox.uIsolate, this.view.isolate);
    gl.uniform1f(this.uVox.uSeaY, (this.opts.seaY || 1024));
    gl.uniform1i(this.uVox.uVox, 0);
    gl.uniform1i(this.uVox.uPal, 1);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.palTex);
    gl.activeTexture(gl.TEXTURE0);

    var list = [];
    var self = this;
    this.regions.forEach(function (r) {
      if (!r.op && !r.fl) return;
      if (!self._visible(r, mvp)) return;
      var ext = r.nx * r.lod;
      var cx = r.origin[0] + ext / 2, cy = r.origin[1] + ext / 2, cz = r.origin[2] + ext / 2;
      list.push({r: r, d: Math.hypot(cx - eye[0], cy - eye[1], cz - eye[2])});
    });
    list.sort(function (a, b) { return a.d - b.d; });

    this.stats.regions = list.length;
    this.stats.quads = 0;
    this.stats.draws = 0;

    gl.enable(gl.DEPTH_TEST);
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.uniform1f(this.uVox.uAlpha, 1.0);
    for (var i = 0; i < list.length; i++) this._drawRegion(list[i].r, 'op');

    if (this.view.showFluids) {
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.depthMask(false);
      gl.disable(gl.CULL_FACE);
      gl.uniform1f(this.uVox.uAlpha, 0.85);
      for (var j = list.length - 1; j >= 0; j--) this._drawRegion(list[j].r, 'fl');
      gl.enable(gl.CULL_FACE);
      gl.depthMask(true);
      gl.disable(gl.BLEND);
    }

    this._drawOverlays(mvp, eye);
  };

  // Frustum cull on the region's world box. Cheap and it matters: at four
  // levels the resident set is ~80 boxes and a fly camera sees a third of them.
  //
  // OUTCODE INTERSECTION, not "is any corner inside": a box larger than the
  // frustum — which every coarse level's box is, up close — has all eight
  // corners outside and would be culled by the naive test, deleting the ground
  // under your feet. A box is out only when all eight corners fail the SAME
  // clip plane.
  WorldView.prototype._visible = function (r, mvp) {
    var ext = r.nx * r.lod;
    var o = r.origin;
    var all = 63;
    for (var i = 0; i < 8; i++) {
      var x = o[0] + (i & 1 ? ext : 0), y = o[1] + (i & 2 ? ext : 0), z = o[2] + (i & 4 ? ext : 0);
      var cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
      var cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
      var cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
      var cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
      var code = (cx < -cw ? 1 : 0) | (cx > cw ? 2 : 0) | (cy < -cw ? 4 : 0) |
                 (cy > cw ? 8 : 0) | (cz < -cw ? 16 : 0) | (cz > cw ? 32 : 0);
      all &= code;
      if (!all) return true;
    }
    return false;
  };

  WorldView.prototype._drawRegion = function (r, slot) {
    var o = r[slot];
    if (!o || !o.count) return;
    var gl = this.gl;
    // The slice plane is a VIEW filter, so it is applied by not drawing rather
    // than by editing: it is how you look inside a hill without carving it.
    if (this.view.sliceEnabled) {
      var ax = this.view.sliceAxis;
      var lo = r.origin[ax], hi = lo + r.nx * r.lod;
      if (lo > this.view.slicePos) return;
      if (this.view.sliceThick > 0 && hi < this.view.slicePos - this.view.sliceThick) return;
    }
    gl.uniform3fv(this.uVox.uRegionOrigin, r.origin);
    gl.uniform1f(this.uVox.uLod, r.lod);
    gl.uniform3i(this.uVox.uDims, r.nx, r.ny, r.nz);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_3D, r.tex);
    gl.bindVertexArray(o.vao);
    gl.drawElements(gl.TRIANGLES, o.count, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
    this.stats.quads += o.quads;
    this.stats.draws++;
  };

  // ---- overlays -----------------------------------------------------------
  WorldView.prototype._lines = function () { this._lineBuf = []; return this._lineBuf; };
  function pushLine(a, x0, y0, z0, x1, y1, z1, c) {
    a.push(x0, y0, z0, c[0], c[1], c[2], c[3], x1, y1, z1, c[0], c[1], c[2], c[3]);
  }
  function pushBox(a, x0, y0, z0, x1, y1, z1, c) {
    pushLine(a, x0,y0,z0, x1,y0,z0, c); pushLine(a, x1,y0,z0, x1,y0,z1, c);
    pushLine(a, x1,y0,z1, x0,y0,z1, c); pushLine(a, x0,y0,z1, x0,y0,z0, c);
    pushLine(a, x0,y1,z0, x1,y1,z0, c); pushLine(a, x1,y1,z0, x1,y1,z1, c);
    pushLine(a, x1,y1,z1, x0,y1,z1, c); pushLine(a, x0,y1,z1, x0,y1,z0, c);
    pushLine(a, x0,y0,z0, x0,y1,z0, c); pushLine(a, x1,y0,z0, x1,y1,z0, c);
    pushLine(a, x1,y0,z1, x1,y1,z1, c); pushLine(a, x0,y0,z1, x0,y1,z1, c);
  }

  WorldView.prototype._drawOverlays = function (mvp, eye) {
    var a = this._lines();
    var v = this.view;
    if (v.showAxes) {
      var L = 64;
      pushLine(a, 0,0,0, L,0,0, [1,0.3,0.3,1]);
      pushLine(a, 0,0,0, 0,L,0, [0.3,1,0.3,1]);
      pushLine(a, 0,0,0, 0,0,L, [0.4,0.5,1,1]);
    }
    if (v.showRegions) {
      this.regions.forEach(function (r) {
        var ext = r.nx * r.lod;
        var t = r.level / 3;
        pushBox(a, r.origin[0], r.origin[1], r.origin[2],
                r.origin[0] + ext, r.origin[1] + ext, r.origin[2] + ext,
                [0.3 + 0.6 * t, 0.9 - 0.5 * t, 1.0 - 0.4 * t, 0.30]);
      });
    }
    if (v.showChunks) {
      // The ENGINE's 16-voxel chunk lattice, near the camera only. This is the
      // unit everything in the sim is dispatched, dirtied and paged in, so it
      // is the grid worth drawing.
      var cx = Math.floor(eye[0] / CHUNK) * CHUNK, cy = Math.floor(eye[1] / CHUNK) * CHUNK,
          cz = Math.floor(eye[2] / CHUNK) * CHUNK;
      var R = 4, col = [0.55, 0.62, 0.75, 0.22];
      for (var i = -R; i <= R; i++) for (var j = -R; j <= R; j++) {
        pushBox(a, cx + i * CHUNK, cy + j * CHUNK, cz - R * CHUNK,
                cx + (i + 1) * CHUNK, cy + (j + 1) * CHUNK, cz + (R + 1) * CHUNK, col);
      }
    }
    if (this.hover && this.tool.name !== 'none') {
      var h = this.hover.vox;
      pushBox(a, h[0], h[1], h[2], h[0] + 1, h[1] + 1, h[2] + 1, [1, 0.95, 0.4, 1]);
      if (this.tool.name === 'brush' || this.tool.name === 'erase') {
        var r0 = this.tool.radius;
        var c = this.brushCenter();
        pushBox(a, c[0] - r0, c[1] - r0, c[2] - r0,
                c[0] + r0 + 1, c[1] + r0 + 1, c[2] + r0 + 1,
                this.tool.name === 'erase' ? [1, 0.4, 0.4, 0.7] : [0.4, 1, 0.7, 0.7]);
      }
    }
    if (this.tool.sel) {
      var s = this.selBox();
      pushBox(a, s[0], s[1], s[2], s[3] + 1, s[4] + 1, s[5] + 1, [0.4, 0.85, 1, 0.95]);
    }
    if (this.tool.measure && this.tool.measure.a && this.tool.measure.b) {
      var m = this.tool.measure;
      pushLine(a, m.a[0] + 0.5, m.a[1] + 0.5, m.a[2] + 0.5,
               m.b[0] + 0.5, m.b[1] + 0.5, m.b[2] + 0.5, [1, 0.6, 0.2, 1]);
    }
    if (!a.length) return;
    var gl = this.gl;
    gl.useProgram(this.progLine);
    gl.uniformMatrix4fv(this.uLine.uMVP, false, mvp);
    gl.bindVertexArray(this.lineVAO);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.lineVBO);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(a), gl.DYNAMIC_DRAW);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.depthMask(false);
    gl.drawArrays(gl.LINES, 0, a.length / 7);
    gl.depthMask(true);
    gl.disable(gl.BLEND);
    gl.bindVertexArray(null);
  };

  // ---- the far heightfield ------------------------------------------------
  WorldView.prototype.setFarHeightfield = function (D, ramp, treeline) {
    var gl = this.gl;
    if (!D) { this.far = null; return; }
    var res = D.res;
    var V = new Float32Array(res * res * 9);
    var step = D.span / res;
    for (var j = 0; j < res; j++) for (var i = 0; i < res; i++) {
      var k = j * res + i, o = k * 9;
      V[o] = D.cx - D.span / 2 + (i + 0.5) * step;
      V[o + 1] = D.h[k];
      V[o + 2] = D.cz - D.span / 2 + (j + 0.5) * step;
      var c = ramp(D, k, treeline);
      V[o + 3] = c[0] / 255; V[o + 4] = c[1] / 255; V[o + 5] = c[2] / 255;
    }
    for (var j2 = 0; j2 < res; j2++) for (var i2 = 0; i2 < res; i2++) {
      var kk = j2 * res + i2, o2 = kk * 9;
      var yl = i2 > 0 ? V[(kk - 1) * 9 + 1] : V[o2 + 1];
      var yr = i2 < res - 1 ? V[(kk + 1) * 9 + 1] : V[o2 + 1];
      var yd = j2 > 0 ? V[(kk - res) * 9 + 1] : V[o2 + 1];
      var yu = j2 < res - 1 ? V[(kk + res) * 9 + 1] : V[o2 + 1];
      var nx = -(yr - yl) / (2 * step), nz = -(yu - yd) / (2 * step), ny = 1;
      var nl = Math.hypot(nx, ny, nz) || 1;
      V[o2 + 6] = nx / nl; V[o2 + 7] = ny / nl; V[o2 + 8] = nz / nl;
    }
    var I = new Uint32Array((res - 1) * (res - 1) * 6), p = 0;
    for (var jj = 0; jj < res - 1; jj++) for (var ii = 0; ii < res - 1; ii++) {
      var b = jj * res + ii;
      I[p++] = b; I[p++] = b + res; I[p++] = b + 1;
      I[p++] = b + 1; I[p++] = b + res; I[p++] = b + res + 1;
    }
    if (!this.far) this.far = {vao: gl.createVertexArray(), vbo: gl.createBuffer(),
                               ibo: gl.createBuffer(), count: 0};
    var f = this.far;
    gl.bindVertexArray(f.vao);
    gl.bindBuffer(gl.ARRAY_BUFFER, f.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, V, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 36, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 36, 12);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 3, gl.FLOAT, false, 36, 24);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, f.ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, I, gl.STATIC_DRAW);
    f.count = I.length;
    gl.bindVertexArray(null);
  };

  WorldView.prototype._drawFar = function (mvp, eye, light, farD) {
    var gl = this.gl, f = this.far;
    if (!f || !f.count) return;
    gl.useProgram(this.progFar);
    gl.uniformMatrix4fv(this.uFar.uMVP, false, mvp);
    gl.uniform3fv(this.uFar.uLight, light);
    gl.uniform3fv(this.uFar.uEye, eye);
    gl.uniform1f(this.uFar.uFade, this._levelExtent(this.view.levels - 1) * 0.9);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
    gl.disable(gl.CULL_FACE);
    gl.bindVertexArray(f.vao);
    gl.drawElements(gl.TRIANGLES, f.count, gl.UNSIGNED_INT, 0);
    gl.bindVertexArray(null);
    gl.enable(gl.CULL_FACE);
    gl.disable(gl.BLEND);
  };

  // ---- tools --------------------------------------------------------------
  WorldView.prototype.brushCenter = function () {
    if (!this.hover) return [0, 0, 0];
    var h = this.hover;
    // Paint ON TOP of the hit face, dig INTO it. Same convention as the game's
    // brush, and the one every voxel editor uses, because the alternative
    // (always the hit cell) makes it impossible to add a voxel to a flat wall.
    if (this.tool.name === 'brush')
      return [h.vox[0] + h.face[0], h.vox[1] + h.face[1], h.vox[2] + h.face[2]];
    return h.vox.slice();
  };

  WorldView.prototype.selBox = function () {
    var s = this.tool.sel;
    if (!s) return null;
    return [Math.min(s.a[0], s.b[0]), Math.min(s.a[1], s.b[1]), Math.min(s.a[2], s.b[2]),
            Math.max(s.a[0], s.b[0]), Math.max(s.a[1], s.b[1]), Math.max(s.a[2], s.b[2])];
  };

  // Every mutation goes through here, so undo is a property of the API rather
  // than of each tool remembering to record itself.
  WorldView.prototype.apply = function (cells, label) {
    if (!cells.length) return 0;
    var undo = [];
    for (var i = 0; i < cells.length; i++) {
      var c = cells[i];
      var prev = this.cellAtWorld(c[0], c[1], c[2], 0);
      if (prev < 0) continue;                 // outside level 0: refuse, see header
      var word = c[3];
      if ((prev & 0xFFFF) === (word & 0xFFFF)) continue;
      undo.push([c[0], c[1], c[2], prev]);
      this.edit.set(c[0], c[1], c[2], word);
      this._poke(c[0], c[1], c[2], word);
    }
    if (!undo.length) return 0;
    this.edit.pushUndo(undo, label || 'edit');
    this._flushPokes();
    return undo.length;
  };

  WorldView.prototype.undo = function () { return this._replay(this.edit.popUndo(), true); };
  WorldView.prototype.redo = function () { return this._replay(this.edit.popRedo(), false); };
  WorldView.prototype._replay = function (entry, isUndo) {
    if (!entry) return 0;
    var inverse = [];
    for (var i = 0; i < entry.cells.length; i++) {
      var c = entry.cells[i];
      var prev = this.cellAtWorld(c[0], c[1], c[2], 0);
      if (prev < 0) continue;
      inverse.push([c[0], c[1], c[2], prev]);
      this.edit.set(c[0], c[1], c[2], c[3]);
      this._poke(c[0], c[1], c[2], c[3]);
    }
    entry = {cells: inverse, label: entry.label};
    if (isUndo) this.edit.redoStack.push(entry); else this.edit.undoStack.push(entry);
    this._flushPokes();
    return inverse.length;
  };

  // Write one cell into every resident level-0 region that holds it, and mark
  // the region for re-mesh. The 3D texture is patched immediately so the colour
  // change is instant; the mesh only has to be rebuilt when SOLIDITY changed,
  // which is what makes a drag feel live instead of chasing the mesher.
  WorldView.prototype._poke = function (x, y, z, word) {
    var ext = REGION_N;
    var key = regionKey(0, Math.floor(x / ext), Math.floor(y / ext), Math.floor(z / ext));
    var r = this.regions.get(key);
    if (!r || !r.cells) return;
    var lx = x - r.origin[0], ly = y - r.origin[1], lz = z - r.origin[2];
    if (lx < 0 || ly < 0 || lz < 0 || lx >= r.nx || ly >= r.ny || lz >= r.nz) return;
    var idx = (lz * r.ny + ly) * r.nx + lx;
    var was = r.cells[idx];
    r.cells[idx] = word & 0xFFFF;
    r.rev = (r.rev || 0) + 1;    // invalidates any mesh already in the worker
    this.editRev++;
    var gl = this.gl;
    gl.bindTexture(gl.TEXTURE_3D, r.tex);
    gl.texSubImage3D(gl.TEXTURE_3D, 0, lx, ly, lz, 1, 1, 1, gl.RED_INTEGER,
                     gl.UNSIGNED_SHORT, new Uint16Array([word & 0xFFFF]));
    if (((was & MAT_MASK) === 0) !== (((word & MAT_MASK) === 0))) r.dirty = true;
    else if (this._solidity(was) !== this._solidity(word)) r.dirty = true;
    this._pokedRegions = this._pokedRegions || new Set();
    this._pokedRegions.add(r.key);
    // A cell on a region face changes the neighbour's mesh too.
    if (lx === 0 || ly === 0 || lz === 0 || lx === r.nx - 1 || ly === r.ny - 1 ||
        lz === r.nz - 1) this._markNeighborsDirty(r);
  };
  WorldView.prototype._solidity = function (w) {
    var m = w & MAT_MASK;
    if (!m) return 0;
    var c = this.classes ? this.classes[m] : 0;
    return (c === CLASS_LIQUID || c === CLASS_GAS) ? 2 : 1;
  };
  WorldView.prototype._flushPokes = function () { this._meshPass(); };

  // ---- brush shapes -------------------------------------------------------
  WorldView.prototype.brushCells = function (center, material, opts) {
    opts = opts || {};
    var r = this.tool.radius, out = [];
    var shape = this.tool.shape;
    var airOnly = this.tool.mode === 'air-only';
    for (var z = -r; z <= r; z++) for (var y = -r; y <= r; y++) for (var x = -r; x <= r; x++) {
      if (shape === 'sphere' && x * x + y * y + z * z > r * r + r * 0.6) continue;
      var wx = center[0] + x, wy = center[1] + y, wz = center[2] + z;
      if (airOnly) {
        var cur = this.cellAtWorld(wx, wy, wz, 0);
        if (cur < 0 || (cur & MAT_MASK) !== 0) continue;
      }
      out.push([wx, wy, wz, material]);
    }
    return out;
  };

  // A palette VARIANT is picked per cell so a painted wall is not one flat
  // colour: the engine's own cells carry one, and matter placed by hand should
  // look like matter, not like a decal. Deterministic in position so a repaint
  // of the same cell does not shimmer.
  WorldView.prototype.wordFor = function (mat, x, y, z) {
    if (!mat) return 0;
    var cls = this.classes ? this.classes[mat] : 0;
    if (cls === CLASS_LIQUID) return mat | (8 << 12);          // full cell
    var h = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791);
    return mat | (((h >>> 3) % 3) << 12);
  };

  // ---- input --------------------------------------------------------------
  WorldView.prototype._bindInput = function () {
    var self = this, cv = this.canvas;
    this.keys = Object.create(null);
    this.drag = null;
    cv.tabIndex = 0;
    cv.addEventListener('contextmenu', function (e) { e.preventDefault(); });
    cv.addEventListener('pointerdown', function (e) {
      cv.focus();
      cv.setPointerCapture(e.pointerId);
      self.drag = {x: e.clientX, y: e.clientY, btn: e.button, moved: false,
                   yaw: self.cam.yaw, pitch: self.cam.pitch,
                   pos: self.cam.pos.slice(), target: self.cam.target.slice()};
      if (e.button === 0) self._toolDown(e);
      e.preventDefault();
    });
    cv.addEventListener('pointermove', function (e) {
      var rect = cv.getBoundingClientRect();
      self._hoverAt(e.clientX - rect.left, e.clientY - rect.top);
      if (!self.drag) return;
      var dx = e.clientX - self.drag.x, dy = e.clientY - self.drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 2) self.drag.moved = true;
      if (self.drag.btn === 2) {
        self.cam.yaw = self.drag.yaw - dx * 0.005;
        self.cam.pitch = Math.max(-1.5, Math.min(1.5, self.drag.pitch - dy * 0.005));
      } else if (self.drag.btn === 1) {
        self._pan(dx, dy);
      } else if (self.drag.btn === 0) {
        self._toolDrag(e);
      }
    });
    cv.addEventListener('pointerup', function (e) {
      if (self.drag && self.drag.btn === 0) self._toolUp(e);
      self.drag = null;
      try { cv.releasePointerCapture(e.pointerId); } catch (err) {}
    });
    cv.addEventListener('wheel', function (e) {
      e.preventDefault();
      if (e.shiftKey) {
        // Shift-wheel resizes the brush: the one adjustment you make constantly
        // while painting, and reaching for a slider for it is the difference
        // between a tool and a form.
        self.tool.radius = Math.max(0, Math.min(32, self.tool.radius + (e.deltaY > 0 ? -1 : 1)));
        self.onToolChange && self.onToolChange();
        return;
      }
      var f = self.camForward();
      var k = (e.deltaY > 0 ? -1 : 1) * self.cam.speed * 0.25 * (e.ctrlKey ? 4 : 1);
      if (self.cam.mode === 'orbit') {
        self.cam.dist = Math.max(4, Math.min(20000, self.cam.dist * (e.deltaY > 0 ? 1.12 : 0.89)));
      } else {
        self.cam.pos[0] += f[0] * k; self.cam.pos[1] += f[1] * k; self.cam.pos[2] += f[2] * k;
      }
    }, {passive: false});
    cv.addEventListener('keydown', function (e) {
      self.keys[e.key.toLowerCase()] = true;
      if (e.key === 'Escape') { self.tool.sel = null; self.tool.measure = null; }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') {
        e.preventDefault();
        if (e.shiftKey) self.redo(); else self.undo();
        self.onEdit && self.onEdit();
      }
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'y') {
        e.preventDefault(); self.redo(); self.onEdit && self.onEdit();
      }
      if (!e.ctrlKey && !e.metaKey && 'wasdqe'.indexOf(e.key.toLowerCase()) >= 0)
        e.preventDefault();
    });
    cv.addEventListener('keyup', function (e) { self.keys[e.key.toLowerCase()] = false; });
    cv.addEventListener('blur', function () { self.keys = Object.create(null); });
  };

  WorldView.prototype._pan = function (dx, dy) {
    var f = this.camForward();
    var right = [f[2], 0, -f[0]];
    var rl = Math.hypot(right[0], right[2]) || 1;
    right = [right[0] / rl, 0, right[2] / rl];
    var s = (this.cam.mode === 'orbit' ? this.cam.dist : 1) * 0.0025 + 0.05;
    var d = this.drag;
    var mv = [(-dx * right[0]) * s, dy * s, (-dx * right[2]) * s];
    if (this.cam.mode === 'orbit')
      this.cam.target = [d.target[0] + mv[0], d.target[1] + mv[1], d.target[2] + mv[2]];
    else
      this.cam.pos = [d.pos[0] + mv[0], d.pos[1] + mv[1], d.pos[2] + mv[2]];
  };

  WorldView.prototype.step = function (dt) {
    if (this.cam.mode !== 'fly') return;
    var k = this.keys, sp = this.cam.speed * dt * (k['shift'] ? 4 : 1) * (k['control'] ? 0.25 : 1);
    if (!(k['w'] || k['a'] || k['s'] || k['d'] || k['q'] || k['e'])) return;
    var f = this.camForward();
    var right = [f[2], 0, -f[0]];
    var rl = Math.hypot(right[0], right[2]) || 1;
    right = [right[0] / rl, 0, right[2] / rl];
    var p = this.cam.pos;
    if (k['w']) { p[0] += f[0] * sp; p[1] += f[1] * sp; p[2] += f[2] * sp; }
    if (k['s']) { p[0] -= f[0] * sp; p[1] -= f[1] * sp; p[2] -= f[2] * sp; }
    if (k['d']) { p[0] += right[0] * sp; p[2] += right[2] * sp; }
    if (k['a']) { p[0] -= right[0] * sp; p[2] -= right[2] * sp; }
    if (k['e']) p[1] += sp;
    if (k['q']) p[1] -= sp;
  };

  WorldView.prototype._hoverAt = function (px, py) {
    var hit = this.pickAt(px, py);
    this.hover = hit;
    this.onHover && this.onHover(hit);
  };

  WorldView.prototype._toolDown = function (e) {
    var t = this.tool;
    if (t.name === 'none') return;
    if (!this.hover) return;
    if (t.name === 'picker') {
      t.material = this.hover.mat;
      this.onToolChange && this.onToolChange();
      return;
    }
    if (t.name === 'measure') {
      if (!t.measure || (t.measure.a && t.measure.b)) t.measure = {a: this.hover.vox.slice(), b: null};
      else t.measure.b = this.hover.vox.slice();
      this.onToolChange && this.onToolChange();
      return;
    }
    if (t.name === 'box') {
      t.sel = {a: this.hover.vox.slice(), b: this.hover.vox.slice()};
      this.onToolChange && this.onToolChange();
      return;
    }
    this._strokeCells = new Map();
    this._paint();
  };

  WorldView.prototype._toolDrag = function (e) {
    var t = this.tool;
    if (!this.hover) return;
    if (t.name === 'box' && t.sel) {
      t.sel.b = this.hover.vox.slice();
      this.onToolChange && this.onToolChange();
      return;
    }
    if (t.name === 'brush' || t.name === 'erase') this._paint();
  };

  WorldView.prototype._toolUp = function () {
    // One undo entry per STROKE, not per cell: a drag that painted 4,000 cells
    // must undo as one action or the stack is useless.
    if (this._strokeCells && this._strokeCells.size) {
      var undo = [];
      this._strokeCells.forEach(function (v) { undo.push(v); });
      this.edit.pushUndo(undo, this.tool.name === 'erase' ? 'erase' : 'paint');
      this.onEdit && this.onEdit();
    }
    this._strokeCells = null;
  };

  WorldView.prototype._paint = function () {
    var t = this.tool;
    var center = this.brushCenter();
    var mat = t.name === 'erase' ? 0 : t.material;
    var cells = this.brushCells(center, mat);
    var store = this._strokeCells;
    var n = 0;
    for (var i = 0; i < cells.length; i++) {
      var c = cells[i];
      var word = mat ? this.wordFor(mat, c[0], c[1], c[2]) : 0;
      var prev = this.cellAtWorld(c[0], c[1], c[2], 0);
      if (prev < 0) continue;
      if ((prev & 0xFFFF) === (word & 0xFFFF)) continue;
      var key = c[0] + ',' + c[1] + ',' + c[2];
      if (store && !store.has(key)) store.set(key, [c[0], c[1], c[2], prev]);
      this.edit.set(c[0], c[1], c[2], word);
      this._poke(c[0], c[1], c[2], word);
      n++;
    }
    if (n) this._flushPokes();
    return n;
  };

  // ---- selection operations ----------------------------------------------
  WorldView.prototype.selCells = function (fn) {
    var b = this.selBox();
    if (!b) return [];
    var out = [];
    for (var z = b[2]; z <= b[5]; z++)
      for (var y = b[1]; y <= b[4]; y++)
        for (var x = b[0]; x <= b[3]; x++) {
          var v = fn(x, y, z);
          if (v !== null && v !== undefined) out.push([x, y, z, v]);
        }
    return out;
  };

  WorldView.prototype.selVolume = function () {
    var b = this.selBox();
    if (!b) return 0;
    return (b[3] - b[0] + 1) * (b[4] - b[1] + 1) * (b[5] - b[2] + 1);
  };

  WorldView.prototype.fillSelection = function (mat) {
    var self = this;
    return this.apply(this.selCells(function (x, y, z) {
      return mat ? self.wordFor(mat, x, y, z) : 0;
    }), mat ? 'fill' : 'clear');
  };

  WorldView.prototype.replaceInSelection = function (fromMat, toMat) {
    var self = this;
    return this.apply(this.selCells(function (x, y, z) {
      var v = self.cellAtWorld(x, y, z, 0);
      if (v < 0 || (v & MAT_MASK) !== fromMat) return null;
      return toMat ? self.wordFor(toMat, x, y, z) : 0;
    }), 'replace');
  };

  WorldView.prototype.hollowSelection = function () {
    var self = this, b = this.selBox();
    if (!b) return 0;
    return this.apply(this.selCells(function (x, y, z) {
      if (x === b[0] || x === b[3] || y === b[1] || y === b[4] || z === b[2] || z === b[5])
        return null;
      var v = self.cellAtWorld(x, y, z, 0);
      if (v <= 0) return null;
      return 0;
    }), 'hollow');
  };

  WorldView.prototype.copySelection = function () {
    var b = this.selBox();
    if (!b) return null;
    var w = b[3] - b[0] + 1, h = b[4] - b[1] + 1, d = b[5] - b[2] + 1;
    var buf = new Uint16Array(w * h * d);
    for (var z = 0; z < d; z++) for (var y = 0; y < h; y++) for (var x = 0; x < w; x++) {
      var v = this.cellAtWorld(b[0] + x, b[1] + y, b[2] + z, 0);
      buf[(z * h + y) * w + x] = v < 0 ? 0 : v;
    }
    this.clipboard = {w: w, h: h, d: d, cells: buf};
    return this.clipboard;
  };

  WorldView.prototype.pasteAt = function (x, y, z, skipAir) {
    var c = this.clipboard;
    if (!c) return 0;
    var cells = [];
    for (var k = 0; k < c.d; k++) for (var j = 0; j < c.h; j++) for (var i = 0; i < c.w; i++) {
      var v = c.cells[(k * c.h + j) * c.w + i];
      if (skipAir && (v & MAT_MASK) === 0) continue;
      cells.push([x + i, y + j, z + k, v]);
    }
    return this.apply(cells, 'paste');
  };

  WorldView.prototype.moveSelection = function (dx, dy, dz) {
    var b = this.selBox();
    if (!b) return 0;
    this.copySelection();
    var cleared = this.fillSelection(0);
    var n = this.pasteAt(b[0] + dx, b[1] + dy, b[2] + dz, false);
    this.tool.sel = {a: [b[0] + dx, b[1] + dy, b[2] + dz],
                     b: [b[3] + dx, b[4] + dy, b[5] + dz]};
    return n;
  };

  // ===========================================================================
  // EditLayer — the authored patch over the generated world.
  // ===========================================================================
  //
  // Keyed by CHUNK, because that is the unit the engine applies it in: the
  // streamer fills a chunk from worldgen and then asks this layer what that
  // chunk owes. A flat "world coord -> word" map would have to be scanned in
  // full for every chunk that streamed in.
  function EditLayer() {
    this.chunks = new Map();      // "cx,cy,cz" -> Map(localIdx -> word)
    this.undoStack = [];
    this.redoStack = [];
    this.dirty = false;
    this.name = 'untitled';
  }
  EditLayer.prototype.key = function (cx, cy, cz) { return cx + ',' + cy + ',' + cz; };
  EditLayer.prototype.set = function (x, y, z, word) {
    var cx = x >> 4, cy = y >> 4, cz = z >> 4;
    var k = this.key(cx, cy, cz);
    var m = this.chunks.get(k);
    if (!m) { m = new Map(); this.chunks.set(k, m); }
    var li = ((z & 15) * CHUNK + (y & 15)) * CHUNK + (x & 15);
    m.set(li, word >>> 0);
    this.dirty = true;
  };
  EditLayer.prototype.get = function (x, y, z) {
    var m = this.chunks.get(this.key(x >> 4, y >> 4, z >> 4));
    if (!m) return undefined;
    return m.get(((z & 15) * CHUNK + (y & 15)) * CHUNK + (x & 15));
  };
  EditLayer.prototype.count = function () {
    var n = 0;
    this.chunks.forEach(function (m) { n += m.size; });
    return n;
  };
  EditLayer.prototype.clear = function () {
    this.chunks.clear(); this.undoStack.length = 0; this.redoStack.length = 0;
    this.dirty = true;
  };
  EditLayer.prototype.pushUndo = function (cells, label) {
    this.undoStack.push({cells: cells, label: label});
    if (this.undoStack.length > 200) this.undoStack.shift();
    this.redoStack.length = 0;
  };
  EditLayer.prototype.popUndo = function () { return this.undoStack.pop() || null; };
  EditLayer.prototype.popRedo = function () { return this.redoStack.pop() || null; };

  // Composite the layer over a freshly arrived region. Only level 0 regions map
  // 1:1 to world cells; at coarser levels one sample stands for up to 512
  // voxels and an edit has no honest place to land, so the layer simply does
  // not apply there — the coarse view shows the generated world, which is what
  // it is for.
  EditLayer.prototype.applyToRegion = function (r) {
    if (r.lod !== 1 || !this.chunks.size) return;
    var c0x = r.origin[0] >> 4, c0y = r.origin[1] >> 4, c0z = r.origin[2] >> 4;
    var nc = r.nx >> 4;
    for (var cz = 0; cz < nc; cz++) for (var cy = 0; cy < nc; cy++) for (var cx = 0; cx < nc; cx++) {
      var m = this.chunks.get(this.key(c0x + cx, c0y + cy, c0z + cz));
      if (!m) continue;
      var bx = cx * CHUNK, by = cy * CHUNK, bz = cz * CHUNK;
      m.forEach(function (word, li) {
        var lx = li % CHUNK, ly = ((li / CHUNK) | 0) % CHUNK, lz = (li / (CHUNK * CHUNK)) | 0;
        r.cells[(((bz + lz) * r.ny) + (by + ly)) * r.nx + (bx + lx)] = word & 0xFFFF;
      });
    }
  };

  // ---- the .svedit binary. Twin of LoadWorldEdits in src/sim/worldedit.cpp;
  // keep the two together.
  //
  //   0  'SVED'      12 u32 voxelCount    24 u32 reserved[2]
  //   4  u32 version 16 u32 seed
  //   8  u32 chunks  20 u32 flags
  //   32 per chunk: i32 cx, cy, cz, u32 n, then n * {u32 localIdx, u32 word}
  EditLayer.prototype.serialize = function (seed) {
    var chunkCount = 0, voxelCount = 0;
    this.chunks.forEach(function (m) { if (m.size) { chunkCount++; voxelCount += m.size; } });
    var bytes = 32 + chunkCount * 16 + voxelCount * 8;
    var buf = new ArrayBuffer(bytes), d = new DataView(buf);
    d.setUint32(0, 0x44455653, true);   // 'SVED'
    d.setUint32(4, 1, true);
    d.setUint32(8, chunkCount, true);
    d.setUint32(12, voxelCount, true);
    d.setUint32(16, seed >>> 0, true);
    var p = 32;
    var self = this;
    this.chunks.forEach(function (m, k) {
      if (!m.size) return;
      var parts = k.split(',');
      d.setInt32(p, parseInt(parts[0], 10), true);
      d.setInt32(p + 4, parseInt(parts[1], 10), true);
      d.setInt32(p + 8, parseInt(parts[2], 10), true);
      d.setUint32(p + 12, m.size, true);
      p += 16;
      m.forEach(function (word, li) {
        d.setUint32(p, li, true);
        d.setUint32(p + 4, word >>> 0, true);
        p += 8;
      });
    });
    return buf;
  };

  EditLayer.prototype.deserialize = function (buf) {
    var d = new DataView(buf);
    if (d.getUint32(0, true) !== 0x44455653) throw new Error('not an SVED layer');
    var chunks = d.getUint32(8, true);
    this.chunks.clear();
    var p = 32;
    for (var i = 0; i < chunks; i++) {
      var cx = d.getInt32(p, true), cy = d.getInt32(p + 4, true), cz = d.getInt32(p + 8, true);
      var n = d.getUint32(p + 12, true);
      p += 16;
      var m = new Map();
      for (var j = 0; j < n; j++) {
        m.set(d.getUint32(p, true), d.getUint32(p + 4, true));
        p += 8;
      }
      this.chunks.set(this.key(cx, cy, cz), m);
    }
    this.undoStack.length = 0;
    this.redoStack.length = 0;
    this.dirty = false;
    return {chunks: chunks, seed: d.getUint32(16, true)};
  };

  global.WorldView = WorldView;
  global.WorldViewEditLayer = EditLayer;
})(window);
