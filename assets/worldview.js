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
 * LOD: AN OCTREE, NOT SHELLS. Four levels of 64^3-sample regions at lod 1, 2,
 * 4 and 8, so a level-k region covers (64 << k) voxels per axis and every
 * region costs the same to mesh and store. lod 8 is the top because a region
 * may not span more than the residency window (512 voxels: the server refuses
 * a lod-16 box), and it is the expensive one — the server generates every
 * voxel of the box, ~0.4 s cold at lod 8 against ~50 ms at lod 1 — which is
 * why the root ring is 3 x 3 columns and not wider.
 * The roots are a ring of lod-8 regions round the camera, sat on the GROUND
 * (opts.groundAt, else the orbit target / the eye) rather than on the eye; a
 * node splits into its eight children while it is close to a FOCUS — the eye,
 * and in orbit mode the target too — relative to its own size. Leaves are what
 * you look at; internal nodes are fetched as well, for two reasons:
 *   - FALLBACK. A node whose children are not all meshed yet draws ITSELF, so
 *     the picture is always the finest complete cover, never a hole.
 *   - SKY IS FREE. Downsampling is by majority over non-air cells, so a parent
 *     sample is air only if its whole block is: an octant the parent holds as
 *     all air is all air at every finer level, exactly. Such children are made
 *     locally as empty regions and never requested.
 *
 * THIS REPLACED a fixed 3x3x3 shell per level centred on the EYE, which drew
 * nothing in the case the locale editor lives in: an orbit camera zoomed out
 * puts the eye in the sky, and every shell round it was air. The same version
 * left a region that fell out of the queue marked pending forever, so a short
 * zoom out and back deleted terrain until Reload — the queue is rebuilt from
 * the tree every update now, and pending means "a request is in flight".
 *
 * Past the last level the heightfield from /api/heightmap draws the horizon,
 * which is the one thing a column map is genuinely better at.
 *
 * SEAMS. A mesher that cannot see across a region boundary must guess, and both
 * guesses are wrong: "outside is air" emits a face the neighbour also emits
 * (z-fighting), "outside is solid" leaves a hole. So the mesher is handed the
 * six neighbour FACE SLABS, sampled from whatever resident region holds each
 * outside cell — same level first, then coarser, then finer, because in an
 * octree the neighbour across a face is often another level. A region is
 * re-meshed when a face neighbour arrives. Cells nothing holds read as solid,
 * so the transient artifact is a hole that fills itself rather than a flicker
 * that does not.
 *
 * ---------------------------------------------------------------------------
 * EDITING. Brush strokes and selection operations write into an EDIT LAYER —
 * a sparse chunk-keyed map of world cell -> voxel word — never into the
 * generated data. The layer is the authored artifact: it saves to
 * assets/worldedits/<name>.svedit (stored GROUND-RELATIVE since map-overhaul
 * P7, converted by the server) and the engine applies it as part of worldgen,
 * at startup and on every chunk stream-in, so an edit survives flying away and
 * coming back and follows the ground under it. The voxel server applies the
 * saved layer to the regions it returns; this page composites only the
 * strokes it has not saved yet. See src/sim/worldedit.h.
 *
 * Edits are only accepted inside level 0, where cells are 1:1 with world
 * voxels. A "brush" at lod 4 would have to invent 63 of every 64 voxels it
 * touched.
 */
