// cloud.wgsl — the sky's clouds: a volumetric deck, a cirrus veil, rain
// curtains and the maps the rest of the renderer reads them through.
//
// docs/PLAN_clouds_weather.md is the plan of record, DESIGN.md §9.w the
// invariants, src/sim/weather.h the thing deciding what the sky is doing.
//
// WHAT THIS IS, IN ONE PARAGRAPH. The Nubis recipe (Schneider, "The Real-time
// Volumetric Cloudscapes of Horizon Zero Dawn", 2015/2017), trimmed to what a
// storage-buffer-only engine can afford and bent to what the corpus said about
// this engine's look (plan §1): a tileable Perlin-Worley SHAPE volume eroded by
// a Worley DETAIL volume, both baked once; a per-frame 2-D WEATHER MAP whose
// coverage LOWERS THE DENSITY THRESHOLD (the three-field model, plan §1c)
// rather than scaling the density; a height profile per cloud TYPE (stratus
// sheet .. cumulus .. cumulonimbus); light by Beer's law toward the key light
// with the Wrenninge multiple-scattering octaves and the "powder" darkening,
// integrated with Hillaire's energy-conserving step. All of it marched at LOW
// resolution and accumulated over frames, then upsampled in raymarch.wgsl.
//
// WHY IT IS NOT IN raymarch.wgsl. That fragment shader has no register
// headroom (gotcha-raymarch-register-cliff: 7 extra live scalars took it from
// 168 to 231 registers and slowed every camera). A cloud march is exactly the
// kind of per-ray state that would push it over, so the march lives here, in
// compute, and the raymarcher does ONE bilinear fetch per sky pixel.
//
// DETERMINISM. None of this is read by the sim, hashed or saved (DESIGN.md §9
// "render-only derived data"). Floats everywhere, freely. The clock that
// drives it is the SIM clock (tick-derived, folded on the CPU), so a replay
// still shows the sky the player saw — but a different GPU drawing a slightly
// different cloud is not a desync, because nothing downstream of these buffers
// is part of the world.
//
// COST SCALES WITH THE SKY, NOT WITH THE WORLD (rule 2's render-path
// reading). weather.clouds off, or a sky with nothing in it, records no row at
// all (Cond::Clouds). Rays that leave the deck, reach T ~ 0, or cross a patch
// of weather map with no coverage stop or stride; a ray that hits the ground
// before the deck marches nothing.

// ---- bindings: shadowBGL_ (simulation.cpp), entries 3 and 12..17 ----------
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(12) var<uniform> C : CloudParams;
@group(0) @binding(13) var<storage, read_write> cloudNoise : array<u32>;
@group(0) @binding(14) var<storage, read_write> cloudWeather : array<u32>;
@group(0) @binding(15) var<storage, read_write> cloudMaps : array<u32>;
@group(0) @binding(16) var<storage, read_write> cloudRaw : array<u32>;
@group(0) @binding(17) var<storage, read_write> cloudHist : array<u32>;

// ---- sizes: MUST MATCH world.h kCloud* (check_invariants.py pairs) --------
const CLOUD_SHAPE_N   : u32 = 128u;
const CLOUD_DETAIL_N  : u32 = 32u;
const CLOUD_WEATHER_N : u32 = 512u;
const CLOUD_DETAIL_BASE : u32 = CLOUD_SHAPE_N * CLOUD_SHAPE_N * CLOUD_SHAPE_N;
const CLOUD_NOISE_TEXELS : u32 = CLOUD_DETAIL_BASE +
                                 CLOUD_DETAIL_N * CLOUD_DETAIL_N * CLOUD_DETAIL_N;
const RAW_WORDS  : u32 = 4u;
const HIST_WORDS : u32 = 3u;
// Light-march extinction as a fraction of the view extinction. Not physics —
// the standard art lever (every Nubis descendant has one): the multiple
// scattering octaves alone leave a kilometre-deep cumulus a dark ball, and
// real ones are white because light diffuses through them for kilometres.
const LIGHT_ABSORB : f32 = 0.30;

// The planet the deck wraps round. Earth's radius: what makes the deck sink
// toward the horizon instead of running off to infinity as a flat ceiling.
// Every altitude below is measured with the CURVATURE DROP approximation
// (y + r^2 / 2R) and every intersection with the stable quadratic, because
// f32 cannot hold 6.36e6 m and a 30 m cloud feature in the same number.
const PLANET_R : f32 = 6360000.0;
// Rain curtains are marched no further than this: past it they are a haze the
// aerial perspective already paints.
const RAIN_MAX_M : f32 = 14000.0;
const RAIN_STEPS : u32 = 10u;
const FOUR_PI : f32 = 12.566371;

// ============================================================================
// small math
// ============================================================================
fn remap(v : f32, lo : f32, hi : f32, nlo : f32, nhi : f32) -> f32 {
  return nlo + (v - lo) * (nhi - nlo) / (hi - lo);
}
fn remap01(v : f32, lo : f32, hi : f32) -> f32 {
  return clamp((v - lo) / (hi - lo), 0.0, 1.0);
}
// Henyey-Greenstein, normalised so an isotropic lobe is 1 (not 1/4pi): the
// light terms below are then in "fraction of the incoming light" units.
fn hg(mu : f32, g : f32) -> f32 {
  let g2 = g * g;
  let d = max(1.0 + g2 - 2.0 * g * mu, 1e-4);
  return (1.0 - g2) / (d * sqrt(d));
}
// Two lobes: the strong forward peak that gives a cloud edge its SILVER
// LINING against the sun, and a weak back-scatter so the side facing away from
// the sun is not flat.
fn cloudPhase(mu : f32, g : f32) -> f32 {
  return mix(hg(mu, g), hg(mu, -0.25), 0.28);
}
// Altitude above sea level with the curvature drop about the camera column.
fn altOf(p : vec3f) -> f32 {
  let d = p.xz - C.camM.xz;
  return p.y + dot(d, d) / (2.0 * PLANET_R);
}
fn luma(c : vec3f) -> f32 { return dot(c, vec3f(0.2126, 0.7152, 0.0722)); }

// Distances along a ray that starts at altitude `hc` with vertical direction
// component `dy`, to the sphere of altitude `a` (radius PLANET_R + a). Both
// roots, ascending; x > y means no hit. The quadratic is written in the
// cancellation-free form: c = (hc - a)(2R + hc + a) instead of
// (R + hc)^2 - (R + a)^2, and the small root as c / q.
fn shellHits(hc : f32, dy : f32, a : f32) -> vec2f {
  let b = (PLANET_R + hc) * dy;
  let c = (hc - a) * (2.0 * PLANET_R + hc + a);
  let disc = b * b - c;
  if (disc < 0.0) { return vec2f(1.0, -1.0); }
  let sq = sqrt(disc);
  let q = -b - select(-sq, sq, b >= 0.0);
  var t0 = q;
  var t1 = c / select(q, -1e-9, abs(q) < 1e-9);
  if (t0 > t1) { let tmp = t0; t0 = t1; t1 = tmp; }
  return vec2f(t0, t1);
}

