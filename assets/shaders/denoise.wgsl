// denoise.wgsl — the shading-LOD filter: a depth-guided à-trous pass over the
// finished world frame, applied where a fine voxel projects to fewer pixels
// than its lighting needs to be resolved.
//
// WHAT THE NOISE IS, because it decides everything below
// ---------------------------------------------------------
// The mid-distance "noise" (~20 m out to the horizon, worst under a low sun)
// is NOT Monte Carlo variance. Every lighting term in raymarch.wgsl is
// evaluated deterministically, once per pixel, at one point on one voxel face:
// the sun term is the shadow cache, AO is three voxel fetches, the far field's
// shadow is one any-hit march. What the eye reads as noise is GEOMETRY: a
// hillside is a staircase of 10 cm voxels (or 40-160 cm cascade cells), each
// tread and riser lit by its own cube-face normal, each tread contact-shadowed
// by the riser above it, and at 30-200 m every one of those faces is 2-6 px
// across. Point-sampling that staircase once per pixel is aliasing of the
// lighting — a salt-and-pepper of lit and shadowed faces at the resolution
// limit — and it is the same picture every frame.
//
// That rules out the denoisers built for path tracing (OptiX, NRD/REBLUR,
// DLSS-RR): they model the input as a noisy ESTIMATE of a smooth signal and
// accumulate it over time. A deterministic staircase has no variance to
// average away; a temporal accumulator sees the same value every frame and
// changes nothing, and their spatial stage is guided by NORMALS — which would
// preserve exactly the riser-vs-tread contrast that is the problem. (They also
// need a G-buffer, motion vectors and, for two of them, an NVIDIA-only SDK.)
//
// WHAT THIS IS INSTEAD
// --------------------
// The prefilter a texture mip chain gives a rasteriser for free: below a
// certain projected size, the lighting of a surface should be the AVERAGE over
// the pixel's footprint, not one sample of it. This pass builds that average
// in screen space — a 5x5 B3-spline kernel dilated à-trous over up to four
// iterations (Dammertz 2010, the SVGF spatial stage), with the weights driven
// by three things:
//
//   1. STRENGTH from projected voxel size. `fpx / viewZ` is how many pixels a
//      fine voxel covers at this pixel's depth. Above `pxStart` the filter is
//      off (the voxels are big enough to be honest geometry); below `pxFull`
//      it is fully on; between, it ramps. Because the far cascade keeps a
//      constant ~6 px per cell (docs/PLAN_far_field_cascades.md, the
//      resolution law) the ramp is effectively a distance band inside the
//      window and "on" everywhere past it — which is where the complaint is.
//
//   2. A DEPTH edge stop, RELATIVE and per pixel of offset: a tap is trusted
//      if its view depth is within `depthTol * z * distance` of the centre's.
//      Relative, because a grazing ground plane changes depth by ~1% per pixel
//      at 100 m and a fixed tolerance either stops on every tread or on
//      nothing. This is what keeps a hill crest from bleeding into the hill
//      behind it, a mob from bleeding into the ground, and the sky (depth 0)
//      out of everything.
//
//   3. A CHROMA edge stop, and deliberately NOT a luminance one. The noise is
//      luminance (lit vs shadowed faces of one material); a material boundary
//      is mostly hue (sand vs water, grass vs rock). Stopping on luminance
//      would preserve the speckle; stopping on chroma preserves the boundary
//      and lets the speckle average.
//
// Nothing here has a history, so there is no ghosting and nothing to reset —
// a still frame is bit-stable frame to frame. Cost is one image->buffer copy
// per iteration plus one fullscreen pass at RENDER resolution, before TAA or
// the upscale blit (a filter at native resolution would be filtering the
// blit's own duplication).
//
// DETERMINISM: render-only float math on render-only data. Nothing here is
// read by the sim, hashed, or saved (CLAUDE.md rule 1 scopes to sim state).

struct DenoiseParams {
  width  : u32,   // render-resolution frame
  height : u32,
  pitch  : u32,   // row stride of srcColor / srcDepth, in texels
  flags  : u32,   // bit 0: source texels are BGRA (swapchain format)
  step   : i32,   // this iteration's à-trous dilation, in pixels (1, 2, 4, 8)
  iter   : u32,   // which iteration this is (0-based) — informational
  iters  : u32,   // how many there are — informational
  pad0   : u32,
  fpx    : f32,   // renderH / (2 * tanHalfFov): pixels per unit at unit depth
  pxFull : f32,   // projected fine-voxel size (px) at/below which strength = 1
  pxStart: f32,   // ...at/above which strength = 0
  depthTol : f32, // relative depth tolerance per pixel of tap offset
  chromaTol : f32,  // chroma edge-stop width (see chromaOf)
  strength  : f32,  // overall ceiling on the filter's strength, 0..1
  pad1 : f32,
  pad2 : f32,
};