(function (global) {
  'use strict';

  // ---- constants ----------------------------------------------------------
  var REGION_N = 64;          // samples per axis, every level
  var LEVELS = 4;             // lod 1, 2, 4, 8: 64 x 8 = 512 = the server's box cap (kWorldN)
  // The word a slab cell gets when no resident region holds it: any material
  // id the palette does not define has class 0 (solid), so the mesher reads it
  // as OPAQUE — the "missing reads as solid" rule, per cell instead of per slab.
  var UNKNOWN_WORD = 0xFFF;
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
    // Which octants hold anything: bit (x | y<<1 | z<<2) is set when that half-
    // box has a non-air sample. The streamer's sky test (see the header): an
    // octant that is all air here is all air at every finer level.
    '  var oct = 0, hx = h.nx >> 1, hy = h.ny >> 1, hz = h.nz >> 1, k2 = 0;',
    '  for (var z = 0; z < h.nz; z++) for (var y = 0; y < h.ny; y++) {',
    '    var ob = (z >= hz ? 4 : 0) | (y >= hy ? 2 : 0);',
    '    for (var x = 0; x < h.nx; x++, k2++) if (cells[k2]) oct |= 1 << (ob | (x >= hx ? 1 : 0));',
    '  }',
    '  h.oct = oct;',
    '  return h;',
    '}',
    '',
    // Occupancy classes the mesher switches on. UNKNOWN is what a missing
    // neighbour slab reads as, and it is deliberately treated as OPAQUE: a hole
    // at a seam fills itself when the neighbour lands, a double-emitted face
    // z-fights for as long as both regions are loaded.
    'var OC_EMPTY = 0, OC_OPAQUE = 1, OC_FLUID = 2;',
    // material id -> OC_*, built once per classes message. The first mesher
    // asked a closure for this per cell, per direction, per layer — twelve
    // bounds-checked calls a cell — and a 16M-cell biome swatch took 2.2 s in
    // it. Same greedy rectangles, lookups hoisted: the same swatch is ~0.25 s.
    'var OCC = null;',
    'function buildOcc(){',
    '  OCC = new Uint8Array(4096);',
    '  for (var m = 1; m < 4096; m++) { var c = CLS ? CLS[m] : 0; OCC[m] = (c === 2 || c === 3) ? OC_FLUID : OC_OPAQUE; }',
    '}',
    // Occupancy of a neighbour slab (raw cell words), or null when absent.
    'function slabOcc(slab){',
    '  if (!slab) return null;',
    '  var o = new Uint8Array(slab.length);',
    '  for (var k = 0; k < slab.length; k++) { var m = slab[k] & 0xFFF; o[k] = m ? OCC[m] : OC_EMPTY; }',
    '  return o;',
    '}',
    '',
    // A growable quad list. Vertices are (x, y, z, face) as Uint16: the first
    // version packed them as BYTES, which wrapped every region past 255 cells
    // on a side — the 320-cell biome swatch drew its far third folded back
    // over its near third, with no error anywhere. 16 bits reach 65535.
    'function QuadList(){ this.v = new Uint16Array(16 * 1024); this.i = new Uint32Array(6 * 1024); this.n = 0; }',
    'QuadList.prototype.push = function(p, du, dv, face, sign){',
    '  var n = this.n;',
    '  if ((n + 1) * 16 > this.v.length) {',
    '    var nv = new Uint16Array(this.v.length * 2); nv.set(this.v); this.v = nv;',
    '    var ni = new Uint32Array(this.i.length * 2); ni.set(this.i); this.i = ni;',
    '  }',
    '  var v = this.v, vo = n * 16, i = this.i, io = n * 6, b = n * 4;',
    '  v[vo] = p[0]; v[vo + 1] = p[1]; v[vo + 2] = p[2]; v[vo + 3] = face;',
    '  v[vo + 4] = p[0] + du[0]; v[vo + 5] = p[1] + du[1]; v[vo + 6] = p[2] + du[2]; v[vo + 7] = face;',
    '  v[vo + 8] = p[0] + du[0] + dv[0]; v[vo + 9] = p[1] + du[1] + dv[1]; v[vo + 10] = p[2] + du[2] + dv[2]; v[vo + 11] = face;',
    '  v[vo + 12] = p[0] + dv[0]; v[vo + 13] = p[1] + dv[1]; v[vo + 14] = p[2] + dv[2]; v[vo + 15] = face;',
    // Winding flips with the sign so both sides stay front-facing under
    // back-face culling.
    '  if (sign > 0) { i[io] = b; i[io + 1] = b + 1; i[io + 2] = b + 2; i[io + 3] = b; i[io + 4] = b + 2; i[io + 5] = b + 3; }',
    '  else { i[io] = b; i[io + 1] = b + 2; i[io + 2] = b + 1; i[io + 3] = b; i[io + 4] = b + 3; i[io + 5] = b + 2; }',
    '  this.n = n + 1;',
    '};',
    'QuadList.prototype.done = function(){ return {verts: this.v.slice(0, this.n * 16), idx: this.i.slice(0, this.n * 6), quads: this.n}; };',
    '',
    // Greedy merge of one signed face mask (mw x mh, +1 facing +d, -1 facing
    // -d) at slice `s` of axis d. Widest run first, then as tall as every row
    // still matches; merged cells are zeroed so the mask is clean for the next
    // slice. Face index 0..5 = -x,+x,-y,+y,-z,+z, which the shader turns into
    // the normal and the two tangents.
    // Only rows j0..j1 and columns i0..i1 can hold a face (the sweep tracked
    // the bounds while it built the mask); the rest of the plane is sky and is
    // skipped without being read. The tall-run test past j1 still reads zeros
    // and stops on the first one.
    'function mergeMask(mask, mw, mh, d, u, v2, s, out, j0, j1, i0, i1){',
    '  var p = [0, 0, 0], du = [0, 0, 0], dv = [0, 0, 0];',
    '  for (var j = j0; j <= j1; j++) {',
    '    var n = j * mw + i0;',
    '    for (var i = i0; i <= i1;) {',
    '      var c = mask[n];',
    '      if (!c) { i++; n++; continue; }',
    '      var w = 1;',
    '      while (i + w <= i1 && mask[n + w] === c) w++;',
    '      var hgt = 1, done = false;',
    '      while (j + hgt < mh) {',
    '        var row = n + hgt * mw;',
    '        for (var kk = 0; kk < w; kk++) if (mask[row + kk] !== c) { done = true; break; }',
    '        if (done) break;',
    '        hgt++;',
    '      }',
    '      p[0] = p[1] = p[2] = 0; p[d] = s; p[u] = i; p[v2] = j;',
    '      du[0] = du[1] = du[2] = 0; du[u] = w;',
    '      dv[0] = dv[1] = dv[2] = 0; dv[v2] = hgt;',
    '      out.push(p, du, dv, d * 2 + (c > 0 ? 1 : 0), c);',
    '      for (var l = 0; l < hgt; l++) { var ro = n + l * mw; for (var kx = 0; kx < w; kx++) mask[ro + kx] = 0; }',
    '      i += w; n += w;',
    '    }',
    '  }',
    '}',
    '',
    // Greedy mesh, both draw layers in one sweep:
    //   layer 0 (opaque):  a solid cell facing empty or fluid
    //   layer 1 (fluid):   a liquid/gas cell facing empty
    // Merging tests only "is this face exposed, and which way does it point",
    // because colour is a texture lookup in the fragment shader. That is what
    // makes a flat plain a few quads instead of one per cell.
    //
    // Out of the region: the neighbour slab if we have it, else OPAQUE. The
    // slabs are laid out as _neighborSlabs builds them — x-slab [z*ny+y],
    // y-slab [z*nx+x], z-slab [y*nx+x] — which in (i along u, j along v2)
    // terms is j*mw+i for x and z, i*mh+j for y.
    'function mesh(cells, nx, ny, nz, nbr){',
    '  if (!OCC) buildOcc();',
    '  var dims = [nx, ny, nz], st = [1, nx, nx * ny], total = nx * ny * nz;',
    '  var occ = new Uint8Array(total);',
    '  for (var k = 0; k < total; k++) { var m = cells[k] & 0xFFF; if (m) occ[k] = OCC[m]; }',
    '  var op = new QuadList(), fl = new QuadList();',
    '  var slabs = [[nbr.nx0, nbr.px0], [nbr.ny0, nbr.py0], [nbr.nz0, nbr.pz0]];',
    '  for (var d = 0; d < 3; d++) {',
    '    var u = (d + 1) % 3, v2 = (d + 2) % 3;',
    '    var mw = dims[u], mh = dims[v2], su = st[u], sv = st[v2], sd = st[d], nd = dims[d];',
    '    var mop = new Int8Array(mw * mh), mfl = new Int8Array(mw * mh);',
    '    var lo = slabOcc(slabs[d][0]), hi = slabOcc(slabs[d][1]);',
    '    var loM = null, hiM = null;',
    '    if (lo || hi) {',
    // Re-lay the slabs in mask order once, so the slice loop reads them by n.
    '      loM = new Uint8Array(mw * mh).fill(OC_OPAQUE); hiM = new Uint8Array(mw * mh).fill(OC_OPAQUE);',
    '      for (var jj = 0; jj < mh; jj++) for (var ii = 0; ii < mw; ii++) {',
    '        var si = d === 1 ? ii * mh + jj : jj * mw + ii, mi = jj * mw + ii;',
    '        if (lo) loM[mi] = lo[si]; if (hi) hiM[mi] = hi[si];',
    '      }',
    '    }',
    // Slice s of the sweep compares plane s-1 (a) with plane s (b). Plane s
    // is next slice's a, so it is read from the volume ONCE and kept: for the
    // x and y axes that read is a strided gather (stride nx, then nx*ny), and
    // reading each plane twice was half the mesher's time.
    '    var prev = new Uint8Array(mw * mh), cur = new Uint8Array(mw * mh);',
    '    if (loM) prev.set(loM); else prev.fill(OC_OPAQUE);',
    '    for (var s = 0; s <= nd; s++) {',
    '      var n = 0, anyO = false, anyF = false, j0 = mh, j1 = -1, i0 = mw, i1 = -1;',
    '      if (s === nd) { if (hiM) cur.set(hiM); else cur.fill(OC_OPAQUE); }',
    '      for (var j = 0; j < mh; j++) {',
    '        var base = s * sd + j * sv;',
    '        for (var i = 0; i < mw; i++, n++) {',
    '          var b = s === nd ? cur[n] : (cur[n] = occ[base + i * su]);',
    '          var a = prev[n], fo = 0, ff = 0;',
    '          if (a !== b) {',
    '            if (a === OC_OPAQUE) fo = 1; else if (b === OC_OPAQUE) fo = -1;',
    '            if (a === OC_FLUID && b === OC_EMPTY) ff = 1; else if (b === OC_FLUID && a === OC_EMPTY) ff = -1;',
    '            if (fo) anyO = true; if (ff) anyF = true;',
    '            if (j < j0) j0 = j; if (j > j1) j1 = j; if (i < i0) i0 = i; if (i > i1) i1 = i;',
    '          }',
    '          mop[n] = fo; mfl[n] = ff;',
    '        }',
    '      }',
    '      var t = prev; prev = cur; cur = t;',
    '      if (anyO) mergeMask(mop, mw, mh, d, u, v2, s, op, j0, j1, i0, i1);',
    '      if (anyF) mergeMask(mfl, mw, mh, d, u, v2, s, fl, j0, j1, i0, i1);',
    '    }',
    '  }',
    '  return {op: op.done(), fl: fl.done()};',
    '}',
    '',
    'self.onmessage = function(e){',
    '  var m = e.data;',
    '  if (m.cmd === "classes") { CLS = m.classes; PASSABLE = m.passable; OCC = null; return; }',
    '  if (m.cmd === "decode") {',
    '    try {',
    '      var h = decode(m.buf);',
    '      self.postMessage({cmd: "decoded", key: m.key, meta: m.meta, head: h, cells: h.cells},',
    '                       [h.cells.buffer]);',
    '    } catch (err) { self.postMessage({cmd: "error", key: m.key, meta: m.meta, error: String(err)}); }',
    '    return;',
    '  }',
    '  if (m.cmd === "mesh") {',
    '    try {',
    '      var r = mesh(m.cells, m.nx, m.ny, m.nz, m.nbr || {});',
    // The cells are NOT sent back: the main thread kept the original and only
    // lent a copy, so returning it would just make garbage.
    '      self.postMessage({cmd: "meshed", key: m.key, gen: m.gen, rev: m.rev,',
    '                        op: r.op, fl: r.fl},',
    '        [r.op.verts.buffer, r.op.idx.buffer, r.fl.verts.buffer, r.fl.idx.buffer]);',
    '    } catch (err) { self.postMessage({cmd: "error", key: m.key, mesh: true, error: String(err)}); }',
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
    // A CLIP BOX in world voxels (the Dungeons tab's x-ray): 0 = off, 1 = draw
    // only inside the box (a dungeon carved in rock shows its own surfaces,
    // backface culling makes the near walls see-through), 2 = cut the column
    // over the box's footprint above max.y (real terrain round a hole down to
    // the dungeon).
    'uniform int uClipMode; uniform vec3 uClipMin; uniform vec3 uClipMax;',
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
    '  if (uClipMode == 1 && (any(lessThan(vWorld, uClipMin)) || any(greaterThan(vWorld, uClipMax)))) discard;',
    '  if (uClipMode == 2 && vWorld.x >= uClipMin.x && vWorld.x <= uClipMax.x && vWorld.z >= uClipMin.z &&',
    '      vWorld.z <= uClipMax.z && vWorld.y > uClipMax.y) discard;',
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
    this.regions = new Map();     // key -> region (live, cached, or made locally as empty)
    this.pending = new Map();     // key -> gen, ONLY while a request is in flight
    this._reqT0 = new Map();
    this._failAt = new Map();     // key -> when its last request failed (retry after 5 s)      // key@gen -> request start, for "why is it slow"
    this.queue = [];              // what to fetch next, rebuilt from the tree every update()
    this.inflight = 0;            // requests not yet decoded, whatever their generation
    this.maxInflight = 6;
    this.tree = null;             // the octree of the last update(): {roots, nodes, leaves}
    this._frame = 0;              // update() counter; region.lastLive is stamped with it
    // The ground's y at a world column, or null where the host does not know.
    // Optional; it puts the root ring on the terrain instead of round the eye,
    // which matters for a camera high above the ground.
    this.groundAt = this.opts.groundAt || null;
    // Bumped by anything that changes what a frame would show without moving
    // the camera — a mesh landing, a region arriving or leaving, an edit, the
    // palette. Hosts that redraw only on change put it in their signature; a
    // signature built from the camera and region COUNT missed meshes that
    // landed after the last fetch, and the view sat on a blank frame until the
    // mouse moved.
    this.drawRev = 0;
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
    this.stats = {regions: 0, quads: 0, draws: 0, fetches: 0, bytes: 0, ms: 0,
                  farD: 0, farNeed: 0, errors: 0, lastError: '',
                  live: 0, ready: 0, resident: 0, queued: 0, meshing: 0, slowMs: 0};

    this.view = {
      mode: 'material',           // material | class | height
      ao: 1.0,
      grid: 1.0,
      levels: LEVELS,
      // A node splits while its distance to a focus is under detail x its own
      // extent. 1 puts a lod-k sample at no more than ~15 px on a 1000 px
      // canvas; 2 halves that and costs roughly 4x the regions.
      detail: 1.0,
      reach: 1,                   // root ring radius, in top-level (512-voxel) regions
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
      wire: false,
      // OPTIONAL HOST HOOKS (the Dungeons tab; off = the old behaviour):
      //   clip  : null | {mode: 1|2, min: [x,y,z], max: [x,y,z]} world voxels
      //           (see uClipMode in FS_VOX)
      //   roi   : null | {min, max}: the octree only grows nodes that touch this
      //           box, so a dungeon preview fetches the dungeon and not a ring
      //           of 512-voxel roots round it
      //   overlayOnTop : host overlay lines ignore depth (sockets through rock)
      clip: null,
      roi: null,
      overlayOnTop: false
    };
    // SOFT REFRESH (refreshBox): a region whose staleSeq is past the seq it was
    // fetched at keeps drawing and is fetched again; the reply replaces it.
    this.refreshSeq = 0;

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
    // Which edit layer the voxel server applies to the regions it hands back
    // (map-overhaul P7): '@map' = the one the map names (what the game spawns
    // into), '-' = bare worldgen, or a layer name (the one being edited).
    this.layerSel = '@map';
    // Which MAP the server generates (map v2 E4): '' = tuning's world.mapLayer
    // (what the game loads); a name = that map (the locale editor's view).
    this.mapName = '';
    // Where an edit may land: null = anywhere at level 0, or [x0, z0, x1, z1]
    // world voxels INCLUSIVE (a locale's bounds: its own layer holds nothing
    // outside them). A cell outside is skipped like a cell outside level 0.
    this.editBounds = null;
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
       'uAO', 'uGrid', 'uMode', 'uIsolate', 'uAlpha', 'uSeaY', 'uClipMode', 'uClipMin', 'uClipMax']);
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

  // A POOL when streaming: a cold octree is a few hundred regions to decode and
  // mesh, and one worker made the last of them land seconds after the first.
  // Replies are self-describing (key, gen, rev), so which worker answers does
  // not matter.
  WorldView.prototype._initWorker = function () {
    var self = this;
    var url = URL.createObjectURL(new Blob([WORKER_SRC], {type: 'text/javascript'}));
    var n = this.opts.streaming === false ? 1 :
      Math.max(1, Math.min(4, ((global.navigator && navigator.hardwareConcurrency) || 4) - 2));
    this.workers = [];
    for (var i = 0; i < n; i++) {
      var w = new Worker(url);
      w.onmessage = function (e) { self._onWorker(e.data); };
      this.workers.push(w);
    }
    this.worker = this.workers[0];
    this._wrr = 0;
  };
  WorldView.prototype._post = function (msg, xfer) {
    this.workers[this._wrr++ % this.workers.length].postMessage(msg, xfer || []);
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
    for (var w = 0; w < this.workers.length; w++)
      this.workers[w].postMessage({cmd: 'classes', classes: classes, passable: passable});
    this.drawRev++;
  };

  // ---- region keys and streaming -----------------------------------------
  function regionKey(level, rx, ry, rz) { return level + ':' + rx + ',' + ry + ',' + rz; }

  WorldView.prototype._levelExtent = function (level) { return REGION_N << level; };

  // Distance from the nearest focus to the cube [x0, x0+e)^3 — zero inside.
  function boxDist(foci, x0, y0, z0, e) {
    var best = Infinity;
    for (var i = 0; i < foci.length; i++) {
      var f = foci[i];
      var dx = Math.max(x0 - f[0], 0, f[0] - (x0 + e));
      var dy = Math.max(y0 - f[1], 0, f[1] - (y0 + e));
      var dz = Math.max(z0 - f[2], 0, f[2] - (z0 + e));
      var d = Math.sqrt(dx * dx + dy * dy + dz * dz);
      if (d < best) best = d;
    }
    return best;
  }

  // The octree for this camera (see the header). Pre-order, so a parent is
  // always in `nodes` before its children — update() relies on that to decide
  // a child from its parent in the same pass.
  WorldView.prototype._buildTree = function () {
    var T = Math.max(0, Math.min(LEVELS, this.view.levels | 0) - 1);
    var eT = REGION_N << T;
    var eye = this.camEye();
    var orbit = this.cam.mode === 'orbit';
    var foci = orbit ? [eye, this.cam.target.slice()] : [eye];
    var detail = Math.max(0.25, +this.view.detail || 1);
    var R = Math.max(0, this.view.reach | 0);
    var refY = orbit ? this.cam.target[1] : eye[1];
    var nodes = new Map(), roots = [], leaves = [];
    var roi = this.view.roi;
    function touches(L, rx, ry, rz) {
      if (!roi) return true;
      var e = REGION_N << L;
      return rx * e <= roi.max[0] && (rx + 1) * e > roi.min[0] &&
             ry * e <= roi.max[1] && (ry + 1) * e > roi.min[1] &&
             rz * e <= roi.max[2] && (rz + 1) * e > roi.min[2];
    }
    function outside(L, rx, ry, rz, parent, octant) {
      var e = REGION_N << L;
      var n = {key: regionKey(L, rx, ry, rz), level: L, rx: rx, ry: ry, rz: rz, e: e,
               d: Infinity, parent: parent, octant: octant, kids: null, outside: true};
      nodes.set(n.key, n);
      return n;
    }
    function visit(L, rx, ry, rz, parent, octant) {
      var e = REGION_N << L;
      var n = {key: regionKey(L, rx, ry, rz), level: L, rx: rx, ry: ry, rz: rz, e: e,
               d: boxDist(foci, rx * e, ry * e, rz * e, e), parent: parent,
               octant: octant, kids: null};
      nodes.set(n.key, n);
      if (L > 0 && n.d < detail * e) {
        n.kids = [];
        for (var k = 0; k < 8; k++) {
          var kx = rx * 2 + (k & 1), ky = ry * 2 + ((k >> 1) & 1), kz = rz * 2 + ((k >> 2) & 1);
          // Outside the ROI a child is known EMPTY without asking (synthesised
          // by update), so its parent's fallback rule still sees it complete.
          if (!touches(L - 1, kx, ky, kz)) { n.kids.push(outside(L - 1, kx, ky, kz, n, k)); continue; }
          n.kids.push(visit(L - 1, kx, ky, kz, n, k));
        }
      } else {
        leaves.push(n);
      }
      return n;
    }
    if (roi) {
      for (var oz = Math.floor(roi.min[2] / eT); oz <= Math.floor(roi.max[2] / eT); oz++)
        for (var oy = Math.floor(roi.min[1] / eT); oy <= Math.floor(roi.max[1] / eT); oy++)
          for (var ox = Math.floor(roi.min[0] / eT); ox <= Math.floor(roi.max[0] / eT); ox++)
            roots.push(visit(T, ox, oy, oz, null, -1));
      return {roots: roots, nodes: nodes, leaves: leaves, top: T, foci: foci};
    }
    var cx = Math.floor(eye[0] / eT), cz = Math.floor(eye[2] / eT);
    for (var z = cz - R; z <= cz + R; z++) {
      for (var x = cx - R; x <= cx + R; x++) {
        // Each root COLUMN spans the ground under it (half a root either side)
        // up to the focus height: the terrain is what there is to see, and a
        // ring of roots round a high eye would be a ring of sky.
        var g = null;
        if (this.groundAt) {
          try { g = this.groundAt((x + 0.5) * eT, (z + 0.5) * eT); } catch (err) { g = null; }
        }
        if (g === null || g === undefined || !isFinite(g)) g = refY;
        var lo = Math.min(g, refY) - eT * 0.5;
        var hi = Math.min(Math.max(g, refY) + eT * 0.5, lo + eT * 4);
        for (var y = Math.floor(lo / eT); y <= Math.floor(hi / eT); y++)
          roots.push(visit(T, x, y, z, null, -1));
      }
    }
    return {roots: roots, nodes: nodes, leaves: leaves, top: T, foci: foci};
  };

  // Does an UNSAVED stroke sit inside node n? Those exist only on this page, so
  // a parent the server sent cannot know about them and its sky test must not
  // be trusted over them.
  WorldView.prototype._unsavedIn = function (n) {
    var set = this.edit.unsavedRegions;
    if (!set || !set.size) return false;
    var s = n.level, hit = false;
    set.forEach(function (k) {
      if (hit) return;
      var p = k.split(',');
      if ((+p[0] >> s) === n.rx && (+p[1] >> s) === n.ry && (+p[2] >> s) === n.rz) hit = true;
    });
    return hit;
  };

  // Is node n deep enough under the host's ground that nothing in it can be
  // seen? Only for levels >= 1: a level-0 region is where edits land, and a
  // basement dug in the editor must have cells to dig into. The ground is
  // sampled over the footprint (a column map is smooth between posts, and the
  // margin of half a region absorbs what the samples miss); any column the
  // host cannot answer for makes the answer no.
  WorldView.prototype._buried = function (n) {
    if (!this.groundAt || n.level < 1) return false;
    // view.keepBox (world {min, max}; unset = none, the default): a box the
    // ground rule must not cull -- the Dungeons tab's in-world view, whose
    // dungeon IS under the ground and must stream, while the rock round it
    // (the margin columns, tens of buried regions) need not.
    var kb = this.view.keepBox;
    if (kb && n.rx * n.e <= kb.max[0] && (n.rx + 1) * n.e > kb.min[0] &&
        n.ry * n.e <= kb.max[1] && (n.ry + 1) * n.e > kb.min[1] &&
        n.rz * n.e <= kb.max[2] && (n.rz + 1) * n.e > kb.min[2]) return false;
    // Memoized per key until invalidate(): a node that is NOT buried is asked
    // again every update while it waits in the queue.
    var memo = this._buriedMemo || (this._buriedMemo = new Map());
    var known = memo.get(n.key);
    if (known !== undefined) return known;
    var ans = this._buriedTest(n);
    if (ans === null) return false;     // ground not known (yet): ask again later
    memo.set(n.key, ans);
    if (memo.size > 20000) memo.clear();
    return ans;
  };
  WorldView.prototype._buriedTest = function (n) {
    var e = n.e, k = e > 128 ? 5 : 3, gmin = Infinity;
    for (var j = 0; j < k; j++) for (var i = 0; i < k; i++) {
      var g;
      try { g = this.groundAt(n.rx * e + (i + 0.5) * e / k, n.rz * e + (j + 0.5) * e / k); }
      catch (err) { return null; }
      if (g === null || g === undefined || !isFinite(g)) return null;
      if (g < gmin) gmin = g;
    }
    return (n.ry + 1) * e < gmin - (e / 2 + 16);
  };

  // A region known without asking: EMPTY (all air — ready at once, draws
  // nothing, reads as air to the picker and to its neighbours' seams; editing
  // into one gives it cells, see _poke) or BURIED (under the ground: draws
  // nothing and reads as unknown, which the mesher treats as solid, so no
  // neighbour grows a wall against it).
  WorldView.prototype._synthEmpty = function (n, buried) {
    var lod = 1 << n.level;
    var r = {
      key: n.key, level: n.level, rx: n.rx, ry: n.ry, rz: n.rz,
      lod: lod, nx: REGION_N, ny: REGION_N, nz: REGION_N,
      origin: [n.rx * n.e, n.ry * n.e, n.rz * n.e],
      cells: null, empty: !buried, buried: !!buried, oct: buried ? 0xFF : 0,
      tex: null, op: null, fl: null,
      meshing: false, meshed: true, dirty: false, gen: this.gen, rev: 0,
      lastLive: this._frame, local: true
    };
    this.regions.set(n.key, r);
    return r;
  };

  // ---- a LOCAL region, with no server behind it ---------------------------
  //
  // Everything above streams: _desired() decides what should be resident, the
  // queue fetches it from /api/voxregion, the worker decodes and meshes it.
  // The Trees tab has no terrain and no server request to make — it has a
  // Uint16Array that assets/editor/treegen.js just produced in this same tab,
  // and it wants it drawn.
  //
  // This is the seam for that: the same region record the `decoded` worker
  // branch builds, handed straight to the same upload and mesh path. The
  // viewer's AO, slice, isolate, palette, orbit camera, screenshot and picking
  // all work on it unchanged, which is the whole reason the Trees tab reuses
  // WorldView instead of growing a second WebGL renderer that would drift.
  //
  // `opts.streaming === false` (set by that tab) is what stops update() from
  // asking a server that is not there for regions that do not exist.
  // SEVERAL authored grids at once, each with its own size and world offset.
  // The Trees tab's quad view is four separate trees rather than one composited
  // grid on purpose: a 2x2 layout of great oaks in a single array is
  // (2*231)^2*150 = 32M cells of which the trees occupy an eighth, and both the
  // Uint16Array and the R16UI texture pay for all of it. Four regions pay for
  // four trees.
  //
  // Keys are 'local0..N' and every entry keeps rx/ry/rz = 0, so _neighborSlabs
  // (which looks up by regionKey) finds nothing for any of them — which is what
  // we want: a missing neighbour meshes as OPAQUE and would otherwise weld the
  // facing sides of two adjacent trees shut.
  WorldView.prototype.setLocalRegions = function (list) {
    var self = this;
    this.regions.forEach(function (r) { self._freeRegion(r); });
    this.regions.clear();
    this.gen++;
    var out = [];
    for (var i = 0; i < list.length; i++) {
      var e = list[i];
      var o = e.origin || [0, 0, 0];
      var r = {
        key: 'local' + i, level: 0, rx: 0, ry: 0, rz: 0,
        lod: e.lod || 1, nx: e.nx, ny: e.ny, nz: e.nz,
        origin: [o[0], o[1], o[2]],
        cells: e.cells, empty: false, tex: null, op: null, fl: null, meshing: false,
        meshed: false, dirty: true, gen: this.gen, rev: 0, local: true
      };
      this.regions.set(r.key, r);
      this._uploadTex(r);
      out.push(r);
    }
    // _meshPass fills every worker slot and _onWorker pumps the rest as each
    // finishes, so one priming call drains the whole list.
    this.drawRev++;
    this._meshPass();
    return out;
  };

  WorldView.prototype.setLocalRegion = function (cells, nx, ny, nz, origin) {
    return this.setLocalRegions(
        [{cells: cells, nx: nx, ny: ny, nz: nz, origin: origin}])[0];
  };

  WorldView.prototype.update = function () {
    // A local region is authored, not streamed: there is nothing to want and
    // nothing to evict, and running the streamer would delete the one region
    // the tab is showing on its first frame.
    if (this.opts.streaming === false) return;
    // Nothing is fetched before the palette: the mesher needs the material
    // classes to tell water from rock, and a region meshed without them keeps
    // its liquids as solid walls until something re-meshes it.
    if (!this.pal) return;
    var self = this;
    var t = this.tree = this._buildTree();
    var frame = ++this._frame;
    var want = [], ready = 0;
    t.nodes.forEach(function (n) {
      var r = self.regions.get(n.key);
      if (!r && n.outside) r = self._synthEmpty(n);
      if (r && r.staleSeq > (r.fetchedSeq || 0) && !r.local && !self.pending.has(n.key)) {
        // Soft refresh: keep drawing the old one, ask again.
        r.lastLive = frame;
        if (r.empty || r.meshed) ready++;
        want.push(n);
        return;
      }
      if (!r && n.parent) {
        var par = self.regions.get(n.parent.key);
        // Top-down: a child waits for its parent, whose octant mask says
        // whether there is anything to ask for at all.
        if (!par) return;
        if (par.buried) r = self._synthEmpty(n, true);
        else if ((par.empty || !(par.oct & (1 << n.octant))) && !self._unsavedIn(n))
          r = self._synthEmpty(n);
      }
      if (!r && self._buried(n)) r = self._synthEmpty(n, true);
      if (r) {
        r.lastLive = frame;
        if (r.empty || r.meshed) ready++;
        return;
      }
      if (self.pending.has(n.key)) return;
      // A failed region is retried, but not every frame: a server answering
      // 500 for it would otherwise be asked sixty times a second.
      var failed = self._failAt.get(n.key);
      if (failed !== undefined && performance.now() - failed < 5000) return;
      want.push(n);
    });
    // Nearest-in-screen-size first: d / e is how big the region looks. What the
    // camera cannot see waits behind everything it can.
    var mvp = this.mvp;
    for (var i = 0; i < want.length; i++) {
      var n = want[i];
      n.pri = n.d / n.e - n.level * 0.01;
      if (mvp && !boxVisible(n.rx * n.e, n.ry * n.e, n.rz * n.e, n.e, n.e, n.e, mvp)) n.pri += 4;
    }
    want.sort(function (a, b) { return a.pri - b.pri; });
    this.queue = want;
    this._failAt.forEach(function (when, key) { if (!t.nodes.has(key)) self._failAt.delete(key); });
    this.stats.live = t.nodes.size;
    this.stats.ready = ready;
    this.stats.queued = want.length;
    this._evict(frame, t.nodes.size);
    this._pump();
  };

  // Regions that left the tree stay as a CACHE until the budget is spent, so a
  // zoom out and back in redraws at once instead of refetching; the oldest go
  // first.
  WorldView.prototype._evict = function (frame, live) {
    var budget = live + (this.view.cacheRegions || 256);
    if (this.regions.size <= budget) return;
    var old = [];
    this.regions.forEach(function (r) { if (r.lastLive !== frame) old.push(r); });
    old.sort(function (a, b) { return a.lastLive - b.lastLive; });
    for (var i = 0; i < old.length && this.regions.size > budget; i++) {
      this._freeRegion(old[i]);
      this.regions.delete(old[i].key);
      this.drawRev++;
    }
  };

  // One line a host can show so an empty view is never unexplained: how much of
  // the tree has landed, what is in flight, and why it is slow if it is.
  WorldView.prototype.streamStatus = function () {
    var s = this.stats;
    if (!this.pal) return 'waiting for the material palette';
    if (!this.tree) return '';
    var out = s.ready + '/' + s.live + ' regions';
    if (this.inflight || s.queued) out += ' · loading ' + this.inflight + ' (+' + s.queued + ' queued)';
    if (s.meshing) out += ' · meshing ' + s.meshing;
    var oldest = 0, now = performance.now();
    this._reqT0.forEach(function (t0) { oldest = Math.max(oldest, now - t0); });
    if (oldest > 2500 || (this.inflight && s.slowMs))
      out += ' · voxel server slow (' + (Math.max(oldest, s.slowMs) / 1000).toFixed(0) +
             ' s: it waits for the machine-wide GPU lock while another sandvox run holds it)';
    // Recent failures only: one that has since been retried successfully is
    // history, and a stale error beside a full picture is a false alarm.
    // A `busy` answer (tuner_server.py: the GPU lock stayed with another run
    // past the request's patience) is a queue, not a failure: say so.
    if (s.lastError && this._failAt.size && /^busy: /.test(s.lastError))
      out += ' · ' + this._failAt.size + ' region(s) waiting for the GPU lock, retrying (' +
             s.lastError.replace(/^busy: waiting for the GPU lock \(held by /, 'held by ').replace(/\) for >\d+s$/, '') + ')';
    else if (s.lastError && this._failAt.size)
      out += ' · ' + this._failAt.size + ' region(s) failing, retrying: ' + s.lastError;
    return out;
  };

  WorldView.prototype._pump = function () {
    while (this.inflight < this.maxInflight && this.queue.length) {
      var n = this.queue.shift();
      var have = this.regions.get(n.key);
      if ((have && !(have.staleSeq > (have.fetchedSeq || 0))) || this.pending.has(n.key)) continue;
      this._fetch(n);
    }
  };

  WorldView.prototype._fetch = function (n) {
    var self = this;
    var key = n.key;
    var lod = 1 << n.level, ext = n.e;
    var q = 'ox=' + (n.rx * ext) + '&oy=' + (n.ry * ext) + '&oz=' + (n.rz * ext) +
            '&nx=' + REGION_N + '&ny=' + REGION_N + '&nz=' + REGION_N +
            '&lod=' + lod + '&seed=' + this.seed +
            '&layer=' + encodeURIComponent(this.layerSel || '@map') +
            (this.mapName ? '&map=' + encodeURIComponent(this.mapName) : '');
    // THE ROI IS A CLIP, NOT ONLY A TREE FILTER: what lies outside it is drawn
    // as air (_buildTree's `outside` nodes), so the server need not generate
    // it. A 512-voxel root over a dungeon ROI used to evaluate ~3x the
    // dungeon's volume in genChunk; the server normalises the box to chunks
    // and drops it when it keeps the whole region (tuner_server.py).
    var roi = this.view.roi;
    if (roi) q += '&clip=' + [Math.floor(roi.min[0]), Math.floor(roi.min[1]), Math.floor(roi.min[2]),
                              Math.ceil(roi.max[0]), Math.ceil(roi.max[1]), Math.ceil(roi.max[2])].join(',');
    var gen = this.gen, seq = this.refreshSeq;
    this.inflight++;
    this.pending.set(key, gen);
    var t0 = performance.now();
    this._reqT0.set(key + '@' + gen, t0);
    fetch('/api/voxregion?' + q).then(function (r) {
      if (!r.ok) return r.json().catch(function () { return {}; }).then(function (j) {
        var m = j.error || ('HTTP ' + r.status);
        throw new Error(j.busy && !/^busy: /.test(m) ? 'busy: ' + m : m);
      });
      return r.arrayBuffer();
    }).then(function (buf) {
      var ms = performance.now() - t0;
      self.stats.fetches++;
      self.stats.bytes += buf.byteLength;
      self.stats.ms = ms;
      // A slow reply is almost always the voxel server waiting for the
      // machine-wide GPU lock (another session's run, a link). Recorded so the
      // host can say so instead of showing an empty view with no reason.
      self.stats.slowMs = ms > 1500 ? ms : 0;
      if (gen !== self.gen) { self._settle(key, gen); return; }
      self._post({cmd: 'decode', key: key, buf: buf,
                  meta: {level: n.level, rx: n.rx, ry: n.ry, rz: n.rz, gen: gen, seq: seq}}, [buf]);
    }).catch(function (e) {
      self._failAt.set(key, performance.now());
      self.stats.errors++;
      self.stats.lastError = String(e && e.message || e);
      self._settle(key, gen);
      if (gen === self.gen && self.onError) self.onError(self.stats.lastError);
    });
  };

  // A request is over (decoded, failed, or answered for a world that has since
  // been invalidated). `pending` is cleared only if it still names THIS
  // request: after an invalidate the same key can be in flight twice.
  WorldView.prototype._settle = function (key, gen) {
    this.inflight = Math.max(0, this.inflight - 1);
    this._reqT0.delete(key + '@' + gen);
    if (this.pending.get(key) === gen) this.pending.delete(key);
    this._pump();
  };

  WorldView.prototype._onWorker = function (m) {
    if (m.cmd === 'error') {
      if (m.mesh) {
        var bad = this.regions.get(m.key);
        if (bad) bad.meshing = false;
        this.stats.errors++;
        this.stats.lastError = 'mesh: ' + m.error;
        this.onError && this.onError(this.stats.lastError);
        this._meshPass();
        return;
      }
      var g = m.meta ? m.meta.gen : this.gen;
      this._failAt.set(m.key, performance.now());
      this.stats.errors++;
      this.stats.lastError = m.error;
      this._settle(m.key, g);
      if (g === this.gen && this.onError) this.onError(m.error);
      return;
    }
    if (m.cmd === 'decoded') {
      var meta = m.meta;
      this._settle(m.key, meta.gen);
      this._failAt.delete(m.key);
      var old = this.regions.get(m.key);
      if (meta.gen !== this.gen || (old && !(old.staleSeq > (old.fetchedSeq || 0)))) return;
      if (old) {
        this._freeRegion(old); this.regions.delete(m.key);
        // Children synthesised from the OLD octant mask (empty / buried) are
        // re-decided from the new one.
        var e0 = (REGION_N << meta.level), o0 = [meta.rx * e0, meta.ry * e0, meta.rz * e0], self0 = this, gone = [];
        this.regions.forEach(function (q) {
          if (!q.local || q.cells || q.level >= meta.level) return;
          if (q.origin[0] >= o0[0] && q.origin[0] < o0[0] + e0 && q.origin[1] >= o0[1] && q.origin[1] < o0[1] + e0 &&
              q.origin[2] >= o0[2] && q.origin[2] < o0[2] + e0) gone.push(q);
        });
        gone.forEach(function (q) { self0._freeRegion(q); self0.regions.delete(q.key); });
      }
      var r = {
        key: m.key, level: meta.level, rx: meta.rx, ry: meta.ry, rz: meta.rz,
        lod: m.head.lod, nx: m.head.nx, ny: m.head.ny, nz: m.head.nz,
        origin: [m.head.ox, m.head.oy, m.head.oz],
        cells: m.cells, empty: false, oct: m.head.oct, tex: null, op: null, fl: null,
        meshing: false, meshed: false, dirty: true, gen: this.gen, rev: 0,
        lastLive: this._frame, fetchedSeq: meta.seq || 0,
        staleSeq: old ? old.staleSeq : 0
      };
      // The SAVED layer is already in the region: the voxel server applies it
      // (P7). What only this page knows is the UNSAVED strokes, so those are
      // composited on arrival — otherwise a fly-away-and-back would lose them.
      var stroked = this.edit.applyToRegion(r);
      if (!r.oct && !stroked) {
        // All sky: keep no cells, no texture, no mesh. Most of a tree is this.
        r.cells = null; r.empty = true; r.meshed = true; r.dirty = false;
      } else {
        this._uploadTex(r);
      }
      this.regions.set(m.key, r);
      this.drawRev++;
      this._markNeighborsDirty(r);
      this._meshPass();
      return;
    }
    if (m.cmd === 'meshed') {
      var reg = this.regions.get(m.key);
      if (!reg) { this._meshPass(); return; }
      reg.meshing = false;
      // A mesh built from cells that have since been edited describes a world
      // that no longer exists. `rev` is bumped by every poke; if it moved while
      // the worker was running, throw the result away and mesh again.
      if (m.gen !== reg.gen || m.rev !== reg.rev) { reg.dirty = true; this._meshPass(); return; }
      this._uploadMesh(reg, 'op', m.op);
      this._uploadMesh(reg, 'fl', m.fl);
      reg.meshed = true;
      this.drawRev++;
      this._meshPass();
      return;
    }
  };

  // Every resident region sharing a FACE with r, at any level: its seam was
  // meshed against cells r now supplies. Edge and corner contacts change no
  // face, so they are not counted. Authored (local) regions have no
  // neighbours by design — two trees side by side must not weld.
  WorldView.prototype._markNeighborsDirty = function (r) {
    if (this.opts.streaming === false) return;
    var a0 = r.origin, s = r.nx * r.lod;
    this.regions.forEach(function (q) {
      if (q === r || q.empty || !q.cells || q.dirty) return;
      var b0 = q.origin, t = q.nx * q.lod, touch = 0;
      for (var i = 0; i < 3; i++) {
        var a1 = a0[i] + s, b1 = b0[i] + t;
        if (a1 < b0[i] || b1 < a0[i]) return;
        if (a1 === b0[i] || b1 === a0[i]) touch++;
      }
      if (touch === 1) q.dirty = true;
    });
  };

  // Up to two meshes per worker in flight — meshing every dirty region at once
  // on arrival is what turns a camera move into a stall. Regions never meshed
  // go first (they draw nothing yet), then re-meshes; within each, the one
  // that looks biggest. A region that left the tree is not meshed until it
  // comes back.
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
    var cap = 2 * this.workers.length;
    var busy = 0;
    this.regions.forEach(function (r) { if (r.meshing) busy++; });
    if (busy >= cap) return;
    var eye = this.camEye(), frame = this._frame;
    var streaming = this.opts.streaming !== false;
    var cand = [];
    this.regions.forEach(function (r) {
      if (!r.dirty || r.meshing || !r.cells || r.empty) return;
      if (streaming && r.lastLive !== frame) return;
      var e = r.nx * r.lod;
      var d = Math.hypot(r.origin[0] + e / 2 - eye[0], r.origin[1] + e / 2 - eye[1],
                         r.origin[2] + e / 2 - eye[2]);
      cand.push({r: r, pri: (r.meshed ? 1e6 : 0) + d / e});
    });
    cand.sort(function (a, b) { return a.pri - b.pri; });
    for (var i = 0; i < cand.length && busy < cap; i++, busy++) {
      var pick = cand[i].r;
      pick.dirty = false;
      pick.meshing = true;
      var nbr = this._neighborSlabs(pick);
      var copy = pick.cells.slice();
      this._post({
        cmd: 'mesh', key: pick.key, gen: pick.gen, rev: pick.rev || 0, cells: copy,
        nx: pick.nx, ny: pick.ny, nz: pick.nz, nbr: nbr
      }, [copy.buffer]);
    }
    this.stats.meshing = busy;
  };

  // The word at world voxel (x,y,z) as resident level `level` holds it: 0 for a
  // region known empty, -1 when that level has no region there. `cache` is one
  // slot per level holding the last region box looked up (hit or miss), which
  // turns a face's 4096 lookups into a handful of Map gets.
  WorldView.prototype._wordAt = function (x, y, z, level, cache) {
    var c = cache[level];
    var e = REGION_N << level;
    if (!c || x < c.x0 || y < c.y0 || z < c.z0 || x >= c.x0 + e || y >= c.y0 + e || z >= c.z0 + e) {
      var rx = Math.floor(x / e), ry = Math.floor(y / e), rz = Math.floor(z / e);
      c = cache[level] = {x0: rx * e, y0: ry * e, z0: rz * e,
                          r: this.regions.get(regionKey(level, rx, ry, rz)) || null};
    }
    var r = c.r;
    if (!r) return -1;
    if (r.empty) return 0;
    if (!r.cells) return -1;
    var lx = ((x - r.origin[0]) / r.lod) | 0, ly = ((y - r.origin[1]) / r.lod) | 0,
        lz = ((z - r.origin[2]) / r.lod) | 0;
    return r.cells[(lz * r.ny + ly) * r.nx + lx];
  };

  // The six face slabs of what lies just outside r, so the mesher can tell a
  // real surface from a region boundary. Each outside cell is read from
  // whichever resident region holds it — r's own level first, then coarser,
  // then finer — because across a face of an octree leaf the neighbour is
  // often another level. A cell no region holds is UNKNOWN_WORD (opaque); a
  // slab with nothing known at all is left absent, which the worker reads the
  // same way.
  WorldView.prototype._neighborSlabs = function (r) {
    var out = {};
    var nx = r.nx, ny = r.ny, nz = r.nz;
    if (this.opts.streaming === false) {
      // Authored regions: no neighbours by design (see setLocalRegions).
      return out;
    }
    var self = this, L = r.level, lod = r.lod, o = r.origin, half = lod >> 1;
    var order = [L];
    for (var a = L + 1; a < LEVELS; a++) order.push(a);
    for (var b = L - 1; b >= 0; b--) order.push(b);
    var cache = [];
    function word(x, y, z) {
      for (var i = 0; i < order.length; i++) {
        var w = self._wordAt(x, y, z, order[i], cache);
        if (w >= 0) return w;
      }
      return -1;
    }
    // Sample centre of the outside layer on each side of axis `ax`.
    function outside(ax, hi) {
      var n = [nx, ny, nz][ax];
      return hi ? o[ax] + n * lod + half : o[ax] - lod + half;
    }
    function slab(ax, hi) {
      var u = ax === 0 ? 1 : 0, v = ax === 2 ? 1 : 2;  // x: (y,z)  y: (x,z)  z: (x,y)
      var nu = [nx, ny, nz][u], nv = [nx, ny, nz][v];
      var s = new Uint16Array(nu * nv), known = 0, p = [0, 0, 0];
      p[ax] = outside(ax, hi);
      for (var j = 0; j < nv; j++) {
        p[v] = o[v] + j * lod + half;
        for (var i = 0; i < nu; i++) {
          p[u] = o[u] + i * lod + half;
          var w = word(p[0], p[1], p[2]);
          // Layouts as the worker expects: x-slab [z*ny+y], y-slab [z*nx+x],
          // z-slab [y*nx+x] — all j*nu+i with (u, v) chosen above.
          if (w < 0) s[j * nu + i] = UNKNOWN_WORD; else { s[j * nu + i] = w; known++; }
        }
      }
      return known ? s : null;
    }
    out.nx0 = slab(0, false); out.px0 = slab(0, true);
    out.ny0 = slab(1, false); out.py0 = slab(1, true);
    out.nz0 = slab(2, false); out.pz0 = slab(2, true);
    return out;
  };

  WorldView.prototype._uploadTex = function (r) {
    var gl = this.gl;
    if (!r.tex) r.tex = gl.createTexture();
    // UNPACK_ALIGNMENT 1, and it is load-bearing for any region that is not
    // 64 wide. The default is 4: GL then expects every row of the upload to be
    // padded to a 4-byte boundary, so a 16-bit region whose width is not even
    // (75 * 2 = 150 bytes) needs 152 bytes per row and the source array is
    // short — the whole texImage3D FAILS with INVALID_OPERATION and the
    // texture keeps whatever it had, which is nothing.
    //
    // Every streamed region is REGION_N = 64 wide (128 bytes, divisible by 4),
    // so this was invisible until the Trees tab handed setLocalRegion a grid
    // sized to a tree. The symptom was a black canvas with a correct mesh,
    // 13,474 quads drawn and no error at draw time — the error had been raised
    // one upload earlier and cleared by the next getError.
    //
    // Explicit unit, for the same class of reason: this used to inherit
    // whatever activeTexture the last render() happened to leave, which was
    // TEXTURE0 by luck rather than by contract.
    gl.activeTexture(gl.TEXTURE0);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
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
    gl.vertexAttribPointer(0, 4, gl.UNSIGNED_SHORT, false, 8, 0);   // (x, y, z, face) as Uint16 — see QuadList
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
    // NOT inflight = 0: the old generation's requests are still out, and each
    // one settles (and is dropped for its gen) when it lands. Zeroing the count
    // here drove it negative as they came back, and the streamer then ran with
    // no limit on concurrent requests. `pending` can go, because _settle only
    // clears an entry that names its own generation.
    this.pending.clear();
    this.queue.length = 0;
    this._failAt.clear();
    if (this._buriedMemo) this._buriedMemo.clear();
    this.stats.errors = 0;
    this.stats.lastError = '';
    this.tree = null;
    this.drawRev++;
  };

  // SOFT REFRESH of a world box (the Dungeons tab: one piece's slider moved,
  // the voxel server already carries the new program). Every region touching
  // the box keeps drawing and is fetched again, coarse and fine alike; the
  // reply replaces it. Regions synthesised as empty / buried inside the box
  // are dropped (the new program may have put something there). A request in
  // flight from before this call lands still stale and is asked again.
  WorldView.prototype.refreshBox = function (min, max) {
    var seq = ++this.refreshSeq, self = this, drop = [];
    this.regions.forEach(function (r) {
      var e = r.nx * r.lod;
      if (r.origin[0] > max[0] || r.origin[0] + e <= min[0] || r.origin[1] > max[1] ||
          r.origin[1] + r.ny * r.lod <= min[1] || r.origin[2] > max[2] || r.origin[2] + r.nz * r.lod <= min[2]) return;
      if (r.local && !r.cells) { drop.push(r); return; }
      if (!r.local) r.staleSeq = seq;
    });
    drop.forEach(function (r) { self._freeRegion(r); self.regions.delete(r.key); });
    if (this._buriedMemo) this._buriedMemo.clear();
    this.drawRev++;
    return seq;
  };
  // How many resident regions are still waiting on a soft refresh.
  // LIVE regions only (in the last update's tree). A refreshBox also marks the
  // CACHE -- regions the camera has left -- and update() refetches a stale
  // region only when it is back in the tree, so counting those held a host's
  // "refreshing" (and the Dungeons tab's readiness) open forever after any
  // edit made away from where the camera had been.
  WorldView.prototype.staleCount = function () {
    var n = 0, f = this._frame;
    this.regions.forEach(function (r) { if (r.lastLive === f && r.staleSeq > (r.fetchedSeq || 0)) n++; });
    return n;
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
    if (!r) return -1;
    if (r.empty) return 0;
    if (!r.cells) return -1;
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
    // Level 0 first, so every hit that CAN be edited is the true cell; past the
    // full-res regions the coarser levels answer, which is what lets the hover
    // readout and a double-click orbit pick work at any zoom. Such a hit carries
    // lod > 1 and edits refuse it (apply() reads level 0 only).
    var streaming = this.opts.streaming !== false, cache = [];
    var top = Math.min(LEVELS, this.view.levels | 0);
    for (var i = 0; i < maxDist * 3 && t <= maxDist; i++) {
      var v = this.cellAtWorld(x, y, z, 0), lod = 1;
      if (v < 0 && streaming) {
        for (var L = 1; L < top && v < 0; L++) { v = this._wordAt(x, y, z, L, cache); lod = 1 << L; }
      }
      if (v > 0) {
        var mat = v & MAT_MASK;
        var cls = this.classes ? this.classes[mat] : 0;
        if (wantFluid || (cls !== CLASS_LIQUID && cls !== CLASS_GAS))
          return {vox: [x, y, z], face: face, word: v, mat: mat,
                  state: (v >> 12) & 15, t: t, lod: lod};
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
    return this.raycast(eye[0], eye[1], eye[2], d[0] / dl, d[1] / dl, d[2] / dl,
                        this.opts.streaming === false ? 400 : 2000, !!this.tool.pickFluids);
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
    var drawSet = this._drawn = this._drawSet();
    this.stats.resident = this.regions.size;
    // The far plane follows the coarsest resident level; a fixed one either
    // clips the horizon or wastes the whole depth range on the near field.
    //
    // BUT THE LEVEL SIZE IS A STREAMING FACT, and an AUTHORED region has
    // nothing to do with REGION_N. The Trees tab runs at levels = 1, which
    // yields 192 voxels of depth — fine for one oak, and less than half of what
    // a 2x2 stand of great oaks needs, so the back of the quad was clipped away
    // and pulling the camera back only deleted more of it. Even a lone great
    // oak (211x169x184) framed past it.
    //
    // So the streaming value is a FLOOR now, not the answer: grow it to reach
    // the farthest corner of everything actually resident. In streaming mode
    // the coarsest shell's corner is ~2.6*ext against a 3*ext floor, so the
    // floor still wins and nothing there changes.
    var farD = this._levelExtent(Math.min(LEVELS, this.view.levels) - 1) * 3;
    var need = 0;
    drawSet.forEach(function (r) {
      var o = r.origin;
      var dx = Math.max(Math.abs(o[0] - eye[0]), Math.abs(o[0] + r.nx * r.lod - eye[0]));
      var dy = Math.max(Math.abs(o[1] - eye[1]), Math.abs(o[1] + r.ny * r.lod - eye[1]));
      var dz = Math.max(Math.abs(o[2] - eye[2]), Math.abs(o[2] + r.nz * r.lod - eye[2]));
      var d = Math.sqrt(dx * dx + dy * dy + dz * dz);
      if (d > need) need = d;
    });
    farD = Math.max(farD, need * 1.05 + 8);
    // Reported because "the back of the scene is missing" is otherwise
    // indistinguishable from "it has not meshed yet": one is this number being
    // too small and the other is a worker still running.
    this.stats.farD = farD;
    this.stats.farNeed = need;
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
    var clip = this.view.clip;
    gl.uniform1i(this.uVox.uClipMode, clip ? (clip.mode | 0) : 0);
    gl.uniform3fv(this.uVox.uClipMin, clip ? clip.min : [0, 0, 0]);
    gl.uniform3fv(this.uVox.uClipMax, clip ? clip.max : [0, 0, 0]);
    gl.uniform1i(this.uVox.uVox, 0);
    gl.uniform1i(this.uVox.uPal, 1);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.palTex);
    gl.activeTexture(gl.TEXTURE0);

    var list = [];
    var self = this;
    drawSet.forEach(function (r) {
      if (!r.op && !r.fl) return;
      if (!self._visible(r, mvp)) return;
      var cx = r.origin[0] + r.nx * r.lod / 2, cy = r.origin[1] + r.ny * r.lod / 2,
          cz = r.origin[2] + r.nz * r.lod / 2;
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

  // What this frame draws. Authored regions: all of them. Streaming: the
  // finest COMPLETE cover of the octree — a node draws its children only when
  // every one of them can draw (meshed, or known empty), and otherwise draws
  // itself if it can. So a region whose children are still on their way stays
  // on screen until they have all landed, and coarse and fine never overlap.
  WorldView.prototype._drawSet = function () {
    var out = [], regions = this.regions;
    if (this.opts.streaming === false || !this.tree) {
      regions.forEach(function (r) { if (!r.empty) out.push(r); });
      return out;
    }
    var memo = new Map();
    function readyR(key) {
      var r = regions.get(key);
      return r && (r.empty || r.meshed) ? r : null;
    }
    function complete(n) {
      var c = memo.get(n.key);
      if (c !== undefined) return c;
      c = !!readyR(n.key) || (!!n.kids && n.kids.every(complete));
      memo.set(n.key, c);
      return c;
    }
    function walk(n) {
      if (n.kids && n.kids.every(complete)) { n.kids.forEach(walk); return; }
      var r = readyR(n.key);
      if (r) { if (!r.empty && !r.buried) out.push(r); return; }
      if (n.kids) n.kids.forEach(walk);
    }
    this.tree.roots.forEach(walk);
    return out;
  };

  function boxVisible(x0, y0, z0, ex, ey, ez, mvp) {
    var all = 63;
    for (var i = 0; i < 8; i++) {
      var x = x0 + (i & 1 ? ex : 0), y = y0 + (i & 2 ? ey : 0), z = z0 + (i & 4 ? ez : 0);
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
  }

  // Frustum cull on the region's world box. Cheap and it matters: at four
  // levels the resident set is ~80 boxes and a fly camera sees a third of them.
  //
  // OUTCODE INTERSECTION, not "is any corner inside": a box larger than the
  // frustum — which every coarse level's box is, up close — has all eight
  // corners outside and would be culled by the naive test, deleting the ground
  // under your feet. A box is out only when all eight corners fail the SAME
  // clip plane.
  WorldView.prototype._visible = function (r, mvp) {
    // PER AXIS, not r.nx three times. A streamed region is a cube so the two
    // agree there, but an AUTHORED one is whatever shape it was handed: a tree
    // is far taller than it is wide, and testing its box as nx^3 culls the
    // crown the moment the camera looks up at it.
    return boxVisible(r.origin[0], r.origin[1], r.origin[2],
                      r.nx * r.lod, r.ny * r.lod, r.nz * r.lod, mvp);
  };

  WorldView.prototype._drawRegion = function (r, slot) {
    var o = r[slot];
    if (!o || !o.count) return;
    var gl = this.gl;
    // The slice plane is a VIEW filter, so it is applied by not drawing rather
    // than by editing: it is how you look inside a hill without carving it.
    if (this.view.sliceEnabled) {
      var ax = this.view.sliceAxis;
      var lo = r.origin[ax], hi = lo + [r.nx, r.ny, r.nz][ax] * r.lod;
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
      (this._drawn || []).forEach(function (r) {
        var ext = r.nx * r.lod;
        var t = r.level / (LEVELS - 1);
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
    // Host overlay. One callback, so a tab can draw its own lines in world
    // space without a second GL program or a second pass — the Trees tab uses
    // it for the branch skeleton, which is where a wrong-looking tree is
    // usually actually wrong (the voxels hide the angle that caused it).
    if (this.onOverlay) {
      var self = this;
      this.onOverlay(function (x0, y0, z0, x1, y1, z1, col) {
        pushLine(a, x0, y0, z0, x1, y1, z1, col || [1, 1, 1, 1]);
      });
      void self;
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
    if (this.view.overlayOnTop) gl.disable(gl.DEPTH_TEST);
    gl.drawArrays(gl.LINES, 0, a.length / 7);
    gl.enable(gl.DEPTH_TEST);
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
    // Fade in where the voxel roots run out: the ring is reach + 1/2 roots
    // from the eye at its nearest edge.
    var eT = this._levelExtent(Math.min(LEVELS, this.view.levels) - 1);
    gl.uniform1f(this.uFar.uFade, (Math.max(0, this.view.reach | 0) + 0.5) * eT * 0.8);
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
      if (!this.editableAt(c[0], c[2])) continue;
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

  // Inside editBounds (or no bounds): the one test apply() and the brush make.
  WorldView.prototype.editableAt = function (x, z) {
    var b = this.editBounds;
    return !b || (x >= b[0] && z >= b[1] && x <= b[2] && z <= b[3]);
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
    if (!r) return;
    if (r.empty) {
      // Painting into open sky: a region known empty has no cells to write.
      // Give it some — it is a real level-0 region from here on.
      r.cells = new Uint16Array(r.nx * r.ny * r.nz);
      r.empty = false; r.meshed = false; r.oct = 0xFF;
      this._uploadTex(r);
    }
    if (!r.cells) return;
    var lx = x - r.origin[0], ly = y - r.origin[1], lz = z - r.origin[2];
    if (lx < 0 || ly < 0 || lz < 0 || lx >= r.nx || ly >= r.ny || lz >= r.nz) return;
    var idx = (lz * r.ny + ly) * r.nx + lx;
    var was = r.cells[idx];
    r.cells[idx] = word & 0xFFFF;
    r.rev = (r.rev || 0) + 1;    // invalidates any mesh already in the worker
    this.editRev++;
    this.drawRev++;
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
      // LMB turns the camera too when no tool is active: "drag to look" is
      // the first thing anyone tries, and it used to do nothing at all.
      var look = self.drag.btn === 2 || (self.drag.btn === 0 && self.tool.name === 'none');
      if (look && e.shiftKey && self.drag.btn === 0) {
        self._pan(dx, dy);
      } else if (look) {
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
    // Double-click (with no tool, or Alt with one): orbit round the cell under
    // the cursor, at any level. The quickest way to say "that, I want to look
    // at that".
    cv.addEventListener('dblclick', function (e) {
      if (self.tool.name !== 'none' && !e.altKey) return;
      var rect = cv.getBoundingClientRect();
      var hit = self.pickAt(e.clientX - rect.left, e.clientY - rect.top);
      if (!hit) return;
      var eye = self.camEye();
      var c = [hit.vox[0] + 0.5, hit.vox[1] + 0.5, hit.vox[2] + 0.5];
      var d = Math.hypot(c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]);
      self.cam.target = c;
      self.cam.dist = Math.max(8, Math.min(d, 400));
      var f = [c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]];
      self.cam.yaw = Math.atan2(-f[0], -f[2]);
      self.cam.pitch = Math.max(-1.5, Math.min(1.5, Math.asin(Math.max(-1, Math.min(1, f[1] / (d || 1))))));
      self.cam.mode = 'orbit';
      self.onCamMode && self.onCamMode('orbit');
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
    var right = screenRight(this.camForward());
    var s = (this.cam.mode === 'orbit' ? this.cam.dist : 1) * 0.0025 + 0.05;
    var d = this.drag;
    var mv = [(-dx * right[0]) * s, dy * s, (-dx * right[2]) * s];
    if (this.cam.mode === 'orbit')
      this.cam.target = [d.target[0] + mv[0], d.target[1] + mv[1], d.target[2] + mv[2]];
    else
      this.cam.pos = [d.pos[0] + mv[0], d.pos[1] + mv[1], d.pos[2] + mv[2]];
  };

  // SCREEN right, flattened: forward x up. The first version used
  // (f.z, 0, -f.x), which is screen LEFT — D strafed left and a middle-drag
  // pan dragged the scene against the mouse sideways but with it vertically.
  function screenRight(f) {
    var rx = -f[2], rz = f[0], rl = Math.hypot(rx, rz) || 1;
    return [rx / rl, 0, rz / rl];
  }

  WorldView.prototype.step = function (dt) {
    var k = this.keys, sp = this.cam.speed * dt * (k['shift'] ? 4 : 1) * (k['control'] ? 0.25 : 1);
    if (!(k['w'] || k['a'] || k['s'] || k['d'] || k['q'] || k['e'])) return;
    var f = this.camForward();
    var right = screenRight(f);
    if (this.cam.mode === 'orbit') {
      // Orbit: WASD slides the point you orbit over the ground, QE lifts it,
      // at a pace that follows the zoom.
      var t = this.cam.target, fl = Math.hypot(f[0], f[2]) || 1;
      var fx = f[0] / fl, fz = f[2] / fl, so = sp * Math.max(1, this.cam.dist / 60);
      if (k['w']) { t[0] += fx * so; t[2] += fz * so; }
      if (k['s']) { t[0] -= fx * so; t[2] -= fz * so; }
      if (k['d']) { t[0] += right[0] * so; t[2] += right[2] * so; }
      if (k['a']) { t[0] -= right[0] * so; t[2] -= right[2] * so; }
      if (k['e']) t[1] += so;
      if (k['q']) t[1] -= so;
      return;
    }
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
      if (!this.editableAt(c[0], c[2])) continue;
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

  // The selection as a MagicaVoxel .vox (P7): what `assets/prefabs/<name>.vox`
  // holds, so a structure built here becomes a stamp the map page's site
  // picker offers. The engine's prefab convention is palette index ==
  // material id (src/sim/voxload.h), so only materials 1..255 fit; the rest
  // are counted in `dropped`, never silently lost. .vox is Z-up and the engine
  // Y-up: voxload.cpp maps a vox cell s to engine (s.x, s.z, -s.y), so engine
  // (x, y, z) goes to vox (x, D-1-z, y). Returns null without a selection or
  // when an axis exceeds the format's 256.
  WorldView.prototype.selectionToVox = function () {
    var b = this.selBox();
    if (!b) return null;
    var W = b[3] - b[0] + 1, H = b[4] - b[1] + 1, D = b[5] - b[2] + 1;
    if (W > 256 || H > 256 || D > 256) return null;
    var xyzi = [], dropped = 0, missing = 0;
    for (var z = 0; z < D; z++) for (var y = 0; y < H; y++) for (var x = 0; x < W; x++) {
      var v = this.cellAtWorld(b[0] + x, b[1] + y, b[2] + z, 0);
      if (v < 0) { missing++; continue; }
      var m = v & MAT_MASK;
      if (!m) continue;
      if (m > 255) { dropped++; continue; }
      xyzi.push(x, D - 1 - z, y, m);
    }
    var n = xyzi.length / 4;
    var sizeBytes = 12 + 12, xyziBytes = 12 + 4 + n * 4;
    var buf = new ArrayBuffer(8 + 12 + sizeBytes + xyziBytes), d = new DataView(buf), p = 0;
    function tag(s) { for (var i = 0; i < 4; i++) d.setUint8(p++, s.charCodeAt(i)); }
    function u32(v) { d.setUint32(p, v >>> 0, true); p += 4; }
    tag('VOX '); u32(150);
    tag('MAIN'); u32(0); u32(sizeBytes + xyziBytes);
    tag('SIZE'); u32(12); u32(0); u32(W); u32(D); u32(H);
    tag('XYZI'); u32(4 + n * 4); u32(0); u32(n);
    for (var i = 0; i < xyzi.length; i++) d.setUint8(p++, xyzi[i]);
    return {buf: buf, voxels: n, dropped: dropped, missing: missing, size: [W, H, D]};
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
    // Cells set since the last save or load, "x,y,z" -> word. The voxel
    // server applies the SAVED layer to every region it returns (P7); these
    // are the only cells a freshly arrived region can be missing.
    this.unsaved = new Map();
    // The level-0 regions ("rx,ry,rz") those cells fall in: the streamer's sky
    // test must not skip a region an unsaved stroke put something into.
    this.unsavedRegions = new Set();
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
    this.unsaved.set(x + ',' + y + ',' + z, word >>> 0);
    this.unsavedRegions.add((x >> 6) + ',' + (y >> 6) + ',' + (z >> 6));
    this.dirty = true;
  };
  // The layer on disk now equals this copy: the server applies it from here on.
  EditLayer.prototype.markSaved = function () {
    this.unsaved.clear();
    this.unsavedRegions.clear();
    this.dirty = false;
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
    this.unsaved.clear();
    this.unsavedRegions.clear();
    this.dirty = true;
  };
  EditLayer.prototype.pushUndo = function (cells, label) {
    this.undoStack.push({cells: cells, label: label});
    if (this.undoStack.length > 200) this.undoStack.shift();
    this.redoStack.length = 0;
  };
  EditLayer.prototype.popUndo = function () { return this.undoStack.pop() || null; };
  EditLayer.prototype.popRedo = function () { return this.redoStack.pop() || null; };

  // Composite the UNSAVED strokes over a freshly arrived region. The saved
  // layer is not composited here any more: the voxel server applies it, with
  // the engine's own resolution of a ground-relative layer (map-overhaul P7),
  // so the page draws what the game spawns into. Only level 0 regions map 1:1
  // to world cells; at coarser levels one sample stands for up to 512 voxels
  // and an unsaved stroke has no honest place to land.
  // Returns whether any stroke landed in it.
  EditLayer.prototype.applyToRegion = function (r) {
    if (r.lod !== 1 || !this.unsaved.size) return false;
    var ox = r.origin[0], oy = r.origin[1], oz = r.origin[2], n = 0;
    this.unsaved.forEach(function (word, k) {
      var p = k.split(',');
      var lx = (+p[0]) - ox, ly = (+p[1]) - oy, lz = (+p[2]) - oz;
      if (lx < 0 || ly < 0 || lz < 0 || lx >= r.nx || ly >= r.ny || lz >= r.nz) return;
      r.cells[((lz * r.ny) + ly) * r.nx + lx] = word & 0xFFFF;
      n++;
    });
    return n > 0;
  };

  // ---- the .svedit binary, v1 (ABSOLUTE cells). Twin of the v1 branch of
  // WorldEdits::LoadBytes / WriteAbsolute in src/sim/worldedit.cpp; keep them
  // together. The page only ever speaks v1: the tuner server converts to and
  // from the ground-relative v2 the engine stores (P7) through --voxserve's
  // EDITS command, because only the engine knows where the ground is.
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
    this.unsaved.clear();
    this.unsavedRegions.clear();
    this.dirty = false;
    return {chunks: chunks, seed: d.getUint32(16, true)};
  };

  global.WorldView = WorldView;
  global.WorldViewEditLayer = EditLayer;
})(window);