// ============================================================================
// THE NOISE BAKE — entry `noise`, recorded once (Cond::CloudBake)
// ============================================================================
// Tileable 3-D noise at a PERIOD of `freq` cells across the unit cube: every
// lattice coordinate is wrapped mod freq, so the texture tiles with no seam
// and the march can index it with `fract`.
fn cellHash(c : vec3<u32>, salt : u32) -> u32 {
  return pcg(c.x ^ pcg(c.y ^ pcg(c.z ^ salt)));
}
fn worley(p : vec3f, freq : u32, salt : u32) -> f32 {
  let q = p * f32(freq);
  let ci = vec3<i32>(floor(q));
  let f = q - floor(q);
  var md = 1e9;
  let fi = i32(freq);
  for (var dz = -1; dz <= 1; dz++) {
    for (var dy = -1; dy <= 1; dy++) {
      for (var dx = -1; dx <= 1; dx++) {
        let d = vec3<i32>(dx, dy, dz);
        let c = ((ci + d) % fi + fi) % fi;
        let hsh = cellHash(vec3<u32>(c), salt);
        let pt = vec3f(d) + vec3f(f32(hsh & 1023u), f32((hsh >> 10u) & 1023u),
                                  f32((hsh >> 20u) & 1023u)) / 1023.0;
        let dv = pt - f;
        md = min(md, dot(dv, dv));
      }
    }
  }
  return 1.0 - clamp(sqrt(md), 0.0, 1.0);
}
// Worley fBm: the "cauliflower" of a cumulus. Three octaves at 1x/2x/4x.
fn worleyFbm(p : vec3f, freq : u32, salt : u32) -> f32 {
  return worley(p, freq, salt) * 0.625 + worley(p, freq * 2u, salt + 1u) * 0.25 +
         worley(p, freq * 4u, salt + 2u) * 0.125;
}
fn gradAt(c : vec3<u32>, salt : u32) -> vec3f {
  let hsh = cellHash(c, salt);
  let v = vec3f(f32(hsh & 1023u), f32((hsh >> 10u) & 1023u),
                f32((hsh >> 20u) & 1023u)) / 511.5 - 1.0;
  return normalize(v + vec3f(1e-4));
}
fn perlin(p : vec3f, freq : u32, salt : u32) -> f32 {
  let q = p * f32(freq);
  let fl = floor(q);
  let f = q - fl;
  let ci = vec3<i32>(fl);
  let fi = i32(freq);
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  var acc = array<f32, 8>();
  for (var k = 0u; k < 8u; k++) {
    let o = vec3<i32>(i32(k & 1u), i32((k >> 1u) & 1u), i32(k >> 2u));
    let c = vec3<u32>(((ci + o) % fi + fi) % fi);
    acc[k] = dot(gradAt(c, salt), f - vec3f(o));
  }
  let x00 = mix(acc[0], acc[1], u.x);
  let x10 = mix(acc[2], acc[3], u.x);
  let x01 = mix(acc[4], acc[5], u.x);
  let x11 = mix(acc[6], acc[7], u.x);
  return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}
fn perlinFbm01(p : vec3f, freq : u32, salt : u32) -> f32 {
  var s = 0.0;
  var a = 1.0;
  var f = freq;
  var norm = 0.0;
  for (var i = 0u; i < 4u; i++) {
    s += perlin(p, f, salt + i) * a;
    norm += a;
    a *= 0.5;
    f *= 2u;
  }
  return clamp(s / norm * 1.25 + 0.5, 0.0, 1.0);
}

@compute @workgroup_size(64)
fn noise(@builtin(global_invocation_id) gid : vec3<u32>,
         @builtin(num_workgroups) nwg : vec3<u32>) {
  let i = gid.x + gid.y * nwg.x * 64u;
  if (i >= CLOUD_NOISE_TEXELS) { return; }
  if (i < CLOUD_DETAIL_BASE) {
    // SHAPE: R = Perlin-Worley (billowy, connected), G/B/A = Worley fBm at
    // rising frequency, which the march folds into the base shape's low end.
    let n = CLOUD_SHAPE_N;
    let c = vec3<u32>(i % n, (i / n) % n, i / (n * n));
    let p = (vec3f(c) + 0.5) / f32(n);
    let pf = perlinFbm01(p, 4u, 11u);
    let w0 = worleyFbm(p, 4u, 101u);
    let pw = remap(pf, 0.0, 1.0, w0, 1.0);
    let w1 = worleyFbm(p, 8u, 201u);
    let w2 = worleyFbm(p, 16u, 301u);
    cloudNoise[i] = pack4x8unorm(vec4f(clamp(pw, 0.0, 1.0), w0, w1, w2));
  } else {
    // DETAIL: three Worley fBms, finer. Eats the base shape's edges into the
    // wisps (low in the cloud) and billows (high in it).
    let j = i - CLOUD_DETAIL_BASE;
    let n = CLOUD_DETAIL_N;
    let c = vec3<u32>(j % n, (j / n) % n, j / (n * n));
    let p = (vec3f(c) + 0.5) / f32(n);
    cloudNoise[i] = pack4x8unorm(vec4f(worleyFbm(p, 2u, 401u), worleyFbm(p, 4u, 501u),
                                       worleyFbm(p, 8u, 601u), 0.0));
  }
}

// Trilinear fetch from a baked volume at tile coordinate `uvw` (any real;
// wrapped). Manual, because this engine binds no samplers (taa.wgsl's header).
fn sampleVol(uvw : vec3f, n : u32, base : u32) -> vec4f {
  let f = fract(uvw) * f32(n) - 0.5;
  let fl = floor(f);
  let fr = f - fl;
  let m = i32(n) - 1;
  let a = vec3<i32>(fl) & vec3<i32>(m);
  let b = (vec3<i32>(fl) + 1) & vec3<i32>(m);
  let ua = vec3<u32>(a);
  let ub = vec3<u32>(b);
  let s000 = unpack4x8unorm(cloudNoise[base + (ua.z * n + ua.y) * n + ua.x]);
  let s100 = unpack4x8unorm(cloudNoise[base + (ua.z * n + ua.y) * n + ub.x]);
  let s010 = unpack4x8unorm(cloudNoise[base + (ua.z * n + ub.y) * n + ua.x]);
  let s110 = unpack4x8unorm(cloudNoise[base + (ua.z * n + ub.y) * n + ub.x]);
  let s001 = unpack4x8unorm(cloudNoise[base + (ub.z * n + ua.y) * n + ua.x]);
  let s101 = unpack4x8unorm(cloudNoise[base + (ub.z * n + ua.y) * n + ub.x]);
  let s011 = unpack4x8unorm(cloudNoise[base + (ub.z * n + ub.y) * n + ua.x]);
  let s111 = unpack4x8unorm(cloudNoise[base + (ub.z * n + ub.y) * n + ub.x]);
  let x00 = mix(s000, s100, fr.x);
  let x10 = mix(s010, s110, fr.x);
  let x01 = mix(s001, s101, fr.x);
  let x11 = mix(s011, s111, fr.x);
  return mix(mix(x00, x10, fr.y), mix(x01, x11, fr.y), fr.z);
}
fn sampleShape(uvw : vec3f) -> vec4f { return sampleVol(uvw, CLOUD_SHAPE_N, 0u); }
fn sampleDetail(uvw : vec3f) -> vec4f {
  return sampleVol(uvw, CLOUD_DETAIL_N, CLOUD_DETAIL_BASE);
}

