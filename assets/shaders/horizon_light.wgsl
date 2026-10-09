// horizon_light.wgsl — THE TERRAIN-SCALE LIGHT MAP (map v2 S1; DESIGN.md
// "The far horizon and the air"). Two compute entries on the shadow layout
// (pass_table.def hz_sky / hz_sun, the per-frame ShadowCache table), each one
// thread per SAMPLE of the far horizon's pyramid (src/sim/horizon.h), writing
// src/sim/horizon.h's light map (kL*), which raymarch.wgsl reads at renderBGL
// 46 (hzSunVis / hzSkyVis).
//
// ---- WHY ---------------------------------------------------------------------
// The renderer's sun shadow is a RAY: the near field's shadow cache, the far
// cascade's patch march, both bounded (render.farShadowReach) because a ray
// per receiver cannot afford kilometres. A 1 km ridge 3 km east of a valley
// shades it all evening, and nothing in the engine could see that; the
// horizon past the cascades had no sun shadow at all. Likewise the openness
// grid's sky probes stop at the window top, so the floor of a 600 m gorge was
// lit by the whole sky. Terrain at this scale is a HEIGHTFIELD, and a
// heightfield has a classical answer: the HORIZON ANGLE. Per sample, march the
// pyramid outward and keep the steepest elevation the terrain makes:
//
//   hzSun  in ONE azimuth, the key light's: the blocker that makes the largest
//          angle, stored as its top Y and its distance, so a receiver at any
//          height y is lit while tan(sun elevation) > (top - y) / distance.
//          A function of the azimuth only -- the sun RISING needs no rebuild,
//          the per-pixel compare takes care of it -- so the CPU re-runs this
//          row only when the azimuth has turned (EncodeShadowResolve).
//   hzSky  in kSkyDirs azimuths: the cosine-weighted visible share of the sky,
//          mean of cos^2 of each horizon angle (the hemisphere above an up
//          face, past a horizon at elevation e, holds cos^2 e of its cosine
//          weight). A function of the map alone: once per upload.
//
// Both start kLightMinDistSamples samples out (~51 m): nearer than that the
// window's own shadow cache and openness grid own the answer, and a sample's
// own footprint would shade it.
//
// ---- THE MARCH ---------------------------------------------------------------
// Distances grow geometrically (step = max(half a sample, 6% of the distance))
// and each step reads the pyramid level whose cell matches the step: the
// cell's MAX, an upper bound of everything in it, so a knife ridge between two
// steps is still seen (one level coarser at most, which blurs its azimuth by
// the cell -- ~1.4 deg -- and never loses it). It stops when nothing the map
// holds (kHMaxY) can rise above the best angle so far.
//
// RENDER-ONLY: derived from the map and the key light, read by the raymarch.

@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(32) var<storage, read> horizon : array<u32>;
@group(0) @binding(33) var<storage, read_write> hzLight : array<u32>;

// horizon.h kH* (the pyramid header). raymarch.wgsl carries the same list.
const HZ_H_VALID : u32 = 0u;
const HZ_H_X0 : u32 = 1u;
const HZ_H_Z0 : u32 = 2u;
const HZ_H_NX : u32 = 3u;
const HZ_H_NZ : u32 = 4u;
const HZ_H_LOG2 : u32 = 5u;
const HZ_H_LEVELS : u32 = 6u;
const HZ_H_SEA_Y : u32 = 7u;
const HZ_H_MAX_Y : u32 = 10u;
const HZ_H_SAMPLES : u32 = 11u;
const HZ_H_MIP : u32 = 16u;
// horizon.h kL* (the light map).
const HZ_L_SKY_VALID : u32 = 0u;
const HZ_L_SUN_VALID : u32 = 1u;
const HZ_L_SUN_DIR_X : u32 = 2u;
const HZ_L_SUN_DIR_Z : u32 = 3u;
const HZ_L_HDR : u32 = 8u;
const HZ_L_DIST_UNIT : f32 = 8.0;
const HZ_L_MIN_DIST : f32 = 2.0;
const HZ_L_SKY_DIRS : u32 = 8u;
// How far each march looks, in METRES: the sun's shadow from a ridge 30 km
// off is a sunset sliver; the sky a horizon 12 km off hides is ~3% of it.
const HZ_SUN_REACH_M : f32 = 30000.0;
const HZ_SKY_REACH_M : f32 = 12000.0;
const HZ_MARCH_CAP : i32 = 160;

fn hzI(w : u32) -> i32 { return bitcast<i32>(horizon[w]); }
fn hzLo(w : u32) -> f32 { return f32(bitcast<i32>(w << 16u) >> 16u); }   // MAX
fn hzHi(w : u32) -> f32 { return f32(bitcast<i32>(w) >> 16u); }          // AVG
fn hzN() -> vec2<i32> { return vec2<i32>(hzI(HZ_H_NX), hzI(HZ_H_NZ)); }
fn hzS() -> f32 { return f32(1u << horizon[HZ_H_LOG2]); }
fn hzSampleW(c : vec2<i32>) -> u32 {
  let n = hzN();
  let q = clamp(c, vec2<i32>(0), n - vec2<i32>(1));
  return horizon[horizon[HZ_H_SAMPLES] + u32(q.y * n.x + q.x)];
}
fn hzCellDims(l : u32) -> vec2<i32> {
  let n = max(hzN() - vec2<i32>(1), vec2<i32>(1));
  return (n + vec2<i32>((1 << l) - 1)) >> vec2<u32>(l);
}
fn hzCellW(l : u32, c : vec2<i32>) -> u32 {
  let d = hzCellDims(l);
  let q = clamp(c, vec2<i32>(0), d - vec2<i32>(1));
  return horizon[horizon[HZ_H_MIP + l] + u32(q.y * d.x + q.x)];
}

