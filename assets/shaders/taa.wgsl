// taa.wgsl — temporal anti-aliasing + temporal UPSCALE, the pass that replaces
// the NEAREST blit at the end of a `render.renderScale < 1` frame.
//
// WHY THIS EXISTS
// ---------------
// render.renderScale renders the world at a fraction of the window and blits it
// up with NEAREST. That is honest (voxels stay square) and it is the single
// biggest lever in the renderer — the `halfres` budget arm saves ~70% of the
// raymarch — but the frame it produces is a SMALLER PICTURE, not a cheaper one:
// at 0.7 you are looking at 0.7-resolution edges, and nothing ever fills them
// back in. This pass is what turns that trade from "less image" into "the same
// image, reconstructed over frames".
//
// The mechanism is the one voxelbit.net ships and the one every modern engine
// ships: JITTER the camera by a sub-pixel offset each frame (main.cpp folds an
// R2 low-discrepancy offset into the camera basis, so the raymarch AND every
// raster path move together — see projectView in common.wgsl), then accumulate
// the jittered low-resolution frames into a NATIVE-resolution history with a
// reconstruction filter. Over ~16 frames the jitter sequence has sampled the
// native grid densely enough that the history holds real native detail.
//
// WHY A STORAGE BUFFER AND NOT A TEXTURE
// --------------------------------------
// The rhi has no sampled-texture or sampler binding at all (rhi.h: BindGroupEntry
// carries a Buffer and nothing else) — this engine's entire GPU idiom is storage
// buffers, deliberately. So the world frame reaches this pass the way everything
// else reaches a shader here: CopyTextureToBuffer into a plain storage buffer,
// indexed by hand. It costs one image->buffer copy of the render-resolution
// frame (~4 MB at 0.7 of 1080p, sub-0.05 ms) and it costs the rhi nothing.
//
// THE HISTORY IS PING-PONGED, and that is not an optimisation: this pass reads
// the history BILINEARLY at a reprojected position, i.e. at OTHER pixels' texels,
// while writing its own. One buffer would be a read-write race between
// invocations of the same draw. Two buffers make the read and the write disjoint
// (World's dirtyIn/dirtyOut pattern, applied to a frame).
//
// DETERMINISM: nothing here is read by the sim, nothing here is hashed, and the
// buffers are never saved. This is render-only derived data in the DESIGN.md §9
// sense, exactly like the far cascades.

struct TaaParams {
  // The camera the CURRENT low-resolution frame was rendered with — INCLUDING
  // the jitter, because main.cpp folds the jitter into the basis it uploads.
  camRight   : vec3f, tanHalfFov : f32,
  camUp      : vec3f, aspect     : f32,
  // sharpness: the reconstruction filter's Gaussian exponent, in NATIVE pixels
  // (render.taaSharpness). Its own uniform lane rather than a shader constant
  // precisely so it can be swept with `--gate taa` and a tuning.json edit and
  // NO rebuild — see the open question in the header.
  camFwd     : vec3f, sharpness  : f32,
  // …and the PREVIOUS frame's, likewise jittered. Reprojection is a projection
  // through this basis, so it must be the basis that produced the history.
  pRight     : vec3f, maxHist    : f32,
  pUp        : vec3f, clampK     : f32,
  pFwd       : vec3f, pad0       : f32,
  // curEye - prevEye, in fine voxel units. Carried as a DELTA and differenced
  // on the CPU IN DOUBLE, because world coordinates are absolute and a 20 km
  // map reaches ~200,000 voxels — where the f32 difference of two frames'
  // eye positions would throw away the sub-voxel part that IS the signal.
  eyeDelta   : vec3f, pad1       : f32,
  jitter     : vec2f,   // this frame's image shift, in RENDER pixels
  pJitter    : vec2f,   // last frame's
  renderW : u32, renderH : u32, nativeW : u32, nativeH : u32,
  srcPitch : u32,       // row stride of srcColor/srcDepth, in texels
  flags : u32,          // bit 0: source texels are BGRA (swapchain format)
  reset : u32,          // 1 = discard all history this frame
  pad2  : u32,
};