// ============================================================================
// THE WEATHER MAP — entry `weather`, when the uniform asks (CLF_WEATHER)
// ============================================================================
// REBUILT ONLY WHEN IT CHANGES (WriteCloudParams, support.cpp). The row is
// recorded every frame, but a frame without CLF_WEATHER returns at the first
// line: the map from the last rebuild is REUSED, because nothing in it moves
// at frame rate. The drift and the camera re-centring are translations of the
// noise domain, which weatherAt applies at lookup (C.spare.yz, metres the
// drift has moved since the rebuild; C.weatherOrigin stays the map's own), so
// reuse costs no accuracy at all; the slow shape terms (weatherEvolve and the
// preset's coverage / type / precip) rebuild it when they have moved by a
// step too small to see. It had been 512^2 texels x six 5-octave fBms every
// frame — ~31M hashes and ~16M sincos for a picture that changes over minutes.
// Four channels per 125 m texel around the camera:
//   R  local coverage (the density threshold the march remaps against)
//   G  cloud type (0 stratus .. 1 cumulonimbus)
//   B  rain: how much of the column under this cloud is precipitating
//   A  base jitter: 0..1, the deck base rises and falls by +-10% of its depth
//
// Built from analytic 2-D gradient noise rather than the baked volume: it is
// 262k texels once a frame, so the cost is nothing, and analytic noise has a
// known, symmetric distribution — which is what lets `coverage` mean
// "roughly this fraction of the sky" instead of a number to be re-tuned every
// time the texture changes.
fn h2(c : vec2<i32>, salt : u32) -> vec2f {
  let hsh = pcg(u32(c.x) ^ pcg(u32(c.y) ^ salt));
  let a = f32(hsh & 65535u) / 65535.0 * 6.2831853;
  return vec2f(cos(a), sin(a));
}
fn perlin2(p : vec2f, salt : u32) -> f32 {
  let fl = floor(p);
  let f = p - fl;
  let c = vec2<i32>(fl);
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  let a = dot(h2(c, salt), f);
  let b = dot(h2(c + vec2<i32>(1, 0), salt), f - vec2f(1.0, 0.0));
  let d = dot(h2(c + vec2<i32>(0, 1), salt), f - vec2f(0.0, 1.0));
  let e = dot(h2(c + vec2<i32>(1, 1), salt), f - vec2f(1.0, 1.0));
  return mix(mix(a, b, u.x), mix(d, e, u.x), u.y);
}
// fBm mapped to ~0..1 with its mean at 0.5. Five octaves, each rotated so the
// lattice never lines up with itself.
fn fbm2(pIn : vec2f, salt : u32) -> f32 {
  var p = pIn;
  var s = 0.0;
  var a = 0.5;
  for (var i = 0u; i < 5u; i++) {
    s += perlin2(p, salt + i * 7u) * a;
    p = vec2f(p.x * 1.6 - p.y * 1.2, p.x * 1.2 + p.y * 1.6) + vec2f(17.3, -9.1);
    a *= 0.5;
  }
  return clamp(s * 1.35 + 0.5, 0.0, 1.0);
}

// CloudParams.flags bit (world.h kClfWeather): rebuild the weather map this
// frame. Declared here, its only reader.
const CLF_WEATHER : u32 = 8u;

@compute @workgroup_size(8, 8)
fn weather(@builtin(global_invocation_id) gid : vec3<u32>) {
  if ((C.flags & CLF_WEATHER) == 0u) { return; }
  if (gid.x >= CLOUD_WEATHER_N || gid.y >= CLOUD_WEATHER_N) { return; }
  let pM = C.weatherOrigin + (vec2f(gid.xy) + 0.5) * C.weatherTexelM;
  let s = pM / TUNE_CLOUD_WEATHER_SCALE_M + C.weatherOff;
  // Domain warp: bends the coverage fronts so the cloud fields are not blobs
  // on a grid. The warp field EVOLVES (weatherEvolve), which is the corpus's
  // "move the texture sample in the 4th dimension ... clouds don't just move
  // with the wind, but also change shape" (plan §1c).
  let ev = C.weatherEvolve;
  let warp = vec2f(fbm2(s * 0.7 + vec2f(ev, 3.1), 31u), fbm2(s * 0.7 + vec2f(-5.2, ev), 37u)) - 0.5;
  let sw = s + warp * 0.9;
  let n = fbm2(sw, 41u);
  let cov = C.coverage;
  // COVERAGE LOWERS THE THRESHOLD. At coverage c, noise above 1 - c is cloud;
  // the soft band makes the edge of a cloud field a thinning, not a line.
  var wc = remap01(n, 1.0 - cov - 0.22, 1.0 - cov + 0.22);
  // A closed deck: above ~80% the fronts close up into a sheet.
  wc = max(wc, remap01(cov, 0.8, 1.0));
  // Type varies about the preset's, and rain pushes it toward cumulonimbus —
  // a raining cell TOWERS, which is what makes the rain legible from afar.
  let nt = fbm2(sw * 1.7 + vec2f(9.7, 1.3), 53u);
  // Rain: an INDEPENDENT field (plan §1i: never two slices of one noise), and
  // only under dense cloud. High raininess makes most cloud rain; low makes
  // only the thickest cores; zero makes none at all.
  let nr = fbm2(sw * 1.3 + vec2f(-4.4, 7.7), 67u);
  let rp = C.precip;
  var rain = 0.0;
  if (rp > 0.0) {
    rain = remap01(nr, 1.0 - rp * 1.1, 1.0 - rp * 1.1 + 0.25) * remap01(wc, 0.45, 0.85);
  }
  let typ = clamp(C.cloudType + (nt - 0.5) * 0.35 + rain * 0.3, 0.0, 1.0);
  let jit = fbm2(s * 3.1 + vec2f(2.2, -8.8), 71u);
  cloudWeather[gid.y * CLOUD_WEATHER_N + gid.x] = pack4x8unorm(vec4f(wc, typ, rain, jit));
  // The camera probe: the texel under the camera writes itself out as f32 for
  // the raymarcher, which has the maps bound but not this map (its rain
  // overlay needs to know whether it is raining HERE, not on average).
  let camT = vec2<u32>(clamp((C.camM.xz - C.weatherOrigin) / C.weatherTexelM,
                             vec2f(0.0), vec2f(f32(CLOUD_WEATHER_N) - 1.0)));
  if (all(gid.xy == camT)) {
    cloudMaps[CLOUD_PROBE_BASE] = bitcast<u32>(wc);
    cloudMaps[CLOUD_PROBE_BASE + 1u] = bitcast<u32>(typ);
    cloudMaps[CLOUD_PROBE_BASE + 2u] = bitcast<u32>(rain);
    cloudMaps[CLOUD_PROBE_BASE + 3u] = bitcast<u32>(jit);
  }
}

// Bilinear weather at absolute metres (x, z). Off the map: the preset's
// global coverage with no rain, so the deck thins into the average rather
// than ending. `C.spare.yz` is the drift since the map was built (zero on a
// rebuild frame): the field at xz now is the built field at xz + that.
fn weatherAt(xz : vec2f) -> vec4f {
  let n = f32(CLOUD_WEATHER_N);
  let f = (xz + C.spare.yz - C.weatherOrigin) / C.weatherTexelM - 0.5;
  if (f.x < 0.0 || f.y < 0.0 || f.x >= n - 1.0 || f.y >= n - 1.0) {
    return vec4f(C.coverage * 0.8, C.cloudType, 0.0, 0.5);
  }
  let i0 = vec2<u32>(floor(f));
  let fr = f - floor(f);
  let b = i0.y * CLOUD_WEATHER_N + i0.x;
  let a00 = unpack4x8unorm(cloudWeather[b]);
  let a10 = unpack4x8unorm(cloudWeather[b + 1u]);
  let a01 = unpack4x8unorm(cloudWeather[b + CLOUD_WEATHER_N]);
  let a11 = unpack4x8unorm(cloudWeather[b + CLOUD_WEATHER_N + 1u]);
  return mix(mix(a00, a10, fr.x), mix(a01, a11, fr.x), fr.y);
}