@group(0) @binding(0) var<uniform> P : DenoiseParams;
// The frame this iteration reads: the world pass's colour for iteration 0, and
// the previous iteration's output after that. One buffer, refilled by a copy
// between passes (simulation.cpp EncodeDenoise) — the recorder derives every
// barrier in that chain from its tracked state.
@group(0) @binding(1) var<storage, read> srcColor : array<u32>;
// The reversed-Z depth the world pass wrote (KNEAR / viewZ, viewZ in fine
// voxels). 0 = nothing was written = sky. Captured once, read by every
// iteration.
@group(0) @binding(2) var<storage, read> srcDepth : array<f32>;

struct VSOut {
  @builtin(position) pos : vec4f,
};

// The same fullscreen triangle taa.wgsl and raymarch.wgsl draw.
@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out : VSOut;
  out.pos = vec4f(p[vi], 0.0, 1.0);
  return out;
}

fn unpackTexel(w : u32) -> vec3f {
  let b0 = f32((w      ) & 255u) * (1.0 / 255.0);
  let b1 = f32((w >>  8u) & 255u) * (1.0 / 255.0);
  let b2 = f32((w >> 16u) & 255u) * (1.0 / 255.0);
  if ((P.flags & 1u) != 0u) { return vec3f(b2, b1, b0); }
  return vec3f(b0, b1, b2);
}

// Colour with its luminance divided out. The +0.05 keeps a near-black pixel's
// chroma from being noise: at (0,0,0) every channel reads 0 rather than 0/0.
fn chromaOf(c : vec3f) -> vec3f {
  return c / (c.r + c.g + c.b + 0.05);
}

// View depth in fine voxels, or -1 for sky.
fn viewZAt(idx : u32) -> f32 {
  let d = srcDepth[idx];
  if (d <= 1e-7) { return -1.0; }
  return KNEAR / d;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  let px = vec2<i32>(in.pos.xy);
  let ci = u32(px.y) * P.pitch + u32(px.x);
  let cc = unpackTexel(srcColor[ci]);
  let zc = viewZAt(ci);
  // Sky: nothing was traced here, and nothing here should be averaged with
  // the terrain beside it (the tap loop below also refuses sky taps, so the
  // terrain edge stays where the raymarch put it).
  if (zc < 0.0) { return vec4f(cc, 1.0); }

  // ---- strength from projected voxel size ----------------------------------
  // How many pixels one fine voxel spans at this depth. A far cascade cell is
  // 2^k of these; the ramp is written against the FINE voxel because that is
  // the detail every representation is a downsample of.
  let voxPx = P.fpx / max(zc, 1e-3);
  let s = P.strength * (1.0 - smoothstep(P.pxFull, P.pxStart, voxPx));
  if (s <= 1e-4) { return vec4f(cc, 1.0); }

  let chc = chromaOf(cc);
  // B3-spline taps: the à-trous kernel of Dammertz et al. Separable, so the
  // 2D weight is h[|ox|] * h[|oy|].
  var h = array<f32, 3>(0.375, 0.25, 0.0625);
  let invChroma = 1.0 / max(P.chromaTol, 1e-3);

  var acc = cc * h[0] * h[0];
  var wsum = h[0] * h[0];
  let lim = vec2<i32>(i32(P.width) - 1, i32(P.height) - 1);
  for (var oy = -2; oy <= 2; oy = oy + 1) {
    for (var ox = -2; ox <= 2; ox = ox + 1) {
      if (ox == 0 && oy == 0) { continue; }
      let q = px + vec2<i32>(ox, oy) * P.step;
      if (q.x < 0 || q.y < 0 || q.x > lim.x || q.y > lim.y) { continue; }
      let qi = u32(q.y) * P.pitch + u32(q.x);
      let zq = viewZAt(qi);
      if (zq < 0.0) { continue; }   // never average terrain with sky
      let cq = unpackTexel(srcColor[qi]);
      // Depth stop, relative and scaled by the tap's screen distance: the
      // tolerance is "this many percent of depth per pixel of offset", which is
      // what a continuous surface seen at a grazing angle actually does.
      let dist = length(vec2f(f32(ox), f32(oy))) * f32(P.step);
      let tol = P.depthTol * zc * dist;
      let wz = exp(-abs(zq - zc) / max(tol, 1e-3));
      // Chroma stop: a hue change is a material edge; a brightness change is
      // the thing being averaged.
      let dch = length(chromaOf(cq) - chc) * invChroma;
      let wc = exp(-dch * dch);
      let w = h[abs(ox)] * h[abs(oy)] * wz * wc * s;
      acc = acc + cq * w;
      wsum = wsum + w;
    }
  }
  return vec4f(acc / wsum, 1.0);
}