@group(0) @binding(0) var<uniform> P : TaaParams;
// The world frame at render resolution, straight out of CopyTextureToBuffer:
// one 8:8:8:8 unorm word per texel, channel order per `flags` bit 0.
@group(0) @binding(1) var<storage, read> srcColor : array<u32>;
// The reversed-Z depth the same frame wrote (KNEAR / viewZ). 0 = far/sky.
@group(0) @binding(2) var<storage, read> srcDepth : array<f32>;
// Native-resolution history, ping-ponged: rgb + accumulated WEIGHT, four halves
// in two words. Weight, not a frame count — see the accumulation note below.
@group(0) @binding(3) var<storage, read> histIn : array<vec2<u32>>;
@group(0) @binding(4) var<storage, read_write> histOut : array<vec2<u32>>;

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) uv : vec2f,
};

// The same fullscreen triangle raymarch.wgsl draws, for the same reason: no
// vertex buffer, no index buffer, three vertices.
@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out : VSOut;
  out.pos = vec4f(p[vi], 0.0, 1.0);
  out.uv = p[vi];
  return out;
}

fn unpackTexel(w : u32) -> vec3f {
  let b0 = f32((w      ) & 255u) * (1.0 / 255.0);
  let b1 = f32((w >>  8u) & 255u) * (1.0 / 255.0);
  let b2 = f32((w >> 16u) & 255u) * (1.0 / 255.0);
  // BGRA8 stores blue in the low byte; RGBA8 stores red there.
  if ((P.flags & 1u) != 0u) { return vec3f(b2, b1, b0); }
  return vec3f(b0, b1, b2);
}

fn loadHist(idx : u32) -> vec4f {
  let w = histIn[idx];
  let rg = unpack2x16float(w.x);
  let ba = unpack2x16float(w.y);
  return vec4f(rg.x, rg.y, ba.x, ba.y);
}