// ============================================================================
// THE DENSITY FIELD
// ============================================================================
// Base of the deck at this weather sample (the jitter channel lifts and drops
// it so the underside is not a ruled line) and the height fraction in it.
fn deckBase(wm : vec4f) -> f32 {
  return C.baseM + (wm.w - 0.5) * 2.0 * cloudBaseJitterM(C.thicknessM);
}
// Vertical profile per cloud TYPE, 0..1 over the deck. Stratus is a thin low
// sheet, cumulus a rounded mid-height body, cumulonimbus the full depth with a
// flat top. Blended piecewise so type 0.25 is "stratocumulus", not a cross-fade
// of two drawings.
fn typeProfile(h : f32, typ : f32) -> f32 {
  let st = remap01(h, 0.0, 0.06) * (1.0 - remap01(h, 0.12, 0.30));
  let cu = remap01(h, 0.0, 0.10) * (1.0 - remap01(h, 0.30, 0.70));
  let cb = remap01(h, 0.0, 0.06) * (1.0 - remap01(h, 0.78, 1.00));
  let a = clamp(typ * 2.0, 0.0, 1.0);
  let b = clamp(typ * 2.0 - 1.0, 0.0, 1.0);
  return mix(mix(st, cu, a), cb, b);
}

// Density (0.. ~1, before extinction) at absolute metres `p`. `cheap` skips
// the detail erosion — the light march and the maps use it, where a sample is
// integrated over hundreds of metres and the detail would only be noise.
fn cloudDensity(p : vec3f, wm : vec4f, cheap : bool) -> f32 {
  let base = deckBase(wm);
  let h = (altOf(p) - base) / C.thicknessM;
  if (h <= 0.0 || h >= 1.0 || wm.x <= 0.001) { return 0.0; }
  let prof = typeProfile(h, wm.y);
  if (prof <= 0.0) { return 0.0; }
  // Towers lean downwind as they grow: the shape volume is sampled shifted by
  // height along the wind, which is the skew real cumulus have.
  let lean = vec3f(C.windX, 0.0, C.windZ) * h * C.thicknessM * 0.35;
  let s = sampleShape((p + lean) / TUNE_CLOUD_SHAPE_SCALE_M + C.shapeOff);
  let wf = s.g * 0.625 + s.b * 0.25 + s.a * 0.125;
  var d = remap(s.r, wf - 1.0, 1.0, 0.0, 1.0) * prof;
  // Anvil: cumulonimbus tops spread — coverage is raised near the top by type.
  let anvil = clamp(wm.y * 2.0 - 1.2, 0.0, 1.0);
  let cov = pow(clamp(wm.x, 0.0, 1.0), mix(1.0, 0.45, anvil * remap01(h, 0.65, 0.85)));
  // COVERAGE AS THRESHOLD (plan §1c): what survives is the part of the shape
  // above 1 - coverage, rescaled, then weighted by coverage so a thin field
  // is thin as well as small.
  d = remap(d, 1.0 - cov, 1.0, 0.0, 1.0) * cov;
  if (d <= 0.0) { return 0.0; }
  if (!cheap) {
    let dt = sampleDetail((p + lean * 1.5) / TUNE_CLOUD_DETAIL_SCALE_M + C.detailOff);
    let df = dt.r * 0.625 + dt.g * 0.25 + dt.b * 0.125;
    // Wispy (inverted) at the base, billowy at the top.
    let md = mix(df, 1.0 - df, clamp(h * 6.0, 0.0, 1.0));
    d = remap(d, md * TUNE_CLOUD_EROSION, 1.0, 0.0, 1.0);
  }
  return clamp(d, 0.0, 1.0) * C.density;
}

// ============================================================================
// LIGHT
// ============================================================================
struct CloudLight {
  dir : vec3f,
  col : vec3f,
  ambTop : vec3f,
  ambBot : vec3f,
};

// The light the clouds are lit by. NOT simply keyLightDirP: a deck 1.5 km up
// still sees the sun for minutes after it has set on the ground, and those
// minutes — cloud undersides going orange then red over a darkening land — are
// the most recognisable sky there is (plan §3, "nearly free, and large").
// So the sun lights the clouds until it is a few degrees BELOW the horizon,
// through the long reddening air mass it has there; only then does the moon
// take over.
fn cloudLight() -> CloudLight {
  var L : CloudLight;
  let sy = R.sunDir.y;
  let eclipse = pow(clamp(R.solarEclipse, 0.0, 1.0), TUNE_ECLIPSE_CURVE) * TUNE_ECLIPSE_DARKNESS;
  if (sy > -0.07) {
    L.dir = R.sunDir;
    let vis = smoothstep(-0.07, 0.02, sy) * (1.0 - eclipse);
    L.col = sunTransmittance(airMass(max(sy, -0.02) + 0.015)) * TUNE_SUN_COLOR *
            TUNE_SUN_INTENSITY * vis;
  } else {
    L.dir = keyLightDirP(R);
    L.col = keyLightColorP(R);
  }
  // Ambient from the SAME hemisphere the terrain is lit by (ambientAtP), so a
  // cloud is never brighter or bluer in its shadow than the ground under it.
  // It already carries the overcast greying and the lightning flash.
  L.ambTop = ambientAtP(vec3f(0.0, 1.0, 0.0), R) * TUNE_CLOUD_AMBIENT;
  L.ambBot = ambientAtP(vec3f(0.0, -1.0, 0.0), R) * TUNE_CLOUD_AMBIENT * 0.6;
  return L;
}

// Optical depth toward the light from `p`, `n` samples on a cone with growing
// steps (Nubis): the near samples resolve a tower's self-shadow, the far one
// catches the neighbour tower that shadows it.
fn lightOD(p : vec3f, L : vec3f, n : u32, seed : f32) -> f32 {
  var od = 0.0;
  var t = 0.0;
  let step0 = C.thicknessM * 0.06;
  for (var i = 0u; i < n; i++) {
    let ds = step0 * (1.0 + f32(i) * 0.9);
    t += ds;
    // Cone jitter: a small hashed lateral offset, growing with distance.
    let a = seed * 6.2831853 + f32(i) * 2.39996;
    let off = vec3f(cos(a), sin(a * 1.7) * 0.5, sin(a)) * t * 0.12;
    let q = p + L * t + off;
    let wm = weatherAt(q.xz);
    od += cloudDensity(q, wm, i >= 2u) * ds;
  }
  // One long sample for what is behind the cone: the shadow of a whole tower
  // standing sunward of this one.
  let far = p + L * (t + C.thicknessM * 0.6);
  od += cloudDensity(far, weatherAt(far.xz), true) * C.thicknessM * 0.4;
  return od;
}

// The Wrenninge multiple-scattering octaves: each octave lets the light
// penetrate further (extinction x a), weighs less (x b) and scatters more
// isotropically (g x c). Three octaves is the whole of the "bright, soft
// interior" that single scattering cannot produce — without it a cumulus is a
// dark ball with a bright rim.
fn sunScatter(od : f32, mu : f32) -> f32 {
  let k = TUNE_CLOUD_MULTI_SCATTER;
  var a = 1.0;
  var b = 1.0;
  var c = 1.0;
  var s = 0.0;
  for (var o = 0u; o < 3u; o++) {
    s += b * exp(-od * TUNE_CLOUD_EXTINCTION * LIGHT_ABSORB * a) *
         cloudPhase(mu, TUNE_CLOUD_PHASE_G * c);
    a *= k;
    b *= k;
    c *= k;
  }
  return s;
}

// ============================================================================
// THE MARCH
// ============================================================================
struct March {
  c : vec3f,     // in-scattered light, already hazed
  t : f32,       // transmittance of the whole column, haze included
  depth : f32,   // transmittance-weighted distance to the cloud, metres
  td : f32,      // DIRECT transmittance: the cloud alone, no haze
};