// The highest surface (MAX, sea included) the pyramid allows near sample-
// space point p for a step of `stepS` samples: the bilinear of the samples
// under a sample, else the MAX of the level-l cell under p, 2^l <= step.
fn hzTopAt(p : vec2f, stepS : f32) -> f32 {
  if (stepS < 1.0) {
    let i = vec2<i32>(floor(p));
    let f = p - floor(p);
    let a = hzLo(hzSampleW(i));
    let b = hzLo(hzSampleW(i + vec2<i32>(1, 0)));
    let c = hzLo(hzSampleW(i + vec2<i32>(0, 1)));
    let d = hzLo(hzSampleW(i + vec2<i32>(1, 1)));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
  }
  let L = i32(horizon[HZ_H_LEVELS]);
  let l = clamp(i32(floor(log2(stepS))), 0, L - 1);
  let cs = f32(1 << u32(l));
  return hzLo(hzCellW(u32(l), vec2<i32>(floor(p / cs))));
}

// The steepest elevation (as a tangent, fine voxels per fine voxel) the
// terrain makes seen from height h0 at sample (i, j) looking along `dir`
// (unit, sample space), and the distance (samples) it is made at. .y = 0:
// nothing was looked at.
fn hzScan(p0 : vec2f, h0 : f32, dir : vec2f, reachS : f32) -> vec2f {
  let S = hzS();
  let maxY = f32(hzI(HZ_H_MAX_Y));
  var best = -1e9;
  var bestD = 0.0;
  var d = HZ_L_MIN_DIST;
  for (var k = 0; k < HZ_MARCH_CAP; k++) {
    if (d > reachS) { break; }
    // Nothing anywhere rises above the best angle from here on.
    if ((maxY - h0) / (d * S) <= best) { break; }
    let stepS = max(0.5, d * 0.06);
    let t = (hzTopAt(p0 + dir * d, stepS) - h0) / (d * S);
    if (t > best) {
      best = t;
      bestD = d;
    }
    d += stepS;
  }
  return vec2f(best, bestD);
}

// The sample's own ground, the height its angles are measured from: the mean
// ground over its footprint, never under the sea (a sea sample's receiver is
// the water surface).
fn hzGround(idx : u32) -> f32 {
  return max(hzHi(horizon[horizon[HZ_H_SAMPLES] + idx]), f32(hzI(HZ_H_SEA_Y)));
}

@compute @workgroup_size(64)
fn hzSky(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (horizon[HZ_H_VALID] == 0u) { return; }
  let n = hzN();
  let idx = gid.x;
  if (idx >= u32(n.x * n.y)) { return; }
  if (idx == 0u) { hzLight[HZ_L_SKY_VALID] = 1u; }
  let p0 = vec2f(f32(i32(idx) % n.x), f32(i32(idx) / n.x));
  let h0 = hzGround(idx);
  let reachS = HZ_SKY_REACH_M / (VOXEL_METERS * hzS());
  var vis = 0.0;
  for (var k = 0u; k < HZ_L_SKY_DIRS; k++) {
    // Offset half a step so no azimuth runs exactly along a lattice axis.
    let a = (f32(k) + 0.5) * (6.2831853 / f32(HZ_L_SKY_DIRS));
    let t = max(hzScan(p0, h0, vec2f(cos(a), sin(a)), reachS).x, 0.0);
    vis += 1.0 / (1.0 + t * t);
  }
  hzLight[HZ_L_HDR + 2u * idx + 1u] = bitcast<u32>(vis / f32(HZ_L_SKY_DIRS));
}

@compute @workgroup_size(64)
fn hzSun(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (horizon[HZ_H_VALID] == 0u) { return; }
  let n = hzN();
  let idx = gid.x;
  if (idx >= u32(n.x * n.y)) { return; }
  let L = keyLightDirP(R);
  var dir = vec2f(1.0, 0.0);
  let lh = length(L.xz);
  if (lh > 1e-4) { dir = L.xz / lh; }
  if (idx == 0u) {
    hzLight[HZ_L_SUN_VALID] = 1u;
    hzLight[HZ_L_SUN_DIR_X] = bitcast<u32>(dir.x);
    hzLight[HZ_L_SUN_DIR_Z] = bitcast<u32>(dir.y);
  }
  let p0 = vec2f(f32(i32(idx) % n.x), f32(i32(idx) / n.x));
  let h0 = hzGround(idx);
  let S = hzS();
  let sc = hzScan(p0, h0, dir, HZ_SUN_REACH_M / (VOXEL_METERS * S));
  // Packed: the blocker's top Y (i16) | its distance in kLightDistUnit voxels
  // (u16; 0xFFFF = nothing was looked at). top = h0 + tan * distance.
  var w = 0xFFFF0000u;
  if (sc.y > 0.0) {
    let distV = sc.y * S;
    let top = clamp(round(h0 + sc.x * distV), -32768.0, 32767.0);
    let du = clamp(round(distV / HZ_L_DIST_UNIT), 1.0, 65534.0);
    w = (u32(i32(top)) & 0xFFFFu) | (u32(du) << 16u);
  }
  hzLight[HZ_L_HDR + 2u * idx] = w;
}