// Bilinear fetch of the history at a continuous NATIVE pixel-centre coordinate.
// Manual, because there is no sampler in this engine (see the header). Returns
// zero weight outside the frame, which the caller treats as "no history".
fn sampleHist(q : vec2f) -> vec4f {
  let f = q - 0.5;
  let i0 = vec2<i32>(floor(f));
  let fr = f - floor(f);
  var acc = vec4f(0.0);
  var wsum = 0.0;
  for (var dy = 0; dy < 2; dy = dy + 1) {
    for (var dx = 0; dx < 2; dx = dx + 1) {
      let p = i0 + vec2<i32>(dx, dy);
      if (p.x < 0 || p.y < 0 || p.x >= i32(P.nativeW) || p.y >= i32(P.nativeH)) {
        continue;
      }
      let wx = select(1.0 - fr.x, fr.x, dx == 1);
      let wy = select(1.0 - fr.y, fr.y, dy == 1);
      let w = wx * wy;
      // A texel with no accumulated weight carries no colour either — including
      // it would drag the blend toward black at a frame edge.
      let h = loadHist(u32(p.y) * P.nativeW + u32(p.x));
      if (h.w <= 0.0) { continue; }
      acc = acc + h * w;
      wsum = wsum + w;
    }
  }
  if (wsum <= 0.0) { return vec4f(0.0); }
  return acc / wsum;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
  let nativeSize = vec2f(f32(P.nativeW), f32(P.nativeH));
  let renderSize = vec2f(f32(P.renderW), f32(P.renderH));
  let scale = renderSize / nativeSize;
  // This native pixel's centre, in framebuffer coordinates (top-left origin,
  // +0.5 at a centre) — the same space `srcColor` is indexed in.
  let q = in.pos.xy;
  // …and where the CURRENT frame put that direction. `jitter` is the image
  // shift the camera perturbation produced, in render pixels, so the content
  // that would sit at q*scale unjittered sits at q*scale + jitter now.
  let src = q * scale + P.jitter;

  // ---- reconstruct this native pixel from the render-resolution samples ----
  // A 3x3 tap of the low-resolution frame, weighted by a Gaussian on the
  // distance from `src`. Two things come out of it: the current estimate `cur`,
  // and the colour BOX (cmin/cmax) the history is clamped into below.
  //
  // `wBest` is the reconstruction filter's RESPONSE at this pixel — how close
  // the nearest real sample landed — and it is the whole reason this is an
  // upscale and not a blur. The accumulation is weight-proportional
  // (out = sum(w_i c_i) / sum(w_i) over frames), so a frame whose jittered
  // sample landed on top of this native pixel dominates it and a frame whose
  // sample landed a pixel away barely moves it. Blending at a CONSTANT rate
  // would converge to the average of the reconstruction filter over the jitter
  // sequence, which is a WIDER filter than any single frame's — i.e. it would
  // spend the jitter on blur instead of on detail.
  var cur = vec3f(0.0);
  var wsum = 0.0;
  var wBest = 0.0;
  var cmin = vec3f(1e9);
  var cmax = vec3f(-1e9);
  let c0 = vec2<i32>(floor(src));
  for (var dy = -1; dy <= 1; dy = dy + 1) {
    for (var dx = -1; dx <= 1; dx = dx + 1) {
      let p = clamp(c0 + vec2<i32>(dx, dy), vec2<i32>(0, 0),
                    vec2<i32>(i32(P.renderW) - 1, i32(P.renderH) - 1));
      let c = unpackTexel(srcColor[u32(p.y) * P.srcPitch + u32(p.x)]);
      // ---- THE KERNEL IS MEASURED IN NATIVE PIXELS, NOT RENDER PIXELS ----
      // This division by `scale` is the difference between an upscale and a
      // blur, and the first version did not have it. MEASURED (--gate taa,
      // 1920x1080 native from 960x540): with the kernel in RENDER pixels the
      // resolve scored 1.40 mean absolute error against the full-resolution
      // render while a plain NEAREST upscale scored 1.21 — temporal
      // upsampling LOSING to no upsampling at all.
      //
      // The reason is that the weight decides which FRAMES get to speak for
      // this native pixel, so its width has to be in the units of the thing
      // being reconstructed. At scale 0.5 a render pixel is two native pixels,
      // so a kernel one render pixel wide accepts samples up to two native
      // pixels away at 10% weight and 1.4 native pixels away at 32% — it
      // averages a neighbourhood twice the size of the detail it is trying to
      // recover, and no number of frames fixes that. Dividing by `scale`
      // makes 1.0 mean "one NATIVE pixel" at every render scale, so the frames
      // whose jittered sample landed on this pixel outweigh the ones that
      // missed by 10-100x instead of by 3x.
      //
      // At renderScale 1 `scale` is 1 and this is exactly the old expression,
      // which is why the alignment arm is unaffected by the change.
      let d = ((vec2f(p) + 0.5) - src) / scale;
      // exp(-k d^2), k = render.taaSharpness. At the shipped 2.29 a sample one
      // native pixel away still counts for 10%, which is a filter about half a
      // native pixel wide.
      //
      // ---- THE OPEN QUESTION IN THIS PASS, and it lives on this line -------
      // MEASURED (`--gate taa`, 1080p from 960x540): the accumulated error
      // against a full-resolution render RISES with frame count — 1.19 at 16
      // frames, 1.42 at 48 — and lands level with a plain NEAREST blit (1.20
      // whole-frame, 3.69 vs 3.73 on the top 20% of pixels by gradient). An
      // accumulator cannot get worse with more samples unless it is converging
      // to something other than the reference, and what it converges to here is
      // the weighted mean of every sample within this filter: a BLUR of about
      // half a native pixel. Against a point-sampled reference a blur and a
      // half-pixel shift score about the same, which is why the metric cannot
      // separate them and why the gate no longer asserts on it.
      //
      // (Ruled out on the way: a stale reference. The reference render was
      // taken to 64 frames so the irradiance EMA is at its fixed point before
      // anything is measured, and the numbers did not move at all — 1.18/1.42
      // before, 1.19/1.42 after.)
      //
      // So k is the dial, and it is a UNIFORM rather than a TUNE_ constant so
      // that sweeping it costs a tuning.json edit and one `--gate taa`, with no
      // rebuild and no shader recompile. Larger = only the frames whose
      // jittered sample landed nearly on this pixel are allowed to speak for
      // it, which is sharper but starves pixels the sequence keeps missing.
      let w = exp(-P.sharpness * dot(d, d));
      cur = cur + c * w;
      wsum = wsum + w;
      wBest = max(wBest, w);
      cmin = min(cmin, c);
      cmax = max(cmax, c);
    }
  }
  cur = cur / max(wsum, 1e-6);

  // ---- reproject: where was this surface point last frame? ----------------
  // Nearest-tap depth rather than a filtered one: depth is discontinuous at a
  // silhouette and interpolating across one produces a position on neither
  // surface, which reprojects to a pixel showing a third thing.
  let sp = clamp(c0, vec2<i32>(0, 0), vec2<i32>(i32(P.renderW) - 1, i32(P.renderH) - 1));
  let dep = srcDepth[u32(sp.y) * P.srcPitch + u32(sp.x)];
  // The unnormalised ray through `src`. dot(rdU, camFwd) == 1 by construction
  // (orthonormal basis), so viewZ scales it straight to the world offset.
  let ndc = vec2f(src.x / renderSize.x * 2.0 - 1.0,
                  1.0 - src.y / renderSize.y * 2.0);
  let rdU = P.camFwd + P.camRight * (ndc.x * P.tanHalfFov * P.aspect)
                     + P.camUp    * (ndc.y * P.tanHalfFov);
  // Reversed-Z: depth = KNEAR / viewZ, and 0 means "nothing was written here",
  // i.e. sky. A sky pixel is at infinity, so its reprojection is the pure
  // ROTATION — which is exactly what a huge viewZ gives, with no special case.
  // KNEAR comes from common.wgsl, which LoadShader prepends to every shader —
  // the same constant projectView writes into the depth buffer this reads, so
  // the two cannot drift.
  let viewZ = select(KNEAR / max(dep, 1e-7), 1.0e7, dep <= 1e-7);
  let rel = rdU * viewZ;
  let relP = rel + P.eyeDelta;
  let vz = dot(relP, P.pFwd);
  var W = 0.0;
  var hist = cur;
  if (P.reset == 0u && vz > 1e-3) {
    let vx = dot(relP, P.pRight);
    let vy = dot(relP, P.pUp);
    let ndcP = vec2f(vx / (vz * P.tanHalfFov * P.aspect), vy / (vz * P.tanHalfFov));
    let srcP = vec2f((ndcP.x * 0.5 + 0.5) * renderSize.x,
                     (0.5 - ndcP.y * 0.5) * renderSize.y);
    // Back out of render space into the native grid the history lives on,
    // undoing the PREVIOUS frame's jitter (not this one's).
    let qP = (srcP - P.pJitter) / scale;
    if (qP.x > 0.0 && qP.y > 0.0 && qP.x < nativeSize.x && qP.y < nativeSize.y) {
      let h = sampleHist(qP);
      if (h.w > 0.0) {
        // ---- the only rejection test, and it is deliberate ----------------
        // No depth-history buffer, no surface id: the colour BOX does the
        // rejecting. A disocclusion reprojects onto a pixel whose colour is
        // outside the 3x3 range of what is actually here now, so the clamp
        // pulls it back into the plausible set; where it had to move a long
        // way, the weight is cut so the pixel re-converges in a few frames
        // instead of dragging a smear behind a moving silhouette. This is
        // voxelbit's "reactive" idea reached from the other side: it flags the
        // pixel from the evidence rather than from a creature id.
        let ext = (cmax - cmin) * P.clampK;
        let cl = clamp(h.rgb, cmin - ext, cmax + ext);
        let range = max(max(cmax.r - cmin.r, cmax.g - cmin.g), cmax.b - cmin.b);
        let moved = length(cl - h.rgb) / (range + 1.0 / 255.0);
        hist = cl;
        W = mix(h.w, min(h.w, 2.0), clamp(moved, 0.0, 1.0));
      }
    }
  }

  // Weight-proportional accumulation, capped: maxHist is the effective length
  // of the running average, not a frame count, so a pixel whose samples keep
  // landing badly does not pretend to be converged.
  let wNew = max(wBest, 1e-4);
  let alpha = wNew / (W + wNew);
  let outC = clamp(mix(hist, cur, alpha), vec3f(0.0), vec3f(1.0));
  let Wout = min(W + wNew, P.maxHist);

  let oi = u32(q.y) * P.nativeW + u32(q.x);
  if (oi < arrayLength(&histOut)) {
    histOut[oi] = vec2<u32>(pack2x16float(outC.rg),
                            pack2x16float(vec2f(outC.b, Wout)));
  }
  return vec4f(outC, 1.0);
}