// Aerial perspective for something `d` metres away: blend it toward the sky
// behind it without knowing that sky. With final = sky*T + C, hazing by h is
// T' = T + (1 - T) h, C' = C (1 - h) — so the composite needs no sky colour
// here, and a far cloud dissolves exactly into whatever the dome is doing.
//
// EXCEPT under a closed deck. There the air between the eye and a far cloud
// is lit by the cloud itself, not by a blue dome — so hazing toward "the sky
// behind" paints a blue band along an overcast horizon that has no blue sky
// anywhere in it. The overcast share of the haze therefore goes to the
// deck's own ambient grey (added as light, T untouched) and only the rest
// thins toward the sky.
fn haze(m : March, d : f32) -> March {
  var o = m;
  let hz = 1.0 - exp(-d / (TUNE_CLOUD_HAZE_M / (1.0 + C.mist * 0.5)));
  let oc = clamp(C.overcast, 0.0, 1.0);
  o.t = m.t + (1.0 - m.t) * hz * (1.0 - oc);
  o.c = m.c * (1.0 - hz) + (1.0 - m.t) * hz * oc * hazeGrey();
  return o;
}
// The airlight under a closed deck: its own diffuse light, a little dimmed by
// how dark the deck is. One function, because the haze and the horizon fill
// must converge to the same colour.
//
// Not the bare ambient: a deck's underside is lit by the sun diffusing down
// through it as well as by the sky, which is what makes an overcast
// horizon a warm pale grey rather than the blue-grey of the ambient alone.
fn hazeGrey() -> vec3f {
  let Lt = cloudLight();
  return (Lt.ambTop * TUNE_CLOUD_AMBIENT * 0.72 +
          Lt.col * TUNE_CLOUD_SUN_GAIN * 0.13) * (1.0 - C.darkness * 0.5);
}

// Three Gaussian bands at once: exp(-((x - c) / w)^2) per channel. Squared by
// multiplication, NOT pow(v, 2.0): pow is undefined for a negative base in
// WGSL/SPIR-V (GLSL.std.450 Pow), and x - c is negative on one side of every
// band — a driver that returns NaN there puts NaN into the cloud history,
// which the temporal resolve then keeps.
fn gaussBands(x : f32, c : vec3f, w : vec3f) -> vec3f {
  let g = (vec3f(x) - c) / w;
  return exp(-(g * g));
}

// The cirrus veil: one analytic sample on the high shell, no march. Thin,
// streaked along the wind, lit mostly by forward scattering — and the 22°
// HALO when the sun shines through it (plan §3, a ring and three smoothsteps).
fn cirrusLayer(rd : vec3f, Lt : CloudLight, mu : f32) -> March {
  var o = March(vec3f(0.0), 1.0, 1e7, 1.0);
  if (C.cirrus <= 0.001) { return o; }
  let hc = C.camM.y;
  let hit = shellHits(hc, rd.y, C.cirrusAltM);
  var t = hit.y;
  if (hc > C.cirrusAltM) { t = hit.x; }
  if (hit.x > hit.y || t <= 0.0) { return o; }
  let p = C.camM + rd * t;
  // Stretch along the wind: rotate into (downwind, crosswind) and squash.
  let w = normalize(vec2f(C.windX, C.windZ) + vec2f(1e-4, 0.0));
  let q = vec2f(dot(p.xz, w) * 0.28, dot(p.xz, vec2f(-w.y, w.x))) /
          TUNE_CLOUD_CIRRUS_SCALE_M + C.cirrusOff;
  let a = sampleDetail(vec3f(q.x, C.cirrusEvolve, q.y));
  let b = sampleShape(vec3f(q.x * 0.21 + 0.3, C.cirrusEvolve * 0.5, q.y * 0.21));
  var den = remap01(a.r * 0.55 + a.g * 0.3 + b.g * 0.35, 0.55, 0.95) *
            remap01(b.r, 0.35, 0.75);
  den *= C.cirrus;
  let alpha = clamp(den * 0.85, 0.0, 0.92);
  if (alpha <= 0.0005) { return o; }
  let fwd = mix(hg(mu, 0.65), 1.0, 0.45);
  var col = Lt.col * fwd * 0.9 + Lt.ambTop * 1.1;
  // THE 22-DEGREE HALO. Hexagonal ice plates refract at a minimum deviation of
  // ~22°, and dispersion puts red on the inside edge. Three rings a hair apart
  // are that spectrum.
  let ang = acos(clamp(mu, -1.0, 1.0));
  let ring = gaussBands(ang, vec3f(0.3815, 0.3850, 0.3890),
                        vec3f(0.010, 0.011, 0.013));
  col += Lt.col * ring * 0.9;
  o.c = col * alpha;
  o.t = 1.0 - alpha;
  o.td = o.t;
  o.depth = t;
  return haze(o, t);
}

// The rain curtains: the column under a raining cloud, from the base down,
// marched coarsely out to RAIN_MAX_M. Grey, faintly lit, and — the reason it
// is marched at all rather than faked — where the sun shines into it at 42°
// from the anti-solar point, a RAINBOW, in exactly the part of the sky that is
// actually raining (plan §3: "highest look-per-instruction item").
fn rainLayer(rd : vec3f, Lt : CloudLight, mu : f32, tMax : f32, jit : f32) -> March {
  var o = March(vec3f(0.0), 1.0, 1e7, 1.0);
  if (C.precip <= 0.001) { return o; }
  let tEnd = min(tMax, RAIN_MAX_M);
  if (tEnd <= 0.0) { return o; }
  let ds = tEnd / f32(RAIN_STEPS);
  var T = 1.0;
  var acc = vec3f(0.0);
  var dAcc = 0.0;
  // Rainbow: primary bow at 42° (138° from the sun), secondary at 51° with
  // the colours reversed and a dark Alexander band between. Needs the sun up.
  let ang = acos(clamp(-mu, -1.0, 1.0));
  let bowP = gaussBands(ang, vec3f(0.7400, 0.7260, 0.7090),
                        vec3f(0.012, 0.012, 0.013));
  let bowS = gaussBands(ang, vec3f(0.8900, 0.9080, 0.9290),
                        vec3f(0.016, 0.016, 0.017)) * 0.35;
  // A real bow is faint against the curtain it stands in: ~a third of the
  // curtain's own brightness at its peak, less toward the sides.
  let bow = (bowP + bowS) * 0.15 * TUNE_CLOUD_RAINBOW * smoothstep(0.0, 0.06, R.sunDir.y) *
            (1.0 - C.precipType);
  for (var i = 0u; i < RAIN_STEPS; i++) {
    let t = (f32(i) + jit) * ds;
    let p = C.camM + rd * t;
    let wm = weatherAt(p.xz);
    if (wm.z <= 0.01) { continue; }
    let base = deckBase(wm);
    let alt = altOf(p);
    if (alt > base) { continue; }
    // Curtains, not a fog: streaks in the fall direction that shimmer as the
    // shafts drift, from the detail volume stretched vertically.
    let st = sampleDetail(vec3f(p.x / 900.0 + C.detailOff.x, p.y / 7000.0 - C.weatherEvolve,
                                p.z / 900.0 + C.detailOff.z));
    let fall = 1.0 - remap01(alt, base - 150.0, base);
    let dens = wm.z * (0.45 + 0.9 * st.r) * fall * TUNE_CLOUD_RAIN_DENSITY * 0.00055;
    if (dens <= 0.0) { continue; }
    let tr = exp(-dens * ds);
    // Snow is brighter and whiter than rain; rain is mostly the grey of the
    // cloud it falls from.
    let lit = Lt.ambTop * mix(0.75, 1.35, C.precipType) +
              Lt.col * (0.10 + 0.35 * hg(mu, 0.5) * 0.1) + Lt.col * bow * 2.5;
    acc += T * lit * (1.0 - tr);
    dAcc += t * T * (1.0 - tr);
    T *= tr;
  }
  o.c = acc;
  o.t = T;
  o.td = T;
  o.depth = select(1e7, dAcc / max(1.0 - T, 1e-4), T < 0.999);
  return o;
}

// Where along `rd` the deck is: [t0, t1] in metres, empty if t0 >= t1. Also
// reports the distance to the ground, so the rain march stops there.
fn deckSpan(rd : vec3f, groundT : ptr<function, f32>) -> vec2f {
  let hc = C.camM.y;
  let lo = C.baseM - cloudBaseJitterM(C.thicknessM);
  let hi = C.baseM + 1.1 * C.thicknessM;
  // The ground sphere: sea level. Terrain here is metres, the deck kilometres,
  // so sea level is a faithful stand-in for "the ray went into the land".
  let g = shellHits(hc, rd.y, min(0.0, hc - 1.0));
  *groundT = select(1e9, g.x, g.x <= g.y && rd.y < 0.0 && g.x > 0.0);
  var t0 = 0.0;
  var t1 = 0.0;
  let sLo = shellHits(hc, rd.y, lo);
  let sHi = shellHits(hc, rd.y, hi);
  if (hc < lo) {
    // Below the deck: enter through the base shell's far root, leave through
    // the top shell's far root.
    if (sLo.x > sLo.y) { return vec2f(0.0, -1.0); }
    t0 = sLo.y;
    t1 = sHi.y;
  } else if (hc <= hi) {
    t0 = 0.0;
    t1 = sHi.y;
    // Going down: leave through the base.
    if (rd.y < 0.0 && sLo.x <= sLo.y && sLo.x > 0.0) { t1 = sLo.x; }
  } else {
    if (sHi.x > sHi.y || sHi.y <= 0.0) { return vec2f(0.0, -1.0); }
    t0 = max(sHi.x, 0.0);
    t1 = sHi.y;
    if (sLo.x <= sLo.y && sLo.x > 0.0) { t1 = sLo.x; }
  }
  if (t0 >= *groundT) { return vec2f(0.0, -1.0); }
  t1 = min(t1, *groundT);
  t1 = min(t1, t0 + TUNE_CLOUD_MAX_DIST_M);
  return vec2f(t0, t1);
}

// The whole column along `rd`: rain in front, then the deck, then the cirrus
// behind it. `quality` 0 = the env map (few steps, no detail), 1 = the screen.
fn marchSky(rd : vec3f, jit : f32, quality : u32) -> March {
  let Lt = cloudLight();
  let mu = dot(rd, Lt.dir);
  var groundT = 1e9;
  let span = deckSpan(rd, &groundT);

  // ---- the deck ----
  var deck = March(vec3f(0.0), 1.0, 1e7, 1.0);
  if (span.x < span.y && C.coverage > 0.001) {
    let full = quality == 1u;
    let nSteps = select(max(TUNE_CLOUD_STEPS / 3u, 12u), TUNE_CLOUD_STEPS, full);
    let seg = span.y - span.x;
    // Steps no finer than 25 m (a vertical ray through a thin sheet would
    // otherwise spend its whole budget on 5 m steps) and no coarser than the
    // budget allows.
    let ds0 = max(seg / f32(nSteps), 25.0);
    var t = span.x + ds0 * jit;
    var T = 1.0;
    var acc = vec3f(0.0);
    var dAcc = 0.0;
    let lightN = select(3u, TUNE_CLOUD_LIGHT_STEPS, full);
    let pw = TUNE_CLOUD_POWDER * (0.5 - 0.5 * mu);
    // Lightning: where the stroke is, and how bright.
    let flashCol = vec3f(0.75, 0.82, 1.0) * C.flashAmp * 60.0;
    var it = 0u;
    loop {
      if (t >= span.y || T < 0.01 || it >= nSteps * 2u) { break; }
      it++;
      let p = C.camM + rd * t;
      let wm = weatherAt(p.xz);
      // Empty-space stride: no coverage here means no cloud for a while.
      if (wm.x < 0.01) { t += ds0 * 2.0; continue; }
      let d = cloudDensity(p, wm, !full);
      if (d > 0.0) {
        let h = clamp((altOf(p) - deckBase(wm)) / C.thicknessM, 0.0, 1.0);
        let od = lightOD(p, Lt.dir, lightN, fract(jit * 7.31 + f32(it) * 0.618));
        var sun = sunScatter(od, mu);
        // Powder: in-scattering needs something to scatter FROM, so the thin
        // leading edge of a cloud facing you (away from the sun) is darker
        // than its interior. Strongest looking away from the sun.
        let powder = 1.0 - exp(-d * TUNE_CLOUD_EXTINCTION * 120.0);
        sun *= mix(1.0, powder, pw);
        // Ambient: sky from above, ground bounce from below, and the SLATE
        // underside of a rain cloud — `darkness` eats the ambient at the base
        // of a thick deck, where no skylight arrives through a kilometre of it.
        // Skylight occlusion: one cheap sample a sixth of the deck above. The
        // height gradient alone lights every point at one height the same,
        // which turns the underside of a big cumulus into a flat grey plate;
        // this is what gives it lumps — darker under the thick towers,
        // brighter under the thin edges.
        let up = p + vec3f(0.0, C.thicknessM * 0.16, 0.0);
        let dUp = cloudDensity(up, wm, true);
        let skyOcc = exp(-dUp * TUNE_CLOUD_EXTINCTION * C.thicknessM * 0.16 * 0.35);
        let amb = mix(Lt.ambBot, Lt.ambTop * mix(0.55, 1.0, skyOcc), h) *
                  (1.0 - C.darkness * 0.8 * (1.0 - h)) * exp(-C.darkness * (1.0 - h) * 1.5);
        let sigT = d * TUNE_CLOUD_EXTINCTION * (1.0 + C.darkness * 1.5 * (1.0 - h));
        let sigS = d * TUNE_CLOUD_EXTINCTION;
        var S = sigS * (Lt.col * sun * TUNE_CLOUD_SUN_GAIN + amb);
        if (C.flashAmp > 0.0) {
          let fd = length(p - C.flash);
          S += sigS * flashCol * exp(-fd / 900.0);
        }
        let tr = exp(-sigT * ds0);
        let Sint = (S - S * tr) / max(sigT, 1e-7);
        acc += T * Sint;
        dAcc += t * T * (1.0 - tr);
        T *= tr;
      }
      t += ds0;
    }
    deck.c = acc;
    deck.t = T;
    deck.td = T;
    deck.depth = select(span.x, dAcc / max(1.0 - T, 1e-4), T < 0.999);
    deck = haze(deck, deck.depth);
  }

  // ---- THE DECK PAST THE MARCH: the horizon ----
  // Under a deck at 1 km the base meets the horizon ~110 km out — past the
  // march's reach and past the curvature of the ground under the eye. A ray
  // that runs out of march (or into the ground) before it reaches the deck
  // is still looking at cloud there, only cloud so far off it is ALL haze.
  // Without this the sky just above the horizon is bare atmosphere while the
  // fogged ground below it is not, and the two meet in a bright band that no
  // real sky has. The fill is the deck's mean cover, drawn in the colour its
  // own haze would give it, and it fades in only for rays within a few
  // degrees of the horizon — steeper rays reach the real deck. It does NOT
  // fade out BELOW the horizon: a sky pixel down there is "past the far
  // field", and the fog on the ground next to it converges to this same
  // function through the env map, so the two have to agree or the horizon
  // grows a seam of bare sky between them.
  if (C.coverage > 0.001 && C.camM.y < C.baseM) {
    let reach = select(span.y, TUNE_CLOUD_MAX_DIST_M, span.x >= span.y);
    let elev = (C.baseM - C.camM.y) / max(reach, 1.0);
    let far = smoothstep(elev + 0.05, elev - 0.01, rd.y);
    // Exactly the limit haze() converges to for an opaque deck: the overcast
    // share is the deck's grey, the rest is the sky showing through — so the
    // marched deck just above the cut and the fill just below it meet.
    let oc = clamp(C.overcast, 0.0, 1.0);
    let cov = clamp(C.coverage * 1.15, 0.0, 1.0) * far * deck.t * oc;
    if (cov > 0.0) {
      deck.c += hazeGrey() * cov;
      deck.t *= 1.0 - cov;
    }
  }

  // ---- behind it: the cirrus veil (only through what the deck lets past) ----
  let ci = cirrusLayer(rd, Lt, mu);
  var o = March(deck.c + deck.t * ci.c, deck.t * ci.t,
                select(ci.depth, deck.depth, deck.t < 0.999), deck.td * ci.td);

  // ---- in front of it: rain ----
  if (C.precip > 0.001 && C.camM.y < C.baseM + C.thicknessM) {
    let rn = rainLayer(rd, Lt, mu, min(groundT, select(RAIN_MAX_M, span.x, span.x > 0.0 && span.x < span.y)), jit);
    o = March(rn.c + rn.t * o.c, rn.t * o.t,
              select(o.depth, rn.depth, rn.t < 0.98 && o.t > 0.9), rn.td * o.td);
  }
  return o;
}

// Interleaved gradient noise, rotated per frame by the golden ratio: the
// march start offset. Blue-ish in space, low-discrepancy in time, which is
// what the temporal resolve needs to converge instead of shimmering.
fn ign(px : vec2f, frame : u32) -> f32 {
  let f = fract(52.9829189 * fract(dot(px, vec2f(0.06711056, 0.00583715))));
  return fract(f + f32(frame & 1023u) * 0.61803398875);
}

// The primary ray through low-res pixel `pix` (pixel-centre + this frame's
// sub-pixel jitter), with the SAME convention raymarch.wgsl's fs() and
// taa.wgsl use: ndc.y = +1 at the top row.
fn rayOf(pix : vec2f) -> vec3f {
  let ndc = vec2f(pix.x / f32(C.lowW) * 2.0 - 1.0, 1.0 - pix.y / f32(C.lowH) * 2.0);
  return normalize(R.camFwd + R.camRight * (ndc.x * R.tanHalfFov * R.aspect) +
                   R.camUp * (ndc.y * R.tanHalfFov));
}

// ============================================================================
// ENTRY: march — one thread per low-res pixel, raw result for this frame
// ============================================================================
@compute @workgroup_size(8, 8)
fn march(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= C.lowW || gid.y >= C.lowH) { return; }
  let pix = vec2f(gid.xy) + 0.5 + C.jitter;
  let rd = rayOf(pix);
  let m = marchSky(rd, ign(vec2f(gid.xy), C.frame), 1u);
  let i = (gid.y * C.lowW + gid.x) * RAW_WORDS;
  cloudRaw[i] = pack2x16float(m.c.xy);
  cloudRaw[i + 1u] = pack2x16float(vec2f(m.c.z, m.t));
  cloudRaw[i + 2u] = pack2x16float(vec2f(m.td, 0.0));
  cloudRaw[i + 3u] = bitcast<u32>(m.depth);
}

// A texel as (r, g, b, T) plus Td, the direct transmittance, in `td`.
struct Tex5 { v : vec4f, td : f32 };
fn loadRaw(p : vec2<i32>) -> Tex5 {
  let q = clamp(p, vec2<i32>(0), vec2<i32>(i32(C.lowW) - 1, i32(C.lowH) - 1));
  let i = (u32(q.y) * C.lowW + u32(q.x)) * RAW_WORDS;
  let a = unpack2x16float(cloudRaw[i]);
  let b = unpack2x16float(cloudRaw[i + 1u]);
  let d = unpack2x16float(cloudRaw[i + 2u]);
  return Tex5(vec4f(a.x, a.y, b.x, b.y), d.x);
}
fn loadHistAt(base : u32, p : vec2<u32>) -> Tex5 {
  let i = base + (p.y * C.lowW + p.x) * HIST_WORDS;
  let a = unpack2x16float(cloudHist[i]);
  let b = unpack2x16float(cloudHist[i + 1u]);
  let d = unpack2x16float(cloudHist[i + 2u]);
  return Tex5(vec4f(a.x, a.y, b.x, b.y), d.x);
}

// ============================================================================
// ENTRY: resolve — temporal accumulation at low resolution
// ============================================================================
// Reproject this pixel's cloud point into last frame's view, fetch the history
// there bilinearly, clamp it to the neighbourhood of this frame's raw samples
// (so a cloud that moved does not leave a ghost), and blend. After ~1/alpha
// frames the jittered, dithered march has converged to a clean image — the
// "low res plus temporal reconstruction" every cloud renderer in the corpus
// describes (plan §1g). TAA then finishes the job at full resolution.
@compute @workgroup_size(8, 8)
fn resolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= C.lowW || gid.y >= C.lowH) { return; }
  let p = vec2<i32>(gid.xy);
  let cur = loadRaw(p);
  var mn = cur.v;
  var mx = cur.v;
  var mnd = cur.td;
  var mxd = cur.td;
  for (var k = 0u; k < 9u; k++) {
    if (k == 4u) { continue; }
    let o = vec2<i32>(i32(k % 3u) - 1, i32(k / 3u) - 1);
    let v = loadRaw(p + o);
    mn = min(mn, v.v);
    mx = max(mx, v.v);
    mnd = min(mnd, v.td);
    mxd = max(mxd, v.td);
  }
  var outv = cur.v;
  var outd = cur.td;
  if ((C.flags & CLF_HIST_VALID) != 0u) {
    let ri = (gid.y * C.lowW + gid.x) * RAW_WORDS;
    let depth = min(bitcast<f32>(cloudRaw[ri + 3u]), 2e5);
    let rd = rayOf(vec2f(gid.xy) + 0.5 + C.jitter);
    // The cloud point relative to LAST frame's eye.
    let v = rd * depth + C.eyeDeltaM;
    let z = dot(v, C.prevFwd);
    if (z > 1.0) {
      let nx = dot(v, C.prevRight) / (z * C.prevTanHalfFov * C.prevAspect);
      let ny = dot(v, C.prevUp) / (z * C.prevTanHalfFov);
      let q = vec2f((nx + 1.0) * 0.5 * f32(C.lowW), (1.0 - ny) * 0.5 * f32(C.lowH)) - 0.5 - C.jitter;
      if (q.x >= 0.0 && q.y >= 0.0 && q.x <= f32(C.lowW) - 1.001 && q.y <= f32(C.lowH) - 1.001) {
        let i0 = vec2<u32>(floor(q));
        let fr = q - floor(q);
        let h00 = loadHistAt(C.histPrev, i0);
        let h10 = loadHistAt(C.histPrev, i0 + vec2<u32>(1u, 0u));
        let h01 = loadHistAt(C.histPrev, i0 + vec2<u32>(0u, 1u));
        let h11 = loadHistAt(C.histPrev, i0 + vec2<u32>(1u, 1u));
        var hist = mix(mix(h00.v, h10.v, fr.x), mix(h01.v, h11.v, fr.x), fr.y);
        var histD = mix(mix(h00.td, h10.td, fr.x), mix(h01.td, h11.td, fr.x), fr.y);
        // Neighbourhood clamp, slightly widened: the raw samples are dithered,
        // so their min/max is noisier than the truth, and a tight clamp would
        // throw the history away on every frame.
        let pad = (mx - mn) * 0.25 + vec4f(0.002);
        hist = clamp(hist, mn - pad, mx + pad);
        histD = clamp(histD, mnd - (mxd - mnd) * 0.25 - 0.002, mxd + (mxd - mnd) * 0.25 + 0.002);
        // A running mean for the first frames after a reset (C.spare.x is the
        // history's age), the steady exponential after that.
        let alpha = max(TUNE_CLOUD_TEMPORAL, 1.0 / (C.spare.x + 1.0));
        outv = mix(hist, cur.v, alpha);
        outd = mix(histD, cur.td, alpha);
      }
    }
  }
  let o = C.histCur + (gid.y * C.lowW + gid.x) * HIST_WORDS;
  cloudHist[o] = pack2x16float(outv.xy);
  cloudHist[o + 1u] = pack2x16float(vec2f(outv.z, clamp(outv.w, 0.0, 1.0)));
  cloudHist[o + 2u] = pack2x16float(vec2f(clamp(outd, 0.0, 1.0), 0.0));
}

// ============================================================================
// ENTRY: shadow — the cloud shadow map (read by common.wgsl cloudSunAtM)
// ============================================================================
// One texel per 40 m of the deck-base plane around where the camera's own
// light ray crosses it. March from the plane up through the deck along the
// key light; store the transmittance. Cheap density only: a shadow is
// integrated over the whole deck and read at voxel scale from kilometres
// below, and the detail erosion would only be shimmer.
@compute @workgroup_size(8, 8)
fn shadow(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= CLOUD_SHADOW_N || gid.y >= CLOUD_SHADOW_N) { return; }
  let L = keyLightDirP(R);
  var T = 1.0;
  if (L.y > 0.03 && C.coverage > 0.001) {
    let q = C.shadowOrigin + (vec2f(gid.xy) + 0.5) * C.shadowTexelM;
    let p0 = vec3f(q.x, C.shadowPlaneM, q.y);
    let len = (C.thicknessM * 1.2) / L.y;
    let n = 12u;
    let ds = len / f32(n);
    var od = 0.0;
    for (var i = 0u; i < n; i++) {
      let p = p0 + L * ((f32(i) + 0.5) * ds);
      od += cloudDensity(p, weatherAt(p.xz), true) * ds;
    }
    T = exp(-od * TUNE_CLOUD_EXTINCTION);
    // Multiple scattering: a cloud shadow is never black — light leaks round
    // and through, more for thin cloud. The floor also keeps an overcast from
    // reading as night.
    T = max(T, 0.18 * (1.0 - C.darkness * 0.6));
  }
  T = mix(1.0, T, TUNE_CLOUD_SHADOW_STRENGTH);
  cloudMaps[gid.y * CLOUD_SHADOW_N + gid.x] = bitcast<u32>(T);
}

// ============================================================================
// ENTRY: env — the octahedral cloud env map (read by cloudEnvAt)
// ============================================================================
@compute @workgroup_size(8, 8)
fn env(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= CLOUD_ENV_N || gid.y >= CLOUD_ENV_N) { return; }
  let rd = cloudOctDecode((vec2f(gid.xy) + 0.5) / f32(CLOUD_ENV_N));
  // Fixed mid-offset, not a dither: this map is not accumulated over frames,
  // and a per-frame dither on a 64^2 map would flicker every fogged hillside.
  let m = marchSky(rd, 0.5, 0u);
  let i = CLOUD_ENV_BASE + (gid.y * CLOUD_ENV_N + gid.x) * 2u;
  cloudMaps[i] = pack2x16float(m.c.xy);
  cloudMaps[i + 1u] = pack2x16float(vec2f(m.c.z, m.t));
  if (all(gid.xy == vec2<u32>(0u))) { writeRainWindProbe(); }
}

// THE WIND THE RAIN LEANS ALONG: windAt averaged over a 20 m disc round the
// camera, written once a frame into probe words 4..6 (m/s) for raymarch.wgsl's
// rainOverlay. The point sample it replaced swung the whole sheet of streaks
// with every gust front — the gust bands have a ~5 m wavelength, so one point
// is the flutter of one blade of grass, not the weather. Seventeen samples,
// the centre plus rings of eight at 10 and 20 m, equally weighted: each stands
// for about the same area of the disc (25pi, 25pi and ~22pi m^2), so this is
// an area average rather than one biased toward the eye. The mean wind, its
// altitude ramp at the eye's height and any wind primitive (fan, spell gust,
// tornado) the disc overlaps all survive the average; the flutter does not.
// One thread of one pass: 17 field evaluations a frame, not per pixel.
const RAIN_WIND_RADIUS_M : f32 = 20.0;
fn writeRainWindProbe() {
  let c = R.camPos;
  let rv = RAIN_WIND_RADIUS_M / VOXEL_METERS;
  var acc = windAt(c, R.time, &R);
  for (var k = 0u; k < 8u; k++) {
    let a = f32(k) * 0.78539816;
    let d = vec3f(cos(a), 0.0, sin(a));
    acc += windAt(c + d * (rv * 0.5), R.time, &R);
    acc += windAt(c + d * rv, R.time, &R);
  }
  var w = acc * (VOXEL_METERS / 17.0);
  // SMOOTHED IN TIME too (word 7 = the R.time of the last write). The overlay
  // shears its whole lattice along this wind, so a flake h metres from the
  // eye moves by dWind * h / fallSpeed when it changes — with snow's 1.1 m/s
  // fall a wind shift swung the entire field round the eye like one rigid
  // sheet. Easing the lean over a few seconds (longer for snow) keeps that
  // swing slower than the flakes' own motion. A first frame, a clock that
  // went backwards or a clock that did NOT move snaps to the new value: a
  // frozen clock (a paused game, every --shot frame at kShotTime) would
  // otherwise blend by exactly 0 forever and keep the first frame's wind --
  // the storm shots leaned the way the calm ones did. Frozen time means the
  // wind is not moving either, so the snap is what the ease converges to.
  let tPrev = bitcast<f32>(cloudMaps[CLOUD_PROBE_BASE + 7u]);
  let dt = R.time - tPrev;
  if (dt > 0.0 && dt < 1.0) {
    let prev = vec3f(bitcast<f32>(cloudMaps[CLOUD_PROBE_BASE + 4u]),
                     bitcast<f32>(cloudMaps[CLOUD_PROBE_BASE + 5u]),
                     bitcast<f32>(cloudMaps[CLOUD_PROBE_BASE + 6u]));
    let tau = mix(1.5, 4.0, clamp(C.precipType, 0.0, 1.0));
    let blend = 1.0 - exp(-dt / tau);
    if (all(prev == prev)) { w = mix(prev, w, blend); }   // NaN guard
  }
  cloudMaps[CLOUD_PROBE_BASE + 4u] = bitcast<u32>(w.x);
  cloudMaps[CLOUD_PROBE_BASE + 5u] = bitcast<u32>(w.y);
  cloudMaps[CLOUD_PROBE_BASE + 6u] = bitcast<u32>(w.z);
  cloudMaps[CLOUD_PROBE_BASE + 7u] = bitcast<u32>(R.time);
}
