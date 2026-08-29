// raymarch.wgsl — fullscreen two-level DDA over the voxel grid.
// Chunk-level skip via per-chunk occupancy counts, voxel DDA inside non-empty
// chunks. The renderer reads the same device-local buffers the sim writes —
// zero upload cost (DESIGN.md §9). Rendering is allowed to use floats; only
// sim state must stay integer.

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
// far-field cascades (render-only LOD — DESIGN.md §9)
@group(0) @binding(4) var<storage, read> farVox : array<u32>;
@group(0) @binding(5) var<storage, read> farOcc : array<u32>;
@group(0) @binding(6) var<uniform> F : FarParams;
// static micro-detail (render-only — see MicroBrick in common.wgsl). Bound HERE
// and nowhere else: these are render data, and putting them on a sim shader's
// bind group would make the renderer a sim input (CLAUDE.md rule 1).
@group(0) @binding(7) var<storage, read> microBricks : array<MicroBrick>;
@group(0) @binding(8) var<storage, read> microPool : array<u32>;
// Software page table (docs/PLAN_page_table.md §5.2a). The renderer performs
// 17 raw voxel reads; under paging every one of them indexes the POOL with a
// SLOT-derived address and would sample the wrong chunk wherever the target is
// a sentinel. The world would render as garbage while hashing perfectly — the
// render path is outside the hashed domain, so no determinism gate could catch
// it. Read-only here and no pageFaults binding: the renderer must never write
// the world, so it gets translation without the write path.
@group(0) @binding(9) var<storage, read> pageTable : array<u32>;
// MPM fluid surface (see the MPM FLUID SURFACE block below). The renderer
// samples the solver's LAST substep's node grid directly — mass for the
// isosurface, velocity for foam, species masses for colour. Both are the sim's
// own device-local buffers, read here exactly like `voxels`: zero upload, and
// the arrow still only points sim -> render. Declared plain (non-atomic) over
// the same memory sim_fluid.wgsl accumulates atomically — the tick's writes
// are ordered before this submit, so there is no concurrent access to race.
@group(0) @binding(10) var<storage, read> fluidBlockMapR : array<u32>;
@group(0) @binding(11) var<storage, read> fluidGridR : array<i32>;
@group(0) @binding(12) var<storage, read> dirtyViz : array<u32>;

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) uv : vec2f,
};

@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var out : VSOut;
  out.pos = vec4f(p[vi], 0.0, 1.0);
  out.uv = p[vi];
  return out;
}

// unpackColor / paletteColor live in common.wgsl: the 3-variant palette is a
// property of the Material struct, which is declared there, and all three
// render paths (raymarch, cubes, micro bodies) decode it identically.

// ============================================================================
// SKY — day/night, sun, moon, stars, aurora
// ============================================================================
// Replaces the old two-colour lerp + pow() sun blob. That version had one
// fundamental problem: the sun disc was a `pow(dot, 800)` lobe ADDED to a flat
// gradient, which is a smear, not a light source. A real sun reads as lit
// because of three things the smear had none of:
//
//   1. A HARD-EDGED disc. The sun subtends ~0.53 degrees and its limb is
//      sharp — the eye reads the crisp edge as "object", and a soft power lobe
//      as "glow". We build the disc from the true angular radius with a
//      pixel-width antialiased edge, then add limb darkening across it.
//   2. Scattering that RESPONDS to it. The sky near the sun brightens because
//      air scatters forward (Mie); the sky opposite stays blue because
//      molecular scattering is wavelength-dependent (Rayleigh). Driving both
//      off the same sun vector is what couples sun and sky into one image.
//   3. An atmosphere that THICKENS toward the horizon. Air mass along the view
//      ray grows as the ray flattens, which is why the horizon is pale and the
//      zenith is deep — and why a low sun turns red (its own light crosses far
//      more air, and blue is scattered out of it first).
//
// All of this is render-only float math (CLAUDE.md rule 1 allows floats here);
// the SIM's notion of day phase is the separate integer path in common.wgsl.

// Angular radius of the solar/lunar disc, radians. Both are ~0.5 deg from
// Earth, which is the coincidence that makes eclipses work; keeping them equal
// is both physically right and visually useful.
const SUN_ANGULAR_RADIUS : f32 = 0.00465;

// RAYLEIGH_RGB / airMass / sunTransmittance moved to common.wgsl: the raster
// body paths (debris.wgsl, microbody.wgsl) share them so detached voxels
// light exactly like the terrain they came off. airMass is the single most
// important function in the sky: it is why the horizon is pale (long path,
// scattering saturated), why the zenith is deep (short path), and why a low
// sun goes red (its own light crosses ~38 air masses and loses blue first).

// Rayleigh phase: gently forward/backward peaked, symmetric.
fn phaseRayleigh(mu : f32) -> f32 { return 0.75 * (1.0 + mu * mu); }

// Henyey-Greenstein: the forward-scattering lobe that puts the bright halo
// around the sun and the glow along the horizon. g near 0.76 is typical haze.
fn phaseMie(mu : f32, g : f32) -> f32 {
  let g2 = g * g;
  let d = 1.0 + g2 - 2.0 * g * mu;
  return (1.0 - g2) / (4.0 * 3.14159265 * max(d * sqrt(max(d, 1e-4)), 1e-4));
}

// sunTransmittance (now in common.wgsl) is independent of TUNE_SKY_RAYLEIGH
// for the same reason as SUN_EXTINCT_K above: the in-scatter strength (how
// blue the sky is) and the extinction strength (how fast the sun reddens)
// want opposite tuning, and sharing one constant makes a rich blue sky imply
// a permanently orange sun. At its coefficient the sun is essentially white
// overhead (mass 1 -> 0.98/0.97/0.92) and deep orange on the horizon
// (mass 38 -> 0.61/0.34/0.07). TUNE_SUN_REDDENING scales it.

// ---- starfield ----------------------------------------------------------
// A star is a POINT SOURCE. It has no resolvable disc — even Betelgeuse is
// ~0.05 arcsec, thousands of times smaller than one pixel. What reaches the
// eye is a point spread function roughly one pixel wide, and brighter stars
// look "bigger" only because their PSF wings clear the visibility floor.
//
// ---- WHY SIZE IS MEASURED IN PIXELS, NOT RADIANS ----
// The first version used a fixed angular radius (0.0055 rad, ~4x the SUN's
// angular radius — genuinely enormous) and a Gaussian falloff across it. Two
// consequences, both of which were visible immediately:
//   * the stars read as nearby round blobs rather than distant points, and
//   * because the falloff spanned many pixels, its per-pixel steps were
//     directly visible as chunky squares.
// Sizing the PSF in PIXELS instead makes a star exactly as small as the
// display allows at any resolution or FOV, which is what "infinitely far
// away" actually looks like. `pxAng` below is the angular size of one pixel.
//
// Density is deliberately low. Naked-eye stars number ~5000 over the whole
// sphere, of which a fraction are in frame; filling a fifth of a fine grid
// (the first version's ~21% across two layers) is the TV-static look. The
// magnitude distribution matters as much as the count: real skies have a very
// few bright stars and a long tail of faint ones, so `mag` is raised to a
// high power to make bright stars genuinely rare.
fn starField(dir : vec3f) -> vec3f {
  // Wheel the sky about the celestial pole. A tilted axis (not straight up)
  // means stars rise and set at an angle, which is most of what makes a night
  // sky read as a rotating dome rather than a static texture.
  //
  // The axis comes from the orbital solve (elevation = latitude, due north),
  // NOT from a constant: it used to be vec3(0.28, 0.92, 0), which tips ~18
  // degrees toward the EAST and ignores latitude, so the stars wheeled about
  // one axis while the sun arced about another.
  let ca = cos(R.starRot);
  let sa = sin(R.starRot);
  let axis = normalize(R.poleDir);
  // Rodrigues rotation about `axis`.
  let d = normalize(dir * ca + cross(axis, dir) * sa +
                    axis * dot(axis, dir) * (1.0 - ca));

  // Angular size of one pixel. The PSF is built as a multiple of this, so a
  // star stays a point at any resolution instead of growing with the window.
  let pxAng = max(R.tanHalfFov * 2.0 / max(R.viewPx, 1.0), 1e-7);

  var acc = vec3f(0.0);
  // Two layers: a sparse bright layer (the stars you would actually name) and
  // a finer, much fainter layer that gives the sky depth without texture.
  for (var layer = 0u; layer < 2u; layer++) {
    let scale = select(TUNE_STAR_DENSITY, TUNE_STAR_DENSITY * 2.7, layer == 1u);
    let p = d * scale;
    let cell = floor(p);
    // Only the containing cell matters: star jitter is kept under half a cell
    // so a star never leaves its own cell and neighbours cannot contribute.
    let h = pcg(u32((i32(cell.x) * 73856093) ^ (i32(cell.y) * 19349663) ^
                    (i32(cell.z) * 83492791)) ^ (layer * 0x9E3779B9u));
    let hf = vec3f(f32(h & 0x3FFu), f32((h >> 10u) & 0x3FFu),
                   f32((h >> 20u) & 0x3FFu)) / 1023.0;
    // Density gate: only a small fraction of cells hold a star at all.
    let occupancyRoll = f32((h >> 6u) & 0xFFu) / 255.0;
    let thresh = select(TUNE_STAR_SPARSITY, TUNE_STAR_SPARSITY * 1.7, layer == 1u);
    if (occupancyRoll > thresh) { continue; }
    // Star centre, jittered within the cell, projected back to the sphere.
    let centre = normalize(cell + 0.5 + (hf - 0.5) * 0.8);
    let cosAng = clamp(dot(d, centre), -1.0, 1.0);
    let ang = acos(cosAng);

    // Magnitude: mag^4 so bright stars are rare and most of the field is faint.
    let magRaw = f32((h >> 18u) & 0xFFu) / 255.0;
    let mag = magRaw * magRaw * magRaw * magRaw;

    // PSF core radius in PIXELS. Even the brightest star stays close to a
    // single pixel; TUNE_STAR_SIZE scales the whole field for taste.
    let corePx = TUNE_STAR_SIZE * (0.62 + mag * 0.85) *
                 select(1.0, 0.72, layer == 1u);
    let core = corePx * pxAng;
    // Cull well outside the visible wings — cheap early-out for most pixels.
    if (ang > core * 4.0) { continue; }

    // Airy-like profile: a tight Gaussian core plus a much weaker, wider skirt.
    // The skirt is what makes a bright star read as bright rather than merely
    // as a lit pixel, and it is what the eye interprets as "size".
    let x = ang / core;
    var b = exp(-x * x * 2.2) + 0.10 * exp(-x * x * 0.35);

    // Twinkle: scintillation is atmospheric, so it is strongest near the
    // horizon where the air mass is greatest, and absent at the zenith.
    let tw = sin(R.time * (1.7 + magRaw * 4.3) + f32(h & 0xFFFFu) * 0.001);
    let horizonBoost = 1.0 + 2.0 * (1.0 - clamp(d.y, 0.0, 1.0));
    b *= 1.0 + TUNE_STAR_TWINKLE * horizonBoost * tw;

    // Fade stars out toward the horizon: real ones are extinguished by the
    // long air path, and it also stops the starfield from meeting the terrain
    // in a hard line. Uses the UNROTATED direction — extinction is fixed to
    // the observer's horizon, not to the rotating celestial sphere.
    let horizonFade = smoothstep(-0.02, 0.16, dir.y);

    // Colour by stellar temperature: cool orange through hot blue-white.
    let temp = f32((h >> 26u) & 0x3Fu) / 63.0;
    let col = mix(vec3f(1.0, 0.78, 0.62), vec3f(0.76, 0.85, 1.0), temp);
    acc += col * max(b, 0.0) * (0.20 + mag * 2.2) * horizonFade *
           select(1.0, 0.35, layer == 1u);
  }
  return acc * TUNE_STAR_BRIGHTNESS;
}

// ---- value noise / fbm on the sphere, for nebulae and aurora ----
fn hash3f(p : vec3f) -> f32 {
  let q = floor(p);
  let h = pcg(u32((i32(q.x) * 374761393) ^ (i32(q.y) * 668265263) ^
                  (i32(q.z) * 1274126177)));
  return f32(h & 0xFFFFFFu) / 16777215.0;
}
fn vnoise(p : vec3f) -> f32 {
  let i = floor(p);
  let f = p - i;
  let u = f * f * (3.0 - 2.0 * f);
  let c000 = hash3f(i + vec3f(0.0, 0.0, 0.0));
  let c100 = hash3f(i + vec3f(1.0, 0.0, 0.0));
  let c010 = hash3f(i + vec3f(0.0, 1.0, 0.0));
  let c110 = hash3f(i + vec3f(1.0, 1.0, 0.0));
  let c001 = hash3f(i + vec3f(0.0, 0.0, 1.0));
  let c101 = hash3f(i + vec3f(1.0, 0.0, 1.0));
  let c011 = hash3f(i + vec3f(0.0, 1.0, 1.0));
  let c111 = hash3f(i + vec3f(1.0, 1.0, 1.0));
  let x00 = mix(c000, c100, u.x);
  let x10 = mix(c010, c110, u.x);
  let x01 = mix(c001, c101, u.x);
  let x11 = mix(c011, c111, u.x);
  return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}
fn fbm(p : vec3f, octaves : u32) -> f32 {
  var a = 0.5;
  var f = 1.0;
  var s = 0.0;
  for (var i = 0u; i < octaves; i++) {
    s += a * vnoise(p * f);
    f *= 2.02;
    a *= 0.5;
  }
  return s;
}

// ---- the Shivering Isles night ------------------------------------------
// The reference look is not "dark blue with dots": it is a sky with STRUCTURE
// — a bright galactic band, coloured nebulae, and slow curtains of aurora that
// give the dome motion. Built in three layers so each is separately tunable
// and any of them can be turned off.
// Returns SMOOTH night emission only — no stars. Stars are added separately by
// skyColorNoBodies(), so that fog and reflection lookups can take this and get
// the right colour without point sources bleeding into solid geometry.
fn nightGlow(rd : vec3f) -> vec3f {
  // Base gradient: deep indigo overhead easing to a warmer, lighter horizon
  // (airglow plus whatever light pollution the world implies).
  let up = clamp(rd.y, 0.0, 1.0);
  var c = mix(TUNE_NIGHT_HORIZON, TUNE_NIGHT_ZENITH, pow(up, 0.65));

  // Galactic band: a great circle tilted off the horizon, thickened with fbm
  // so its edges are ragged rather than a clean stripe.
  // Same celestial pole the starfield turns about (see starField): the galaxy
  // is fixed to the star sphere, so if these two axes disagree the band slides
  // through the constellations over a night.
  let ca = cos(R.starRot);
  let sa = sin(R.starRot);
  let axis = normalize(R.poleDir);
  let d = rd * ca + cross(axis, rd) * sa + axis * dot(axis, rd) * (1.0 - ca);
  let galNormal = normalize(TUNE_GALAXY_NORMAL);
  let bandDist = abs(dot(normalize(d), galNormal));
  let bandWidth = TUNE_GALAXY_WIDTH * (1.0 + 0.59 * fbm(d * 3.1, 3u));
  let band = 1.0 - smoothstep(0.0, bandWidth, bandDist);
  // Dust lanes: dark filaments cutting through the band, which is what makes
  // it read as a galaxy rather than a painted smear.
  let dust = fbm(d * 7.5 + vec3f(11.0, 3.0, 7.0), 4u);
  let bandBody = band * band * (0.55 + 0.75 * fbm(d * 4.5, 4u));
  c += TUNE_MILKYWAY_COLOR * bandBody * TUNE_MILKYWAY_STRENGTH *
       smoothstep(0.62, 0.28, dust);

  // Nebulae: two tinted fbm masses, one cool one warm, confined mostly to the
  // band so they read as part of the galaxy.
  let n1 = fbm(d * 2.2 + vec3f(31.0, 17.0, 5.0), 5u);
  let n2 = fbm(d * 1.7 + vec3f(-9.0, 23.0, 41.0), 5u);
  let nebMask = 0.35 + 0.65 * band;
  c += TUNE_NEBULA_COOL * smoothstep(0.52, 0.86, n1) * nebMask *
       TUNE_NEBULA_STRENGTH;
  c += TUNE_NEBULA_WARM * smoothstep(0.58, 0.92, n2) * nebMask *
       TUNE_NEBULA_STRENGTH * 0.8;

  // Aurora: vertical curtains that ripple. Modelled as a height-banded noise
  // field in the horizontal plane, faded in above the horizon and out toward
  // the zenith, with the classic green base / magenta tip gradient.
  if (TUNE_AURORA_STRENGTH > 0.0 && rd.y > 0.0) {
    // Project onto a plane well above the camera: curtains converge toward a
    // vanishing region the way real aurora do near magnetic north.
    let t = TUNE_AURORA_HEIGHT / max(rd.y, 0.06);
    let pp = vec3f(rd.x, 0.0, rd.z) * t;
    // Two scrolling noise fields multiplied: the product is thin and filament-
    // like where a single field would be blobby.
    let a1 = fbm(vec3f(pp.x * 0.010, R.time * 0.021, pp.z * 0.010) * 3.0, 4u);
    let a2 = fbm(vec3f(pp.x * 0.021 + 5.0, R.time * 0.013,
                       pp.z * 0.021 - 3.0) * 3.0, 3u);
    var curtain = smoothstep(0.48, 0.86, a1) * smoothstep(0.35, 0.80, a2);
    // Fade in just off the horizon, out before the zenith.
    curtain *= smoothstep(0.02, 0.22, rd.y) * (1.0 - smoothstep(0.35, 0.85, rd.y));
    // Vertical colour ramp along the curtain: green low, magenta at the tips.
    let ramp = clamp(rd.y * 2.6, 0.0, 1.0);
    let auroraCol = mix(TUNE_AURORA_LOW, TUNE_AURORA_HIGH, ramp * ramp);
    c += auroraCol * curtain * TUNE_AURORA_STRENGTH;
  }

  return c;
}

// ---- the moons ------------------------------------------------------------
// A real disc with a terminator, maria, and a soft glow. Since the celestial
// overhaul there are TWO of them on independent Keplerian orbits, so this is
// parameterised over the body rather than reading R.moonDir directly:
//
//   mDir     unit direction to the body (world space)
//   mPhase   0 = new, 0.5 = full — the illuminated fraction, derived on the
//            CPU from the real sun-moon-planet elongation angle
//   mSign    +1/-1, which limb is lit. Waxing and waning are the same
//            illuminated FRACTION and differ only in this; without it the
//            terminator flips as a moon passes full and the disc appears to
//            roll over.
//   mRad     angular radius in radians, already modulated by orbital
//            distance so perigee is genuinely larger
//   mSeed    offsets the maria fbm so the two moons are not the same rock
//   tint     per-body surface colour
//   bright   brightness multiplier (moon B is a dimmer, greyer body)
//
// Everything about the phase geometry is shared — the CPU hands each body the
// same three numbers and this draws them the same way.
fn moonDisc(rd : vec3f, mDir : vec3f, mPhase : f32, mSign : f32, mRad : f32,
            mSeed : vec3f, tint : vec3f, bright : f32) -> vec3f {
  let cosAng = dot(rd, mDir);
  // The glow falloff is tied to the disc size so a small moon does not carry a
  // halo sized for a large one. 0.03 rad is the reference the exponent 220 was
  // tuned against; mRad is now the ORBIT's apparent radius (dayNight.moon*
  // AngularRadius, modulated by distance), not a separate render knob — the
  // discs and the eclipse test must read one number or they disagree about how
  // big a moon is.
  let glowP = 220.0 * (0.03 / max(mRad, 1e-4));
  if (cosAng < 0.9) {
    // Far from the disc: only the broad glow, which is cheap.
    let glow = pow(max(cosAng, 0.0), glowP) * TUNE_MOON_GLOW;
    return tint * glow * bright;
  }
  let ang = acos(clamp(cosAng, -1.0, 1.0));
  let r = max(mRad, 1e-4);
  var c = vec3f(0.0);

  // Broad halo around the disc.
  c += tint * pow(max(cosAng, 0.0), glowP) * TUNE_MOON_GLOW * bright;

  if (ang < r * 1.6) {
    // Build a local frame on the disc so we can texture it and cut the phase.
    // MUST match the frame PhaseOf() builds on the CPU (celestial.cpp), or the
    // lit-limb sign is measured against a different +x and crescents point the
    // wrong way.
    var upv = vec3f(0.0, 1.0, 0.0);
    if (abs(mDir.y) > 0.95) { upv = vec3f(1.0, 0.0, 0.0); }
    let mx = normalize(cross(upv, mDir));
    let my = cross(mDir, mx);
    // Offset of this ray from disc centre, in disc radii.
    let off = vec2f(dot(rd, mx), dot(rd, my)) / r;
    let rr = length(off);
    if (rr < 1.25) {
      // Antialias the limb against one pixel of angular size.
      let px = max(R.tanHalfFov * 2.0 / max(R.viewPx, 1.0), 1e-6) / r;
      let disc = 1.0 - smoothstep(1.0 - px * 1.5, 1.0 + px * 1.5, rr);
      // Surface: hemisphere normal, so maria and the terminator wrap.
      let z = sqrt(max(1.0 - rr * rr, 0.0));
      let nrm = normalize(mx * off.x + my * off.y + mDir * z);
      // Maria: large dark basalt patches plus fine cratering. mSeed is what
      // makes the second moon a different rock rather than a copy.
      let maria = fbm(nrm * 3.4 + mSeed, 4u);
      let craters = fbm(nrm * 15.0 + mSeed * 0.31, 3u);
      var albedo = mix(0.55, 1.0, smoothstep(0.38, 0.72, maria));
      albedo *= 0.86 + 0.14 * craters;
      // PHASE: the terminator is the shadow of the moon's own sphere. Build
      // the illumination direction from mPhase (0 = new, 0.5 = full) and
      // light the hemisphere with it. Real lunar photometry is famously flat
      // (retroreflective regolith), so use a very wrapped diffuse rather than
      // Lambert or the disc looks like a shaded billiard ball.
      let pa = (mPhase - 0.5) * 6.2831853;
      let lightDir = normalize(mx * sin(pa) * mSign + mDir * cos(pa) * -1.0 +
                               my * 0.06);
      let ndl = dot(nrm, lightDir);
      let lit = smoothstep(-0.09, 0.09, ndl);
      // Earthshine: the dark limb is faintly visible, lit by planetshine.
      let earthshine = TUNE_MOON_EARTHSHINE * (1.0 - lit);
      c += tint * disc * albedo *
           (lit * TUNE_MOON_BRIGHTNESS + earthshine) * bright;
    }
  }
  return c;
}

// Both moons, in the right order. Moon B is given the LARGER semi-major axis
// by celestial.cpp, so it is always the farther body — which means moon A
// occults it and never the other way round. R.lunarEclipse is the fraction of
// B's disc A currently covers; multiplying B down by it is the whole
// moon-on-moon eclipse, and it is correct because A's own disc is drawn on top
// at exactly the covering geometry.
fn moonLayer(rd : vec3f) -> vec3f {
  // The seed vectors are the only thing making the two moons different ROCK
  // rather than the same face drawn twice — they offset the fbm that carves
  // maria and craters, so changing one rerolls that moon's surface entirely.
  var c = moonDisc(rd, R.moon2Dir, R.moon2Phase, R.moon2PhaseSign,
                   R.moon2AngRadius, TUNE_MOON2_MARIA_SEED,
                   TUNE_MOON2_COLOR, TUNE_MOON2_BRIGHTNESS) *
          (1.0 - R.lunarEclipse);
  c += moonDisc(rd, R.moonDir, R.moonPhase, R.moonPhaseSign, R.moonAngRadius,
                TUNE_MOON_MARIA_SEED, TUNE_MOON_COLOR, 1.0);
  return c;
}

// ---- the day sky ---------------------------------------------------------
fn daySky(rd : vec3f) -> vec3f {
  let mu = dot(rd, R.sunDir);
  let viewMass = airMass(rd.y);
  let sunMass = airMass(R.sunDir.y);

  // Direct sunlight colour after its own trip through the atmosphere. Drives
  // the disc, the halo and the horizon warmth together.
  //
  // NOTE the deliberate asymmetry below: this reddened colour is applied to
  // the MIE term but only partially to the RAYLEIGH term. Applying it fully to
  // both is physically tempting and visually wrong — it double-counts the
  // extinction, because the Rayleigh in-scatter integral ALREADY accounts for
  // the wavelength-dependent loss along the path. Doing it anyway multiplies
  // a blue in-scatter by a red transmittance and the two cancel, which turns
  // the noon sky olive-khaki. (Measured: zenith came out 0.33/0.34/0.13.)
  let sunLight = sunTransmittance(sunMass) * TUNE_SUN_INTENSITY;

  // Rayleigh in-scatter: how much blue this view ray picks up. Scales with the
  // air mass along the ray, which is what makes the horizon pale and the
  // zenith deep. The (1 - exp(-tau)) form saturates instead of growing without
  // bound at the horizon — and that saturation IS the pale horizon band: by
  // ~10 air masses every channel is near 1.0, so the colour washes to white.
  let tauR = RAYLEIGH_RGB * TUNE_SKY_RAYLEIGH * viewMass * 0.06;
  // (1 - exp(-tau)) alone saturates every channel toward 1 at the horizon, so
  // the horizon band comes out warm WHITE. Real horizon light is also
  // EXTINGUISHED on its way in, which is what keeps it from blowing out.
  //
  // Crucially the extinction that COLOURS the horizon is the one along the
  // SUNLIGHT's path, not the view path. Using the view path reddens the
  // horizon even at midday (the view mass is ~38 at the horizon whatever the
  // sun is doing), which is wrong: a noon horizon is pale blue-white and only
  // goes orange when the SUN gets low. Splitting them this way is what makes
  // one model cover both.
  //
  // This coefficient is DELIBERATELY not TUNE_SKY_RAYLEIGH. Reusing the
  // in-scatter strength here couples two things that need opposite tuning: a
  // high Rayleigh makes the sky a richer blue (good) but, reused as extinction,
  // also crushes blue out of the daylight sky entirely. At 12 it made a 15-deg
  // sun keep 72% of red and only 20% of blue, so the whole dome went khaki
  // (measured 102,98,75). Kept small and separate, the sunset still reddens
  // (mass ~38 at the horizon) while a mid-morning sky stays blue.
  const SUN_EXTINCT_K : f32 = 0.045;
  let extinctR = exp(-RAYLEIGH_RGB * sunMass * SUN_EXTINCT_K * TUNE_SUN_REDDENING);
  let inscatterR = (1.0 - exp(-tauR)) * extinctR * phaseRayleigh(mu);

  // Mie in-scatter: white-ish forward haze that concentrates around the sun.
  // This one DOES take the full reddened sunlight, because haze scattering is
  // essentially wavelength-neutral — whatever colour reaches the particles is
  // the colour they scatter, which is why the glow around a setting sun is
  // orange while the sky opposite stays blue.
  let tauM = TUNE_SKY_MIE * viewMass * 0.012;
  let inscatterM = vec3f(1.0 - exp(-tauM)) * phaseMie(mu, TUNE_SKY_MIE_G) *
                   TUNE_SKY_MIE_STRENGTH * 22.0;

  // Brightness follows the sun's elevation: the dome dims as the sun sets
  // rather than staying at full noon brightness until it clips the horizon.
  let sunAlt = clamp(R.sunDir.y, 0.0, 1.0);
  let lit = 0.12 + 0.88 * pow(sunAlt, 0.55);

  // inscatterR already carries the sun's colour through extinctR; inscatterM
  // takes the full reddened sunlight because haze scatters whatever reaches it.
  var c = (inscatterR + inscatterM * sunLight) * lit * TUNE_SKY_EXPOSURE;

  // Ground bounce near and below the horizon, so the dome does not simply go
  // black under the camera when looking down at distant fog.
  let below = smoothstep(0.06, -0.20, rd.y);
  c = mix(c, TUNE_SKY_GROUND * (0.25 + 0.75 * max(R.sunDir.y, 0.0)), below * 0.75);

  return c;
}

// ---- the sun disc --------------------------------------------------------
// Kept separate from daySky so reflections and fog can take the sky WITHOUT
// the disc — a mirrored sun in every ripple is the classic giveaway.
fn sunDisc(rd : vec3f) -> vec3f {
  let cosAng = dot(rd, R.sunDir);
  if (cosAng < 0.999) { return vec3f(0.0); }
  let ang = acos(clamp(cosAng, -1.0, 1.0));
  let r = SUN_ANGULAR_RADIUS * TUNE_SUN_SIZE;
  // Antialias against one pixel's angular size so the limb is crisp at any
  // resolution but never stair-steps.
  let px = max(R.tanHalfFov * 2.0 / max(R.viewPx, 1.0), 1e-6);
  let disc = 1.0 - smoothstep(r - px, r + px, ang);
  if (disc <= 0.0) { return vec3f(0.0); }
  // Limb darkening: the sun is a ball of gas, so the edge is dimmer and redder
  // than the centre because we see less deep (hotter) plasma there. This is
  // the detail that makes the disc read as a SPHERE rather than a white
  // sticker, and it costs one sqrt.
  let x = clamp(ang / r, 0.0, 1.0);
  let mu2 = sqrt(max(1.0 - x * x, 0.0));
  // Eddington-style limb law, per channel: red darkens least, blue most.
  let limb = vec3f(0.34 + 0.66 * pow(mu2, 0.52),
                   0.28 + 0.72 * pow(mu2, 0.62),
                   0.22 + 0.78 * pow(mu2, 0.74));
  let sunMass = airMass(R.sunDir.y);
  // ECLIPSE. R.solarEclipse is the fraction of the sun's AREA a moon covers
  // right now (circle-circle lens area, computed from the real orbital
  // geometry in celestial.cpp). Scaling the disc by the uncovered fraction is
  // the physically correct thing: an annular eclipse leaves a bright ring and
  // this leaves the corresponding brightness, and totality goes to zero.
  //
  // The occulting moon's own disc is drawn on top by moonLayer(), so the black
  // bite out of the sun is a real body in front of it rather than a painted
  // mask — which is why nothing here has to know WHICH moon it was.
  let uncovered = clamp(1.0 - R.solarEclipse, 0.0, 1.0);
  return sunTransmittance(sunMass) * limb * disc * TUNE_SUN_DISC_GAIN * 40.0 *
         uncovered;
}

// ---- the three sky tiers -------------------------------------------------
// Point-like detail (stars, sun disc, moon disc) belongs ONLY to a primary ray
// that actually escaped to space. Everything else — fog targets, reflections
// off LOD water, ambient lookups — must use the smooth tier, because those
// consumers integrate over a solid angle far larger than a star.
//
// Getting this wrong is not subtle: aerial perspective converging on the full
// sky rendered stars *through* distant terrain and below the horizon line,
// since heavily-fogged surfaces are exactly the ones that take the most sky.
//
// Tier 1 — skyAirglow(): smooth emission only. Gradient + galactic band +
//          nebulae, no point sources. This is the fog/reflection/ambient target.
// Tier 2 — skyColorNoBodies(): airglow + stars, no sun/moon discs. For a
//          primary ray that reached space where a mirrored sun would double up.
// Tier 3 — skyColor(): everything. Background pixels only.
// ---- eclipse weight -------------------------------------------------------
// How much daylight an eclipse has taken away, 0..1. The sky is lit by the
// sun's whole disc, so it dims by the covered AREA — but not linearly: a 50%
// eclipse is barely noticeable to the eye in reality, and the last few percent
// of coverage is where the light collapses. The cubic is that curve, and it is
// the reason a partial eclipse reads as "slightly odd light" rather than as
// someone turning the exposure down.
fn eclipseDim() -> f32 {
  let f = clamp(R.solarEclipse, 0.0, 1.0);
  // The exponent is the PERCEPTUAL curve, not the physics: covered area falls
  // linearly, but the eye's adaptation means half a sun still reads as broad
  // daylight. A high power keeps the world bright until the last sliver goes,
  // which is what real partial eclipses feel like; 1.0 makes the dimming track
  // the covered area directly and reads as someone pulling the exposure down.
  return pow(f, TUNE_ECLIPSE_CURVE) * TUNE_ECLIPSE_DARKNESS;
}

// The effective daylight weight during an eclipse. Everything that keyed off
// R.sunUp — sky brightness, star fade, the moons' visibility — goes through
// this instead, so totality brings the stars out and lifts the moons into a
// daytime sky the same way real totality does. Without routing the STAR fade
// through it too, a total eclipse would darken the dome and leave it blank.
fn dayWeight() -> f32 {
  return R.sunUp * (1.0 - eclipseDim());
}

fn skyAirglow(rdIn : vec3f) -> vec3f {
  let rd = normalize(rdIn);
  let dayW = dayWeight();
  var c = daySky(rd) * dayW;
  if (dayW < 0.999) { c += nightGlow(rd) * (1.0 - dayW); }
  return max(c, vec3f(0.0));
}

fn skyColorNoBodies(rdIn : vec3f) -> vec3f {
  let rd = normalize(rdIn);
  let dayW = dayWeight();
  var c = skyAirglow(rd);
  // Stars fade out under daylight rather than popping off.
  if (dayW < 0.999) { c += starField(rd) * (1.0 - dayW); }
  return max(c, vec3f(0.0));
}

fn skyColor(rdIn : vec3f) -> vec3f {
  let rd = normalize(rdIn);
  let dayW = dayWeight();
  var c = skyColorNoBodies(rd);
  // The moons fade in as the sky darkens — including when it darkens because
  // one of them is in front of the sun. During totality the occulter is at
  // full strength, which is exactly what puts a black disc on the sun.
  if (dayW < 0.999) { c += moonLayer(rd) * (1.0 - dayW); }
  // The sun disc uses the RAW sunUp, not dayWeight: the disc has its own
  // eclipse term (its uncovered area), and dimming it twice would erase the
  // bright ring of an annular eclipse and the diamond ring of a total one.
  if (R.sunUp > 0.001) { c += sunDisc(rd) * R.sunUp; }
  return max(c, vec3f(0.0));
}

// ---- direct light: sun by day, moon by night --------------------------------
// One function so every shading path (near field, far field, reflections,
// water) agrees on what "the key light" is at this moment. Returns the light
// colour x intensity; callers multiply by their own N.L / shadow terms.
// Declared here, immediately after the sky, because everything below shades
// against it — WGSL requires definition before use.
fn keyLightColor() -> vec3f {
  // Sunlight reddens as it sets because its own path through the atmosphere
  // grows — the same sunTransmittance() the disc and the sky use, so a red sun
  // always lights the world red. Moonlight is sunlight bounced off a dark grey
  // rock: same spectrum, ~400k times dimmer, read as blue because of the
  // Purkinje shift at scotopic levels — the tint is a perceptual choice and
  // lives in TUNE_MOON_LIGHT_COLOR. MUST MATCH keyLightColorP (common.wgsl),
  // which the raster body paths use: inlined here rather than calling it
  // because this sits in per-hit shading loops and passing RenderParams by
  // value per call is a real cost at this call frequency.
  let sunCol = sunTransmittance(airMass(R.sunDir.y)) * TUNE_SUN_COLOR *
               TUNE_SUN_INTENSITY;
  // TWO moons. The brighter contributor owns the key light; the other adds
  // only ambient (see ambientAt). moonContribP/eclipseDayWeightP are the
  // SHARED helpers from common.wgsl — they take scalars rather than the
  // RenderParams struct, so the "inline it, don't pass R by value" argument
  // above does not apply to them and the two copies cannot drift on the part
  // that actually got complicated.
  let a = moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY);
  let b = moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY);
  let moonCol = select(TUNE_MOON2_LIGHT_COLOR * b, TUNE_MOON_LIGHT_COLOR * a,
                       a >= b);
  // An eclipse takes the sun's light out of the world, not just off the sky.
  return mix(moonCol, sunCol, eclipseDayWeightP(R));
}

// Direction of the key light — the sun by day, the brighter moon by night.
// Shadows are cast from whichever is dominant, so moonlit shadows point the
// right way. MUST MATCH keyLightDirP (common.wgsl); inlined for the same
// reason as above.
//
// Deliberately the RAW sunUp, not the eclipse-dimmed weight: a total eclipse
// must not swing every shadow in the world round to a lunar direction. Both
// bodies are in nearly the same place at totality anyway, so the swing would
// buy nothing and would be the single most visible artefact on screen.
fn keyLightDir() -> vec3f {
  let a = moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY);
  let b = moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY);
  let moonDir = select(R.moon2Dir, R.moonDir, a >= b);
  return normalize(mix(moonDir, R.sunDir, step(0.5, R.sunUp)));
}

// Per-meter absorption scale applied to material opacity — trace() (media
// early-out) and fs() (final tint) must agree on it.
const MEDIA_ABSORB : f32 = TUNE_MEDIA_ABSORB;
// Optical depth past which the background is invisible (exp(-6) ~ 0.25%):
// stop marching instead of walking the rest of a smoke plume voxel-by-voxel.
const MEDIA_TAU_MAX : f32 = TUNE_MEDIA_TAU_MAX;

struct Hit {
  hit      : bool,
  saturated: bool,    // media absorbed the ray before any surface hit
  t        : f32,
  tExit    : f32,     // where the ray leaves the window box (0 if it misses):
                      // the far-field march starts here, never inside the
                      // window, so coarse data can't occlude fine data

  cell     : vec3<i32>,
  axis     : i32,     // axis stepped into the hit cell
  sgn      : f32,     // ray direction sign on that axis
  word     : u32,
  mediaTau : f32,     // optical depth: per-cell opacity x fullness x length
  mediaTint: vec3f,   // tau-weighted media color (divide by mediaTau to shade)
  mediaMat : u32,     // first media material crossed (liquid surface term)
  mediaSurf: f32,     // fullness (0..1) of the first media cell — surface term
  fireGlow : f32,     // flicker- and transmittance-weighted emissive path
  fireMat  : u32,     // first emissive media material (palette for the ramp)

  // ---- water surface (see shadeWater) ----
  // A translucent liquid is BOTH a surface and a volume. The media fields above
  // are the volume half; these are the surface half: where the ray first
  // crossed into the liquid, and which cell it entered, so fs() can build a
  // normal there and shade a real air/water interface instead of only tinting.
  liqT     : f32,     // t of the first liquid entry (0 if none)
  liqCell  : vec3<i32>,
  liqAxis  : i32,     // face the ray entered the liquid through
  liqSgn   : f32,
  liqPath  : f32,     // total distance travelled INSIDE liquid, fine voxels —
                      // drives per-channel Beer-Lambert depth absorption

  // ---- translucent solid (ice, glass — see shadeTranslucent) ----
  // Same surface-plus-volume split as the liquid fields above, but a solid
  // needs its own set: a ray can cross ice and THEN water (a frozen pond is
  // exactly that), and collapsing the two would make the ice surface vanish
  // the moment the ray reached the water under it.
  //
  // The ray does not stop at a translucent solid — it keeps marching and
  // accumulates depth, so whatever is behind is still shaded normally and the
  // slab tints it by how far the ray travelled inside.
  tsT      : f32,     // t of the first translucent-solid entry (0 if none)
  tsCell   : vec3<i32>,
  tsAxis   : i32,     // face the ray entered through
  tsSgn    : f32,
  tsMat    : u32,     // which translucent material (palette + absorption)
  tsPath   : f32,     // distance travelled INSIDE it, in fine voxels

  // ---- static micro-detail (see traceMicro) ----
  // A hit inside a micro brick reports the WORLD cell in `cell` as usual, but
  // the material and normal come from the sub-voxel that was struck rather than
  // from the cell's own material and entry face. Zero micMat means "this hit is
  // an ordinary voxel", which is every hit that predates the feature.
  micMat   : u32,     // material of the micro voxel struck (0 = not a micro hit)
};

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, R.origin); }

fn chunkOcc(cell : vec3<i32>) -> u32 {
  return occupancy[chunkIndexW(cell)];
}

// ============================================================================
// STATIC MICRO-DETAIL — the nested brick DDA
// ============================================================================
// docs/PLAN_voxel_editor.md §A. The world DDA has landed on a solid cell whose
// material carries MATF_MICRO. Instead of reporting a surface there, run a
// SECOND Amanatides-Woo DDA over that cell's subdiv^3 brick in cell-local
// space. A hit gives the micro voxel's material and the axis it was entered
// through; a MISS means the ray passes through the cell entirely, which is the
// whole point — a grass cell is mostly air, and the world march has to keep
// going or every tuft would render as a solid block.
//
// Everything here is render-only float math on render-only data. The two
// sources of per-cell variety are keyed on integer hashes of the cell so they
// are stable frame to frame and identical on every machine, without any of it
// being sim state (CLAUDE.md rule 1 scopes to what can write voxels).
//
// COST: the loop is capped at 3*subdiv + 4 steps, which is the exact worst case
// for a diagonal ray crossing an S^3 box (S steps per axis plus slack for the
// entry rounding). The CALLER additionally caps how many bricks one ray may
// enter (TUNE_MICRO_MAX_PER_RAY) — a grazing ray over a meadow crosses dozens
// of cells and each MISS keeps it alive, so per-cell bounding is not enough.

struct MicroHit {
  hit  : bool,
  mat  : u32,   // material of the sub-voxel struck
  t    : f32,   // distance from the cell entry point, in WORLD voxel units
  axis : i32,   // axis last stepped (face normal)
  sgn  : f32,   // ray direction sign on that axis
};

// Which flipbook frame is showing at `tick`. Integer-only and a pure function
// of the tick, exactly like the sim's day phase: two machines at the same tick
// draw the same frame, and no frame timing leaks in. (It is render-only either
// way — this is about the animation being reproducible in a replay, not about
// determinism of the world.)
// Identity hashes for a micro cell's authored variation (yaw, jitter, frame
// phase). The COLUMN hash ignores Y: everything that must agree up a stacked
// plant (flipbook phase for all materials, yaw/jitter for swaying ones) keys
// on it. The cell hash is the original per-cell draw, kept for non-swaying
// materials so their fields keep the exact scatter they were tuned with.
// The shading site (microNormalToWorld's caller) re-derives microCellHash, so
// the two must never diverge.
fn microColumnHash(cell : vec3<i32>) -> u32 {
  return hash3(R.seed, bitcast<u32>(cell.x), bitcast<u32>(cell.z));
}
fn microCellHash(flags : u32, cell : vec3<i32>) -> u32 {
  if ((flags & MICROF_SWAY) != 0u) { return microColumnHash(cell); }
  return hash3(R.seed, 0u, cellIndexW(cell));
}

// `colH` is the per-COLUMN hash (see microColumnHash): offsetting the flipbook
// phase by it is what stops every tuft in a meadow flipping its frames on the
// same tick, which read as the whole field twitching in lockstep. Keyed on the
// column rather than the cell because the stacked species (foxglove, reed,
// tall grass) repeat one model up a column — a per-cell phase would put
// adjacent cells of ONE plant on different frames and tear it at every cell
// boundary.
fn microFrameAt(b : MicroBrick, tick : u32, colH : u32) -> u32 {
  let n = microFrameCount(b);
  if (n <= 1u) { return 0u; }
  let period = microPeriod(b);
  if (period == 0u) { return 0u; }
  let phase = (tick + (colH & 0x3FFu)) % period;
  // Linear scan over the cumulative-offset header. n is <= 255 by construction
  // and realistically 2-8, so a scan beats any cleverness and has no divides.
  for (var i = 0u; i < n; i++) {
    if (phase < microPool[b.base + i]) { return i; }
  }
  return n - 1u;
}

// Fetch one sub-voxel's material from a packed brick. `p` must already be
// inside [0, S)^3.
fn microVoxAt(b : MicroBrick, frame : u32, p : vec3<i32>) -> u32 {
  let s = 1u << b.subdivLog2;
  let idx = (u32(p.z) * s + u32(p.y)) * s + u32(p.x);
  // frameCount header words, then `frame` whole bricks of S^3/4 words.
  let wordsPerBrick = (s * s * s) >> 2u;
  let w = b.base + microFrameCount(b) + frame * wordsPerBrick + (idx >> 2u);
  if (w >= MICRO_POOL_WORDS) { return 0u; }  // defensive: never index past the pool
  return (microPool[w] >> ((idx & 3u) * 8u)) & 0xFFu;
}

// `entry` is the ray's position in CELL-LOCAL coords (0..1 per axis) at the
// point it crossed into the cell; `rd` is the (normalised) world direction,
// which is also the cell-local direction because a cell is a unit cube.
fn traceMicro(b : MicroBrick, cell : vec3<i32>, entry : vec3f, rd : vec3f,
              tick : u32) -> MicroHit {
  var out : MicroHit;
  out.hit = false;
  out.mat = 0u;
  out.t = 0.0;
  out.axis = 1;
  out.sgn = -1.0;

  let sl = b.subdivLog2;
  let S = i32(1u << sl);

  // ---- per-cell variation, keyed on the cell, not on time ----
  // ONE hash draw feeds both the yaw and the jitter, so a cell's identity is a
  // single value and the two never decorrelate. hash3(seed, 0, cellIndexW) is
  // the same shape the sim's RNG uses; the middle argument is 0 because there
  // is no tick here — a tuft must not re-roll its orientation every frame.
  //
  // Swaying materials key on the COLUMN instead (microCellHash): they are the
  // ones worldgen stacks into multi-cell plants, and a per-cell yaw would give
  // each cell of one blade a different quarter-turn.
  let swaying = (b.flags & MICROF_SWAY) != 0u;
  let colH = microColumnHash(cell);
  let h = microCellHash(b.flags, cell);
  let frame = microFrameAt(b, tick, colH);

  // ---- wind, evaluated once per cell trace --------------------------------
  // Render-only (rule 1 untouched): R.time never feeds voxel state, exactly
  // like the water ripple field. The bend is applied further down as a shear
  // on the SAMPLE coordinate — the DDA still marches the unsheared grid, so
  // hit t/axis stay exact box faces and the cost is two rounds per sample row.
  var swayVec = vec2f(0.0);
  var riseBase = 0.0;
  var riseNorm = 0.0;
  if (swaying) {
    // Where is this cell within its PLANT? Count contiguous swaying cells
    // below and above (bounded at 8 — taller than anything worldgen stacks),
    // so an 8-cell stand and a single tuft both bend from their own root to
    // their own tip rather than by absolute cell height. Reading neighbour
    // voxels here is fine: this whole path is primary-camera-ray only.
    var below = 0;
    for (var i = 1; i <= 8; i++) {
      let cb = cell - vec3<i32>(0, i, 0);
      if (!inBounds(cb)) { break; }
      let mb = voxMat(voxWordAt(cb));
      if (mb == 0u || (microBricks[mb].flags & MICROF_SWAY) == 0u) { break; }
      below++;
    }
    var above = 0;
    for (var i = 1; i <= 8; i++) {
      let ca = cell + vec3<i32>(0, i, 0);
      if (!inBounds(ca)) { break; }
      let ma = voxMat(voxWordAt(ca));
      if (ma == 0u || (microBricks[ma].flags & MICROF_SWAY) == 0u) { break; }
      above++;
    }
    riseBase = f32(below);
    riseNorm = 1.0 / f32(below + 1 + above);

    // THE SHARED WIND FIELD. The two incommensurate bands, the travelling
    // gust phase and the elliptical anisotropy all used to be built right
    // here; they are now `windAt` in common.wgsl and this samples it (research
    // doc §4.7, DESIGN.md §12 invariant 2). What changed in the LOOK is one
    // thing: the ellipse's major axis was hardcoded to X and now follows the
    // weather vector, so turning wind.windDirDeg turns the grass.
    //
    // `ph` is the per-column scatter — the "not all synced" part — and it is
    // fed to the field as a phase offset exactly as it was before, because
    // decorrelation is what makes a field read as wind rather than as one
    // rocking object (Crysis / GPU Gems 3 ch.16).
    let ph = f32(colH & 1023u) * 0.006136;  // 0..2pi per column
    // R.time x microSwaySpeed, NOT the raw clock: microSwaySpeed is a
    // foliage-local trim and defaults to 1.0, at which the grass and the debug
    // arrow overlay are sampling the same field at the same phase.
    let ws = windSampleAt(vec3f(cell), R.time * TUNE_MICRO_SWAY_SPEED, ph, &R);
    // The primitive sum (fans, gusts, tornadoes) rides with the MEAN rather
    // than with the bands: a fan is a steady push at this cell, not an
    // oscillation, and it must not be re-weighted per blade the way a gust is.
    // Adding it here is the whole of "the grass leans in a fan's blast" —
    // nothing wires foliage to fans, they simply sample the same field.
    let wvel = windMeanWS(ws) + windPrimAt(vec3f(cell), &R)
                              + windBandWS(ws, ws.b1) * WIND_BAND_W1
                              + windBandWS(ws, ws.b2) * WIND_BAND_W2;
    swayVec = windSway(wvel) * TUNE_MICRO_SWAY_AMP;
  }

  // Local ray, in SUB-VOXEL units (0..S). Working in sub-voxels rather than in
  // 0..1 makes the DDA identical in shape to the world one, and `t` comes back
  // out in world-voxel units by dividing by S at the end.
  var p = entry * f32(S);
  var d = rd;

  // ---- quarter-turn yaw as a coordinate swizzle ----
  // A rotation of the RAY by -theta is equivalent to rotating the MODEL by
  // +theta, and for multiples of 90 degrees it is an exact swap-and-negate with
  // no trig and no resampling. Four variants is enough: a tuft of grass has no
  // preferred face, and the eye reads four orientations scattered over a field
  // as "not tiled" long before it can count them.
  if ((b.flags & MICROF_YAW) != 0u) {
    let q = h & 3u;
    let fS = f32(S);
    if (q == 1u) {        // 90 deg:  (x, z) -> (z, S - x)
      p = vec3f(p.z, p.y, fS - p.x);
      d = vec3f(d.z, d.y, -d.x);
      swayVec = vec2f(swayVec.y, -swayVec.x);
    } else if (q == 2u) { // 180 deg
      p = vec3f(fS - p.x, p.y, fS - p.z);
      d = vec3f(-d.x, d.y, -d.z);
      swayVec = -swayVec;
    } else if (q == 3u) { // 270 deg
      p = vec3f(fS - p.z, p.y, p.x);
      d = vec3f(-d.z, d.y, d.x);
      swayVec = vec2f(-swayVec.y, swayVec.x);
    }
    // swayVec turns WITH the ray (same swap-and-negate as `d`): the wind is a
    // world-space direction, and without this a field of yaw variants would
    // bend four different ways under one gust.
  }

  // ---- sub-cell XZ jitter ----
  // Shifts the MODEL within its cell so a field of tufts does not sit on a
  // perfect lattice. Bounded to +-1 sub-voxel: any more and a blade would poke
  // out of the cell the world DDA is standing in, and the part outside would
  // simply never be marched. Quantised to whole sub-voxels so it costs the DDA
  // nothing (the brick lookup stays an integer index) and so it cannot open a
  // seam by landing mid-voxel.
  if ((b.flags & MICROF_JITTER) != 0u) {
    let jx = f32(i32((h >> 8u) & 3u) - 1) * 0.5;
    let jz = f32(i32((h >> 10u) & 3u) - 1) * 0.5;
    p = vec3f(p.x - jx, p.y, p.z - jz);
  }

  // ---- wind: shear the RAY, not the samples ----
  // The bend displaces the model's content by D(y) = swayVec * riseFrac(y),
  // which is LINEAR in y within one cell — so instead of quantising sample
  // coordinates (which stepped 1.25 cm at a time and read as pixelated
  // motion), transform the ray into the model's REST space: shift the origin
  // by -D(p.y) and tilt the direction by the per-height shear. The sheared
  // ray is still a straight line, the ordinary DDA below marches it
  // untouched, and the content renders continuously displaced — smooth
  // float motion, exact t values, same axis-aligned face normals.
  // Applied after the yaw swizzle (swayVec was rotated with the ray) and
  // after jitter, both of which are rigid offsets the shear composes with.
  if (swaying) {
    let fS = f32(S);
    let disp = swayVec * ((riseBase + p.y / fS) * riseNorm);
    let perY = swayVec * (riseNorm / fS);
    p = vec3f(p.x - disp.x, p.y, p.z - disp.y);
    d = vec3f(d.x - perY.x * d.y, d.y, d.z - perY.y * d.y);
  }

  // ---- Amanatides-Woo over the brick ----
  // Nudge off the entry face before flooring: a ray entering at exactly x = 0
  // otherwise floors to cell -1 or 0 depending on float noise, and half the
  // blades would drop their first row.
  p = p + d * 1e-4;
  var c = vec3<i32>(floor(p));
  // Entry can still land just outside on the face the ray came through (or,
  // with jitter, genuinely outside). Clamp only the axes that are within one
  // voxel of the box; anything further out is a real miss.
  let stepv = vec3<i32>(sign(d));
  var inv = vec3f(0.0);
  inv.x = 1.0 / select(d.x, select(-1e-6, 1e-6, d.x >= 0.0), abs(d.x) < 1e-6);
  inv.y = 1.0 / select(d.y, select(-1e-6, 1e-6, d.y >= 0.0), abs(d.y) < 1e-6);
  inv.z = 1.0 / select(d.z, select(-1e-6, 1e-6, d.z >= 0.0), abs(d.z) < 1e-6);
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(c[a]) + select(0.0, 1.0, d[a] > 0.0);
    tMax[a] = (boundary - p[a]) * inv[a];
  }

  var axis = 0;
  var tCur = 0.0;
  // 3*S covers a full diagonal traverse; +8 is slack for the entry rounding
  // above and for a start displaced outside the box — up to one voxel of
  // jitter plus up to two sub-voxels of wind shear on the entry point.
  let maxSteps = 3 * S + 8;
  for (var i = 0; i < maxSteps; i++) {
    if (c.x >= 0 && c.y >= 0 && c.z >= 0 && c.x < S && c.y < S && c.z < S) {
      let m = microVoxAt(b, frame, c);
      if (m != 0u) {
        out.hit = true;
        out.mat = m;
        out.t = tCur / f32(S);  // sub-voxel units back to world-voxel units
        out.axis = axis;
        out.sgn = sign(d[axis]);
        return out;
      }
    } else if (i > 0) {
      // Left the box after having been inside it (or after the entry slack ran
      // out): the ray misses the model entirely.
      //
      // The `i > 0` guard is what lets a jittered start begin one voxel outside
      // and still march in. Without it the very first out-of-box test would
      // abort before the DDA ever stepped.
      let outAway = (c.x < 0 && stepv.x <= 0) || (c.x >= S && stepv.x >= 0) ||
                    (c.y < 0 && stepv.y <= 0) || (c.y >= S && stepv.y >= 0) ||
                    (c.z < 0 && stepv.z <= 0) || (c.z >= S && stepv.z >= 0);
      if (outAway) { break; }
    }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      c.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      c.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      c.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
  }
  return out;
}

// ---- analytic strand plants (MICROF_STRANDS) --------------------------------
// The cells of a strand material hold no brick. Instead each COLUMN carries a
// small set of parametric blades — root position, own height, own wind phase —
// and this function intersects the slice of them passing through `cell` in
// closed form. This is the path for content that must move smoothly and
// PER-STRAND: the brick sway above bends a whole cell's content as one rigid
// piece on a sub-voxel lattice, while a strand here is a true entity whose
// position is a continuous function of time. Nothing is quantised anywhere.
//
// Geometry: a blade is a vertical square-section rod bent by a quadratic
// cantilever curve D(u) = wind * u^2 (root stiff, tip floppy), u = height /
// plant height. Within one cell the curve is taken as linear (a 10 cm chord of
// a gentle bend), which makes the rod a SHEARED BOX: substituting the shear
// into the ray gives a still-straight ray, so the test collapses to the
// classic three-slab AABB intersection — exact, no iteration. Chord endpoints
// are exact curve evaluations, so consecutive cells of a column share them and
// the blade is continuous across every cell boundary.
//
// Every strand attribute derives from hash(column, strandIndex): each cell of
// a stack reconstructs the identical strand set independently, which is what
// lets worldgen keep stacking plain voxel cells (segment + head materials)
// while the renderer treats the column as one plant. The material of a hit is
// the authored body until the strand's own tip fraction, where it switches to
// the tip material — so dried tips land per-BLADE, not per-cell.
//
// Normals are reported as axis/sgn exactly like the brick DDA (the slab that
// decided entry), so lighting shades a blade as a voxel-crisp rod and the
// whole MicroHit pipeline downstream is unchanged.
fn traceStrands(b : MicroBrick, cell : vec3<i32>, entry : vec3f, rd : vec3f)
    -> MicroHit {
  var out : MicroHit;
  out.hit = false;
  out.mat = 0u;
  out.t = 0.0;
  out.axis = 1;
  out.sgn = -1.0;

  // Pool layout: see the strands block in LoadMicroVox (sim/microvox.cpp).
  let w0 = microPool[b.base];
  let count = w0 & 0xFFu;
  let bodyMat = (w0 >> 8u) & 0xFFu;
  let tipMat = (w0 >> 16u) & 0xFFu;
  if (b.base + 5u + count * 2u > MICRO_POOL_WORDS) { return out; }
  let halfW = bitcast<f32>(microPool[b.base + 1u]);
  let tipFrac = bitcast<f32>(microPool[b.base + 2u]);
  let swayScale = bitcast<f32>(microPool[b.base + 3u]);
  let heightVary = bitcast<f32>(microPool[b.base + 4u]);

  let colH = microColumnHash(cell);

  // Plant extent: same probe as traceMicro, same reasoning — a strand's bend
  // and height are fractions of the PLANT, which only the column knows.
  var below = 0;
  for (var i = 1; i <= 8; i++) {
    let cb = cell - vec3<i32>(0, i, 0);
    if (!inBounds(cb)) { break; }
    let mb = voxMat(voxWordAt(cb));
    if (mb == 0u || (microBricks[mb].flags & MICROF_SWAY) == 0u) { break; }
    below++;
  }
  var above = 0;
  for (var i = 1; i <= 8; i++) {
    let ca = cell + vec3<i32>(0, i, 0);
    if (!inBounds(ca)) { break; }
    let ma = voxMat(voxWordAt(ca));
    if (ma == 0u || (microBricks[ma].flags & MICROF_SWAY) == 0u) { break; }
    above++;
  }
  let totalH = f32(below + 1 + above);
  let rise0 = f32(below);

  // The shared wind field (common.wgsl `windAt`), sampled ONCE per cell and
  // kept in its component parts. Brick plants and strand plants visibly live
  // in the same wind because they are literally reading the same function —
  // that is DESIGN.md §12 invariant 2, and it is also what makes the debug
  // arrow overlay evidence about this grass rather than a picture of a
  // different field.
  //
  // The parts stay separate because each strand blends the GUSTS with its own
  // hash-drawn weights (below), which decorrelates neighbouring blades without
  // costing extra transcendentals per strand. The MEAN is deliberately not
  // re-weighted: every blade in the cell stands in the same average wind and
  // they disagree only about the gusts.
  let ph = f32(colH & 1023u) * 0.006136;
  let ws = windSampleAt(vec3f(cell), R.time * TUNE_MICRO_SWAY_SPEED, ph, &R);
  let band1 = windSway(windBandWS(ws, ws.b1));
  let band2 = windSway(windBandWS(ws, ws.b2));
  // The mean, plus the wind primitives (fans/gusts/tornadoes) — with the mean
  // for the same reason as the brick path above: a primitive is a steady push
  // on the whole cell, and the per-strand re-weighting below applies to the
  // GUSTS only. One `windPrimAt` per column, not per blade.
  let bandMean = windSway(windMeanWS(ws) + windPrimAt(vec3f(cell), &R));
  // TUNE_MICRO_SWAY_AMP is authored in sub-voxels of a subdiv-8 cell; strands
  // work in cell units, hence the /8.
  let amp = TUNE_MICRO_SWAY_AMP * 0.125 * swayScale;

  var bestT = 1e9;
  for (var s = 0u; s < count; s++) {
    let hs = hash3(colH, s, 0x57A4Du);
    // This strand's own height: a fraction of the plant, so a stand's top is
    // ragged per-blade rather than sheared flat at the column height.
    let sH = totalH * (1.0 - heightVary * f32(hs & 255u) / 255.0);
    if (rise0 >= sH) { continue; }
    let yTop = min(1.0, sH - rise0);

    // Root: the authored slot, plus a small per-column jitter so a field of
    // columns is not the same 5 blades on a lattice.
    let rt = vec2f(bitcast<f32>(microPool[b.base + 5u + s * 2u]),
                   bitcast<f32>(microPool[b.base + 6u + s * 2u])) +
             (vec2f(f32((hs >> 8u) & 15u), f32((hs >> 12u) & 15u)) - 7.5) *
                 0.008;

    // Per-strand wind: an individual blend of the two shared bands, plus the
    // mean the whole cell shares. THIS line is "each strand is its own entity"
    // — two blades in one cell disagree about the GUST by their weights, not
    // by living in different fields, and they agree exactly about the average.
    let wind = (bandMean +
                band1 * (0.55 + 0.45 * f32((hs >> 16u) & 255u) / 255.0) +
                band2 * (0.20 + 0.55 * f32((hs >> 24u) & 255u) / 255.0)) * amp;

    // Cantilever chord through this cell: exact curve points at the cell's
    // bottom and top, clamped so root + bend + half-width never leaves the
    // cell (the world DDA would never march the part that crossed over).
    let u0 = rise0 / totalH;
    let u1 = (rise0 + yTop) / totalH;
    let lo = vec2f(halfW + 0.01) - rt;
    let hi = vec2f(0.99 - halfW) - rt;
    let d0 = clamp(wind * u0 * u0, lo, hi);
    let d1 = clamp(wind * u1 * u1, lo, hi);
    let shear = (d1 - d0) / max(yTop, 1e-4);

    // Sheared-slab test. Substituting the linear centreline C(y) = rt + d0 +
    // shear*y into the ray turns "distance to a slanted rod" into an ordinary
    // AABB slab test on a resampled ray — same guard against near-zero
    // components as the DDA above.
    let dpx = rd.x - shear.x * rd.y;
    let dpz = rd.z - shear.y * rd.y;
    let ix = 1.0 / select(dpx, select(-1e-6, 1e-6, dpx >= 0.0), abs(dpx) < 1e-6);
    let iy = 1.0 / select(rd.y, select(-1e-6, 1e-6, rd.y >= 0.0), abs(rd.y) < 1e-6);
    let iz = 1.0 / select(dpz, select(-1e-6, 1e-6, dpz >= 0.0), abs(dpz) < 1e-6);
    let ox = entry.x - rt.x - d0.x - shear.x * entry.y;
    let oz = entry.z - rt.y - d0.y - shear.y * entry.y;
    var t0x = (-halfW - ox) * ix;
    var t1x = (halfW - ox) * ix;
    if (t0x > t1x) { let tmp = t0x; t0x = t1x; t1x = tmp; }
    var t0y = (0.0 - entry.y) * iy;
    var t1y = (yTop - entry.y) * iy;
    if (t0y > t1y) { let tmp = t0y; t0y = t1y; t1y = tmp; }
    var t0z = (-halfW - oz) * iz;
    var t1z = (halfW - oz) * iz;
    if (t0z > t1z) { let tmp = t0z; t0z = t1z; t1z = tmp; }
    let tEnter = max(max(t0x, t0y), t0z);
    let tExit = min(min(t1x, t1y), t1z);
    if (tEnter > tExit || tEnter < 0.0 || tEnter >= bestT) { continue; }

    bestT = tEnter;
    out.hit = true;
    out.t = tEnter;  // cell units == world-voxel units: no conversion
    if (t0y >= t0x && t0y >= t0z) {
      out.axis = 1;
      out.sgn = sign(rd.y);
    } else if (t0x >= t0z) {
      out.axis = 0;
      out.sgn = sign(dpx);
    } else {
      out.axis = 2;
      out.sgn = sign(dpz);
    }
    // Dried tips per BLADE: past this strand's own tip fraction — or the whole
    // blade for the occasional dead one — the hit shades as the tip material.
    let hitRise = rise0 + entry.y + rd.y * tEnter;
    let dead = (hash3(colH, s, 0xD1EDu) & 7u) == 0u;
    out.mat = bodyMat;
    if (dead || hitRise > sH * (1.0 - tipFrac)) { out.mat = tipMat; }
  }
  return out;
}

// After the yaw swizzle above, the normal the DDA reports is in MODEL space and
// has to come back to world space or the lighting rotates with the variant.
// The inverse of each quarter turn is the quarter turn by the negated angle,
// which for a normal (a direction) is again a swap-and-negate.
fn microNormalToWorld(n : vec3f, flags : u32, h : u32) -> vec3f {
  if ((flags & MICROF_YAW) == 0u) { return n; }
  let q = h & 3u;
  if (q == 1u) { return vec3f(-n.z, n.y, n.x); }   // inverse of (x,z)->(z,-x)
  if (q == 2u) { return vec3f(-n.x, n.y, -n.z); }
  if (q == 3u) { return vec3f(n.z, n.y, -n.x); }
  return n;
}

const SUBOCC_SKIP : bool = false;

fn trace(ro : vec3f, rdIn : vec3f, maxSteps : i32, wantMedia : bool) -> Hit {
  var out : Hit;
  out.hit = false;
  out.saturated = false;
  out.tExit = 0.0;
  out.mediaTau = 0.0;
  out.mediaTint = vec3f(0.0);
  out.mediaMat = 0u;
  out.mediaSurf = 0.0;
  out.fireGlow = 0.0;
  out.fireMat = 0u;
  out.liqT = 0.0;
  out.liqCell = vec3<i32>(0);
  out.liqAxis = 1;
  out.liqSgn = -1.0;
  out.liqPath = 0.0;
  out.tsT = 0.0;
  out.tsCell = vec3<i32>(0);
  out.tsAxis = 1;
  out.tsSgn = -1.0;
  out.tsMat = 0u;
  out.tsPath = 0.0;
  out.micMat = 0u;

  // ---- micro-detail budget for THIS ray ----
  // `wantMedia` is exactly "this is the primary camera ray" at every call site
  // (shadow and reflection rays pass false), so it doubles as the micro gate.
  // That IS the v1 shadow policy from the plan: SHADOW RAYS SKIP MICRO CELLS
  // ENTIRELY and pass straight through them.
  //
  // Which is also why isRayBlocker excludes micro materials — the two have to
  // agree. A meadow therefore casts no shadow from its grass, which reads
  // correctly (grass shadows at 6 cm are sub-pixel at any normal viewing
  // distance) and costs nothing, where marching a brick per shadow ray would
  // multiply the most expensive ray in the frame by 3*subdiv.
  var microBudget = select(0, TUNE_MICRO_MAX_PER_RAY, wantMedia);

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  // clip to the residency window AABB (world coords)
  let nf = f32(WORLD_N);
  let wlo = vec3f(R.origin * i32(CHUNK));
  let tt0 = (wlo - ro) * inv;
  let tt1 = (wlo + vec3f(nf) - ro) * inv;
  let tmin = min(tt0, tt1);
  let tmax = max(tt0, tt1);
  let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, 0.0));
  var tExit = min(tmax.x, min(tmax.y, tmax.z));

  // ---- IN-WINDOW LOD HANDOFF (PLAN_surface_flight_perf.md A1) ----
  // The far-field cascade used to be reachable only after a ray left the whole
  // window, so everything within 25.6 m marched at full 10 cm resolution and a
  // hillside 25 m out cost the same per pixel as a wall 2 m out.
  //
  // WHAT THIS ACTUALLY BOUGHT, since the plan predicted much more: ~8-11% on
  // the offscreen 1080p sweep, saturating at 22-24 m (see tuning.h). The plan
  // held Part A responsible for ~46 ms of a 51 ms dense frame and expected
  // this to flatten the altitude curve; it does not. The altitude curve was
  // already gone before this change — f8c1bc7's free-probe batching removed
  // it, and the residual "46 ms render+present" the plan cited was measured on
  // a machine with competing GPU work (an unrelated app was respawning under
  // the harness; the same gate read 101 / 14 / 127 ms on three back-to-back
  // runs). On a quiet machine the whole offscreen frame is ~10 ms.
  //
  // So this is a real but small win, kept because it is also the mechanism any
  // future in-window LOD needs, not because it rescued the frame.
  //
  // The fix is to end the FINE march early and let the cascade cover the rest.
  // It is deliberately implemented as a clamp on tExit and nothing else,
  // because tExit is ALREADY the contract with the caller: fs() hands
  // h.tExit to traceFar as the start distance, so shortening it moves the
  // handoff without touching the handoff machinery (level selection, the
  // one-sided seam dither, the tPrev ordering that stops levels re-covering
  // each other). The cascade then starts at the LOD distance instead of at
  // the window face, which is the whole feature.
  //
  // Why a distance and not a projected-size test: px_per_cell for a fine cell
  // is VOXEL_METERS*H / (2*d*tan(fov/2)), so a true 1-px rule solves to a
  // distance anyway. Making that distance the knob keeps it inspectable and
  // hot-reloadable (F5) rather than burying it in a per-ray divide, and the
  // plan explicitly accepts a distance threshold as v1.
  //
  // THE CLAMP IS ONLY EVER A SHORTENING (min), which is what keeps it safe:
  //   * A ray whose window exit is already nearer than the handoff distance
  //     is untouched, so nothing changes underground or in interiors — where
  //     rays terminate in 1-3 m and the renderer was never the problem.
  //   * Shortening cannot open a gap the way a lengthened handoff could: the
  //     cascade's levels tile t-space from wherever they are told to start,
  //     and traceFar's own tPrev max() clamps against re-covering. The finer
  //     representation simply stops earlier and the coarser one takes over.
  //
  // SHADOW AND REFLECTION RAYS ARE EXCLUDED (wantMedia gates it). Those rays
  // are already budget-capped and, more importantly, a shadow ray that gave up
  // at 18 m would report "lit" for a receiver whose blocker is at 20 m --
  // unshadowing terrain rather than coarsening it. A3 handles shadow cost by
  // routing the whole ray through the cascade instead, which still answers the
  // occlusion question.
  if (wantMedia && TUNE_LOD_HANDOFF_DIST < WINDOW_HALF_EXTENT_METERS) {
    tExit = min(tExit, max(tEnter, TUNE_LOD_HANDOFF_DIST / VOXEL_METERS));
  }

  if (tExit <= tEnter) { return out; }
  out.tExit = tExit;

  var t = tEnter + 1e-4;
  var p = ro + rd * t;
  let wloI = R.origin * i32(CHUNK);
  var cell = clamp(vec3<i32>(floor(p)), wloI, wloI + vec3<i32>(i32(WORLD_N) - 1));
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
    tMax[a] = (boundary - ro[a]) * inv[a];
  }

  // which axis did we enter through (for first-cell normal)
  var axis = 0;
  if (tmin.y > tmin.x && tmin.y > tmin.z) { axis = 1; }
  else if (tmin.z > tmin.x && tmin.z > tmin.y) { axis = 2; }

  var tCur = t;
  // Optical depth from GASES only — drives the saturation early-out, which is
  // a smoke optimization and must not cut a ray short inside clear water.
  var gasTau = 0.0;

  // ---- PER-CHUNK LOOKUP CACHE ----
  // The loop needs four chunk-scope words (occupancy count, page-table entry,
  // two sub-chunk mask words) and every one of them is invariant for as long as
  // the ray stays inside a chunk — which is 16-48 steps in exactly the content
  // this function is slow on. Fetching them once per CHUNK instead of once per
  // CELL removes a load per step and, with voxWordAtEntry below, removes the
  // DEPENDENCY between the page-table load and the voxel load (plan item A4).
  //
  // MEASURED AT ZERO. Offscreen 1080p, ground camera: 6.22 ms cached against
  // 6.18-6.25 uncached; elevated 5.04-5.08 against 4.96-5.44. So A4's "the
  // page table made every step more expensive" is not true on this hardware —
  // pageTable[] is 128 KiB and the ray re-reads the same entry, so it was
  // already an L1 hit and the latency was already hidden. Kept because it is
  // strictly less work and it is what the mask below needs, not because it
  // bought anything.
  //
  // Safe because the buffers are read-only for the whole draw, and a ray cannot
  // revisit a slot index it has left: chunkIndexW aliases world cells WORLD_N
  // apart, but inBounds() breaks the loop at the window face long before a ray
  // could wrap onto its own slots.
  //
  // 0xFFFFFFFF is not a valid chunk index (they are < NUM_CHUNKS), so the first
  // iteration always misses and no separate priming step is needed.
  var cchIdx = 0xFFFFFFFFu;
  var cchOcc = 0u;
  var cchPt = 0u;
  var cchMask0 = 0u;
  var cchMask1 = 0u;
  // Which sub-chunk class this ray reads, decided once: media-aware primary
  // rays need TOTAL, media-blind shadow/reflection rays need BLOCKERS. Same
  // split as occTotal/occBlockers below, one level down.
  let subClass = select(1u, 0u, wantMedia);
  // With SUBOCC_SKIP false the two loads below are dead and Tint removes them,
  // so an off build pays nothing for the experiment being kept.
  let subLoad = SUBOCC_SKIP;

  for (var i = 0; i < 4096; i++) {
    if (i >= maxSteps) { break; }
    if (!inBounds(cell)) { break; }

    let chIdx = chunkIndexW(cell);
    if (chIdx != cchIdx) {
      cchIdx = chIdx;
      cchOcc = occupancy[chIdx];
      cchPt = pageEntryOf(chIdx);
      if (subLoad) {
        let mbase = subOccIndex(chIdx, subClass, 0u);
        cchMask0 = occupancy[mbase];
        cchMask1 = occupancy[mbase + 1u];
      }
    }

    // Chunk skip. Media-blind rays (shadows) skip on the BLOCKER count, so a
    // chunk holding only smoke/steam is as cheap as air; media-aware rays
    // need the per-cell march whenever anything at all is present.
    let occ = cchOcc;
    let occN = select(occBlockers(occ), occTotal(occ), wantMedia);

    // ---- SENTINEL-AWARE CHUNK RESOLUTION (PLAN_surface_flight_perf.md A5) ----
    // The page table is not just a memory map; it is a compressed DESCRIPTION
    // of a chunk's contents, and the renderer used to be blind to it. A
    // UNIFORM/JITTER chunk reports occupancy FULL (sim_occupancy.wgsl), so the
    // occupancy test below never fired for one and the ray marched 16-48 cells
    // through a body of stone it could have answered in one lookup — paying a
    // hash3 per cell on top, for JITTER.
    //
    // One extra load (the entry is in the same cache line neighbourhood
    // voxWordAt is about to touch anyway) buys two exits:
    //   blocker material  -> the whole chunk is solid, so the CURRENT cell is
    //                        solid: report the hit right here. No chunk-face
    //                        geometry needed — the DDA already arrived at this
    //                        cell through its entry face, and tCur/axis are
    //                        that crossing.
    //   non-blocking      -> nothing in the chunk can stop this ray class, so
    //                        take the same exit-face jump an empty chunk takes.
    //
    // The non-blocking arm is deliberately NARROW: it fires only for MAT_AIR
    // (which is PT_EMPTY, already covered by occupancy, but free to include)
    // and for a media-blind ray meeting a micro material — the case where a
    // whole chunk of grass must not stop a shadow ray, which is exactly
    // isRayBlocker's micro exclusion restated at chunk granularity. Anything
    // else (gas, translucent liquid, ice, a micro material under a PRIMARY
    // ray) still falls through to the per-cell march, because those cells
    // contribute to the pixel and skipping them would drop the media, the
    // Beer-Lambert path or the grass model itself.
    let ptEntry = cchPt;
    var chunkSkip = (occN == 0u);
    if ((ptEntry & PT_SENTINEL_BIT) != 0u) {
      let sMat = ptEntry & PT_MAT_MASK;
      if (sMat == MAT_AIR) {
        chunkSkip = true;
      } else if (isRayBlocker(materials[sMat]) &&
                 !(wantMedia && isTranslucentSolid(materials[sMat]))) {
        // Uniform blocker: this cell is that material. synthWordAt is the same
        // word voxWordAt would have produced, so stain/state/stamp are exactly
        // what the per-cell path would have reported.
        //
        // The isTranslucentSolid exclusion is load-bearing and easy to miss:
        // ice and glass are CLASS_SOLID and so ARE ray blockers (a shadow ray
        // must stop on them — see the translucent-solid branch below), but a
        // PRIMARY ray must keep marching through them to accumulate tsPath.
        // Reporting an opaque hit at the near face of a glacier would erase
        // the Beer-Lambert tint and everything behind it. Media-blind rays
        // keep the fast path, which is where a solid ice chunk is a pure win.
        out.hit = true;
        out.t = tCur;
        out.cell = cell;
        out.axis = axis;
        out.sgn = sign(rd[axis]);
        out.word = synthWordAt(ptEntry, cell, ptSeed());
        return out;
      } else if (!wantMedia && (materials[sMat].flags & MATF_MICRO) != 0u) {
        // A chunk of nothing but grass, seen by a shadow ray: passes straight
        // through, per the micro shadow policy at the top of trace().
        chunkSkip = true;
      }
    }

    // ---- SUB-CHUNK BLOCK SKIP (PLAN_surface_flight_perf.md A2) ----
    // IMPLEMENTED, MEASURED, DEFAULT OFF — the premise does not survive its
    // own arithmetic. Kept in the same shape A3's refutation is kept: flip
    // SUBOCC_SKIP and the experiment re-runs, and `--measure` (MEASUREMENT 1d)
    // still reports the content number that decides it.
    //
    // THE LAW, which is the part worth inheriting: the mean chord of a ray
    // through a box of side s is 4V/S = (2/3)s. A box-exit jump costs a floor(),
    // three tMax rebuilds and a divergent branch — call it 3-4 DDA steps. So a
    // skip level only pays when (2/3)s comfortably exceeds that. The CHUNK skip
    // pays because (2/3)*16 = 10.7 voxels. A 4-voxel block is (2/3)*4 = 2.7
    // voxels: it jumps over LESS ray than the jump itself costs. That is a
    // property of the granularity, not of this implementation, and no amount of
    // tuning the producer changes it.
    //
    // MEASURED, offscreen 1080p, interleaved arms on one binary:
    //   ground cam   base 6.18-6.22   jump 6.43-6.44   load-elide 6.39-6.43
    //   elevated off base 4.96-5.00   jump 5.26        load-elide 5.26
    // The load-elision form (mask says air -> skip voxWordAt, keep the DDA
    // step, no jump at all, so no divergence) was tried precisely because it
    // has no chord threshold. It loses too: the per-cell TEST alone costs more
    // than it saves.
    //
    // AND THE CEILING SAYS IT COULD NEVER HAVE BEEN MUCH. Capping the fine
    // primary march at 2 m instead of the shipped 24 m — i.e. deleting all the
    // marching this could ever optimise — takes the ground frame 6.22 -> 4.97
    // and the elevated 5.00 -> 3.89. The whole in-window fine march is ~1.26 ms
    // of a 6.2 ms frame, and `--measure` says only 35.9% of the volume of the
    // 2,636 chunks a ray must actually march is empty at this granularity
    // (26.7% of them have every block occupied). The y-slab variant — one
    // 16x4x16 jump, the right shape for a heightfield — covers only 17.88%.
    // The chunk test above is all-or-nothing at 1.6 m and keyed on a COUNT, so
    // one grass voxel in 4,096 forces the ray to march every cell of the chunk.
    // The mask (common.wgsl SUB-CHUNK OCCUPANCY) answers the same question for
    // a (1 << SUBOCC_SHIFT)-voxel block, in the class this ray actually cares
    // about, and a clear bit means "no cell of that class in this block" —
    // EXACTLY the premise the chunk skip already runs on, so the jump below is
    // the chunk jump with a smaller box and needs no new argument about
    // correctness.
    //
    // The blocker class is not an approximation for a media-blind ray: gas and
    // thin liquid fall through `if (wantMedia)`, micro falls through its own
    // branch, ice/glass are CLASS_SOLID and so ARE blockers. Every cell a clear
    // blocker block can contain is a cell that ray would have stepped past.
    //
    // Sentinels never reach here: the branch above either returned a hit, set
    // chunkSkip, or left a chunk whose mask is conservatively all-ones.
    // Everything on the MARCHING path costs every step of every ray in the
    // frame, so the test is the whole of it: an AND with a chunk-local mask,
    // two shifts and a bit test against a value already in a register. The
    // block corner is derived only in the branch that jumps.
    var subSkip = false;
    if (SUBOCC_SKIP && !chunkSkip) {
      let sbit = subOccBitLocal(vec3<u32>(cell & vec3<i32>(CHUNK_MASK)));
      let mw = select(cchMask1, cchMask0, sbit < 32u);
      subSkip = (mw & (1u << (sbit & 31u))) == 0u;
    }

    if (chunkSkip || subSkip) {
      // empty box (whole chunk, or one sub-chunk block): jump to its exit face.
      // Masking off the low bits is floor-to-box-corner for negative world
      // coords too, and is cheaper than the shift-and-multiply worldChunkOf
      // pair it replaces.
      let blkSize = select(i32(CHUNK), i32(SUBOCC_BLOCK), subSkip);
      let blkLo = cell & vec3<i32>(~(blkSize - 1));
      let lo = vec3f(blkLo);
      let hi = lo + f32(blkSize);
      let e0 = (lo - ro) * inv;
      let e1 = (hi - ro) * inv;
      let ex = max(e0, e1);
      // The jump target must never sit behind the ray: a cell floor()ed onto a
      // shared face belongs to a box the ray is already exiting, so the raw
      // exit t can be <= tCur and the march would stall in place.
      let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
      t = tOut + 1e-4;
      if (t >= tExit) { break; }
      p = ro + rd * t;
      var nc = vec3<i32>(floor(p));
      // Force the crossing on the exit axis: float noise at a shared face can
      // floor() back into the box just exited, which reads as a see-through
      // seam along box boundaries.
      //
      // `axis` IS updated here, and it was not before A2 — a latent wart the
      // sub-chunk skip would have made loud. The DDA carries `axis` as "which
      // face did the ray enter the current cell through", and a jump that left
      // it alone reported the last DDA step's face (often the WINDOW entry face,
      // for a ray that skipped sky the whole way) as the normal of whatever it
      // hit in the very first cell after landing. Rare when boxes are 16 voxels
      // and a chunk usually has air above its terrain; common when boxes are 4
      // and the ray lands right on the surface. The box exit face is exactly the
      // face the new cell is entered through, so this is the same answer the DDA
      // would have produced had it stepped there one cell at a time.
      if (ex.x <= ex.y && ex.x <= ex.z) {
        nc.x = select(blkLo.x - 1, blkLo.x + blkSize, rd.x > 0.0);
        axis = 0;
      } else if (ex.y <= ex.z) {
        nc.y = select(blkLo.y - 1, blkLo.y + blkSize, rd.y > 0.0);
        axis = 1;
      } else {
        nc.z = select(blkLo.z - 1, blkLo.z + blkSize, rd.z > 0.0);
        axis = 2;
      }
      if (!inBounds(nc)) { break; }
      cell = nc;
      for (var a = 0; a < 3; a++) {
        let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
        tMax[a] = (boundary - ro[a]) * inv[a];
      }
      tCur = t;
      continue;
    }

    // The chunk's table entry is already in a register (see the per-chunk
    // lookup cache), so this is one INDEPENDENT load instead of voxWordAt's
    // dependent pair.
    let w = voxWordAtEntry(cchPt, cell);
    let mat = voxMat(w);
    var weight = 0.0;   // this cell's media contribution per unit length
    var cellOp = 0.0;   // this cell's opacity (per-cell, not first-material)
    var cellTint = vec3f(0.0);
    var cellFire = 0.0; // this cell's flicker-weighted emission
    var cellLiq = 0.0;
    var cellTS = 0.0;
    var cellWaterY = 0.0;
    if (mat != MAT_AIR) {
      let k = materials[mat].klass;
      // gases and translucent liquids are participating media; OPAQUE liquids
      // (lava, molten glass) read as surfaces
      if (k == CLASS_GAS ||
          (k == CLASS_LIQUID && (materials[mat].flags & MATF_OPAQUE) == 0u)) {
        // Partial-fill geometry: settled liquid fills from the bottom.
        // The air gap above the fill line must not contribute media or
        // record a liquid interface — a ray that never descends to waterY
        // passes through as air.
        var liqInAir = false;
        if (k == CLASS_LIQUID) {
          cellWaterY = f32(cell.y) + f32(voxState(w) + 1u) / 8.0;
          if (cellWaterY < f32(cell.y) + 1.0 - 1e-4) {
            let entryY = ro.y + rd.y * tCur;
            if (entryY > cellWaterY + 1e-4 && rd.y >= -1e-6) {
              liqInAir = true;
            }
          }
        }
        if (wantMedia && !liqInAir) {
          weight = 1.0;
          cellOp = f32(materials[mat].opacity) / 255.0;
          cellTint = (unpackColor(materials[mat].color0) +
                      unpackColor(materials[mat].color1)) * 0.5;
          if ((R.flags & 2u) != 0u) {
            let gs = chunkSlotIndex(worldChunkOf(cell));
            let snapTick = dirtyViz[gs];
            if (snapTick != 0u) {
              let st = voxStamp(w);
              if (st == stampFor(snapTick, 0u) || st == stampFor(snapTick, 1u)) {
                cellTint = vec3f(1.0, 0.05, 0.05);
                cellOp = 1.0;
              }
            }
          }
          if (out.mediaMat == 0u) {
            out.mediaMat = mat;
            out.mediaSurf = select(1.0, f32(voxState(w) + 1u) / 8.0,
                                   k == CLASS_LIQUID);
          }
          if (k == CLASS_LIQUID) {
            cellLiq = 1.0;
          }
          // emissive media (fire): per-cell spatio-temporal flicker so each
          // flame voxel pulses on its own phase instead of the whole plume
          // beating in sync (render-only floats, same trick as ember surfaces)
          if (materials[mat].emission > 0u) {
            let fh = pcg(bitcast<u32>(cell.x * 7 + cell.y * 131 + cell.z * 2917));
            let fl = TUNE_FIRE_FLICKER_BASE + TUNE_FIRE_FLICKER_AMP * sin(R.time * TUNE_FIRE_FLICKER_RATE + f32(fh & 0x3FFu) * 0.00614);
            cellFire = (f32(materials[mat].emission) / 255.0) * fl;
            if (out.fireMat == 0u) { out.fireMat = mat; }
          }
        }
        // fall through and keep marching
      } else if (wantMedia && isTranslucentSolid(materials[mat])) {
        // ---- translucent solid: a surface AND a volume ----
        // Ice and glass do not stop the ray. It keeps marching so whatever is
        // behind still shades normally, and the distance travelled inside
        // (tsPath) drives the Beer-Lambert tint in shadeTranslucent.
        //
        // `wantMedia` gates this deliberately. Media-blind rays are the SHADOW
        // rays, and they must keep treating ice as a blocker: it is what the
        // sim already believes (isRayBlocker, which seesSky reads to decide
        // that ice shades the water under it and stops it re-freezing), and a
        // shadow ray that passed through ice would disagree with the sim about
        // what is lit. Ice therefore casts a solid shadow — physically it is a
        // dense scatterer, so that reads correctly.
        cellTS = 1.0;
        if (out.tsT == 0.0) {
          out.tsT = tCur;
          out.tsCell = cell;
          out.tsAxis = axis;
          out.tsSgn = sign(rd[axis]);
          out.tsMat = mat;
        }
        // fall through and keep marching
      } else if (microBudget > 0 && (materials[mat].flags & MATF_MICRO) != 0u &&
                 microBricks[mat].base != MICRO_NONE) {
        // ---- static micro-detail: substitute a subdiv^3 model for this cell --
        // See traceMicro. The cell is an ordinary solid voxel as far as the sim
        // is concerned; only the RAY treats it as a finer model.
        //
        // LOD: past TUNE_MICRO_LOD_DIST a world cell is roughly one pixel, so
        // the nested march is spending 3*subdiv steps to decide the colour of a
        // sub-pixel — the model's silhouette cannot survive the sample anyway.
        // Beyond it the cell shades as a plain voxel, which is not merely
        // cheaper but the SAME answer averaged, and it keeps distant meadows
        // reading as continuous ground instead of dissolving into stipple.
        if (tCur * VOXEL_METERS > TUNE_MICRO_LOD_DIST) {
          out.hit = true;
          out.t = tCur;
          out.cell = cell;
          out.axis = axis;
          out.sgn = sign(rd[axis]);
          out.word = w;
          return out;
        }
        // Cell-local entry point. tCur is where the ray crossed INTO this cell
        // (the DDA sets it at the step that arrived here), so ro + rd*tCur
        // minus the cell corner is a 0..1 coordinate on each axis.
        let entry = (ro + rd * tCur) - vec3f(cell);
        let mb = microBricks[mat];
        var mh : MicroHit;
        if ((mb.flags & MICROF_STRANDS) != 0u) {
          mh = traceStrands(mb, cell, clamp(entry, vec3f(0.0), vec3f(1.0)), rd);
        } else {
          mh = traceMicro(mb, cell, clamp(entry, vec3f(0.0), vec3f(1.0)),
                          rd, R.tick);
        }
        microBudget -= 1;
        if (mh.hit) {
          out.hit = true;
          out.t = tCur + mh.t;
          out.cell = cell;
          out.axis = mh.axis;
          out.sgn = mh.sgn;
          // Keep the world voxel's word (stamp/stain/state travel with the
          // CELL, not with the sub-voxel) but report the micro material
          // separately, so fs() shades the blade's colour on the tuft's stain.
          out.word = w;
          out.micMat = mh.mat;
          return out;
        }
        // MISS — and this is the crucial half. The ray passes through: fall out
        // of the `if` and let the world DDA step past the cell exactly as if it
        // were air. A micro cell that blocked on a miss would render every tuft
        // of grass as a solid 6 cm cube.
        //
        // Note what happens once `microBudget` reaches 0: this branch stops
        // matching and control drops to the plain-solid `else` at the bottom,
        // so a ray that has already entered TUNE_MICRO_MAX_PER_RAY bricks
        // treats the next micro cell as SOLID. That is the intended bound —
        // terminating is bounded and reads as distant ground, where letting the
        // ray fly on unbounded would punch a hole through a whole meadow.
      } else if (!wantMedia && (materials[mat].flags & MATF_MICRO) != 0u) {
        // ---- shadow / reflection ray meets a micro cell ----
        // Pass straight through. These rays never march bricks (see the
        // microBudget comment at the top of trace), and treating the cell as
        // solid instead would make a grass blade cast a full 6 cm cube of
        // shadow — the exact artifact isRayBlocker's micro exclusion exists to
        // prevent, and the two must agree or chunk skipping and per-cell
        // marching would disagree about the same meadow.
        //
        // Falls through with no state written, so the DDA steps past it as air.
      } else {
        out.hit = true;
        out.t = tCur;
        out.cell = cell;
        out.axis = axis;
        out.sgn = sign(rd[axis]);
        out.word = w;
        return out;
      }
    }

    // Save pre-step state for deferred liquid-interface recording.
    let marchCell = cell;
    let marchAxis = axis;
    let tPrev = tCur;
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
    let segRaw = tCur - tPrev;
    // Geometric water-surface clipping for partial liquid cells.
    // waterFrac = fraction of the segment the ray spends BELOW cellWaterY.
    // Full cells: waterFrac stays 1.0 (no clip). The geometric model
    // replaces the old fullness-as-density scaling: the water volume is at
    // full density, and only the submerged portion of the segment counts.
    var waterFrac = 1.0;
    if (cellLiq > 0.0) {
      let wy = cellWaterY;
      let y0 = ro.y + rd.y * tPrev;
      let y1 = ro.y + rd.y * tCur;
      if (wy < f32(marchCell.y) + 1.0 - 1e-4) {
        if (y0 >= wy && y1 >= wy) {
          waterFrac = 0.0;
        } else if (y0 >= wy || y1 >= wy) {
          if (abs(rd.y) > 1e-6 && segRaw > 1e-6) {
            let tCross = clamp((wy - ro.y) / rd.y, tPrev, tCur);
            waterFrac = select((tCross - tPrev) / segRaw,
                               (tCur - tCross) / segRaw,
                               y0 >= wy);
          }
        }
      }
      // Deferred liquid-interface recording: only record liqT once the
      // segment confirms the ray actually entered water in this cell.
      if (out.liqT == 0.0 && waterFrac > 0.0) {
        if (wy < f32(marchCell.y) + 1.0 - 1e-4 && y0 > wy + 1e-4) {
          // Entered above the fractional water surface — the interface is
          // at the Y plane where the ray descends to waterY.
          out.liqT = clamp((wy - ro.y) / rd.y, tPrev, tCur);
          out.liqCell = marchCell;
          out.liqAxis = 1;
          out.liqSgn = -1.0;
        } else {
          out.liqT = tPrev;
          out.liqCell = marchCell;
          out.liqAxis = marchAxis;
          out.liqSgn = sign(rd[marchAxis]);
        }
      }
    }
    out.liqPath += segRaw * cellLiq * waterFrac;
    out.tsPath += segRaw * cellTS;
    let seg = segRaw * weight * select(1.0, waterFrac, cellLiq > 0.0);
    if (seg > 0.0 && cellOp > 0.0) {
      // fire is dimmed by the media already crossed in front of it, so flames
      // deep inside their own smoke fade out instead of x-raying the plume
      if (cellFire > 0.0) {
        let trans = exp(-out.mediaTau * VOXEL_METERS * MEDIA_ABSORB);
        out.fireGlow += seg * cellFire * trans;
      }
      let dTau = seg * cellOp;
      out.mediaTau += dTau;
      out.mediaTint += cellTint * dTau;
      if (cellLiq == 0.0) { gasTau += dTau; }
    }
    // Media early-out: fs() will mix the background in at exp(-tau); once
    // that is ~0 the rest of the march (often hundreds of per-voxel steps
    // through a smoke plume) cannot change the pixel.
    //
    // GASES ONLY. This is a smoke optimization, and applying it to liquids is
    // what kept lake beds invisible: water's authored opacity (90/255) against
    // the legacy MEDIA_ABSORB saturates after ~2.7 m of path, so any lake
    // deeper than waist height — or any shallow one viewed at a grazing angle,
    // which is most of them — terminated the ray in mid-water and reported no
    // hit. shadeWater() attenuates with real per-channel Beer-Lambert instead,
    // under which 1.5 m of water still transmits ~53% green / ~74% blue, so
    // the bed is genuinely visible and the march has to reach it.
    // Liquids get their own far looser cap below.
    if (gasTau * VOXEL_METERS * MEDIA_ABSORB > MEDIA_TAU_MAX) {
      out.saturated = true;
      out.t = tCur;
      break;
    }
    // Liquid depth cap: past this much water even blue is gone (exp(-0.2*24)
    // ~ 0.8%), so the bed cannot affect the pixel and the march can stop.
    // Purely a perf bound on very deep water, ~24 m of path.
    if (out.liqPath * VOXEL_METERS > 24.0) {
      out.saturated = true;
      out.t = tCur;
      break;
    }
    // Same bound for translucent solids. Ice absorbs far more per metre than
    // water, so this cuts in much sooner: past TUNE_ICE_DEPTH_MAX metres of
    // glacier the far side cannot affect the pixel, and without a cap a ray
    // entering a large ice body would march it voxel by voxel to the horizon.
    if (out.tsPath * VOXEL_METERS > TUNE_ICE_DEPTH_MAX) {
      out.saturated = true;
      out.t = tCur;
      break;
    }
    if (tCur >= tExit) { break; }
  }
  return out;
}

// ---- far-field cascade march (DESIGN.md §9) ----
// Continues a ray that left the residency window without hitting anything.
// Each cascade level is marched in ITS OWN cell units (the same DDA as the
// fine march, occupancy-skipped per level chunk); `t` values convert back to
// fine-voxel units so depth and fog reuse the existing math. Levels are
// nested boxes: starting level k at max(its entry, level k-1's exit) makes
// the t-ordering skip every region covered by finer data automatically.
//
// ---- level-transition dither (plan phase 3A) ----
// Every handoff above is a HARD distance: at one exact t the representation
// jumps from 2^k-voxel cells to 2^(k+1)-voxel cells, and because the two
// resolutions disagree about where the surface is by up to half a coarse
// cell, that constant-t surface draws as a visible arc across hillsides —
// the same artifact as an unblended terrain-LOD ring.
//
// Fix: pull each handoff distance NEARER by a per-pixel random amount of up to
// half an OUTER cell at that seam. Neighboring pixels then cross the seam at
// slightly different distances, so the ring dissolves into a stipple that
// reads as texture instead of as a line. This is stratified sampling of the
// seam, not a blend: each pixel still picks exactly one level, so there is no
// extra marching cost.
//
// THE OFFSET IS ONE-SIDED (nearer only), and that is not a style choice. The
// levels tile t-space exactly: level k+1 starts where level k's box ends.
// Pushing a handoff FARTHER opens a gap that no level marches, and rays through
// it fall past the terrain into whatever is behind — measured as ~3.2k pixels
// of hole-speckle punched through tree edges when this was first written
// two-sided. Pulling it NEARER only makes the coarser level re-cover a sliver
// the finer level already marched and found empty, which is exactly the
// intended "this pixel takes the coarse surface a bit early".
//
// Two rules the hash must obey:
//   * NO TIME INPUT. A time-varying hash makes the stipple crawl, which is
//     far more objectionable than the seam it replaces; the pattern must be
//     frozen to the pixel so it reads as static dither.
//   * KEYED ON PIXEL, NOT ON WORLD POSITION. The seam is a screen-space
//     artifact of the camera-centered cascade boxes, so screen space is where
//     it must be broken up; a world-space key would leave the pattern
//     stationary in the world and re-align into arcs as the boxes recenter.
// Render-only float math on render-only data — determinism is untouched
// (CLAUDE.md rule 1 scopes to sim state).
//
// Returns an offset in [0, 0.5) cells, uniform-ish per pixel, to SUBTRACT.
fn farDither(px : vec2f) -> f32 {
  let h = pcg(u32(px.x) * 1973u + u32(px.y) * 9277u + 0x9E3779B9u);
  // 16 bits of mantissa is plenty: the seam only needs enough distinct
  // offsets that no two adjacent pixels line up, not a smooth distribution.
  return f32(h & 0xFFFFu) * (0.5 / 65536.0);
}

struct FarHit {
  hit   : bool,
  t     : f32,         // fine-voxel units
  axis  : i32,
  sgn   : f32,
  mat   : u32,
  cell  : vec3<i32>,   // level cells (palette jitter, AO neighbors)
  level : u32,         // which cascade level the hit lives in (shadow march)
};

fn farMatAt(level : u32, c : vec3<i32>) -> u32 {
  let bi = farVoxByteIndex(level, c);
  return (farVox[bi >> 2u] >> ((bi & 3u) * 8u)) & 0xFFu;
}

fn traceFar(ro : vec3f, rdIn : vec3f, tStart : f32, px : vec2f) -> FarHit {
  var out : FarHit;
  out.hit = false;

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;

  // One draw per pixel, reused at every seam scaled by that seam's cell size:
  // the offsets stay correlated across levels for one pixel (a pixel that
  // takes the coarse side early keeps taking it), which stipples cleanly
  // instead of re-randomizing into per-level speckle.
  let dith = farDither(px);

  // Window -> level 1 seam. tStart is where the ray left the residency window;
  // the fine march already found no hit out to there, so starting level 1 up to
  // half a level-1 cell earlier only lets it re-cover the last sliver of
  // already-marched empty fine space. It exists to break the hard line where
  // the two representations of the same terrain disagree, nothing more.
  var tPrev = max(tStart - dith * f32(1u << farCellShift(1u)), 0.0);

  for (var level = 1u; level <= FAR_LEVELS; level++) {
    let s = f32(1u << farCellShift(level));   // fine voxels per level cell
    let org = F.origins[level - 1u].xyz;
    // everything below is in LEVEL-CELL coords: pos/s, t/s (same rd)
    let roL = ro / s;
    let lo = vec3f(org * i32(CHUNK));
    let tt0 = (lo - roL) * inv;
    let tt1 = (lo + f32(FAR_N) - roL) * inv;
    let tmin = min(tt0, tt1);
    let tmax = max(tt0, tt1);
    let tEnter = max(max(tmin.x, tmin.y), max(tmin.z, tPrev / s));
    let tExit = min(tmax.x, min(tmax.y, tmax.z));
    if (tExit <= tEnter) { continue; }   // box missed (or fully behind tPrev)

    var t = tEnter + 1e-4;
    var p = roL + rd * t;
    let loI = org * i32(CHUNK);
    var cell = clamp(vec3<i32>(floor(p)), loI, loI + vec3<i32>(i32(FAR_N) - 1));
    let stepv = vec3<i32>(sign(rd));
    let tDelta = abs(inv);
    var tMax : vec3f;
    for (var a = 0; a < 3; a++) {
      let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
      tMax[a] = (boundary - roL[a]) * inv[a];
    }
    var axis = 0;
    if (tmin.y > tmin.x && tmin.y > tmin.z) { axis = 1; }
    else if (tmin.z > tmin.x && tmin.z > tmin.y) { axis = 2; }
    var tCur = t;

    for (var i = 0; i < TUNE_FAR_STEPS; i++) {
      if (!farInBox(cell, org)) { break; }
      if (farOcc[farOccIndex(level, cell)] == 0u) {
        // empty level chunk: jump to its exit face (same seam-safe jump as
        // the fine march — force the crossing on the exit axis)
        let ch = worldChunkOf(cell);
        let clo = vec3f(ch * i32(CHUNK));
        let e0 = (clo - roL) * inv;
        let e1 = (clo + f32(CHUNK) - roL) * inv;
        let ex = max(e0, e1);
        let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
        t = tOut + 1e-4;
        if (t >= tExit) { break; }
        p = roL + rd * t;
        var nc = vec3<i32>(floor(p));
        if (ex.x <= ex.y && ex.x <= ex.z) {
          nc.x = select(ch.x * i32(CHUNK) - 1, (ch.x + 1) * i32(CHUNK), rd.x > 0.0);
        } else if (ex.y <= ex.z) {
          nc.y = select(ch.y * i32(CHUNK) - 1, (ch.y + 1) * i32(CHUNK), rd.y > 0.0);
        } else {
          nc.z = select(ch.z * i32(CHUNK) - 1, (ch.z + 1) * i32(CHUNK), rd.z > 0.0);
        }
        if (!farInBox(nc, org)) { break; }
        cell = nc;
        for (var a = 0; a < 3; a++) {
          let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
          tMax[a] = (boundary - roL[a]) * inv[a];
        }
        tCur = t;
        continue;
      }

      let mat = farMatAt(level, cell);
      if (mat != 0u) {
        out.hit = true;
        out.t = tCur * s;   // back to fine-voxel units
        out.axis = axis;
        out.sgn = sign(rd[axis]);
        out.mat = mat;
        out.cell = cell;
        out.level = level;
        return out;
      }

      if (tMax.x < tMax.y && tMax.x < tMax.z) {
        cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
      } else if (tMax.y < tMax.z) {
        cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
      } else {
        cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
      }
      if (tCur >= tExit) { break; }
    }
    // Level k -> k+1 seam. The outer level's cells are 2s fine voxels, so half
    // a coarse cell is s: pull this handoff up to s nearer and the ring where
    // level k's box ends dissolves. Still max()'d against the running tPrev so
    // the start can never precede an even earlier level's coverage.
    tPrev = max(tPrev, tExit * s - dith * 2.0 * s);
  }
  return out;
}

// ---- far-field sun shadows (phase 4: distance look) ----
// One coarse DDA toward the sun through the SAME cascade level the hit lives
// in, occupancy-skipped and hard-capped. Lighting mismatch is what makes LOD
// terrain read as "LOD terrain": the near field casts real shadow rays, so an
// unshadowed far field renders every hillside and every spot under a canopy at
// identical brightness and the horizon flattens into wallpaper. One level only
// — a sun ray leaves the hit level's box within a few dozen cells, and
// cross-level shadow reach buys nothing visible through fog at that range.
// Render-only float math on render-only data (CLAUDE.md rule 1 scopes to sim).
// ---- THE REACH MUST BE A DISTANCE, NOT A STEP COUNT ----
// This loop used to run a bare `for (i = 0; i < 128; i++)`, which silently ties
// the shadow's WORLD-SPACE reach to the level's cell size: 128 steps is 128
// cells, so a level whose cells are half as wide casts a shadow ray half as
// far. That coupling is invisible until the cascade's shift base moves - when
// every cell halved (see kFarShiftBase in world.h), the near levels' rays
// stopped terminating on a caster inside their budget and every one of them
// burned the full 128 steps plus occupancy lookups, turning a 10.5 ms frame
// into a 606 ms one: a 58x cliff out of a constant that reads like a safety
// cap.
//
// The budget below is therefore expressed in METERS and converted into this
// level's cells, so the shadow reaches the same distance into the world at
// every level and the step count falls out of the geometry. The clamp bounds
// both ends: never so few steps that a coarse level cannot leave its own cell,
// never more than the old cap, which is what protects the frame.
fn farShadowSteps(level : u32) -> i32 {
  let cellM = f32(1u << farCellShift(level)) * VOXEL_METERS;
  return clamp(i32(TUNE_FAR_SHADOW_REACH / max(cellM, 1e-4)), 8, 128);
}

// Which cascade level covers a point, given as a distance from the camera in
// FINE voxels. The levels are nested boxes centred on the camera whose
// half-extents double (world.h: level k's half-extent is 2^k window radii), so
// the covering level is the first whose box contains the point.
//
// This exists for A3: a hit found by the FINE march has no `level` of its own,
// but routing its shadow through the cascade needs one. Walking the levels
// rather than computing a log keeps it agreeing with kFarHalfExtentMeters by
// construction — the same derivation traceFar's box tests use — instead of
// re-deriving the geometry with a formula that could drift from it.
//
// Clamped to FAR_LEVELS: a point past the outermost box is behind ~99% fog
// anyway (see the fog pin in world.h), so the outermost level is the right
// answer for it and there is nothing beyond to fall through to.
fn farLevelForDist(distFine : f32) -> u32 {
  let dM = distFine * VOXEL_METERS;
  for (var k = 1u; k <= FAR_LEVELS; k++) {
    // Level k half-extent in metres = FAR_N/2 * 2^(k+FAR_SHIFT_BASE) * VOXEL_METERS.
    let halfM = f32(FAR_N) * 0.5 * f32(1u << farCellShift(k)) * VOXEL_METERS;
    if (dM <= halfM) { return k; }
  }
  return FAR_LEVELS;
}

fn farShadowed(level : u32, roFine : vec3f) -> bool {
  var rd = keyLightDir();
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;
  let s = f32(1u << farCellShift(level));
  let roL = roFine / s;
  let org = F.origins[level - 1u].xyz;
  let lo = vec3f(org * i32(CHUNK));
  let tt0 = (lo - roL) * inv;
  let tt1 = (lo + f32(FAR_N) - roL) * inv;
  let tExit = min(max(tt0.x, tt1.x), min(max(tt0.y, tt1.y), max(tt0.z, tt1.z)));

  var cell = vec3<i32>(floor(roL));
  let stepv = vec3<i32>(sign(rd));
  let tDelta = abs(inv);
  var tMax : vec3f;
  for (var a = 0; a < 3; a++) {
    let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
    tMax[a] = (boundary - roL[a]) * inv[a];
  }
  var tCur = 0.0;
  let steps = farShadowSteps(level);
  for (var i = 0; i < steps; i++) {
    if (!farInBox(cell, org)) { return false; }
    if (farOcc[farOccIndex(level, cell)] == 0u) {
      // empty level chunk: jump to its exit face (seam-safe, as in traceFar)
      let ch = worldChunkOf(cell);
      let clo = vec3f(ch * i32(CHUNK));
      let e0 = (clo - roL) * inv;
      let e1 = (clo + f32(CHUNK) - roL) * inv;
      let ex = max(e0, e1);
      let tOut = max(min(ex.x, min(ex.y, ex.z)), tCur);
      let t = tOut + 1e-4;
      if (t >= tExit) { return false; }
      let p = roL + rd * t;
      var nc = vec3<i32>(floor(p));
      if (ex.x <= ex.y && ex.x <= ex.z) {
        nc.x = select(ch.x * i32(CHUNK) - 1, (ch.x + 1) * i32(CHUNK), rd.x > 0.0);
      } else if (ex.y <= ex.z) {
        nc.y = select(ch.y * i32(CHUNK) - 1, (ch.y + 1) * i32(CHUNK), rd.y > 0.0);
      } else {
        nc.z = select(ch.z * i32(CHUNK) - 1, (ch.z + 1) * i32(CHUNK), rd.z > 0.0);
      }
      if (!farInBox(nc, org)) { return false; }
      cell = nc;
      for (var a = 0; a < 3; a++) {
        let boundary = f32(cell[a]) + select(0.0, 1.0, rd[a] > 0.0);
        tMax[a] = (boundary - roL[a]) * inv[a];
      }
      tCur = t;
      continue;
    }
    if (farMatAt(level, cell) != 0u) { return true; }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      cell.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x;
    } else if (tMax.y < tMax.z) {
      cell.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y;
    } else {
      cell.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z;
    }
    if (tCur >= tExit) { return false; }
  }
  return false;
}

// Aerial perspective: distance fog that converges EXACTLY to the sky color in
// that ray's direction. The old `skyColor * 0.9` target left every distant
// surface hanging slightly darker than the sky it should dissolve into, which
// read as a gray veil over the whole horizon instead of atmosphere.
fn applyAerial(color : vec3f, rd : vec3f, tFine : f32) -> vec3f {
  let f = 1.0 - exp(-tFine * VOXEL_METERS * R.fogDensity);
  // Star-free sky. Aerial perspective is AIR between the eye and a SOLID
  // surface, so it must converge to the airglow in that direction and nothing
  // else. Using the full skyColor() here mixes the starfield, moon and
  // nebulae into distant terrain — which renders stars *through* hillsides,
  // trees and the ground near the horizon, because those surfaces are exactly
  // the ones carrying the most fog.
  return mix(color, skyAirglow(rd), f);
}

// ============================================================================
// OPAQUE SURFACE LOOK (DESIGN.md §9)
// ============================================================================
// Everything below shades a solid/powder voxel face. It replaces what used to
// be four lines — palette pick, a hardcoded per-axis constant, a binary shadow
// ray, and a flat 0.38 ambient — which is why terrain read as matte plastic:
// every up-facing voxel in the frame returned the exact same brightness, so
// there was no ambient occlusion anywhere, no sky/bounce color split, and the
// only spatial variation in the whole image was per-voxel palette confetti.
//
// All render-only float math on render-only data — the sim never sees any of
// it and the world hash never covers it (CLAUDE.md rule 1 scopes to sim state).

// ---- hemisphere ambient ----
// Ambient was a scalar, and a scalar ambient is the single strongest "untextured
// prototype" cue there is: it lights the underside of an overhang exactly as
// brightly, and in exactly the same hue, as a face pointing at open sky.
//
// Real outdoor ambient has two very different sources, and splitting them costs
// one mix(): the SKY (cool, from above) and BOUNCE off the ground (warm, from
// below, since sunlight that missed the surface hit the dirt first). Terrain
// shaded this way gets its form back for free — north faces go blue-shifted,
// undersides go earth-toned, and the eye reads that split as shape.
fn ambientAt(n : vec3f) -> vec3f {
  // n.y = -1 -> full bounce, n.y = +1 -> full sky. Day/night: at night the
  // sky term is replaced by a much dimmer, bluer moon/starlight ambient, and
  // the warm ground bounce nearly vanishes (there is no sun to bounce).
  // Scaling the SAME split rather than adding a separate night ambient keeps
  // the shape cues the sky/ground split buys. MUST MATCH ambientAtP
  // (common.wgsl), which the raster body paths use; inlined here because this
  // sits in per-hit shading loops (see keyLightColor).
  let base = mix(TUNE_AMB_GROUND, TUNE_AMB_SKY, n.y * 0.5 + 0.5);
  let nightAmb = mix(TUNE_NIGHT_AMB_GROUND, TUNE_NIGHT_AMB_SKY, n.y * 0.5 + 0.5);
  // Both moons fill here — the secondary one is real ambient on a night when
  // they are both up, which is the payoff for having two. Normalised against
  // moon A's own intensity so the 0.30/1.40 ramp (tuned when there was one
  // moon) still means the same thing when only A is up.
  let inv = 1.0 / max(TUNE_MOON_LIGHT_INTENSITY, 1e-4);
  let a = moonContribP(R.moonDir, R.moonPhase, TUNE_MOON_LIGHT_INTENSITY) * inv;
  let b = moonContribP(R.moon2Dir, R.moon2Phase, TUNE_MOON2_LIGHT_INTENSITY) * inv;
  let moonAmt = 0.30 * step(0.001, a + b) + 1.40 * (a + b) * 0.5;
  return mix(nightAmb * (0.45 + moonAmt), base, eclipseDayWeightP(R));
}

// ---- diffuse response ----
// Plain max(dot(n,l),0) is wrong for terrain built out of axis-aligned voxel
// faces, and it is the specific reason a grassy hillside rendered as harsh
// horizontal banding. A voxel slope is a STAIRCASE: every 1-voxel rise puts a
// vertical face next to a horizontal one. With a hard Lambert term the top face
// gets dot ~= 0.66 and the away-facing riser gets exactly 0, so the two
// alternate at ~1.8x brightness down the whole hill. The eye reads that
// alternation as noise, not as slope, because a real grass slope has no such
// discontinuity — the two facets differ by a few percent, not by 80%.
//
// Wrapped diffuse fixes it at the source: remap dot from [-1,1] so the falloff
// continues smoothly past the terminator instead of clamping to zero. This is
// the standard cheap stand-in for the light a rough/scattering surface picks up
// at grazing angles, and it keeps risers lit enough to sit next to their tops.
fn wrapDiffuse(ndl : f32, wrap : f32) -> f32 {
  return clamp((ndl + wrap) / (1.0 + wrap), 0.0, 1.0);
}

// ---- voxel-scale grain ----
// The palette variants are picked by the state nibble, which worldgen fills
// with `rnd % 3` — white noise. Three colors at ~8% lightness spread, assigned
// independently per voxel, is precisely the recipe for the green confetti that
// covered every grass field: maximum spatial frequency at maximum contrast.
//
// Fix without touching sim state (the nibble is hashed into the world hash, so
// worldgen cannot change): keep the palette pick, but modulate it with a
// SMOOTH, correlated value-noise field so neighbouring voxels agree. That turns
// per-voxel static into patches that read as material variation. Two octaves at
// different world scales: a broad one for large-scale mottling, a fine one that
// still varies per voxel but at a fraction of the amplitude.
fn vnHash(c : vec3<i32>) -> f32 {
  return f32(pcg(u32(c.x * 374761393 + c.y * 668265263 + c.z * 1274126177)) &
             0xFFFFu) * (1.0 / 65535.0);
}
// Trilinear value noise over a lattice of `scale` voxels.
fn valueNoise(p : vec3f, scale : f32) -> f32 {
  let q = p / scale;
  let i = vec3<i32>(floor(q));
  var f = fract(q);
  f = f * f * (3.0 - 2.0 * f);   // smoothstep fade — no lattice creases
  let c000 = vnHash(i + vec3<i32>(0,0,0));
  let c100 = vnHash(i + vec3<i32>(1,0,0));
  let c010 = vnHash(i + vec3<i32>(0,1,0));
  let c110 = vnHash(i + vec3<i32>(1,1,0));
  let c001 = vnHash(i + vec3<i32>(0,0,1));
  let c101 = vnHash(i + vec3<i32>(1,0,1));
  let c011 = vnHash(i + vec3<i32>(0,1,1));
  let c111 = vnHash(i + vec3<i32>(1,1,1));
  let x00 = mix(c000, c100, f.x);
  let x10 = mix(c010, c110, f.x);
  let x01 = mix(c001, c101, f.x);
  let x11 = mix(c011, c111, f.x);
  return mix(mix(x00, x10, f.y), mix(x01, x11, f.y), f.z);
}

// Multiplicative brightness grain in roughly [1-amp, 1+amp].
fn surfaceGrain(cell : vec3<i32>, amp : f32) -> f32 {
  let p = vec3f(cell);
  // ~11 voxels (0.7 m) for the broad patches, ~2.5 voxels for the fine break-up.
  let broad = valueNoise(p, TUNE_GRAIN_BROAD_SCALE);
  let fine  = valueNoise(p, TUNE_GRAIN_FINE_SCALE);
  let n = broad * TUNE_GRAIN_MIX + fine * (1.0 - TUNE_GRAIN_MIX);
  return 1.0 + (n - 0.5) * 2.0 * amp;
}

// ---- the stain overlay (DESIGN.md §3, §6) ----------------------------------
// A stained voxel carries a 3-bit stain TYPE and a 4-bit AMOUNT in its word's
// spare bits, written by the sim (see doStaining in sim_step.wgsl). This paints
// that stain over the surface albedo.
//
// It is composited over the ALBEDO, before lighting, not added to the final
// colour — a stain is a change to what the surface IS, so it has to take the
// scene's light, shadow and AO exactly like the material under it. Adding it
// afterwards makes stains glow in shadow, which reads as decals floating above
// the geometry rather than as something soaked in.
//
// The colour comes from the reserved stain-palette entries of the material
// table (STAIN_PALETTE_BASE, world.h): no extra binding, and it hot-reloads
// with materials.json.
//
// Returns the stained albedo, and writes the 0..1 stain coverage to `wetOut`
// so the caller can add a wet sheen — a fresh stain is damp and catches a
// highlight, which is most of what makes it read as blood rather than as rust.
fn applyStain(albedo : vec3f, w : u32, cell : vec3<i32>, wetOut : ptr<function, f32>) -> vec3f {
  *wetOut = 0.0;
  if (!voxStained(w)) { return albedo; }
  let amt = f32(voxStainAmt(w)) / f32(STAIN_AMT_MAX);
  let stainCol = unpackColor(materials[STAIN_PALETTE_BASE + voxStainType(w)].stainColor);

  // Break the stain up so it does not cover the voxel as a flat wash. A real
  // splatter has a mottled, uneven edge; sampling the existing value-noise
  // field at a fine scale and using it as a THRESHOLD against the amount gives
  // exactly that for one noise tap, and it means a light stain (amount 1-2)
  // appears as scattered flecks while a heavy one (12-15) is near-solid.
  let mottle = valueNoise(vec3f(cell), TUNE_STAIN_MOTTLE_SCALE);
  // Amount drives BOTH how far the threshold opens and how opaque the covered
  // part is, so a stain deepens in two ways at once as it builds up.
  let cover = clamp((amt * (1.0 + TUNE_STAIN_MOTTLE) - mottle * TUNE_STAIN_MOTTLE) *
                    TUNE_STAIN_COVERAGE, 0.0, 1.0);
  if (cover <= 0.0) { return albedo; }
  *wetOut = cover * amt;

  // MULTIPLY toward the stain colour rather than mixing to it. A stain soaks
  // in and DARKENS what is under it — it does not repaint it. Mixing makes a
  // stain on dark stone come out lighter than the stone, which looks like
  // paint; multiplying keeps the substrate's own texture and shading visible
  // through the stain, which is what soaking looks like. The lerp toward the
  // pure stain colour at full coverage is what lets a really heavy stain still
  // read as its own colour rather than as an arbitrarily dark patch.
  let soaked = albedo * mix(vec3f(1.0), stainCol * TUNE_STAIN_DARKEN, cover);
  return mix(soaked, stainCol, cover * TUNE_STAIN_OPACITY);
}

// ---- voxel ambient occlusion ----
// The classic Minecraft-style per-vertex AO, evaluated per PIXEL because a
// raymarcher has no vertices: for the face we hit, sample the two tangent
// neighbours and the diagonal on the side the hit point leans toward, and
// darken by how many are solid. This is what puts a dark seam in every inside
// corner and under every overhang, and its absence is why the wooden frame
// looked like flat cardboard cutouts.
//
// Cost is 3 voxel fetches (plus 1 for the face-above term) on the primary hit
// only — reflections and shadow rays skip it entirely.
fn aoSolidAt(c : vec3<i32>) -> f32 {
  if (!inBounds(c)) { return 0.0; }   // unloaded space must not cast AO
  let w = voxWordAt(c);
  let m = voxMat(w);
  if (m == MAT_AIR) { return 0.0; }
  // Only ray blockers occlude: smoke and shallow water must not stamp hard
  // AO shadows onto the terrain they touch.
  return select(0.0, 1.0, isRayBlocker(materials[m]));
}

// `uv` is the hit position's fractional offset within the face, in [0,1]^2
// along the two tangent axes (a1, a2).
fn voxelAO(cell : vec3<i32>, n : vec3<i32>, a1 : i32, a2 : i32, uv : vec2f) -> f32 {
  // The neighbour cell in front of the face — AO samples live in that plane, so
  // an occluder is something sitting beside the face, not inside the solid.
  let base = cell + n;
  // Pick the quadrant the hit leans into: this is what makes the darkening ramp
  // smoothly across the face instead of switching at the midpoint.
  let s1 = select(-1, 1, uv.x > 0.5);
  let s2 = select(-1, 1, uv.y > 0.5);
  var d1 = vec3<i32>(0); d1[a1] = s1;
  var d2 = vec3<i32>(0); d2[a2] = s2;

  let side1 = aoSolidAt(base + d1);
  let side2 = aoSolidAt(base + d2);
  let corner = aoSolidAt(base + d1 + d2);

  // Standard vertex-AO rule: two touching sides fully enclose the corner, so
  // the diagonal cannot lighten it.
  var occ = side1 + side2;
  if (side1 > 0.0 && side2 > 0.0) { occ = 3.0; } else { occ += corner; }

  // How strongly this pixel belongs to the chosen quadrant: at the face centre
  // the AO fades out, at the corner it is full. Without this the AO tiles as
  // four flat quadrants per voxel and reads as a checkerboard.
  let w1 = abs(uv.x - 0.5) * 2.0;
  let w2 = abs(uv.y - 0.5) * 2.0;
  let reach = clamp(max(w1, w2), 0.0, 1.0);

  // 0.55 at a fully enclosed corner — deep enough to read, shallow enough that
  // interiors don't crush to black.
  let ao = 1.0 - (occ / 3.0) * TUNE_AO_STRENGTH * reach;
  return clamp(ao, 0.0, 1.0);
}

// ---- soft sun shadows ----
// The old shadow term was binary (`if (s.hit) { lambert = 0.0; }`), which gives
// razor-sharp shadow edges everywhere — the look of a point light in vacuum.
// The real sun subtends ~0.5 degrees, so shadow edges soften with the distance
// between blocker and receiver; that gradient is a strong depth cue and its
// absence makes shadows read as painted-on decals.
//
// ---- WHY THIS IS NOT A JITTERED CONE ----
// The obvious cheap trick — jitter the ray direction per pixel inside a cone
// and take one sample, letting neighbouring pixels integrate the penumbra — was
// tried here and is WRONG for voxel terrain. One binary sample per pixel cannot
// resolve a penumbra; it only dithers between fully lit and fully shadowed. On
// near-flat ground under a grazing sun, adjacent pixels then randomly hit or
// miss the next terrace step, and the result is per-pixel salt-and-pepper over
// every hillside. Measured: the jitter DOUBLED high-frequency luminance energy
// on foreground grass (mean |dL| between horizontally adjacent pixels went
// 2.33 -> 4.74) — that stipple was the "noise on the floor", not the palette
// and not the AO. Without temporal accumulation or many samples there is
// nothing to average it back out, so the noise is the final image.
//
// Instead: ONE deterministic ray along the exact sun direction, and take the
// softness from the geometry it already reports. A shadow edge's penumbra width
// grows with the distance between blocker and receiver, so `s.t` (how far the
// ray travelled before being blocked) is exactly the quantity a soft shadow
// needs — a contact shadow right at the surface stays crisp, and a shadow cast
// from far away goes soft. Same one-ray cost, no noise, and it is a closer
// model of the real effect than a cone of one sample ever was.
//
// Cast from the KEY light, so at night the shadows are the moon's and point
// the other way — a scene whose shadows still track the sun after dark reads
// as broken immediately.
// ---- SHADOW-RAY LOD (PLAN_surface_flight_perf.md A3) — MEASURED, DOES NOT PAY
// DEFAULT-OFF (TUNE_SHADOW_MAX_DIST = 999). Kept because the measurement is
// the point: the plan's A3 premise is WRONG, and the next person to read
// "sunShadow is 384 steps on every lit pixel with no falloff" will have the
// same idea unless the refutation is written down where the idea lives.
//
// The premise was that farShadowed is the cheap shadow and the fine trace is
// the expensive one, so routing distant receivers through the cascade should
// roughly halve open-terrain render cost. Measured on the offscreen 1080p
// sweep (camera 12 m over canopy, the surface-flight case), quiet machine:
//
//   shadowMaxDist 999 (all fine, control) : 10.35 ms
//   shadowMaxDist 12  (mixed)             : 10.26 ms   ~1%, at the noise floor
//   shadowMaxDist 0   (all cascade)       : 497.46 ms  48x WORSE
//
// The 48x is the explanation. A fine shadow ray is cheap for the reason the
// plan overlooked: it TERMINATES on the first blocker, which over terrain is
// usually a few voxels away, and the chunk-occupancy skip covers the rest. A
// cascade shadow ray does not get to be cheap — farShadowed has to cross
// TUNE_FAR_SHADOW_REACH (60 m) of world before it may conclude "unshadowed",
// and for a NEAR receiver farLevelForDist picks level 1, whose cells are only
// FAR_CELL1_VOX voxels wide, so that reach costs the full 128-step clamp plus
// an occupancy lookup per step. Swapping a ray that stops at 3 voxels for one
// that always walks 60 m is a pessimisation, and the nearer the receiver the
// worse it gets — exactly backwards from an LOD.
//
// So the cascade shadow is not a cheaper shadow; it is the shadow that exists
// where there are no fine voxels to march. That is why the far field uses it
// and why it is right there and wrong here.
//
// Whatever does eventually cut shadow cost, this is the shape of the answer it
// has to beat. The remaining honest levers are making the fine ray terminate
// sooner (A2's sub-chunk occupancy bitmask, so a half-full canopy chunk skips
// internally instead of being marched per voxel) or casting fewer of them —
// not swapping which volume it marches.
//
// The code below is correct and hot-reloadable; set shadowMaxDist to a real
// distance to re-run the experiment. It is not on any frame's critical path
// while the default stands.
fn sunShadowAt(hp : vec3f, n : vec3f, px : vec2f, camDistFine : f32) -> f32 {
  if (camDistFine * VOXEL_METERS > TUNE_SHADOW_MAX_DIST) {
    // Lift the start point off the face by half a CASCADE cell, not half a
    // voxel: at this level the receiver's own cell is what the ray would
    // otherwise immediately hit, exactly as the far-field call site does.
    let lvl = farLevelForDist(camDistFine);
    let off = n * (0.55 * f32(1u << farCellShift(lvl)));
    if (farShadowed(lvl, hp + off)) { return TUNE_SHADOW_FAR_LIFT; }
    return 1.0;
  }
  let s = trace(hp + n * TUNE_SHADOW_BIAS, keyLightDir(), TUNE_SHADOW_STEPS, false);
  if (!s.hit) { return 1.0; }
  // Distance from receiver to blocker, in metres. Near blockers (a voxel
  // resting on the ground) keep a hard, dark contact shadow; distant ones (a
  // tree canopy over a meadow) soften and lift, which is what stops every
  // terrace step from stamping a hard black band onto the hillside.
  let dM = s.t * VOXEL_METERS;
  // A shadowed point keeps NO direct sun — the hemisphere ambient term is what
  // fills it in, and that is already occluded by AO. Letting direct sun leak
  // into shadow instead washes the whole scene out and erases the cast shadow
  // under overhangs. The softening is in the EDGE, not in the depth: a distant
  // blocker only partially covers the solar disc, so its shadow lifts toward
  // ~0.45 of full sun, while a contact shadow stays at 0.
  return clamp(smoothstep(TUNE_SHADOW_SOFT_NEAR, TUNE_SHADOW_SOFT_FAR, dM) * TUNE_SHADOW_LIFT, 0.0, 1.0);
}

// Callers that shade a surface whose camera distance is not to hand (the
// translucent-solid path, which is already gated to near ice by its own
// reflection budget) keep the full-quality near shadow.
fn sunShadow(hp : vec3f, n : vec3f, px : vec2f) -> f32 {
  return sunShadowAt(hp, n, px, 0.0);
}

// ============================================================================
// WATER SURFACE (DESIGN.md §9)
// ============================================================================
// A translucent liquid is not fog. Before this pass water was shaded purely as
// participating media — a flat per-meter tint — which is why a lake read as a
// blue disc painted onto the terrain: no interface, so no reflection, no
// glint, no refraction, and no depth cue at all (the bed 24 voxels down was
// invisible). Real water gets its whole look from the AIR/WATER INTERFACE plus
// what happens to the light that makes it through:
//
//   1. a normal — smoothed from the fullness field, not the voxel face
//   2. ripples  — animated normal perturbation
//   3. Fresnel  — reflect vs refract, angle-dependent (this is the big one)
//   4. reflection — a real traced secondary ray, sky as fallback
//   5. refraction — the transmitted ray, bent, so the bed distorts
//   6. absorption — per-channel Beer-Lambert over the underwater path
//   7. glint    — a sharp specular lobe on the rippled normal
//
// All of it is render-only float math on render-only data. The sim never sees
// any of this and the world hash never covers it, so determinism rule #1 is
// untouched (it scopes to sim state — CLAUDE.md).

// Index of refraction, air -> water. Drives both the Schlick F0 below and the
// refract() bend.
const WATER_IOR : f32 = 1.333;
// Schlick F0 for that IOR: ((1-n)/(1+n))^2 = 0.0204. Water reflects only ~2%
// head-on but ~100% at grazing — that spread IS the look, and it's exactly
// what a constant tint cannot reproduce.
const WATER_F0 : f32 = TUNE_WATER_F0;

// Per-channel absorption, per METRE of path, for clear water. Red is absorbed
// roughly an order of magnitude faster than blue — that is why shallow water
// reads cyan-green and deep water reads deep blue, and it's the single
// strongest depth cue available. A scalar tint (what this shader used to do)
// is flat by construction no matter how it's tuned.
const WATER_ABSORB : vec3f = TUNE_WATER_ABSORB;
// Scattering back toward the eye — the color deep water TENDS toward rather
// than going black. Without it, depth just crushes to black and reads as a pit.
const WATER_SCATTER : vec3f = TUNE_WATER_SCATTER;

// ---- ripples ----
// Sum of directional waves evaluated in world XZ. Cheap gradient-of-a-height
// -field: each wave contributes its analytic slope, so there is no texture
// fetch and no normal map. Frequencies are deliberately non-harmonic (and the
// directions non-parallel) so the pattern never visibly tiles or beats.
//
// SCALE MATTERS: one voxel is VOXEL_METERS, so wave lengths are written in
// METRES and converted, exactly like the tree dimensions in worldgen. Writing
// them as bare voxel counts gives you either mirror-flat water or static.
struct Ripple { dir : vec2f, len : f32, amp : f32, speed : f32 };
const RIPPLE_BANDS : i32 = 5;

// `footM` is the width in METRES that one pixel covers on the surface here.
// Each wave is faded out once the footprint approaches its wavelength (i.e.
// once it can no longer be sampled), which is per-band mip selection done
// analytically. Pass 0 to disable damping.
// ---- SURFACE WAVES (docs/PLAN_water_master.md component 9) -----------------
//
// THE ONE EXPENSIVE MISTAKE, named in the plan and avoided here by
// construction: this is evaluated where a ray HITS the water surface, never
// per sample through the volume. The perf audit identified the raymarch media
// march as what collapsed the frame rate during fires; the cost of this field
// is O(water pixels), not O(volume). Every call site below is a surface hit.
//
// THE RENDER/SIM BOUNDARY IS ABSOLUTE. A render wave can never push anything.
// The body's LEVEL is sim (the ledger owns it) and its DISPLACEMENT is render
// (this file owns it), and the CA never sees the displacement. The moment a
// wave height is fed back so a boat bobs, a render field has become
// authoritative for sim — design guideline #3, and the reason this returns a
// SLOPE for a normal rather than a height anyone could sample.
//
// gravity in m/s^2, for the dispersion relation below.
const WAVE_G : f32 = 9.81;
// Depth a wave sees when nobody has measured one (the caustic path, which is
// looking at the bed from above and does not have a column to probe). 8 m is
// deep against every band here, so tanh(kh) is 1 and the relation degenerates
// to the deep-water case — which is what "no depth information" should mean.
const WAVE_DEEP_H : f32 = 8.0;

// THE FULL FIELD: a Gerstner sum with per-octave speed from the dispersion
// relation, advected by the current field, faded at the shore.
//
// w^2 = g*k*tanh(k*h)      k = 2*pi/lambda, h = local depth
//   deep    (h >> lambda):  tanh -> 1     => c = sqrt(g*lambda/2*pi)
//   shallow (h << lambda):  tanh(kh)~kh   => c = sqrt(g*h), lambda cancels
//
// THIS IS THE HIGHEST-LEVERAGE CONSTANT CHOICE IN THE RENDER TIER AND IT COSTS
// NOTHING. If every octave scrolls at one speed the surface reads as a moving
// texture. At `pondDepth` 26 (2.6 m) the spread across the bands worth
// rendering is 4x — 0.88 m/s at 0.5 m against 3.48 m/s at 8 m. And because `h`
// is the LOCAL depth the same term pays twice: approaching a bank at 0.3 m the
// long swell slows to 1.70 m/s while the short chop barely changes, which is
// shoaling. TUNE_WAVE_DISPERSION mixes between the two so the claim is an A/B
// rather than an assertion.
//
// WHAT THIS IS NOT. A sum of fixed-direction waves does not REFRACT: the
// crests do not physically turn to run parallel to the shore, they only slow
// and steepen there. Amplitude shoaling (Green's law) is included because it
// falls out of the same term; directional refraction would need the wave
// vectors to be functions of position, which is a different field.
//
// `flowMS` is the current at this point in METRES per second. The wave phase
// is evaluated at `position - current*t`, so the pattern drifts and stretches
// downstream — which is what makes a flowing surface look like it is going
// somewhere rather than like a texture scrolling over still water.
fn waveSlope(pWorldM : vec2f, t : f32, footM : f32, depthM : f32,
             flowMS : vec2f) -> vec2f {
  // Shore fade. A sum of sinusoids cannot reflect off a bank, and shallow
  // water damps chop anyway, so the cheap fix is also the physically right
  // one. Without it the waves march straight through the shoreline.
  let shore = smoothstep(0.0, max(TUNE_WAVE_SHORE_DEPTH, 1e-3), depthM);
  if (shore <= 0.0) { return vec2f(0.0); }
  // Advected sample point — component 8 feeding component 9, and ~free.
  let pAdv = pWorldM - flowMS * (t * TUNE_WAVE_FLOW_SCALE);
  var num = vec2f(0.0);   // d(height)/d(position), the plain sinusoid part
  var sharp = 0.0;        // the Gerstner denominator: 1 - sum(Q*k*A*sin)
  var slope = vec2f(0.0);
  // len in metres, amp in metres of height. Four octaves is enough to read as
  // water; the two short ones carry the glint sparkle, the two long ones give
  // the surface a sense of swell so it isn't uniformly busy.
  // Amplitudes are deliberately SMALL relative to wavelength (steepness
  // amp*k stays ~0.03-0.04). Real calm water is very nearly flat: push the
  // steepness up and the perturbed normals start pointing at the shore
  // instead of at the sky, which collapses the Fresnel reflection into a
  // uniform dark green and reads as pond scum rather than water.
  // Steepness (amp*k) per band runs ~0.055 down to ~0.03. This is the setting
  // the look is most sensitive to and it is a narrow window:
  //   * too low  -> a flat sheet; the reflection is uniform and the sun
  //                 highlight fuses into one blown-out white slab
  //   * too high -> normals swing far enough to point at the shore, which
  //                 reads as choppy corrugated metal and crawls when animated
  // The 0.22 m band exists purely to break the sun highlight into sparkle;
  // it carries almost no relief of its own.
  var waves = array<Ripple, RIPPLE_BANDS>(
    Ripple(normalize(vec2f( 1.0,  0.35)), 2.60, 0.0230, 0.55),
    Ripple(normalize(vec2f(-0.42, 1.0 )), 1.70, 0.0135, 0.73),
    Ripple(normalize(vec2f( 0.78, -0.75)), 0.85, 0.0060, 1.15),
    Ripple(normalize(vec2f(-0.85, -0.5)), 0.48, 0.0030, 1.60),
    Ripple(normalize(vec2f( 0.30, -0.95)), 0.22, 0.0011, 2.30));
  for (var i = 0; i < RIPPLE_BANDS; i++) {
    let w = waves[i];
    let k = 6.28318 / w.len;                 // angular wavenumber
    var amp = w.amp * TUNE_RIPPLE_AMP_SCALE * shore;
    // ---- per-octave speed ----
    // FLAT arm: the authored per-band speed this shader shipped with, so
    // TUNE_WAVE_DISPERSION = 0 reproduces the old look exactly and the knob is
    // a real before/after rather than a claim.
    let omegaFlat = w.speed * k;
    // DISPERSED arm: the relation. tanh saturates, so a deep body costs the
    // same as a shallow one and no branch is needed.
    let th = tanh(clamp(k * depthM, 1e-3, 20.0));
    let omegaDisp = sqrt(WAVE_G * k * th);
    let omega = mix(omegaFlat, omegaDisp, TUNE_WAVE_DISPERSION) *
                TUNE_RIPPLE_SPEED_SCALE;
    // SHOALING GAIN (Green's law, A ~ (c_deep/c)^(1/2)). Falls out of the same
    // tanh already computed: a wave slowing as it climbs a bank piles its
    // energy into height. Capped, because Green's law diverges at h -> 0 and
    // the shore fade above is what actually ends the wave there.
    amp *= mix(1.0, clamp(inverseSqrt(max(th, 0.02)), 1.0, 1.8),
               TUNE_WAVE_DISPERSION);
    let phase = dot(pAdv, w.dir) * k + t * omega;
    // Per-band fade: full amplitude while the footprint is comfortably under
    // half a wavelength (Nyquist), gone by the time it exceeds it.
    var band = 1.0;
    if (footM > 0.0) { band = 1.0 - smoothstep(w.len * 0.28, w.len * 0.85, footM); }
    // d/dp of (amp * sin(phase)) = amp * k * cos(phase) * dir
    num += w.dir * (amp * k * cos(phase)) * band;
    // GERSTNER SHARPENING. A Gerstner wave displaces the surface horizontally
    // toward its crests, so the normal picks up a 1/(1 - sum(Q*k*A*sin))
    // denominator: crests get sharp, troughs get flat. That asymmetry is the
    // difference between "sine waves" and "water" and it is two extra ALU ops.
    sharp += TUNE_WAVE_STEEPNESS * amp * k * sin(phase) * band;
  }
  // Clamped well away from zero: a steepness knob turned past the point where
  // the wave would fold over on itself must saturate, not divide by nothing.
  slope = num / max(1.0 - sharp, 0.35);
  // ---- impact ripples ----
  // A ripple is the memory of an event, so this is the one part of the field
  // that reads state — a BOUNDED ring, evaluated as a pure function of
  // (eventList, t). See kWaveImpactCap and the note over WaveImpactRing.
  if (R.waveImpactCount > 0u) {
    let ik = 6.28318 / max(TUNE_WAVE_IMPACT_LEN, 0.05);
    let n = min(R.waveImpactCount, WAVE_IMPACT_CAP);
    for (var i = 0u; i < n; i = i + 1u) {
      let e = R.waveImpacts[i];
      if (e.w <= 0.0) { continue; }
      let age = t - e.z;
      if (age < 0.0 || age > TUNE_WAVE_IMPACT_DECAY * 3.0) { continue; }
      // Ring radius grows linearly; the crest train rides it and the whole
      // thing decays exponentially. Amplitude also falls as 1/sqrt(r), which
      // is energy spreading round a growing circle, not an art choice.
      let d = pWorldM - vec2f(e.x, e.y) * VOXEL_METERS;
      let r = length(d);
      if (r < 1e-4) { continue; }
      let ringR = age * TUNE_WAVE_IMPACT_SPEED;
      let widthM = max(TUNE_WAVE_IMPACT_LEN, 0.05) * 1.5;
      let env = exp(-age / max(TUNE_WAVE_IMPACT_DECAY, 0.05)) *
                exp(-((r - ringR) * (r - ringR)) / (widthM * widthM)) *
                inverseSqrt(max(r, 0.25));
      if (env < 1e-4) { continue; }
      slope += (d / r) * (e.w * shore * ik * env * cos(ik * (r - ringR)));
    }
  }
  return slope;
}

// The deep, still-water case, for callers with no column to probe and no flow
// to advect by — the caustic path. Kept as its own name so those call sites
// read as what they are rather than as waveSlope with two magic arguments.
fn rippleSlope(pWorldM : vec2f, t : f32, footM : f32) -> vec2f {
  return waveSlope(pWorldM, t, footM, WAVE_DEEP_H, vec2f(0.0));
}

// ---- surface normal ----
// The voxel face normal is axis-aligned and would make a lake look like tiled
// glass. Instead take the GRADIENT OF THE FULLNESS FIELD: the liquid state
// nibble is fill level in eighths (DESIGN.md §4), so the liquid column height
// varies cell to cell and its gradient is the true macro slope of the surface
// — this is the standard scalar-field-gradient normal from the smooth-voxel
// literature, applied to data the sim already maintains for free.
//
// This is what makes a settling / flowing / wavy body of water read as a
// surface with shape rather than as a staircase, and it costs 4 taps.
fn liquidFullnessAt(c : vec3<i32>, mat : u32) -> f32 {
  if (!inBounds(c)) { return 0.0; }
  let w = voxWordAt(c);
  if (voxMat(w) != mat) { return 0.0; }
  return f32(voxState(w) + 1u) / 8.0;
}

// Surface height (in voxels, relative to the cell floor) of the liquid column
// at XZ: full cells below stack, and the topmost partial cell adds its
// fullness. Sampling the column rather than one cell is what lets the gradient
// see a slope of more than one cell.
fn liquidColumn(c : vec3<i32>, mat : u32) -> f32 {
  // Walk up from the sample cell while cells stay full; the first non-full
  // cell contributes its fraction and ends the column.
  var h = 0.0;
  for (var i = 0; i < 3; i++) {
    let f = liquidFullnessAt(c + vec3<i32>(0, i, 0), mat);
    h += f;
    if (f < 0.999) { break; }
  }
  return h;
}

// LOCAL DEPTH of the water under a surface cell, in METRES.
//
// Component 9's dispersion relation needs `h`, and `h` is what makes the same
// tanh term pay twice — per-octave speed AND shoaling toward a bank. The plan
// says "the descriptor provides both", and for a governed body it does; but the
// renderer must also be right on the 95% of water that is not governed (a
// puddle, a flooded cellar, the CA's own transient), so this measures it.
//
// A GEOMETRIC PROBE, not a linear one: 8 taps at increasing stride reach 26
// voxels (2.6 m, exactly `pondDepth`) with 1-voxel resolution where it matters.
// Resolution near the surface is what the shore fade and the shoaling gain are
// sensitive to; resolution at 2 m is not, because tanh has already saturated.
// A linear 26-tap probe would spend 18 extra taps buying nothing.
fn waterDepthM(cell : vec3<i32>, mat : u32) -> f32 {
  var d = 0;
  var step = 1;
  for (var i = 0; i < 8; i++) {
    if (liquidFullnessAt(cell - vec3<i32>(0, d + step, 0), mat) <= 0.0) {
      break;
    }
    d += step;
    if (i >= 2) { step = step * 2; }
  }
  return f32(d + 1) * VOXEL_METERS;
}

// ---- how OPEN is this body of water? ----
// Ripples are wind-driven gravity waves, and a gravity wave needs FETCH: an
// open surface many wavelengths across for the wind to work on. The shortest
// band in rippleSlope is 0.22 m — three and a half voxels — and the longest is
// 2.6 m, or forty. A puddle one voxel wide cannot physically carry any of them.
//
// Running the field on one anyway is what made single droplets on the ground
// look like a fast-moving current: the waves are defined in world XZ and
// animate at 0.55-2.3 m/s regardless of what they are drawn on, so a 6 cm
// splash shows a full-speed wavefront crossing it and reads as rushing water.
// Scale is the giveaway — the same slope that looks like gentle chop on a lake
// looks like whitewater when it crosses a droplet in a fraction of a second.
//
// So measure the surface's horizontal extent and fade the ripples out with it.
// A horizontal probe, deliberately: fetch is a property of how far the surface
// runs, and depth has nothing to do with it. Samples the COLUMN (not one cell)
// so a shallow-but-wide pond still counts as open, which is the case that would
// otherwise go glassy and dead.
//
// 12 taps on a 2-voxel spacing, primary up-facing water hits only.
fn waterOpenness(cell : vec3<i32>, mat : u32) -> f32 {
  var n = 0.0;
  for (var i = 0; i < 12; i++) {
    // Two rings at 2 and 4 voxels: the inner one separates a droplet from a
    // small puddle, the outer one a puddle from something with real fetch.
    let ring = select(2, 4, i >= 6);
    let a = f32(i % 6) * 1.0472;   // 60 degrees apart
    let o = vec3<i32>(i32(round(cos(a) * f32(ring))), 0,
                      i32(round(sin(a) * f32(ring))));
    if (liquidColumn(cell + o, mat) > 0.05) { n += 1.0; }
  }
  return smoothstep(TUNE_WATER_FETCH_LOW, TUNE_WATER_FETCH_HIGH, n / 12.0);
}

// PER-WAVE DISTANCE DAMPING (the analytic stand-in for normal-map mipping).
// One pixel far across a lake covers many wavelengths, so the ripples inside it
// should average toward flat; without damping, distant water aliases into
// crawling speckle — full-amplitude slope at a collapsed sampling rate, exactly
// the undersampling a mip chain exists to fix.
//
// The footprint estimate must be the SCREEN-SPACE one, not raw distance. Raw
// distance is a radial function about the camera, and multiplying the ripple
// field by it stamps that radial function onto the water: the waves visibly
// bend into concentric rings centred on the viewer, which moves with the camera
// and is far worse than the aliasing it fixes. Grazing angle is the other half
// of it — a surface seen edge-on has a footprint stretched along the view
// direction no matter how near it is.
//
// R.viewPx is the render height in pixels, so tanHalfFov*2/viewPx is the
// vertical angle one pixel subtends — the same quantity a mip LOD is chosen
// from. Hardcoding a resolution here would make the water's apparent
// choppiness change with window size.
//
// FACTORED OUT of waterNormal because the MPM fluid surface needs the SAME
// number. Where the fluid march draws SETTLED water (its virtual-mass seam,
// fluidCellAt) that water is part of the same lake the CA shades, and if the
// two carry differently-damped ripples the seam between them reads as a
// rectangle of still glass laid on a moving surface.
fn waterRippleFootprint(hitP : vec3f) -> f32 {
  let toEye = R.camPos - hitP;
  let distM = length(toEye) * VOXEL_METERS;
  // |cos| between the view ray and the surface normal-ish up axis: small at
  // grazing incidence, where the footprint blows up.
  let graze = max(abs(normalize(toEye).y), 0.06);
  return distM * (R.tanHalfFov * 2.0 / max(R.viewPx, 1.0)) / graze;
}

// ---- caustics ----
// Sunlight refracting through the wave surface focuses into the bright shifting
// web everyone recognises on a lake bed, and its absence is a strong "this is
// fake" cue even when the absorption is right.
//
// Proper caustics need photon transport; the standard real-time cheat is that
// the caustic intensity tracks the CONVERGENCE of the refracted rays, and for a
// small-slope surface that convergence is the curvature of the wave height
// field. Sampling the ripple slope at two nearby points and taking the
// difference gives that curvature for a couple of extra ALU ops and no new data.
//
// Projected along the SUN direction, not straight down, so the pattern shifts
// across the bed with the sun's angle instead of being pinned under the waves
// that cast it.
//
// Returns a MULTIPLIER for the bed colour, not an addition. Caustics are a
// redistribution of the sunlight already landing on the bed, so they scale what
// is there: bright sand goes brighter, dark stone stays dark. Adding a constant
// instead lights up the water itself, which reads as glowing blobs floating in
// the volume rather than light playing over a surface.
//
// Shared by shadeWater and the MPM fluid's seam path so a lake whose surface is
// half CA and half marched isosurface throws ONE caustic web across its bed.
fn waterCaustics(hitP : vec3f, rd : vec3f, pathVox : f32, depthM : f32) -> f32 {
  // where on the surface the sunlight entering this bed point came from
  let bedP = hitP + rd * pathVox;
  let kdc = keyLightDir();
  let drift = kdc.xz / max(kdc.y, 0.25) * (depthM / VOXEL_METERS);
  let cp = (vec2f(bedP.x, bedP.z) - drift) * VOXEL_METERS;
  // Difference over a WIDE baseline and damp out the short bands. Caustic webs
  // come from the long swell — a crest metres across is what has the focal
  // length to reach the bed. Differencing at the scale of the 22 cm chop
  // instead samples curvature that focuses far below any real bed and renders
  // as a fine dotted grid of per-pixel noise, not a caustic. Passing a
  // footprint of 0.5 m mutes every band under ~0.6 m, leaving the 2.6 m and
  // 1.7 m swell to drive the pattern.
  let e = 0.22;    // metres — finite-difference baseline for curvature
  let cf = 0.5;    // metres — band damping footprint
  let s0 = rippleSlope(cp, R.time, cf);
  let sx = rippleSlope(cp + vec2f(e, 0.0), R.time, cf);
  let sz = rippleSlope(cp + vec2f(0.0, e), R.time, cf);
  // divergence of the slope field = Laplacian of the height field. Negative
  // curvature (a wave crest acting as a converging lens) is the bright case.
  let curv = ((sx.x - s0.x) + (sz.y - s0.y)) / e;
  // Focusing strength grows with depth (longer lever arm from surface to bed)
  // then saturates — deep water's caustics wash out as the light scatters, and
  // unbounded growth would blow the bed out to white.
  let focus = clamp(depthM * 1.5, 0.0, 1.4);
  let caustic = max(-curv, 0.0) * focus;
  return 1.0 + min(caustic * TUNE_CAUSTIC_GAIN, TUNE_CAUSTIC_CAP);
}

fn waterNormal(cell : vec3<i32>, mat : u32, axis : i32, sgn : f32,
               hitP : vec3f, upFacing : bool) -> vec3f {
  // Side/bottom faces of a liquid volume keep their flat voxel normal: the
  // fullness gradient describes the TOP surface, and applying it to a wall
  // would tilt it into the terrain.
  if (!upFacing) {
    var n = vec3f(0.0);
    n[axis] = -sgn;
    return n;
  }

  // Central differences of the column height across X and Z. dh/dx in voxels
  // per voxel is already a slope, so the normal is (-dh/dx, 1, -dh/dz).
  let hx0 = liquidColumn(cell + vec3<i32>(-1, 0, 0), mat);
  let hx1 = liquidColumn(cell + vec3<i32>( 1, 0, 0), mat);
  let hz0 = liquidColumn(cell + vec3<i32>(0, 0, -1), mat);
  let hz1 = liquidColumn(cell + vec3<i32>(0, 0,  1), mat);
  var slope = vec2f((hx0 - hx1) * 0.5, (hz0 - hz1) * 0.5);

  // Ripples on top of the macro slope, in world metres.
  let pm = vec2f(hitP.x, hitP.z) * VOXEL_METERS;
  // Damping is applied PER WAVE against its own wavelength inside
  // rippleSlope() — a 2.6 m swell stays visible long after 0.4 m chop has
  // averaged out, which is what gives distance a sense of scale. `gain` here
  // is just the footprint handed down (see waterRippleFootprint).
  let gain = waterRippleFootprint(hitP);
  // Ripple slope is metres-of-height per metre — same units as `slope` above
  // (voxels per voxel), so they add directly.
  //
  // Scaled by fetch (see waterOpenness): a droplet or a one-voxel puddle gets
  // no travelling waves at all and keeps the macro slope from the column
  // gradient, so it reads as a still bead of water. A lake keeps the full
  // field. Everything between crosses over smoothly.
  //
  // DEPTH and FLOW are what turn the ripple field into component 9's wave
  // field: depth sets each octave's speed and its shoaling, and the current
  // advects the phase so a drifting surface reads as drifting. Both are
  // measured HERE, at the surface hit, and nowhere else in the march.
  let depthM = waterDepthM(cell, mat);
  let flowMS = currentAt(hitP, &R).xz * VOXEL_METERS;
  slope += waveSlope(pm, R.time, gain, depthM, flowMS) *
           waterOpenness(cell, mat);

  return normalize(vec3f(-slope.x, 1.0, -slope.y));
}

// Sky-only reflection fallback plus a horizon-grounded haze, used when a
// reflected ray finds nothing. A raw skyColor() lookup below the horizon
// returns the ground-ward gradient, which reads as a bright band; clamping the
// reflected direction's downward component keeps distant reflections plausible.
fn reflectionSky(rd : vec3f) -> vec3f {
  var d = rd;
  if (d.y < 0.02) { d.y = 0.02; }
  // Body-free: the sun and moon discs are handled by the dedicated glint lobe
  // below, which is shaped for a rippled surface. Reflecting the real disc
  // here as well would double it AND scatter single-pixel fireflies wherever
  // a ripple normal happens to line up.
  return skyColorNoBodies(normalize(d));
}

// ---- traced reflection ----
// The engine already has a DDA, so the reflection can be a REAL ray rather
// than a screen-space approximation: no missing-information artifacts at the
// screen edge, and objects behind the camera reflect correctly (Teardown's
// screen-space reflections explicitly can't do this). Budget is small — a
// reflected ray off water is nearly always either sky or the near shore, and
// this runs per water pixel.
fn traceReflection(p : vec3f, n : vec3f, rd : vec3f) -> vec3f {
  let rr = reflect(rd, n);
  // A reflected ray pointing DOWN came from a normal that the ripples tilted
  // past the view ray; it has nothing valid to reflect. Take the sky, but
  // BLEND across the horizon rather than switching at exactly rr.y == 0.
  //
  // A hard cutoff here is visible: at a grazing view the reflected rays sit
  // within a few degrees of horizontal, so the ripple field pushes adjacent
  // pixels back and forth across the threshold and the surface breaks into
  // per-pixel speckle instead of reading as one continuous sheet.
  let horizon = smoothstep(-0.06, 0.02, rr.y);
  if (horizon <= 0.0) { return reflectionSky(rr); }
  // Step budget, not distance: this is one secondary ray per water pixel and a
  // lake can fill the screen, so this number is a direct frame-time multiplier
  // on water-heavy views. Rays that run out return sky, which is what a long
  // unobstructed reflection was going to be anyway — the budget only truncates
  // reflections of things far across the water, where the aerial perspective
  // below has already faded them most of the way to sky.
  //
  // 96 is chosen against the geometry, not by taste: a reflected ray off
  // near-flat water leaves at a shallow angle and spends most of its steps
  // crossing empty air above the surface, where the chunk-skip advances it a
  // whole 16-cell chunk per iteration. 96 steps therefore reaches well past
  // the far shore of any pond-sized body while capping the pathological case —
  // a reflection grazing INTO dense canopy, where every step is a real voxel
  // step and the skip never fires. Media is off (`wantMedia = false`) so
  // reflected rays skip on the blocker count and smoke costs them nothing.
  let h = trace(p + n * 0.05, rr, TUNE_REFLECTION_STEPS, false);
  if (!h.hit) { return reflectionSky(rr); }

  // Reflected geometry is seen across the water plus its own distance, so it
  // takes aerial perspective too — without this, a reflected far hillside is
  // sharper than the real one and the reflection reads as a decal.
  return mix(reflectionSky(rr), applyAerial(shadeSecondaryHit(h), rr, h.t),
             horizon);
}

// Shade a SECONDARY ray's hit with the same terms as a primary hit, minus the
// shadow ray and AO (neither is resolvable at a one-bounce budget, and each
// would double the cost). Shared by traceReflection above and the MPM fluid's
// traced refraction — the two must not drift, or the same shore looks
// different reflected off the surface vs seen through it.
fn shadeSecondaryHit(h : Hit) -> vec3f {
  let m = materials[voxMat(h.word)];
  var albedo = paletteColor(m, voxState(h.word));
  if (m.klass == CLASS_LIQUID) { albedo = unpackColor(m.color0); }
  var rn = vec3f(0.0);
  rn[h.axis] = -h.sgn;
  var face = 1.0;
  if (h.axis == 0) { face = TUNE_FACE_X; }
  else if (h.axis == 2) { face = TUNE_FACE_Z; }
  if (m.klass != CLASS_LIQUID) { albedo *= surfaceGrain(h.cell, TUNE_GRAIN_AMP); }
  // Stains show in secondary rays too — a pool reflecting (or revealing) the
  // stained wall beside it should not show a clean wall. Albedo-only (the
  // sheen is not resolvable at this budget), same as the grain above.
  var rwet = 0.0;
  albedo = applyStain(albedo, h.word, h.cell, &rwet);
  let lam = wrapDiffuse(dot(rn, keyLightDir()), 0.55);
  var c = albedo * face * (ambientAt(rn) + keyLightColor() * lam * 0.52);
  let emis = f32(m.emission) / 255.0;
  if (emis > 0.0) { c += albedo * emis * 1.7; }
  return c;
}

// ============================================================================
// SUBMERGED VIEW — being UNDER the water (DESIGN.md §9)
// ============================================================================
// Everything above this point shades water seen from OUTSIDE. Being inside it
// is a different problem and it was previously handled by two lines in
// shadeWater (flip the normal, swap the reflection for a scatter constant),
// which is why going under produced a flat blue wash with no light in it.
//
// Four terms carry the whole look, in rising order of cost:
//
//   1. absorption + in-scatter over the WHOLE view. Not a bounded depth: under
//      water every ray is inside the medium for its entire length, so this is
//      the underwater equivalent of aerial perspective and it replaces it.
//   2. CAUSTICS on every sunlit surface below the waterline. This is the term
//      the request is really about — the rippling web of light on the rocks.
//      It is deliberately NOT the caustic term inside shadeWater: that one is
//      a multiplier on `sceneBehind` at the moment a ray from dry land crosses
//      a surface. There is no such crossing when you are already under, so
//      from below the old code applied no caustics to anything at all.
//   3. GOD RAYS — a ray-marched, occlusion-tested volumetric integral. The
//      shafts have to break around the shore and any overhang, which is what
//      a real shadow test buys and what an analytic approximation cannot.
//   4. SILT — drifting motes. Cheap, and it is most of what makes the shafts
//      legible: a light shaft is only visible because something is IN it.
//
// All render-only float math on render-only data. The sim never sees any of
// it, so determinism rule #1 is untouched (it scopes to sim state).

// ---- how much water is above this point? ----
// Walks UP from a surface point counting liquid cells until it reaches air.
// Returns metres of water overhead, or -1 if the point is not under any.
//
// The step count is the cost, and it is bounded rather than complete on
// purpose: this runs per lit surface pixel, and a point under more water than
// the cap is one whose caustics have washed out entirely anyway (they fade to
// nothing by bedCausticFade, which is well inside the cap at any sane setting).
// Reporting the cap as "deep" is therefore the correct answer, not a
// truncation artifact.
const SUB_DEPTH_STEPS : i32 = 40;

// Takes the CONTINUOUS hit point, not the cell. That matters: quantising the
// depth to the cell makes every pixel on one voxel face share a single depth
// value, so the caustic strength (which ramps and then fades with depth) jumps
// in hard steps at every cell boundary. On the vertical rim wall of a pool
// that renders as exactly what it is — a barcode of flat stripes, one per
// voxel row — and it was the most obvious artifact in the first cut. Carrying
// the fractional part of the start height removes the stair entirely for the
// cost of one subtraction.
fn waterAbove(p : vec3f) -> f32 {
  let cell = vec3<i32>(floor(p));
  var n = 0.0;
  for (var i = 0; i <= SUB_DEPTH_STEPS; i++) {
    let c = cell + vec3<i32>(0, i, 0);
    // Out of the window reads as "no more water". Unloaded space is solid and
    // inert (CLAUDE.md), so treating it as more water would paint caustics
    // under every overhang at the window edge.
    if (!inBounds(c)) { break; }
    let w = voxWordAt(c);
    let mt = voxMat(w);
    if (mt == MAT_AIR) { break; }
    let m = materials[mt];
    // Only a TRANSLUCENT liquid counts as water overhead. An opaque one (lava)
    // transmits nothing, and a solid lid ends the column.
    if (m.klass != CLASS_LIQUID || (m.flags & MATF_OPAQUE) != 0u) { break; }
    var f = f32(voxState(w) + 1u) / 8.0;
    // The cell the point is IN contributes only the part above the point.
    if (i == 0) { f = max(f - fract(p.y), 0.0); }
    n += f;
  }
  return n * VOXEL_METERS;
}

// ---- the caustic web itself ----
// Same physical idea as the caustic term in shadeWater — the intensity tracks
// the CONVERGENCE of rays refracted through the wave surface, which for a
// small-slope surface is the curvature (Laplacian) of the wave height field —
// but projected differently, and that difference is the entire reason this is
// a separate function rather than a shared one.
//
// shadeWater projects from the point where the PRIMARY RAY crossed the
// surface. That is correct for looking down into water from dry land, and it
// is meaningless from below, where the primary ray never crossed anything.
//
// Here the projection runs from the patch of surface DIRECTLY ABOVE the lit
// point, drifted along the sun direction by the depth — i.e. where the
// sunlight landing on this point actually entered the water. That makes the
// pattern correct from any viewpoint, including from underneath looking up at
// a lit wall, and it makes it stable when the camera moves (it is a property
// of the surface and the sun, not of the eye).
fn bedCaustic(p : vec3f, n : vec3f, depthM : f32) -> f32 {
  if (depthM <= 0.0) { return 0.0; }
  let kd = keyLightDir();
  // Sun below the horizon: no caustics. Guarded before the divide.
  if (kd.y < 0.05) { return 0.0; }
  // Trace back up the sun direction to the surface: the entry point is the lit
  // point plus the sun vector scaled to cover `depthM` of vertical rise.
  let up = depthM / max(kd.y, 0.05);
  let entry = p * VOXEL_METERS + kd * up;
  let cp = vec2f(entry.x, entry.z);

  // Curvature by finite difference of the slope field. The baseline and the
  // band-damping footprint are the same as the shadeWater caustic and for the
  // same reason: differencing at the scale of the shortest (22 cm) chop
  // samples curvature that focuses far below any real bed and renders as a
  // fine dotted grid of per-pixel noise. Only the long swell has the focal
  // length to reach a bed metres down.
  let e = 0.22;   // metres — finite-difference baseline
  let cf = 0.5;   // metres — band damping footprint
  let s0 = rippleSlope(cp, R.time, cf);
  let sx = rippleSlope(cp + vec2f(e, 0.0), R.time, cf);
  let sz = rippleSlope(cp + vec2f(0.0, e), R.time, cf);
  let curv = ((sx.x - s0.x) + (sz.y - s0.y)) / e;

  // Only CONVERGING curvature makes a bright band; diverging is the dark gap
  // between bands, and it is already dark by being unlit.
  //
  // NORMALISE BEFORE THE EXPONENT. This is not cosmetic — getting it wrong
  // silently deletes the whole effect. The raw Laplacian of this wave field
  // peaks around 0.15, and raising a number that far below 1 to a power >1
  // SHRINKS it (0.15^2.2 = 0.015): the "sharpen" step was cutting the signal
  // by an order of magnitude, so the finished caustic came out at ~3% of the
  // surface brightness and was invisible against the bed. Scaling into 0..1
  // first means the exponent does what it is meant to do — redistribute
  // contrast into thin filaments — while leaving the peak at full strength.
  //
  // CAUSTIC_NORM is the reciprocal of that measured peak. It is a property of
  // the ripple band table (amplitudes x wavenumbers), so if those change, this
  // wants re-measuring: sample -curv over the field and take the max.
  const CAUSTIC_NORM : f32 = 6.7;
  var c = clamp(max(-curv, 0.0) * CAUSTIC_NORM, 0.0, 1.0);
  // Sharpen into filaments. A raw curvature field is a smooth blob pattern;
  // real caustics are thin bright lines with wide dark gaps, and the exponent
  // is what turns one into the other.
  c = pow(c, TUNE_BED_CAUSTIC_SHARP);

  // Focus grows with depth (longer lever arm from surface to bed) then washes
  // out as scattering smears the pattern. Both ends matter: no depth ramp and
  // a surface right at the waterline gets full-strength caustics it physically
  // cannot have; no fade and the deepest water is the brightest, which is
  // backwards.
  let focus = clamp(depthM * 1.5, 0.0, 1.4) *
              (1.0 - smoothstep(0.0, TUNE_BED_CAUSTIC_FADE, depthM));

  // Caustics land on a surface in proportion to how square-on it faces the
  // sun, exactly like any other direct light — a wall parallel to the incoming
  // shafts catches almost none. Without this the web wraps uniformly around
  // every face of a rock and reads as glowing paint rather than as projected
  // light.
  let facing = max(dot(n, kd), 0.0);

  return c * focus * facing;
}

// ---- Henyey-Greenstein phase function ----
// The standard single-parameter model for how strongly a medium scatters
// forward vs backward. g > 0 is forward-scattering, which is what makes a
// light shaft blaze when you look toward the sun and fade to almost nothing
// when you look away — the single term that separates "god rays" from "the
// whole volume got brighter".
fn phaseHG(cosTheta : f32, g : f32) -> f32 {
  let g2 = g * g;
  let d = 1.0 + g2 - 2.0 * g * cosTheta;
  // d can reach 0 only at g = 1, which LoadTuning clamps away from; the max()
  // is belt-and-braces against a hand-edited prelude.
  return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(d, 1e-4), 1.5));
}

// ---- volumetric light shafts ----
// Marches the view ray through the water and, at each sample, asks whether the
// sun reaches that point. The occlusion test is a real trace, so a shaft is
// cut by the shore, by an overhang, by a rock — which is the whole reason to
// pay for it. An analytic shaft (modulating in-scatter by the ripple field
// alone) is nearly free but passes straight through solid terrain, and under
// water you are constantly looking at beams that ought to be interrupted by
// the bank you are swimming next to.
//
// Cost: godRaySteps x godRayShadowSteps texture-ish reads per submerged pixel.
// It is gated on being submerged, so a dry frame pays nothing at all, and both
// counts are clamped in LoadTuning because their product is the frame time.
fn godRays(ro : vec3f, rd : vec3f, maxDistVox : f32, px : vec2f) -> f32 {
  let steps = TUNE_GODRAY_STEPS;
  if (steps <= 0) { return 0.0; }
  let kd = keyLightDir();
  // No shafts with the key light at or below the horizon: at that angle the
  // refracted light is running nearly horizontally and there is nothing to
  // see. Cheap early-out that skips the whole march at night.
  if (kd.y < 0.08) { return 0.0; }

  let rangeVox = TUNE_GODRAY_RANGE / VOXEL_METERS;
  let march = min(maxDistVox, rangeVox);
  if (march <= 0.0) { return 0.0; }
  let dt = march / f32(steps);

  // Dither the start offset per pixel so the fixed sample count does not
  // produce visible banding — the classic slice artifact of any volumetric
  // march. Screen-space and TIME-FREE, matching farDither's reasoning: a
  // time-varying jitter would crawl, and this is a still-frame-stable pattern
  // that the eye integrates spatially instead.
  let jitter = fract(dot(px, vec2f(0.7548776662, 0.5698402909)));

  // Forward-scattering phase, constant along the ray (the sun is directional).
  let phase = phaseHG(dot(rd, kd), TUNE_GODRAY_ANISO);

  var acc = 0.0;
  for (var i = 0; i < steps; i++) {
    let t = (f32(i) + jitter) * dt;
    let p = ro + rd * t;
    // Is this sample still inside water? A view ray under water can leave the
    // liquid (through the surface, or into an air pocket), and scattering must
    // stop where the medium does or shafts extend out into the sky.
    let c = vec3<i32>(floor(p));
    if (!inBounds(c)) { break; }
    let w = voxWordAt(c);
    let mt = voxMat(w);
    if (mt == MAT_AIR) { continue; }
    let m = materials[mt];
    if (m.klass != CLASS_LIQUID || (m.flags & MATF_OPAQUE) != 0u) { continue; }

    // Occlusion: can the sun reach this point? Short budget on purpose — this
    // ray only has to find the surface just above or a nearby blocker, and the
    // chunk-skip in trace() covers open water in a few steps.
    let s = trace(p, kd, TUNE_GODRAY_SHADOW_STEPS, false);
    if (s.hit) { continue; }

    // Reaching here means sunlight lands on this sample. Weight it by the
    // ripple curvature at the surface above, so the shafts inherit the same
    // moving structure as the caustics on the bed — the beams and the web on
    // the floor are the same light, and having them animate independently is
    // an immediate tell.
    let dAbove = waterAbove(p);
    let shaft = 1.0 + bedCaustic(p, kd, dAbove) * 0.6;
    acc += shaft * dt;
  }

  // dt is in voxels; convert to metres so the strength knob is scale-free.
  return acc * VOXEL_METERS * phase * TUNE_GODRAY_STRENGTH;
}

// ---- suspended particulate ----
// Motes drifting in the water. Render-only, procedural, no particles and no
// buffer: a 3D value-noise field thresholded hard so it reads as discrete
// specks rather than as fog, sampled along the view ray at a few depths.
//
// This is the cheapest term here and one of the most effective. A light shaft
// in clear water is invisible — you see a shaft precisely because there is
// something suspended in it to scatter off — so silt and god rays are really
// one effect, and the silt is what gives the water a sense of volume and
// motion when you turn your head.
fn siltMotes(ro : vec3f, rd : vec3f, maxDistVox : f32, lit : f32) -> f32 {
  if (TUNE_SILT_DENSITY <= 0.0) { return 0.0; }
  var acc = 0.0;
  // Four slabs at increasing distance. Sampling more than this does not read
  // as more particles, it reads as fog — the eye wants sparse discrete specks.
  for (var i = 0; i < 4; i++) {
    let t = maxDistVox * (0.12 + 0.24 * f32(i));
    if (t <= 0.0) { continue; }
    var p = ro + rd * t;
    // Slow vertical drift plus a lateral sway, so the field is alive without
    // reading as falling snow. Deliberately very slow: real particulate in
    // still water barely moves, and anything fast immediately looks like a
    // weather effect happening indoors.
    p.y -= R.time * TUNE_SILT_DRIFT / VOXEL_METERS;
    p.x += sin(R.time * 0.11 + p.y * 0.05) * 0.6;
    // Hard threshold on a noise field = sparse specks. The 1/(1+t) falloff
    // keeps distant motes from stacking into a haze, which is what happens if
    // every slab contributes equally.
    let nz = valueNoise(p, 0.9);
    let spec = smoothstep(0.82, 0.97, nz);
    acc += spec / (1.0 + t * VOXEL_METERS * 0.35);
  }
  return acc * TUNE_SILT_DENSITY * TUNE_SILT_BRIGHTNESS * (0.25 + lit);
}

// ============================================================================
// THE SUBMERGED PROFILE — what being inside ANY liquid looks like
// ============================================================================
// Every liquid a body can be inside gets a complete submerged treatment for
// free, derived from what materials.json already authors: its PALETTE and its
// OPACITY. Nothing here names a material or an id, so a liquid added tomorrow
// is submersible tomorrow (CLAUDE.md conventions), and the tuning knobs below
// shape the MAPPING rather than any one liquid's numbers.
//
// The first cut of this had a one-line escape hatch — `isWater`, defined as
// "has any tag AND opacity < 0.45" — and everything else fell into a rough
// else branch. That was wrong twice over. As a classifier it was accidental:
// acid (opacity 170) failed it, and so would any new clear liquid authored
// without tags, so "is this water" was really "did the author happen to write
// these two fields this way". And the else branch was a stub — no visibility
// distance, no vignette, no god-ray or Snell gating — so a non-water liquid
// got a half-finished look that no amount of tuning could fix.
//
// OPACITY IS THE AXIS. It is already the authored measure of how much a medium
// blocks, it is already what the media path uses, and across the shipped
// liquids it orders them exactly the way submersion should: water 90 (clear,
// you see across a pond), acid 170, blood 200, oil 235 (nearly opaque, arm's
// length). Everything below is a function of it, so a new liquid's look
// follows from one number the author was going to write anyway.
struct SubProfile {
  absorbK   : vec3f,  // per-channel extinction per metre
  scatter   : vec3f,  // colour the volume tends toward
  visM      : f32,    // metres to full fade — the "how murky" distance
  clarity   : f32,    // 0 = opaque sludge, 1 = clear water. Gates the extras.
  vignette  : f32,    // screen-edge darkening
  snellGain : f32,    // brightness of the window looking up
};

fn submergedProfile(m : Material) -> SubProfile {
  var p : SubProfile;
  // Authored palette average — the liquid's own colour is what the volume
  // tends toward with distance, for every liquid including water.
  let base = (unpackColor(m.color0) + unpackColor(m.color1)) * 0.5;
  // 0 for a perfectly clear liquid, 1 for a fully blocking one. Water sits at
  // 0.35, oil at 0.92.
  let op = clamp(f32(m.opacity) / 255.0, 0.0, 1.0);

  // CLARITY drives everything that only makes sense in a medium you can see
  // through. It is deliberately non-linear: opacity 90 (water) has to land
  // near "clear" and opacity 235 (oil) near "blind", and a straight 1-op maps
  // water to 0.65 and oil to 0.08, which reads as murky water rather than as
  // oil. The curve pushes the ends apart.
  p.clarity = pow(clamp(1.0 - op, 0.0, 1.0), 0.55);

  // Visibility: how far you can see before the view is entirely the liquid's
  // own colour.
  //
  // Deliberately a STEEPER function of clarity than the gating above, and the
  // two curves have to be separate. One shared exponent cannot serve both: a
  // gentle curve leaves oil seeing 3 m (which reads as murky water, not
  // sludge), and a steep enough curve to fix that drags water's clarity down
  // out of the refined band and loses water's hand-tuned look entirely. So
  // clarity^2.2 collapses the murky end hard while the gentler `clarity`
  // itself still classifies water as clear. Oil lands near 1 m, blood ~2 m,
  // acid ~3 m, and water is overridden to its authored 11 m below.
  p.visM = mix(TUNE_SUB_MURK_VIS, TUNE_SUB_VISIBILITY,
               pow(p.clarity, TUNE_SUB_VIS_CURVE));

  // Absorption absorbs the COMPLEMENT of the liquid's colour — a green acid
  // must absorb red and blue, which is what leaves it green at depth. Scaled
  // by opacity so a dense liquid kills light faster. The floor keeps even a
  // notionally clear liquid from being a perfect vacuum.
  p.absorbK = (vec3f(1.0) - base) * (op * TUNE_SUB_ABSORB_GAIN) +
              vec3f(TUNE_SUB_ABSORB_FLOOR);

  // In-scatter colour. A dense liquid scatters more of its own colour back at
  // you (it is what you see instead of the scene), a clear one much less.
  p.scatter = base * mix(TUNE_SUB_SCATTER_DENSE, TUNE_SUB_SCATTER_CLEAR,
                         p.clarity);

  // A dense medium presses in at the edges of vision harder than a clear one.
  p.vignette = clamp(TUNE_SUB_VIGNETTE * mix(1.6, 1.0, p.clarity), 0.0, 0.95);
  // Snell's window needs a medium you can see the sky through at all.
  p.snellGain = TUNE_SUB_SNELL_GAIN * p.clarity;

  // ---- WATER'S REFINEMENT ----
  // Water is the one liquid whose submerged look has been tuned by eye rather
  // than derived, and those hand-set coefficients are better than the generic
  // curve can be — the per-channel red kill that makes water read as water is
  // not recoverable from a palette average. So the derivation above is the
  // DEFAULT and this is an override on top of it, not the other way round.
  //
  // Keyed on clarity rather than on a tag or an id: any liquid authored as
  // clear as water gets water's treatment, which is the correct generalisation
  // ("clear liquids behave like this") rather than a special case for one
  // material. The blend means there is no cliff — a liquid authored slightly
  // murkier than water slides smoothly off the refined values onto the
  // derived ones.
  let refined = smoothstep(TUNE_SUB_CLEAR_LOW, TUNE_SUB_CLEAR_HIGH, p.clarity);
  p.absorbK = mix(p.absorbK, TUNE_SUB_ABSORB, refined);
  p.scatter = mix(p.scatter, TUNE_SUB_SCATTER, refined);
  p.visM = mix(p.visM, TUNE_SUB_VISIBILITY, refined);
  return p;
}

// ---- the full submerged shade ----
// Replaces the old two-line `underwater` branch. `sceneBehind` is whatever the
// primary march resolved — a rock, the bed, or the underside of the surface —
// and `pathVox` is how far the ray travelled through the liquid to get there.
//
// Returns the final colour for a pixel whose ray is inside the liquid for its
// whole length. Because the medium covers the entire view, this REPLACES
// aerial perspective rather than composing with it: fogging air in front of
// water that the eye is already inside would double-count the same haze.
fn shadeSubmerged(ro : vec3f, rd : vec3f, mat : u32, pathVox : f32,
                  sceneBehind : vec3f, px : vec2f, sawSky : bool) -> vec3f {
  let m = materials[mat];
  let distM = max(pathVox, 0.0) * VOXEL_METERS;

  // ---- absorption + in-scatter ----
  // Derived per liquid from its authored palette and opacity, with water's
  // hand-tuned coefficients blended in at the clear end. See submergedProfile:
  // there is no "is this water" test anywhere in here, only "how clear is it".
  let prof = submergedProfile(m);
  let absorbK = prof.absorbK;
  let scatterCol = prof.scatter;

  // The scatter colour is lit by the key light, so a pond at night is dark
  // water rather than the same daytime turquoise at lower brightness.
  //
  // The 0.45 is not a fudge: in-scattered light has been scattered out of the
  // beam before reaching the eye, so it is intrinsically dimmer than the
  // direct sun that a surface reflects. Driving it at full keyLightColor()
  // makes the water itself as bright as a sunlit surface, which flattens the
  // entire view into one luminance and is what "washed out" looks like.
  let sunUp = clamp(keyLightDir().y * 1.5, 0.05, 1.0);
  let ambientWater =
      scatterCol * keyLightColor() * sunUp * TUNE_SUB_SCATTER_GAIN * 0.45;

  // ---- the surface, seen from underneath (Snell's window) ----
  // A ray that reached the sky left through the underside of the surface, and
  // that interface is emphatically not a window: going water -> air the light
  // bends AWAY from the normal, so the entire 180-degree hemisphere above
  // compresses into a cone of about 97 degrees straight up. Outside that cone
  // there is TOTAL internal reflection — the surface is a mirror showing you
  // the murk below, not the sky.
  //
  // That bright disc ringed by dark mirror is the single most recognisable
  // thing about looking up underwater, and without it a submerged view of the
  // sky is just the normal sky slightly tinted, which reads as a bug.
  //
  // GATED ON CLARITY. A window is only a window if the medium transmits: in
  // oil there is no disc of sky above you, just dark. The refraction physics
  // are identical in any liquid, but at opacity 235 nothing survives the trip
  // to the eye, so drawing a bright sky disc through sludge is the single most
  // obviously wrong thing this function could do. snellGain carries the fade,
  // so the term disappears smoothly as a liquid is authored murkier.
  var behind = sceneBehind;
  if (sawSky && prof.snellGain > 0.001) {
    // Angle off vertical. The critical angle for water is asin(1/1.333) =
    // 48.6 degrees, i.e. cos = 0.661 — that is the edge of the window.
    let cosUp = clamp(rd.y, -1.0, 1.0);
    const CRIT_COS : f32 = 0.661;
    // Ripple the boundary rather than letting it be a clean circle: the real
    // edge shimmers because the surface itself is moving, and a hard analytic
    // ring immediately reads as a post-effect. Reuse the ripple field so the
    // distortion agrees with the waves being drawn everywhere else.
    let pm = vec2f(ro.x + rd.x * pathVox, ro.z + rd.z * pathVox) * VOXEL_METERS;
    let s = rippleSlope(pm, R.time, 0.0) * TUNE_SUB_SURFACE_RIPPLE;
    let edge = CRIT_COS + (s.x + s.y) * 0.35;
    // Inside the window: the sky, but squeezed. Outside: the murk, mirrored.
    let window = smoothstep(edge - 0.10, edge + 0.06, cosUp);
    // The sky inside the window is compressed toward the zenith. Scaling the
    // horizontal component of the direction toward vertical is a cheap,
    // monotonic stand-in for the real refraction map and gets the important
    // part right: the horizon ring crowds into the rim of the disc.
    var skyDir = normalize(vec3f(rd.x * 0.62, max(rd.y, 0.05), rd.z * 0.62));
    skyDir += vec3f(s.x, 0.0, s.y) * 0.25;
    let windowSky = skyColorNoBodies(normalize(skyDir)) * prof.snellGain;
    behind = mix(scatterCol * keyLightColor() * sunUp * 1.2, windowSky, window);
  }

  // ---- extinction ----
  // ONE transmittance term, not two. The first cut applied Beer-Lambert AND
  // then mixed the result toward the water colour again over `subVisibility`,
  // which double-counts the same falloff: every surface past a couple of
  // metres landed on the scatter colour twice and the whole view flattened
  // into a uniform pale wash with no contrast left in it. The bed rendered as
  // a featureless white sheet — brighter than the water, which is backwards.
  //
  // So `subVisibility` folds INTO the extinction coefficient rather than
  // being a second blend on top of it. It stays the predictable "distance at
  // which things disappear" knob (at distM == subVisibility the view is
  // 1/e ~ 37% original), and the per-channel absorption still tilts the hue
  // with depth on top of it.
  // Per liquid, so a murky one goes blind close in and a clear one does not.
  let extinction = absorbK + vec3f(1.0 / prof.visM);
  let trans = exp(-extinction * distM);
  var color = behind * trans + ambientWater * (vec3f(1.0) - trans);

  // ---- god rays and silt ----
  // BOTH GATED ON CLARITY, and both skipped outright in a dense liquid.
  //
  // Visually: a shaft of sunlight is only visible because the medium transmits
  // it far enough to be seen as a beam, and suspended motes are only visible
  // if light reaches them. In oil neither survives a centimetre, so drawing
  // sunbeams and drifting specks inside sludge reads as water with the wrong
  // colour rather than as oil.
  //
  // And they are the two most expensive terms in the function — godRays is a
  // march with a real occlusion trace per sample. Skipping them where they
  // cannot be seen means a dense liquid is also the CHEAP case, which is the
  // right way round: you are usually submerged in something dense because you
  // fell in it, and that is a bad moment for a frame-time spike.
  if (prof.clarity > 0.08) {
    // Added, not mixed: scattered light is light ARRIVING at the eye from the
    // volume, on top of whatever survived from behind.
    let shafts = godRays(ro, rd, max(pathVox, 1.0), px) * prof.clarity;
    color += ambientWater * shafts;

    // Motes are lit by the same shaft integral, so ones inside a beam flare
    // and ones in shadow stay dim — which is what makes the beams look like
    // they occupy space rather than being painted over the image.
    let motes = siltMotes(ro, rd, max(pathVox, 1.0), shafts) * prof.clarity;
    color += ambientWater * motes * 2.0 + vec3f(motes * 0.05);
  }

  // ---- the surface overhead, in a medium too dense to see through ----
  // In a dense liquid the sky is gone (the Snell window above is gated off),
  // and what that left was a completely featureless field of colour - correct
  // in the sense that you genuinely cannot see anything, but it reads as a
  // broken shader rather than as being submerged in oil. There is no cue for
  // which way is up and nothing moves, so the frame looks static even as the
  // camera turns.
  //
  // The physical answer is that even a near-opaque medium transmits a LITTLE
  // light from above, and it arrives smeared into a soft directional gradient
  // rather than an image. That is what this adds: a faint glow toward the
  // surface, modulated by a slow churn field so it drifts and gives the volume
  // orientation and motion without ever resolving into anything you could
  // mistake for a view.
  //
  // Gated to the murky case - a clear liquid already has the Snell window and
  // does not need a stand-in for it.
  let murk = 1.0 - prof.clarity;
  if (murk > 0.25) {
    let up = clamp(rd.y, 0.0, 1.0);
    let pm = (ro + rd * min(pathVox, 24.0)) * VOXEL_METERS;
    let churn = valueNoise(vec3f(pm.x * 0.7, pm.z * 0.7, R.time * 0.08), 1.0);
    let glow = pow(up, 2.5) * mix(0.55, 1.0, churn);
    // Daylight that has struggled through the medium, so it carries both the
    // key light's colour and the liquid's own tint.
    color += scatterCol * keyLightColor() * sunUp * glow *
             TUNE_SUB_MURK_GLOW * murk;
  }

  // ---- vignette ----
  // Cheap, and a strong "you are inside a medium" cue: light reaching the edge
  // of your view underwater has travelled further through it. Keyed on the
  // angle off the view axis rather than on screen UV so it does not stretch
  // with aspect ratio. Stronger in a dense liquid, which is what makes being
  // in oil feel like being in oil.
  let off = 1.0 - clamp(dot(rd, R.camFwd), 0.0, 1.0);
  color *= 1.0 - clamp(off * prof.vignette * 2.5, 0.0, prof.vignette);

  return color;
}

// ---- the full water shade ----
// `sceneBehind` is whatever the primary march already resolved BEHIND the
// water (lake bed, terrain, or sky) — this function decides how much of it
// survives the trip back up through the water, and what covers the rest.
//
// Returns the final color for a pixel whose primary ray crossed a water
// surface. `underwater` flips the treatment: from below there is no sky to
// reflect and the absorption applies to the whole view, not just the depth.
// ---- translucent solids: ice, glass ----------------------------------------
// A translucent solid is the same surface-plus-volume problem as water, but it
// is NOT water with different constants, and the differences are what make ice
// read as ice:
//
//   * No ripples, no caustics, no fullness gradient. Ice is a rigid slab; its
//     normal is the flat voxel face, perturbed only by a little frost grain.
//     Driving it with the liquid column height (waterNormal) would tilt a
//     frozen surface as though it were still flowing.
//   * Absorption is much stronger per metre than water and biased to keep the
//     cyan. That is the whole "thin ice is clear, thick ice is deep blue"
//     behaviour, and it comes out of ONE authored number via Beer-Lambert
//     rather than a per-thickness alpha.
//   * The internal scatter term is what separates ice from glass. Ice is full
//     of trapped bubbles and grain boundaries, so it glows slightly from
//     within rather than being a clean window; glass authored with a low
//     scatter stays a window.
//
// `sceneBehind` is everything the march already resolved past the slab — the
// pond bed, the terrain, the sky. This decides how much of it survives the
// trip through, and what covers the rest.
fn shadeTranslucent(hitP : vec3f, rd : vec3f, mat : u32, cell : vec3<i32>,
                    axis : i32, sgn : f32, pathVox : f32,
                    sceneBehind : vec3f, tSurf : f32, px : vec2f) -> vec3f {
  let m = materials[mat];

  // ---- normal: flat voxel face + frost grain ----
  // The grain is a small, stable per-voxel perturbation, not a wave: it breaks
  // the mirror up so a frozen pond does not read as one flat plate of glass,
  // and it is the same surfaceGrain the opaque solids use, so ice sits in the
  // same visual family as the rest of the world.
  var n = vec3f(0.0);
  n[axis] = -sgn;
  // Perturb across the two axes that are not the face normal, so the face
  // stays facing outward and only tilts. Sampled in WORLD space rather than
  // per-cell so the frost reads as one continuous field across a frozen
  // surface instead of stopping at every voxel boundary.
  let a1 = (axis + 1) % 3;
  let a2 = (axis + 2) % 3;
  var nn = n;
  nn[a1] += (valueNoise(hitP, TUNE_ICE_GRAIN_SCALE) - 0.5) * TUNE_ICE_GRAIN;
  nn[a2] += (valueNoise(hitP + vec3f(37.0, 11.0, 5.0),
                        TUNE_ICE_GRAIN_SCALE) - 0.5) * TUNE_ICE_GRAIN;
  n = normalize(nn);

  let v = -rd;
  let cosI = clamp(dot(n, v), 0.0, 1.0);

  // ---- Fresnel (Schlick) ----
  // Same physics as water and the same reason it matters: head-on you look
  // through the ice, at a grazing angle it turns into a sheet of reflected
  // sky. Ice's index of refraction (1.31) is very close to water's (1.33), so
  // F0 sits at essentially the same 2%.
  let f0 = TUNE_ICE_F0;
  var fres = f0 + (1.0 - f0) * pow(1.0 - cosI, TUNE_ICE_FRESNEL_POWER);

  // ---- transmission: per-channel Beer-Lambert over the REAL path ----
  // pathVox is the distance the ray actually spent inside the slab, so a
  // grazing view through the same ice is correctly darker than a head-on one,
  // and a 1-voxel rim of new ice is nearly clear while a metre-thick block is
  // deep cyan. Absorption is derived from the material's own authored opacity
  // and palette so glass and ice differ without any material ids here.
  let depthM = max(pathVox, 0.0) * VOXEL_METERS;
  let base = (unpackColor(m.color0) + unpackColor(m.color1)) * 0.5;
  // Absorb the COMPLEMENT of the material colour, scaled by authored opacity:
  // a pale-cyan ice absorbs red hardest, which is what leaves thick ice blue.
  let k = (f32(m.opacity) / 255.0) * TUNE_ICE_ABSORB;
  let absorbK = (vec3f(1.0) - base) * k + vec3f(TUNE_ICE_ABSORB_FLOOR);
  let trans = exp(-absorbK * depthM);

  // ---- internal scatter ----
  // Trapped bubbles and grain boundaries scatter light back out, so ice is not
  // a clean window: it picks up its own colour with depth. Saturating with
  // depth (rather than growing without bound) is what keeps thick ice reading
  // as ice rather than as flat paint.
  let sunUp = clamp(keyLightDir().y, 0.0, 1.0);
  let scatterAmt = (1.0 - exp(-depthM * TUNE_ICE_SCATTER_DEPTH)) * TUNE_ICE_SCATTER;
  let scatterCol = base * mix(TUNE_ICE_SCATTER_NIGHT, 1.0, sunUp);

  // What survives the slab, plus what the slab itself adds.
  var through = sceneBehind * trans + scatterCol * scatterAmt;

  // ---- reflection ----
  // At grazing angles this is most of what you see, and it is what sells the
  // surface as solid and polished.
  //
  // But it is gated on the Fresnel weight, which water does NOT need to do,
  // and the difference is geometric rather than aesthetic: a lake is ONE
  // surface, so a traced reflection costs one secondary ray per water pixel.
  // A translucent solid is a volume the ray passes through, so a hollow glass
  // shell presents two surfaces per pixel and a stack of ice presents more —
  // and since the ray no longer terminates, every one of them would fire its
  // own reflection. Measured: an unconditional reflection here took the
  // selftest's glass ball from 85 fps to 6.
  //
  // Below the threshold the reflection is being mixed in at a few percent and
  // is genuinely invisible, so falling back to the sky lookup costs nothing
  // visually and skips the trace entirely. Head-on views — the common case,
  // and the one where you are looking THROUGH the ice anyway — take the cheap
  // path; grazing views still get the real reflection.
  var refl : vec3f;
  if (fres > TUNE_ICE_REFLECT_MIN) { refl = traceReflection(hitP, n, rd); }
  else { refl = reflectionSky(reflect(rd, n)); }

  // ---- specular glint ----
  // The sharp highlight that says "hard, smooth surface". Much tighter than
  // water's, because ice does not have a wave field to spread it out.
  let ld = keyLightDir();
  let hv = normalize(ld + v);
  let spec = pow(max(dot(n, hv), 0.0), TUNE_ICE_GLOSS) * TUNE_ICE_SPEC;
  let shadow = sunShadow(hitP, n, px);

  var color = mix(through, refl, fres);
  color += vec3f(spec) * shadow * keyLightColor();
  return color;
}

fn shadeWater(hitP : vec3f, rd : vec3f, mat : u32, cell : vec3<i32>,
              axis : i32, sgn : f32, pathVox : f32, surfFull : f32,
              sceneBehind : vec3f, tSurf : f32, underwater : bool) -> vec3f {
  let m = materials[mat];
  // Up-facing means the ray entered through the TOP of the liquid — the only
  // face that gets the fullness-gradient normal and the sky reflection.
  let upFacing = (axis == 1 && sgn < 0.0);
  var n = waterNormal(cell, mat, axis, sgn, hitP, upFacing);
  if (underwater) { n = -n; }   // seen from below, the interface faces down

  let v = -rd;                                  // toward the eye
  let cosI = clamp(dot(n, v), 0.0, 1.0);

  // ---- Fresnel (Schlick) ----
  // The single most important term. At 2% head-on and ~100% at grazing, this
  // is what makes water look wet: you see THROUGH it at your feet and see the
  // SKY in it at the far shore, across one continuous surface.
  var fres = WATER_F0 + (1.0 - WATER_F0) * pow(1.0 - cosI, TUNE_WATER_FRESNEL_POWER);
  // Non-water liquids (oil, acid, blood) are dielectrics too but far more
  // absorbing; they get the same interface with a muted reflection so they
  // read as their own substance rather than all becoming "water".
  let isWater = (m.tagMask != 0u) && (f32(m.opacity) / 255.0 < 0.45);
  if (!isWater) { fres *= 0.55; }
  // A partially-filled surface cell is a thin film / spray, not a mirror:
  // fade the specular interface out with fullness so a 1/8 puddle skin doesn't
  // reflect the sky as hard as a lake does.
  fres *= mix(0.35, 1.0, surfFull);
  if (!upFacing) { fres *= 0.5; }   // side walls: glancing, but not mirrors

  // ---- transmitted light: per-channel Beer-Lambert ----
  // Depth in METRES, so the look is independent of voxel size. Absorption is
  // applied per channel, so the bed goes green-cyan then blue-black with depth
  // instead of uniformly darkening. Path length is doubled for the down-and-
  // back trip only when we can see a bed; for an unbounded view the march
  // already accumulated the true path.
  let depthM = max(pathVox, 0.0) * VOXEL_METERS;
  var absorbK = WATER_ABSORB;
  var scatter = WATER_SCATTER;
  if (!isWater) {
    // Other liquids: derive the absorption from their authored opacity and
    // palette so oil stays black-brown and acid stays acid-green, without
    // hardcoding material IDs (CLAUDE.md conventions).
    let base = (unpackColor(m.color0) + unpackColor(m.color1)) * 0.5;
    let k = (f32(m.opacity) / 255.0) * 9.0;
    // absorb the COMPLEMENT of the material color: a green liquid must absorb
    // red and blue, which is what leaves it looking green at depth
    absorbK = (vec3f(1.0) - base) * k + vec3f(0.05);
    scatter = base * 0.22;
  }
  let trans = exp(-absorbK * depthM);

  // ---- caustics ----
  // Sunlight refracting through the wave surface focuses into the bright
  // shifting web everyone recognises on a lake bed, and its absence is a
  // strong "this is fake" cue even when the absorption is right.
  //
  // Proper caustics need photon transport; the standard real-time cheat is
  // that the caustic intensity tracks the CONVERGENCE of the refracted rays,
  // and for a small-slope surface that convergence is the curvature of the
  // wave height field. Sampling the ripple slope at two nearby points and
  // taking the difference gives that curvature for a couple of extra ALU ops
  // and no new data.
  //
  // Projected along the SUN direction, not straight down, so the pattern
  // shifts across the bed with the sun's angle instead of being pinned under
  // the waves that cast it.
  var lit = sceneBehind;
  if (!underwater && depthM > 0.02) {
    lit *= waterCaustics(hitP, rd, pathVox, depthM);
  }

  // What comes back up: the bed (plus its caustics), filtered by the water
  // column, plus the column's own in-scattered light (which is what keeps
  // deep water blue rather than black).
  var refracted = lit * trans + scatter * (vec3f(1.0) - trans);

  // ---- reflection ----
  var reflection : vec3f;
  if (underwater) {
    // From below, the sky is compressed into Snell's window and everything
    // outside it is total internal reflection of the murk. Approximating that
    // with the in-scatter color is both cheap and closer than a sky lookup.
    reflection = scatter * 1.6;
  } else if (upFacing) {
    // Only spend a secondary ray where the reflection can actually be SEEN.
    // Fresnel runs from 2% head-on to ~100% at grazing, so a top-down water
    // pixel is ~98% refracted and a traced reflection changes it by less than
    // the dither — while costing exactly as much as a grazing pixel where the
    // reflection is the whole image. Below the cutoff, take the sky: at that
    // weight the difference between a real reflection and a sky sample is not
    // resolvable, and this is what stops a screenful of water from firing a
    // full-budget ray per pixel for no visible return.
    if (fres > TUNE_REFLECTION_CUTOFF) {
      reflection = traceReflection(hitP, n, rd);
    } else {
      reflection = reflectionSky(reflect(rd, n));
    }
  } else {
    reflection = reflectionSky(reflect(rd, n));
  }

  var color = mix(refracted, reflection, fres);

  // ---- sun glint ----
  // Sharp Blinn-Phong lobe on the RIPPLED normal. This is what turns a
  // correct-but-dull surface into something that reads as water in motion:
  // the ripple slopes scatter the highlight into moving sparkle rather than
  // one blob. Gated on the sun being above the horizon relative to the normal.
  if (upFacing && !underwater) {
    // Key light, so a moonlit lake gets a moon track instead of staying flat
    // black at night — a still lake under a full moon is one of the strongest
    // night-lighting cues there is.
    let kd = keyLightDir();
    let hv = normalize(kd + v);
    // Roughen the lobe with distance to match the ripple damping in
    // waterNormal(): those flattened far normals would otherwise all agree and
    // collapse the sparkle into one hard mirror disc. Widening the lobe as the
    // ripples fade spreads that energy back out into a glitter path, which is
    // what a real sun track on water looks like.
    let distM = length(hitP - R.camPos) * VOXEL_METERS;
    // Keep the lobe TIGHT with distance rather than widening it. Widening it
    // was wrong: as the ripple damping flattens the far normals they all agree,
    // and a broad lobe over agreeing normals integrates into one blown-out
    // white slab across the sun track instead of a glitter path. A narrow lobe
    // on flat water gives a small bright highlight, which is correct.
    let power = mix(TUNE_GLINT_POWER_NEAR, TUNE_GLINT_POWER_FAR, clamp(distM / 40.0, 0.0, 1.0));
    let spec = pow(max(dot(n, hv), 0.0), power);
    // Modulated by Fresnel so the glint follows the same angular law as the
    // reflection instead of floating on top of it. Capped: the highlight is a
    // bloom cue, not a light source, and letting it run to 2.6x saturates a
    // whole band of the lake to flat white and destroys the ripple detail
    // underneath it.
    // Tinted by the key light so the track is amber at sunset and cold blue
    // under the moon, matching whatever is actually casting it.
    let glintTint = normalize(keyLightColor() + vec3f(1e-4)) * 1.732;
    color += glintTint * min(spec, 1.0) * TUNE_GLINT_INTENSITY * (0.25 + fres);
  }

  // ---- shoreline foam ----
  // Where the water column is thin, it is meeting the bed — a shore or a
  // sandbar. Real shorelines break there. A touch of brightening keyed on
  // shallowness, broken up by the ripple field so it isn't a clean contour
  // ring, sells the boundary between water and land far better than the hard
  // color step it replaces.
  if (upFacing && !underwater) {
    let shallow = 1.0 - smoothstep(0.0, TUNE_FOAM_DEPTH, depthM);
    // FOAM ON CONVERGENCE LINES (plan component 9). Surface water piles up
    // where the flow converges — the inflow line of a drain, the seam where a
    // jet's outward spread meets the bank — and that is where the froth
    // collects. The current field is ANALYTIC, so its divergence is available
    // without storing anything: currentConvergeAt is four evaluations of a
    // function that early-outs to one compare where there is no current, so a
    // still lake pays nothing for this.
    let conv = currentConvergeAt(hitP, &R) * VOXEL_METERS;   // per second
    let flowFoam = clamp((conv - TUNE_WAVE_FOAM_THRESHOLD) * TUNE_WAVE_FOAM_GAIN,
                         0.0, 1.0);
    let amount = max(shallow, flowFoam);
    if (amount > 0.0) {
      let pm = vec2f(hitP.x, hitP.z) * VOXEL_METERS;
      // reuse the ripple field as the foam mask so foam moves with the waves
      // (undamped: foam is a shoreline feature, always near the camera, and
      // damping it would dissolve the far shore's foam line)
      let s = rippleSlope(pm, R.time, 0.0);
      let mask = smoothstep(0.010, 0.055, length(s));
      color = mix(color, vec3f(0.92, 0.95, 0.97), amount * mask * TUNE_FOAM_STRENGTH);
    }
  }

  return color;
}

// ============================================================================
// VISCOUS LIQUIDS — BLOOD (DESIGN.md §9)
// ============================================================================
// Blood is a third case, distinct from both water and lava, and reusing either
// gets it wrong.
//
// Water's look is REFRACTION and DEPTH: you see through it, the bed shifts, the
// colour tells you how deep it is, and a wind-driven ripple field covers the
// whole surface. Lava's look is EMISSION. Blood is neither — it is a nearly
// opaque, strongly absorbing, VISCOUS liquid, and its look is almost entirely:
//
//   1. a WET SHEEN — a tight specular highlight. This is the single term that
//      makes a red patch read as fluid rather than as red paint, and it is
//      what survives at every scale from a single droplet to a pool.
//   2. NO travelling ripples. Water's five wave bands are wind-driven gravity
//      waves on an open surface; a splash of blood is centimetres across and
//      far too viscous to carry them. Running the water ripple field over
//      blood makes a puddle look like it is boiling — this is the most
//      important thing NOT to inherit.
//   3. depth that saturates almost immediately. A few centimetres of blood is
//      already opaque, so unlike water there is no bed to see and no
//      shallow-to-deep colour ramp worth modelling; what varies with thinness
//      is how much of the SURFACE UNDER it shows through.
//   4. a dark, desaturating rim where it thins out to nothing — the edge of a
//      real splatter is browner and darker than its middle, not a clean
//      contour of the same red.
//
// ---- MOVING vs POOLED, which is the requirement that shapes this ----
// Blood in this game comes out of NPCs, so the overwhelmingly common case is
// blood that is NOT a still pool: single voxels in flight, thin trails running
// down a wall, a spray of disconnected droplets. Those need to read as WET and
// BRIGHT and self-contained — a droplet has high curvature, so it catches a
// broad highlight and shows its own colour, not the colour of a deep column.
//
// A still pool is the opposite: flat, darker, more mirror-like, with a coherent
// surface that can hold a sharp reflection of the sky.
//
// `bloodPooling` measures which of the two a hit is, exactly the way
// moltenPooling does for lava (and for the same reason — a treatment that
// assumes a continuous surface paints nonsense onto an isolated speck). The
// shade then interpolates every term along that axis. See the function itself
// for why the neighbourhood is sampled in 3D here where lava's is horizontal.
//
// All render-only float math on render-only data — the sim never sees it.

// ---- how OILY is this viscous liquid? ----
// 0 = a blood-like biological fluid, 1 = a petroleum-like one. Both take the
// viscous surface path (isViscousLiquid), but they look nothing alike, and the
// blood constants applied to oil are what made the oil pool render as a sheet
// of flat beige mud: matte, desaturated, no highlight and no reflection.
//
// Three things separate them physically, and all three follow from this one
// number: oil is GLOSSY (a smooth mirror-dark film, where blood is a diffuse
// suspension), oil is DARK and near-neutral, and oil carries a thin-film
// IRIDESCENCE that nothing biological does.
//
// Derived from the AUTHORED PALETTE, not from a material id and not from a new
// JSON key - the same principle isViscousLiquid itself follows, and for the
// same reason: any modder's petroleum-like liquid gets the treatment for free,
// and nothing here has to be kept in step with materials.json.
//
// SATURATION is the discriminator. Blood's authored colour0 is 0.85 saturated;
// oil's is 0.46. Biological fluids are strongly chromatic (haemoglobin,
// chlorophyll, bile) because they are pigment suspensions; petroleum is a dark
// near-neutral brown-black. The measure is scale-free, so authoring oil
// lighter or darker changes how it reads, not what it IS.
fn oiliness(m : Material) -> f32 {
  let c = unpackColor(m.color0);
  let mx = max(c.r, max(c.g, c.b));
  let mn = min(c.r, min(c.g, c.b));
  // HSV saturation. Pure black reads as fully oily, which is right: a black
  // liquid is far closer to oil than to blood.
  let sat = select((mx - mn) / max(mx, 1e-4), 0.0, mx < 1e-4);
  // Wide band on purpose: blood (0.85) firmly at 0, oil (0.46) firmly at 1,
  // with a real gradient between so a liquid authored in the middle blends
  // rather than falling off a cliff.
  return 1.0 - smoothstep(TUNE_OIL_SAT_LOW, TUNE_OIL_SAT_HIGH, sat);
}

// ---- is this liquid FLOATING ON another one? ----
// Returns 1 when a lighter liquid is lying on top of a heavier, different one
// — oil on water — and 0 for a pool of the stuff on its own.
//
// This is the gate for the iridescent sheen, and the physics is the reason it
// has to exist. Thin-film interference needs a FILM: two closely spaced
// interfaces, so light bouncing off the top can interfere with light bouncing
// off the bottom. Oil spread on water is exactly that (an air/oil interface a
// few microns above an oil/water one) and it is why a puddle in a car park
// shows rainbows. A deep pool of oil on rock has no second interface anywhere
// near the surface — the bottom is metres down and the light never gets there
// — so it is just a dark glossy liquid, and painting rainbows on it is the
// giveaway that the effect is decoration rather than a model of anything.
//
// Probes DOWNWARD for a different liquid with a HIGHER density. Density is
// what decides which floats (the sim already orders liquids by it), so this
// asks the same question the sim does and cannot disagree with what the world
// actually did. No material ids: any light liquid on any heavy one gets a
// sheen, which is the correct generalisation.
fn floatingOnLiquid(cell : vec3<i32>, mat : u32) -> f32 {
  let m = materials[mat];
  // A film is THIN. Probing far down would find the water under a metre-deep
  // oil column and call it a film, which is the case this exists to exclude,
  // so the probe reaches only a few voxels.
  for (var i = 1; i <= 3; i++) {
    let c = cell + vec3<i32>(0, -i, 0);
    if (!inBounds(c)) { return 0.0; }
    let w = voxWordAt(c);
    let bm = voxMat(w);
    if (bm == MAT_AIR) { return 0.0; }        // nothing under it
    if (bm == mat) { continue; }              // still our own liquid: keep going
    let b = materials[bm];
    if (b.klass != CLASS_LIQUID) { return 0.0; }  // resting on a solid bed
    // A DIFFERENT liquid, and denser than us, so we are the one floating.
    // Fade in with how much denser it is: a marginal difference is a mixture,
    // a large one is a genuine layer boundary.
    if (b.density > m.density) {
      let ratio = f32(b.density - m.density) / max(f32(m.density), 1.0);
      return clamp(ratio * TUNE_OIL_FLOAT_SENS, 0.0, 1.0);
    }
    return 0.0;
  }
  // Ran out of probe without finding anything: too deep to be a film.
  return 0.0;
}

// ---- thin-film interference (the rainbow sheen on oil) ----
// The one thing everybody recognises oil by. A film microns thick makes light
// reflected off its TOP surface interfere with light reflected off its BOTTOM,
// and which wavelengths cancel depends on the optical path difference - so the
// colour swims with viewing angle and with film thickness.
//
// Modelled the standard cheap way: drive a phase from (thickness / cos of the
// refracted angle), then convert to RGB with three cosines 120 degrees apart.
// That is not a spectral integral, but it produces the right BEHAVIOUR - bands
// that slide across the surface as the eye moves and as the film varies -
// which is the whole visual signature. A static rainbow texture is not.
//
// Thickness varies via the same value-noise field the rest of the renderer
// uses, animated slowly: a real slick's film is dragged around by the fluid
// under it, and a uniform film would show one flat colour rather than bands.
fn filmIridescence(p : vec3f, cosI : f32) -> vec3f {
  let pm = p * VOXEL_METERS;
  // Two octaves at different rates so the bands drift and stretch rather than
  // sliding rigidly, which is what reads as liquid rather than as a scrolling
  // texture.
  // LOW spatial frequency, deliberately. At 1.7 and 3.1 cycles per metre the
  // interference bands land near PIXEL scale across a slick at any real
  // viewing distance, and the field aliases into a shimmering moire that reads
  // as a broken screen rather than as oil. A real film's thickness varies over
  // tens of centimetres, so the bands should be broad, soft and few.
  let t1 = valueNoise(vec3f(pm.x * 0.45, pm.y * 0.45 + R.time * 0.05, pm.z * 0.45), 1.0);
  let t2 = valueNoise(vec3f(pm.z * 0.8 - R.time * 0.03, pm.x * 0.8, pm.y * 0.8), 1.0);
  let thick = mix(t1, t2, 0.4) * TUNE_OIL_FILM_SCALE;
  // Optical path difference grows as the ray slants through the film, which is
  // why the bands crowd toward a grazing view. That angular term is most of
  // what sells it as interference rather than as painted-on colour.
  let opd = thick / max(cosI, 0.18);
  let ph = opd * 6.28318;
  let rgb = vec3f(cos(ph), cos(ph - 2.0944), cos(ph + 2.0944)) * 0.5 + vec3f(0.5);
  // Kept LINEAR rather than squared. Squaring deepens the gaps between bands
  // and pushes them toward primaries, which on a real surface reads as a
  // psychedelic decal instead of a faint oily sheen; the raw cosines are
  // already pastel, which is what a slick looks like away from its thinnest
  // fringes.
  return rgb;
}

// Returns 0 for an isolated droplet / thin trail and 1 for the interior of a
// pool. Sampled in ALL THREE axes, unlike moltenPooling's horizontal-only
// probe: a one-voxel-deep sheet of lava spread on a floor is still a pool and
// should crust, but a one-voxel-wide RUN of blood down a wall is precisely the
// "moving" case this needs to catch. Vertical extent is the signal that tells a
// wall trail (tall, thin, moving) from a floor pool (wide, flat, still), so it
// has to be part of the measurement.
fn bloodPooling(cell : vec3<i32>, mat : u32) -> f32 {
  var n = 0.0;
  var total = 0.0;
  // 2-voxel spacing over a 5^3-ish neighbourhood: wide enough that a pool
  // interior saturates, tight enough that a 2-3 voxel droplet does not.
  for (var dx = -2; dx <= 2; dx += 2) {
    for (var dy = -2; dy <= 2; dy += 2) {
      for (var dz = -2; dz <= 2; dz += 2) {
        total += 1.0;
        let c = cell + vec3<i32>(dx, dy, dz);
        if (!inBounds(c)) { continue; }
        if (occTotal(chunkOcc(c)) == 0u) { continue; }
        if (voxMat(voxWordAt(c)) == mat) { n += 1.0; }
      }
    }
  }
  // Threshold placed LOW for the same reason moltenPooling's is: the
  // interesting distinction is "isolated droplet" vs "part of a body", and a
  // high threshold would put every pool's rim in the transition band and draw
  // a bright ring around each puddle.
  return smoothstep(TUNE_BLOOD_POOL_LOW, TUNE_BLOOD_POOL_HIGH, n / max(total, 1.0));
}

// ---- the smooth liquid field ----
// THE function that decides whether blood reads as fluid or as a heap of
// gelatin cubes, so it is worth being explicit about why it exists.
//
// A voxel liquid hit gives you an axis-aligned face normal. Water gets away
// with replacing that by the gradient of the COLUMN HEIGHT (waterNormal): a
// lake is a wide, essentially 2D surface, its top is a height field, and the
// slope of that field is the true macro normal.
//
// Blood is not a height field. It arrives as droplets, runs down walls, and
// pools in patches a few voxels across — fully 3D, and often only one or two
// voxels thick. Applying the height-field treatment to it leaves every side
// and bottom face with its raw voxel normal, and the eye reads the result as
// exactly what it is: individually shaded cubes. Adding a per-cell dome on top
// (which an earlier version of this did) makes it worse, not better — it
// renders each voxel as a rounded cube, which is precisely the gelatin look.
//
// The fix is the standard one for voxel fluids: treat the liquid as a scalar
// DENSITY FIELD, sample it with trilinear interpolation so it is continuous
// across cell boundaries, and take its gradient as the normal. Because the
// interpolation is continuous, so is the normal, and the cube structure
// dissolves into one smooth surface — the same reason marching cubes produces
// smooth isosurfaces from blocky data.
//
// Density is the liquid's FULLNESS (the state nibble, DESIGN.md §4), which the
// sim already maintains: 0 for a cell that is not this liquid, 1/8..8/8 for one
// that is. So a droplet is a small blob of density in an empty field and comes
// out spherical; a pool is a slab and comes out flat on top. Both fall out of
// the same code with no special cases.
fn liquidDensityAt(c : vec3<i32>, mat : u32) -> f32 {
  if (!inBounds(c)) { return 0.0; }
  let w = voxWordAt(c);
  if (voxMat(w) != mat) { return 0.0; }
  return f32(voxState(w) + 1u) / 8.0;
}

// Trilinearly-interpolated density at an arbitrary world point. Samples on the
// lattice of cell CENTRES (hence the -0.5), so the field is smooth everywhere
// rather than piecewise-constant per cell.
fn liquidFieldAt(p : vec3f, mat : u32) -> f32 {
  let g = p - vec3f(0.5);
  let b = floor(g);
  let f = g - b;
  let c0 = vec3<i32>(b);
  var acc = 0.0;
  // 8 corners, standard trilinear weights.
  for (var i = 0; i < 8; i++) {
    let o = vec3<i32>(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    let wgt = mix(1.0 - f, f, vec3f(o));
    acc += liquidDensityAt(c0 + o, mat) * wgt.x * wgt.y * wgt.z;
  }
  return acc;
}

// Gradient of that field = the smooth surface normal. Central differences at a
// one-voxel baseline: wide enough to span a cell (so the normal reflects the
// neighbourhood's shape rather than one cell's face) and narrow enough to keep
// a droplet's curvature.
//
// 6 field samples x 8 taps = 48 voxel reads. That is the real cost of this
// function, and it is why it runs ONLY on the primary blood surface hit — not
// in reflections, not in shadow rays, and not on any other material.
fn liquidFieldNormal(p : vec3f, mat : u32, fallback : vec3f) -> vec3f {
  // Sampling baseline in voxels. This is the smoothing control: at 1.0 the
  // gradient spans one cell either side, which removes the per-voxel faceting
  // while keeping a droplet's shape. Raising it smooths harder (a blobbier,
  // more merged surface) at the cost of small-detail shape; below ~0.5 the
  // samples fall inside a single cell and the cubes come back.
  let e = TUNE_BLOOD_SMOOTH;
  let gx = liquidFieldAt(p + vec3f(e, 0.0, 0.0), mat) -
           liquidFieldAt(p - vec3f(e, 0.0, 0.0), mat);
  let gy = liquidFieldAt(p + vec3f(0.0, e, 0.0), mat) -
           liquidFieldAt(p - vec3f(0.0, e, 0.0), mat);
  let gz = liquidFieldAt(p + vec3f(0.0, 0.0, e), mat) -
           liquidFieldAt(p - vec3f(0.0, 0.0, e), mat);
  // The gradient points INTO the liquid (density increases inward), so the
  // outward surface normal is its negation.
  let g = vec3f(-gx, -gy, -gz);
  let len = length(g);
  // A degenerate gradient means the field is locally flat — the interior of a
  // large body, or a lone voxel whose neighbours are all empty. Neither has a
  // meaningful gradient, so fall back to the voxel face normal.
  if (len < 1e-4) { return fallback; }
  return g / len;
}

// The full viscous-liquid shade. `sceneBehind` is what the primary march
// resolved behind the blood; `pathVox` is how far the ray travelled inside it.
//
// Returns the final colour for a pixel whose ray crossed a viscous liquid
// surface. Deliberately NOT a variant of shadeWater(): it shares the Fresnel
// idea and nothing else, and every attempt to express it as water-with-
// different-constants ends up with either travelling ripples or a see-through
// puddle.
fn shadeViscous(hitP : vec3f, rd : vec3f, mat : u32, cell : vec3<i32>,
                axis : i32, sgn : f32, pathVox : f32, surfFull : f32,
                sceneBehind : vec3f, underwater : bool) -> vec3f {
  let m = materials[mat];
  let upFacing = (axis == 1 && sgn < 0.0);
  let pool = bloodPooling(cell, mat);
  // 0 = blood-like, 1 = petroleum-like. Every oil-specific term below rides
  // this, so a liquid authored between the two blends rather than switching.
  let oily = oiliness(m);

  // ---- normal ----
  // The smooth field gradient (see liquidFieldNormal) on EVERY face, not just
  // up-facing ones. This is the difference between fluid and gelatin cubes: a
  // trail running down a wall and a droplet in mid-air are shaded by the shape
  // of the blood AROUND them, so neighbouring voxels agree on their normal and
  // the surface reads as continuous.
  var flat = vec3f(0.0);
  flat[axis] = -sgn;
  var n = liquidFieldNormal(hitP, mat, flat);
  // Blend back toward the face normal on big flat pools. A pool's interior has
  // a weak, noisy gradient (the field is saturated in every direction), and
  // letting that noise drive the normal makes a still puddle shimmer; its true
  // surface really is flat and horizontal.
  if (upFacing) { n = normalize(mix(n, vec3f(0.0, 1.0, 0.0), pool * 0.5)); }

  {
    // A slow, low-amplitude wobble — surface tension relaxing, not wind waves.
    // Two orders of magnitude slower and shallower than the water ripples, and
    // faded out on pools, which genuinely are still. Perturbs the smooth normal
    // rather than a height slope, so it works on vertical runs too.
    let pm = hitP * VOXEL_METERS;
    let wob = TUNE_BLOOD_WOBBLE * (1.0 - pool * 0.75);
    n = normalize(n + vec3f(sin(pm.x * 9.0 + R.time * 0.6),
                            sin(pm.y * 10.0 + R.time * 0.45),
                            cos(pm.z * 11.0 + R.time * 0.5)) * wob);
  }
  if (underwater) { n = -n; }

  let v = -rd;
  let cosI = clamp(dot(n, v), 0.0, 1.0);

  // ---- Fresnel ----
  // Same dielectric law as water (blood's IOR is ~1.35, near enough), but the
  // grazing reflection is pulled down: an absorbing, slightly rough organic
  // fluid does not go to a 100% mirror at the horizon the way clean water does,
  // and letting it turns every pool edge into a bright white rim.
  //
  // OIL IS THE OPPOSITE CASE. Blood's grazing reflectance is pulled down
  // because it is a rough absorbing suspension; oil is a smooth dielectric film
  // and really does approach a mirror at the horizon - that hard bright rim is
  // the look, not the artifact the blood constant guards against. Oil also has
  // a higher IOR (~1.47 vs water's 1.33), so its head-on reflectance is about
  // double. Blending both endpoints on `oily` is what turns the flat matte
  // pool into something that reads as wet.
  let f0 = mix(TUNE_BLOOD_F0, TUNE_OIL_F0, oily);
  let graze = mix(TUNE_BLOOD_GRAZE, TUNE_OIL_GRAZE, oily);
  var fres = f0 + (graze - f0) *
             pow(1.0 - cosI, TUNE_WATER_FRESNEL_POWER);
  // Thin films are not mirrors — same reasoning as water's surfFull term.
  fres *= mix(0.45, 1.0, surfFull);
  if (!upFacing) { fres *= 0.6; }

  // ---- body colour ----
  // Blood is dense enough that a couple of centimetres is opaque, so rather
  // than water's per-channel Beer-Lambert over a deep column, the useful
  // variable is how much of the surface BEHIND shows through a THIN film.
  // Derived from the authored palette + opacity, never from a material ID, so
  // any liquid tagged viscous gets a coherent look (CLAUDE.md conventions).
  let deep = unpackColor(m.color1);   // darkest palette entry: pooled interior
  let bright = unpackColor(m.color2); // lightest: fresh, thin, oxygenated
  let depthM = max(pathVox, 0.0) * VOXEL_METERS;
  // Opacity drives how fast it goes opaque. The 55x is what turns the
  // authored 200/255 into "opaque past about 2 cm", which is the real scale.
  let k = (f32(m.opacity) / 255.0) * TUNE_BLOOD_ABSORB;
  let trans = exp(-k * depthM);

  // Thin blood reads BRIGHTER and more orange-red (less path length, more of
  // the light scattering straight back out); deep blood reads near-black
  // maroon. That ramp is the depth cue, in place of water's colour shift.
  var body = mix(bright, deep, clamp(depthM * TUNE_BLOOD_DEPTH_RAMP, 0.0, 1.0));
  // Pools are darker than droplets even at equal path length: more of the
  // light that enters a large body is absorbed before it can scatter back.
  body = mix(body, deep, pool * 0.35);
  // Oil goes DARKER still, and this is the term that kills the beige. Blood's
  // ramp is built around a suspension that backscatters brightly - thin blood
  // genuinely reads lighter and more orange. Petroleum does the opposite: it
  // absorbs almost everything that enters and reflects the rest off its
  // surface, so its body should approach black and let the reflection and the
  // glint carry the image. Rendering oil with blood's backscatter is what made
  // a pool of it look like a pan of wet clay.
  body = mix(body, deep * TUNE_OIL_DARKEN, oily);

  // What comes back out: the surface behind, filtered by the film, plus the
  // blood's own scattered colour. Blood scatters strongly (it is a suspension,
  // not a clear fluid), so the body colour dominates as soon as it is not a
  // one-voxel film — which is what keeps it from ever looking like tinted glass.
  //
  // TRANSMISSION IS CAPPED, and that cap is what stops blood reading as red
  // glass. Beer-Lambert alone is the wrong model for a dense suspension at
  // droplet scale: a lone voxel's path is a fraction of VOXEL_METERS, so
  // exp(-k*d) stays high and a large slice of the wall behind survives, no
  // matter how absorbing the material is authored to be. Real blood does not
  // work that way — it is opaque at well under a millimetre because it
  // BACKSCATTERS, and the ray never reaches the far side to begin with.
  // Clamping the surviving fraction models that scattering albedo directly, so
  // even a single droplet shows its own colour rather than the scene through it.
  let seeThrough = min(trans * TUNE_BLOOD_TRANSMIT, TUNE_BLOOD_MAX_TRANSMIT);
  var refracted = sceneBehind * seeThrough + body * (1.0 - seeThrough);

  // ---- reflection ----
  // A pool can hold a real reflection; a droplet cannot (there is no coherent
  // surface, and tracing a ray per droplet is a waste of the budget). Blend by
  // pooling, and only trace where the reflection is actually worth it.
  var reflection : vec3f;
  if (underwater) {
    reflection = body * 1.4;
  } else if (upFacing && pool > mix(0.5, 0.18, oily) &&
             fres > TUNE_REFLECTION_CUTOFF) {
    // The pooling threshold drops with oiliness. Blood needs a real pool before
    // a traced reflection is worth a secondary ray - a droplet has no coherent
    // surface to reflect anything. An oil slick is coherent at a much smaller
    // scale (that is what a slick IS: a film that spreads flat), and the
    // reflection is the DOMINANT term in its look rather than a garnish, so it
    // earns the ray far sooner.
    reflection = traceReflection(hitP, n, rd);
  } else {
    // The cheap fallback: a plain sky lookup. On a POOL that is a fine stand-in
    // for a traced ray, but on a droplet or a thin trail it is the other half
    // of oil reading as see-through. Fresnel at a grazing angle drives `color`
    // almost entirely to this term, and a droplet returning full-brightness
    // sky is indistinguishable from a droplet you are looking THROUGH.
    //
    // A real droplet does reflect the sky, but it is a tiny curved mirror
    // scattering it in every direction, so what reaches the eye is far dimmer
    // than the sky itself. Damping the fallback on unpooled oil models that,
    // and it is what keeps a spray of droplets reading as dark specks of oil
    // rather than as holes in the world.
    let sky = reflectionSky(reflect(rd, n));
    reflection = sky * mix(1.0, mix(TUNE_OIL_DROP_REFLECT, 1.0, pool), oily);
  }
  // Reflections off blood are TINTED by it — a dielectric this dark reflects a
  // dimmer, redder version of what a clean surface would.
  //
  // Oil is barely tinted at all, and that difference matters: a smooth
  // petroleum film is a near-NEUTRAL dark mirror, so what you see in it is the
  // sky and the far bank rather than a brown wash of its own body colour.
  // Pushing blood's tint onto oil was a large part of what flattened the pool
  // into mud - it dragged the one term carrying real scene information back
  // toward the same beige as everything else.
  let tintAmt = mix(0.5, TUNE_OIL_REFLECT_TINT, oily);
  reflection = mix(reflection, reflection * (bright + vec3f(0.25)), tintAmt);

  var color = mix(refracted, reflection, fres);

  // ---- the wet sheen ----
  // The load-bearing term. A tight specular lobe on the key light is what the
  // eye reads as "wet", and it is the one thing that has to work on a single
  // voxel in mid-air as well as on a pool.
  //
  // Unlike water's glint this is NOT gated to up-facing surfaces: a trail
  // running down a wall is wet too, and it is one of the main things the
  // player sees. Side faces get a slightly broader, weaker lobe since their
  // normal is the flat voxel face rather than a real gradient.
  {
    let kd = keyLightDir();
    let hv = normalize(kd + v);
    // Droplets get a BROADER lobe than pools. A bead's curvature spreads the
    // highlight over its whole face, while a flat pool concentrates it — using
    // the pool exponent on a droplet gives a highlight so small it disappears
    // at any distance, which is exactly how blood ends up looking like paint.
    // Oil's lobe is TIGHTER than blood's at both ends. Blood is a scattering
    // suspension whose surface is microscopically rough, so its highlight is
    // broad and soft; oil is a smooth film and gives a small hard glint. That
    // narrowness is most of what the eye reads as "glossy" rather than "damp".
    let power = mix(mix(TUNE_BLOOD_SHEEN_DROP, TUNE_BLOOD_SHEEN_POOL, pool),
                    TUNE_OIL_GLOSS, oily);
    var spec = pow(max(dot(n, hv), 0.0), power);
    if (!upFacing) { spec *= 0.55; }
    // Ambient-lit sheen as well as key-lit: a wet surface in shadow still
    // reads wet, because it reflects the sky. Without this, blood indoors or
    // at night goes completely matte and dead.
    let ambientSheen = pow(1.0 - cosI, 4.0) * TUNE_BLOOD_AMBIENT_SHEEN;
    let tint = normalize(keyLightColor() + vec3f(1e-4)) * 1.732;
    let sheenAmt = mix(TUNE_BLOOD_SHEEN, TUNE_OIL_SHEEN, oily);
    color += tint * min(spec, 1.0) * sheenAmt * (0.35 + fres)
           + ambientAt(n) * ambientSheen;

    // ---- thin-film iridescence ----
    // The rainbow slick, and it ONLY appears where oil is floating on water.
    //
    // Interference needs a FILM — two interfaces close enough together that
    // light off the top can interfere with light off the bottom. Oil lying on
    // water is exactly that and is why a car-park puddle shows rainbows; a
    // deep pool of oil on rock has no second interface within reach of the
    // light, so it is simply a dark glossy liquid. Painting rainbows on one
    // anyway was the tell that this was decoration rather than a model, and it
    // made every isolated droplet and every standalone pool look wrong.
    //
    // Also weighted by FRESNEL, so within a real slick it shows at glancing
    // angles and stays off the head-on centre, where it would read as paint.
    // Scaled by `oily`, so blood never gets a drop of it.
    let onWater = floatingOnLiquid(cell, mat);
    if (oily > 0.01 && onWater > 0.01 && !underwater) {
      color += filmIridescence(hitP, cosI) * oily * onWater * fres *
               TUNE_OIL_IRIDESCENCE;
    }
  }

  // ---- thin edge darkening ----
  // The feathered edge of a real splatter is darker and browner than its
  // middle: less material, more of the substrate's shadow, and the iron has
  // oxidised. Keyed on a THIN column rather than on pooling, so it catches the
  // trailing edge of a run as well as the rim of a puddle.
  if (!underwater) {
    // Faded out on oil: that browning is OXIDISED IRON specifically, a
    // biological detail with no petroleum equivalent. A thinning oil film goes
    // iridescent (above), it does not go rust-brown.
    let thin = 1.0 - smoothstep(0.0, TUNE_BLOOD_EDGE_DEPTH, depthM);
    color = mix(color, color * TUNE_BLOOD_EDGE_TINT,
                thin * TUNE_BLOOD_EDGE_STRENGTH * (1.0 - oily));
  }

  // ---- silhouette softening ----
  // The smooth normal fixes the SHADING, but the ray still stops on a voxel
  // FACE, so a droplet's outline is a hard-edged cube no matter how well it is
  // lit. That silhouette is the other half of the gelatin-cube look, and it
  // cannot be fixed by shading alone.
  //
  // The same density field gives the fix for free: sample it at the hit point,
  // and where the value is low the surface is the feathered fringe of the blob
  // rather than its solid body. Fading toward what is BEHIND across that
  // fringe replaces the hard cube face with a soft edge — the standard
  // isosurface-antialiasing trick, and it costs one more field sample.
  //
  // Deliberately only the OUTER fringe (the smoothstep's low end): fading too
  // deep makes the whole droplet translucent and washed out instead of just
  // softening its rim.
  //
  // The threshold must be RELATIVE to what the field actually reaches here, and
  // getting that wrong is what produced a red halo around every splash. An
  // absolute band (the previous 0.10..0.38) assumes the field approaches 1.0
  // inside the liquid, which only holds in the interior of a POOL. Trilinear
  // interpolation of an isolated voxel peaks near 0.125 even when that voxel is
  // completely full, so a droplet sat ENTIRELY below the band: `solid` was
  // small across its whole face, not just at its rim, and the droplet was
  // blended toward the background everywhere while its sheen still lit the air
  // around it. That is the aura.
  //
  // Referencing the field's local peak instead makes the feather scale-free: it
  // trims the same fraction of the outer fringe off a droplet and off a pool,
  // and never eats into the body of either.
  //
  // OIL GETS A MUCH NARROWER FEATHER, and this is what stopped it reading as
  // see-through. The 0.28 band is a large fraction of the field's range, so on
  // a droplet or a thin film a big part of the visible surface — not just the
  // outermost rim — sits inside the fade and gets mixed toward whatever is
  // behind it. On blood that is an acceptable trade for killing the gelatin
  // cube silhouette, because blood's own body colour is bright enough to keep
  // reading through the blend. Oil's body is nearly black by design, so the
  // same blend has almost nothing to hold up against the background and the
  // droplet turns into a smear of the scene behind it.
  //
  // Narrowing the band to the true outer fringe keeps the anti-aliasing (the
  // silhouette is still soft, which is the point of the term) while leaving
  // the body of the blob opaque.
  let edgeField = liquidFieldAt(hitP - rd * 0.35, mat);
  // Peak the field can reach for this blob, floored so a full pool still uses
  // the authored feather rather than a vanishing one.
  let peak = max(liquidFieldAt(hitP - rd * 1.1, mat), 0.35);
  let lo = TUNE_BLOOD_EDGE_FEATHER * peak;
  let band = mix(0.28, TUNE_OIL_EDGE_BAND, oily);
  let solid = smoothstep(lo, lo + band * peak, edgeField);
  color = mix(sceneBehind, color, clamp(solid, 0.0, 1.0));

  return color;
}

// ============================================================================
// MOLTEN SURFACES (DESIGN.md §9)
// ============================================================================
// Lava is the OPPOSITE problem to water, and reusing the water treatment would
// get it wrong in every particular. Water's look comes from what it REFLECTS
// and TRANSMITS — Fresnel, refraction, depth absorption. Lava is opaque
// (MATF_OPAQUE, so it resolves as a surface hit and never enters the media
// path at all) and its look comes almost entirely from what it EMITS. There is
// no reflection worth tracing, nothing behind it to refract, and no depth to
// absorb through.
//
// Before this pass, lava was: flat palette albedo, one per-cell random
// flicker, added uniformly at emission 215/255 * 1.7 = 1.43x. Every channel
// saturated, so a lava pool rendered as a featureless WHITE slab — brighter
// than the sky and with less structure than the grass around it. Turning the
// intensity down alone would only have produced a flat ORANGE slab: the
// problem is the absence of spatial structure, not the exposure.
//
// What actually makes molten rock read as molten rock:
//   1. a CRUST — real flows are dark basaltic plates with glowing cracks
//      between them, not uniform orange. This is the whole look.
//   2. a blackbody ramp — black -> red -> orange -> yellow -> white, driven
//      by temperature rather than by palette index
//   3. flow — the crust drifts and the cracks shear open and closed
//   4. tonemapping — so "hotter" reads as a colour shift, not as clipping
//   5. light spill onto the surroundings, so the pool lights its own rim
//
// Render-only float math on render-only data: the sim never sees any of this
// (CLAUDE.md rule 1 scopes to sim state).

// Blackbody-ish ramp for incandescent rock, parameterised on normalised
// temperature 0..1. Not a Planck-law fit: it is anchored on the AUTHORED
// palette so a modder retinting lava in materials.json still gets a coherent
// heat ramp, which the data-driven-materials convention requires. c2/c0/c1 are
// the cool/mid/hot palette entries by convention for emissive materials.
fn moltenRamp(m : Material, temp : f32) -> vec3f {
  let t = clamp(temp, 0.0, 1.0);
  // Below the incandescence threshold the rock is genuinely dark — this is the
  // crust, and it must be allowed to go nearly black or there are no plates,
  // only a bright field with darker bits.
  // Basalt, not mud. Cooled crust is near-black with only a slight warm cast;
  // at 0.055/0.040/0.038 it came out mid-brown and the pool read as dried
  // earth with glowing cracks. The plates need to be genuinely dark for the
  // cracks to have anything to contrast against.
  let dark = vec3f(0.020, 0.014, 0.014);
  let cool = unpackColor(m.color1);   // deep red
  let mid  = unpackColor(m.color0);   // orange
  let hot  = unpackColor(m.color2);   // yellow-orange
  // The top of the ramp stays SATURATED. It is tempting to drive it to white
  // here, but the per-channel tonemap in fs() already desaturates bright
  // values on its own — pushing the ramp toward white as well compounds the
  // two and the crack cores come out bleached to grey-white, losing the hue
  // that made them read as molten. Let the hottest authored colour be the
  // hottest colour, and let the tonemap decide how white it looks.
  let white = mix(hot, vec3f(1.0, 0.86, 0.55), 0.35);
  // Band boundaries are pushed LATE on purpose. Most of a real flow's visible
  // area is crust and cooling red rock; the orange-and-above range belongs to
  // the crack cores alone, which are a small fraction of the surface. Spread
  // the bands evenly instead and the average pixel lands in the orange/yellow
  // part of the ramp, which — compounded by the tonemap's per-channel
  // desaturation on the way up — renders the whole pool as glowing gold
  // honeycomb rather than molten rock.
  // Hold pure dark across the low range. Starting the dark->cool blend at 0
  // means a plate at t=0.15 is already a sixth of the way to red, and since
  // most of the surface sits in that low band the whole pool washes to brown
  // — the "dried mud" failure. The crust must stay crust until it is
  // genuinely warming.
  var c = mix(dark, cool, smoothstep(0.22, 0.46, t));
  c = mix(c, mid,   smoothstep(0.48, 0.78, t));
  c = mix(c, hot,   smoothstep(0.80, 0.94, t));
  c = mix(c, white, smoothstep(0.95, 1.00, t));
  return c;
}

// ---- the crust field ----
// A value in 0..1 across the surface: 0 = cooled plate, 1 = molten crack.
//
// Built from layered value noise in WORLD metres (so plates are a fixed
// physical size regardless of voxel scale), ADVECTED so the whole crust drifts
// like a skin being dragged along. The crack network is the ridge transform
// (1 - |2n-1|), which is what turns smooth blobs into the thin branching
// filaments that read as fractures between plates; smooth noise alone gives
// soft mottling that reads as rust.
fn hashNoise2(p : vec2f) -> f32 {
  let i = floor(p);
  let f = fract(p);
  // Quintic smoothstep — C2 continuous, so the derivative used by the glow
  // gradient below doesn't show the lattice.
  let u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  let a = f32(pcg(u32(i32(i.x) * 374761393 + i32(i.y) * 668265263)) & 0xFFFFu) / 65535.0;
  let b = f32(pcg(u32(i32(i.x + 1.0) * 374761393 + i32(i.y) * 668265263)) & 0xFFFFu) / 65535.0;
  let c = f32(pcg(u32(i32(i.x) * 374761393 + i32(i.y + 1.0) * 668265263)) & 0xFFFFu) / 65535.0;
  let d = f32(pcg(u32(i32(i.x + 1.0) * 374761393 + i32(i.y + 1.0) * 668265263)) & 0xFFFFu) / 65535.0;
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Ridged multi-octave noise -> crack network. Returns 0..1, high in cracks.
fn crustCracks(pm : vec2f, t : f32) -> f32 {
  // Advection: each octave drifts at its own rate and direction, so the crust
  // shears rather than sliding rigidly. Slow — lava is viscous, and anything
  // fast enough to notice per-frame reads as boiling water, not rock.
  var v = 0.0;
  var amp = 1.0;
  var norm = 0.0;
  // Base frequency in cycles per metre. Plates want to be roughly fist- to
  // head-sized; at 0.55 they came out metres across and the pool read as
  // polished marble rather than crust.
  var p = pm * TUNE_LAVA_CRACK_FREQ;
  var drift = vec2f(0.031, -0.019);
  // ROTATE THE DOMAIN PER OCTAVE. hashNoise2 samples a square lattice, and the
  // ridge transform turns that lattice's axes into visible creases: without
  // rotation every octave creases along the SAME x/y directions, they
  // reinforce, and the crack network comes out full of right angles and long
  // straight runs — it reads as cracked tile, not as rock. An irrational-ish
  // angle per octave (~31.7 deg, chosen so no small multiple lands back on an
  // axis) decorrelates them and the network turns organic.
  let ca = 0.851; let sa = 0.525;   // cos/sin of ~31.7 degrees
  for (var i = 0; i < 4; i++) {
    let n = hashNoise2(p + drift * t);
    // Ridge transform: peaks become creases. `pow` below 2 WIDENS the crease —
    // squaring gives the thin, hard-edged filaments that read as fractured
    // tile; a gentler exponent keeps a crack network but lets it bloom into
    // the softer molten channels of classic lava.
    let ridge = 1.0 - abs(n * 2.0 - 1.0);
    v += pow(ridge, 1.72) * amp;
    norm += amp;
    // Amplitude falls off FASTER than 0.5 so the fine octaves contribute
    // texture without carving additional hard creases of their own — the
    // high-frequency detail is what makes the network look busy and angular.
    amp *= 0.42;
    p = vec2f(p.x * ca - p.y * sa, p.x * sa + p.y * ca) * 2.07 +
        vec2f(11.3, 7.7);            // non-integer lacunarity: no re-tiling
    drift = vec2f(-drift.y, drift.x) * 1.35;
  }
  return v / max(norm, 1e-5);
}

// ---- the full molten shade ----
// Returns the emitted colour of a molten surface cell, in linear HDR (values
// well above 1 are expected and are handled by the tonemap in fs()).
// ---- heat spill ----
// Molten surfaces are bright light sources, and before this the rock one voxel
// from a lava pool was lit purely by the sun — the pool sat in its basin like
// a decal, casting nothing. Nothing else in the scene betrays "this glow is
// painted on" as fast as an unlit surround.
//
// Rather than a light-propagation volume (DESIGN.md's eventual GI plan), this
// takes a handful of taps along the surface normal and counts molten cells:
// cheap, local, and enough to warm a rim convincingly. It runs ONLY for
// surfaces that pass a cheap chunk-level test, so a world with no lava in view
// pays almost nothing — the "costs nothing when idle" rule (CLAUDE.md #2)
// applies to render work too.
//
// Returns a linear HDR colour to ADD to the surface shade.
fn heatSpill(cell : vec3<i32>, n : vec3f) -> vec3f {
  var glow = vec3f(0.0);
  // Step outward along the normal; a molten cell found near contributes more.
  // 4 taps at ~1.6-voxel spacing reaches ~6 voxels. Measured: at 6 taps this
  // function cost ~13 ms/frame of a 30 ms frame — it runs on EVERY
  // non-emissive surface pixel in the world, so its per-tap cost is paid by
  // terrain that will never see lava. Keep the loop short; the falloff makes
  // the far taps nearly worthless anyway.
  for (var i = 1; i <= 4; i++) {
    let s = f32(i) * 1.6;
    // floor(), not a bare cast: WGSL's f32->i32 conversion truncates toward
    // zero, so a negative offset like -1.5 becomes -1 instead of -2 and the
    // tap lands on the wrong side of the surface. That asymmetry made
    // up-facing rims sample sideways into the pool and glow pink.
    let c = cell + vec3<i32>(floor(n * s + vec3f(0.5)));
    if (!inBounds(c)) { break; }
    // chunk-level reject first: an empty or lava-free chunk costs one read
    if (occTotal(chunkOcc(c)) == 0u) { continue; }
    let mm = materials[voxMat(voxWordAt(c))];
    if (mm.klass == CLASS_LIQUID && (mm.flags & MATF_OPAQUE) != 0u &&
        mm.emission > 0u) {
      // inverse-square-ish falloff, normalised so a touching cell gives ~1
      let fall = 1.0 / (1.0 + s * s * 0.55);
      glow += unpackColor(mm.color0) * (f32(mm.emission) / 255.0) * fall;
    }
  }
  // Deliberately subtle. Heat spill should read as a warm lick along the rim
  // nearest the pool, not as a pink wash over every surface in the basin —
  // this is a contact cue, and once it covers a broad flat area it stops
  // looking like light and starts looking like the wrong albedo.
  return glow * TUNE_HEAT_SPILL_STRENGTH;
}

// ---- how "pooled" is this molten cell? ----
// Returns 0 for an isolated blob and 1 for the interior of a body of lava.
//
// The crust model assumes a CONTINUOUS SURFACE: plates, cracks between them,
// a skin that cools and shears. None of that means anything on a single voxel
// of lava — a laser-melted speck or a splash droplet has no room for a plate,
// so the crack field just paints an arbitrary slice of noise onto it and it
// reads as a dirty smudge. Those cases looked better under the old flat
// emissive shade, and this is the term that lets both coexist.
//
// Measured by sampling the horizontal neighbourhood at the hit: lava in a pool
// is surrounded by lava, a droplet is not. Horizontal only, and deliberately:
// what matters is whether there is EXTENT for a crust to form across, and a
// one-voxel-deep sheet of lava spread over a floor is still a pool. A radius
// of 3 (~0.4 m) is about the smallest patch that can show a plate.
fn moltenPooling(cell : vec3<i32>, mat : u32) -> f32 {
  var n = 0.0;
  var total = 0.0;
  for (var dx = -3; dx <= 3; dx += 3) {
    for (var dz = -3; dz <= 3; dz += 3) {
      total += 1.0;
      let c = cell + vec3<i32>(dx, 0, dz);
      if (!inBounds(c)) { continue; }
      if (occTotal(chunkOcc(c)) == 0u) { continue; }
      if (voxMat(voxWordAt(c)) == mat) { n += 1.0; }
    }
  }
  let frac = n / max(total, 1.0);
  // THRESHOLD LOW. The interesting distinction is "isolated speck" vs
  // "everything else", not "pool interior" vs "pool edge": a cell on a pool's
  // rim has neighbours on only one side, so with a high threshold the entire
  // boundary of every pool falls in the transition band and renders with the
  // flat treatment — drawing a bright ring around the pool that is far more
  // objectionable than either look on its own. Anything with even a couple of
  // lava neighbours has enough surface to carry a crust, so the ramp is placed
  // to catch only the genuinely isolated case.
  return smoothstep(0.10, 0.42, frac);
}


fn shadeMolten(m : Material, mat : u32, cell : vec3<i32>, hitP : vec3f,
               n : vec3f, rd : vec3f) -> vec3f {
  // How much surface is there for a crust to form on? 0 = isolated speck,
  // 1 = the middle of a pool. Everything crust-related below is scaled by
  // this, so scattered lava keeps the simple emissive look it had before the
  // crust model existed (see moltenPooling).
  let pool = moltenPooling(cell, mat);

  // Plates live in the horizontal plane for a pool surface; for a wall of
  // lava, project onto whichever plane the face points out of, so a vertical
  // flow gets vertical structure instead of a smeared top-down pattern.
  var pm : vec2f;
  if (abs(n.y) > 0.5) { pm = vec2f(hitP.x, hitP.z); }
  else if (abs(n.x) > 0.5) { pm = vec2f(hitP.z, hitP.y); }
  else { pm = vec2f(hitP.x, hitP.y); }
  pm *= VOXEL_METERS;

  let cracks = crustCracks(pm, R.time);

  // Map the crack field to temperature with a SHARP knee. The knee is what
  // separates plate from crack: a soft ramp gives a uniformly warm surface
  // with no plate boundaries, which is the failure mode this whole function
  // exists to avoid.
  // The knee still sits high enough that most of the surface stays crust —
  // the dark area between cracks is what gives the glow something to be
  // brighter *than*. But it is deliberately WIDE (0.30 of range, not 0.41 of
  // a hard step): a narrow knee gives every crack a crisp edge, and a field
  // of crisp-edged creases is what read as cracked tile. Widening it lets the
  // cracks fade into their plates, which is most of the way back toward
  // classic lava's soft molten channels while keeping the plate structure.
  var temp = smoothstep(TUNE_LAVA_CRACK_KNEE_LOW, TUNE_LAVA_CRACK_KNEE_HIGH, cracks);
  // Bias the whole field warm a touch: classic lava is a hot surface with dark
  // skin on it, not dark rock with hot seams. This is the single knob that
  // moves furthest along the classic<->crust axis, so it stays small — at 0.10
  // it flooded the pool and the crust stopped reading as crust at all.
  temp += TUNE_LAVA_WARM_BIAS;

  // FADE THE CRUST OUT ON ISOLATED LAVA. A speck of lava — laser spatter, a
  // splash droplet, a single melted voxel — has no room for plates, and the
  // crack field just paints an arbitrary slice of noise across it, which reads
  // as a dirty smudge rather than as molten rock. Below full pooling the
  // temperature blends toward a uniform hot value, which is exactly the flat
  // emissive look scattered lava had before the crust model and which suited
  // it far better. Pools are unaffected (pool == 1 there).
  temp = mix(0.86, temp, pool);

  // Per-cell variation so two adjacent plates are not the same temperature —
  // some crust is freshly congealed and still glowing, some is old and dark.
  // Keyed on the CELL (stable as the camera moves), coarsened to ~4-voxel
  // patches so it reads as plate-scale variation and not per-voxel noise.
  let pc = cell >> vec3<u32>(2u);
  let ph = pcg(u32(pc.x * 7 + pc.y * 131 + pc.z * 2917));
  // Biased COOL: a symmetric jitter lifts as many plates as it drops, and
  // since the ramp climbs steeply the lifted ones dominate the average and
  // re-warm the whole crust. Most plates should be cooling, a few still hot.
  // Kept small for the same reason as the face bias below: this is quantised
  // per 4-voxel patch, so a large amplitude tiles the surface into visible
  // rectangles rather than reading as variation within a continuous crust.
  // Scaled by pooling for the same reason as the crack field: plate-to-plate
  // temperature variation is meaningless on something that is not a plate.
  temp += (f32(ph & 0xFFu) / 255.0 - 0.68) * 0.10 * pool;

  // Slow per-cell pulse: convection turning fresh melt over. Distinct phase
  // per patch so the pool does not beat in unison (same reasoning as the fire
  // flicker, which this deliberately does NOT reuse — fire flickers fast and
  // randomly, lava breathes).
  temp += TUNE_LAVA_PULSE_AMP * sin(R.time * TUNE_LAVA_PULSE_RATE + f32(ph & 0x3FFu) * 0.0061);

  // Top faces are the coolest (they radiate to the sky and skin over first);
  // the sides of a flow are freshly exposed melt and run hotter.
  //
  // SMALL, and asymmetric on purpose. A pool surface is not flat: mass-
  // conserving flow leaves partial cells, so the top of a settled pool is a
  // field of one-voxel steps whose SIDE faces are exposed among their
  // neighbours' top faces. A large split (this was -0.10 / +0.12, a 0.22 jump
  // across a single voxel edge) therefore paints those risers as bright pale
  // rectangles scattered over the surface — it reads as a shading bug, which
  // is exactly what it is. Keep the difference under the width of one ramp
  // band so a riser blends with its neighbours instead of banding away.
  if (n.y > 0.5) { temp -= 0.015 * pool; }
  else { temp += 0.030 * pool; }

  temp = clamp(temp, 0.0, 1.0);

  var c = moltenRamp(m, temp);

  // Emission scales STEEPLY with temperature (Stefan-Boltzmann is T^4; the
  // exponent here is tuned rather than physical, but the point is the same —
  // the cracks must out-radiate the plates by a large factor, or the surface
  // averages back out into the flat slab this replaces).
  let emis = f32(m.emission) / 255.0;
  // Peak intensity is bounded on purpose. The steep exponent is what makes
  // cracks out-radiate plates, but run it to 3.4x and the crack cores land
  // deep in the tonemap's shoulder, where per-channel compression bleaches
  // them to white and the hue is lost exactly where the surface is most
  // interesting. ~1.9x peak keeps the cores inside the range where the
  // shoulder still discriminates colour.
  let power = temp * temp * (0.35 + 2.2 * temp);
  // The constant term is the crust's own dim self-illumination, NOT an ambient
  // light — it has to stay small or it lifts the dark plates back into brown.
  c *= 0.055 + power * emis * TUNE_LAVA_EMISSION_GAIN;

  // Fresnel-ish rim: a glancing view of any surface catches more of its
  // emission, and on a pool this draws a hot lip around the far edge that
  // makes it read as a volume of liquid rather than a painted disc.
  let grazing = 1.0 - abs(dot(n, -rd));
  c += moltenRamp(m, min(temp + 0.25, 1.0)) * pow(grazing, 3.0) * emis * 0.55;

  return c;
}

struct FSOut {
  @location(0) color : vec4f,
  @builtin(frag_depth) depth : f32,
};

// ============================================================================
// MPM FLUID SURFACE — the Splash-style water look for the MLS-MPM liquid
// (sim_fluid.wgsl). Where Splash (matsuoka-601) renders its MLS-MPM fluid in
// SCREEN SPACE (depth sprites -> narrow-range filter -> thickness -> compose),
// this engine has no sampled textures at all — everything is buffers — so the
// same visual result is built the way this renderer builds everything: by
// MARCHING A FIELD. The solver's node grid (mass per node, one node per cell)
// IS the fluid's density field; sampled trilinearly it is continuous, its
// gradient is a smooth normal, and the screen-space filter chain falls away
// because the field is already 3D-smooth. What screen space cannot give and
// this can: pixel-exact depth against terrain for free, TRUE refraction (the
// bent ray re-marches the world and shows you the actually-refracted shore),
// and reflections via the same real secondary rays the CA water uses.
//
// COST DISCIPLINE (rule 2 applied to the render path):
//   * R.fluidCount == 0 or the tuner's surface toggle off -> not one
//     instruction of this runs;
//   * R.fluidLo/fluidHi is the world AABB of the live fluid; a ray that misses
//     it pays ONE slab test and nothing else, and a ray that hits it marches
//     only the [enter, exit] span. This is what makes off-screen fluid free
//     and a settled pool free — see fluidBoundsSpan;
//   * the march skips CHUNK-SIZED strides through space with no fluid block
//     (one buffer read per skipped chunk — the same trick the terrain DDA's
//     occupancy skip uses);
//   * fine steps and the 32-tap tetrahedral gradient run only near an actual
//     surface;
//   * the traced refraction/reflection rays run only for pixels that hit
//     fluid, with the same step budget as water reflections.
//
// The grid holds the LAST substep's state: mass (Q10) at word 0, post-BC node
// velocity (Q16.16 cells/tick) at words 1..3, species masses at 4..6. The
// block map is valid until the next tick rebuilds it. A chunk past the
// kFluidBlocks budget has no block, so its fluid is invisible for that tick —
// the same bounded degradation the solver itself accepts (particles there are
// frozen too).

// First WORD of node c's grid row, or -1 when the node has no block.
fn fluidNodeBase(c : vec3<i32>) -> i32 {
  let wc = worldChunkOf(c);
  if (!chunkInWindow(wc, R.origin)) { return -1; }
  let bm = fluidBlockMapR[chunkSlotIndex(wc)];
  if (bm == 0u) { return -1; }
  let lo = vec3<u32>(c & vec3<i32>(CHUNK_MASK));
  return i32(((bm - 1u) * CHUNK_VOL + (lo.z * CHUNK + lo.y) * CHUNK + lo.x) *
             FLUID_GW);
}

// Everything the field needs about one cell, for ONE voxel read:
//   .x = normalized FILL, 0..1 — how much of the cell is water.
//   .y = 1 if the cell is a BLOCKER (a material that is neither gas nor
//        liquid). Nothing to do with density: it is what tells the level model
//        below that a film here is RESTING on the ground rather than hanging in
//        mid-air, and it rides along free on the voxel read the fill already
//        pays for.
//   .z = the cell's VERTICAL SPEED in voxels/second, weighted by its fill so
//        the row blend can take a mass-weighted mean. See the CALM note in
//        fluidFieldAt: this is the one thing that tells a sheet SPREADING
//        across the ground (a film) from a column FALLING through the air
//        (not a film), and support cannot tell them apart because falling
//        water is supported by the water falling underneath it.
//
// Seam continuity (plan §6.7): SETTLED liquid voxels contribute their
// fullness as virtual mass, so the isosurface's boundary taps see the
// voxel water next door and the two surfaces meet instead of leaving a
// gap. A full settled cell reads exactly rest density. Render-only — the
// sim's grid never sees this. The whole-lake case costs nothing: the
// march only queries cells near active blocks (fluidChunkActive skips the
// rest), and lakes with no excited water never enter the march at all
// (R.fluidCount == 0). max(), not +: a cell mid-conversion briefly holds
// both representations of the SAME water.
//
// The fill is CLAMPED to 1. A cell cannot be more than full however much
// transient compression the solver's node mass reports, and the level model
// reads the number as a geometric height, so an over-dense cell must not be
// allowed to push its surface through its own ceiling. The blob model never
// looked above 1 either (iso <= 1), so nothing there changes.
fn fluidCellAt(c : vec3<i32>) -> vec3f {
  var m = 0.0;
  var vy = 0.0;
  let b = fluidNodeBase(c);
  if (b >= 0) {
    m = f32(fluidGridR[u32(b)]) * (1.0 / 1024.0);   // Q10 -> particle masses
    // Word 2 is the node's post-BC Y velocity, Q16.16 cells/tick; x30 for the
    // tick rate puts it in voxels/second, the unit fluidSettleEps is authored
    // in. It comes off the same grid row as the mass one word away, so in
    // practice it is not a second fetch.
    vy = abs(f32(fluidGridR[u32(b) + 2u])) * (30.0 / 65536.0);
  }
  var fill = m / max(TUNE_FLUID_REST_DENSITY, 1.0);
  var blocker = 0.0;
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat != MAT_AIR) {
    let k = materials[mat].klass;
    if (k == CLASS_LIQUID) {
      fill = max(fill, f32(voxState(w) + 1u) * (1.0 / 8.0));
    } else if (k != CLASS_GAS) {
      blocker = 1.0;
    }
  }
  fill = min(fill, 1.0);
  // Speed is returned fill-WEIGHTED so the row blend below can take a
  // mass-weighted mean. Weighting matters at the edge of a pour: the empty
  // cells around a falling column would otherwise average its motion away and
  // the fringe would go back to being drawn as discs.
  return vec3f(fill, blocker, fill * vy);
}

// Bilinear-XZ blend of ONE CELL ROW's (fill, blocker, fill*speed), on weights
// the caller computed once. Four taps. Both models below are built out of these
// rows, so they cannot disagree about which columns they are looking at.
fn fluidRowAt(b : vec2<i32>, f : vec2f, cy : i32) -> vec3f {
  var acc = vec3f(0.0);
  for (var i = 0; i < 4; i++) {
    let o = vec2<i32>(i & 1, (i >> 1) & 1);
    let w = mix(1.0 - f, f, vec2f(o));
    acc += fluidCellAt(vec3<i32>(b.x + o.x, cy, b.y + o.y)) * (w.x * w.y);
  }
  return acc;
}

// ============================================================================
// TWO MODELS OF THE SAME WATER — and why water that is RESTING gets its own
// ============================================================================
// This used to be one function: trilinear density over cell centres, surface
// where it crosses `iso`. That is the right model for water in the air — a
// droplet, a splash arch, a crown — and it is the WRONG model for water lying
// on something, for two reasons that between them are the entire "the pour
// hovers as a blob and then splats into a pancake" complaint.
//
//  1. IT PUTS THE SURFACE IN THE WRONG PLACE. Trilinear interpolation smears a
//     cell's mass symmetrically about its CENTRE, but water fills a cell from
//     the FLOOR. A half-full cell over a full one crosses iso=0.3 about 0.4 of
//     a voxel above where the water actually stops, so the marched surface
//     floats above the pool it is standing in — and the moment the seam settles
//     those particles into voxels, the CA draws the same water at
//     `cell.y + (state+1)/8` and it drops. That drop IS the pop.
//
//  2. IT CANNOT DRAW A THIN FILM AT ALL. A cell holding one eighth reads 0.125,
//     which is below iso, so the isosurface simply does not exist there. A
//     spreading sheet of MPM water is INVISIBLE until it settles. That is the
//     other half of the pop: the water does not flatten when it settles, it
//     APPEARS.
//
// So supported water is evaluated as a HEIGHT FIELD instead, on exactly the
// CA's geometry: the surface is at `cell.y + fill`, fill bilinearly smoothed
// across columns. A film one eighth deep is drawn one eighth deep. A settled
// pool and the excited water on top of it are the same plane. `fluidLevel`
// (render tuning) blends the two models; at 0 this file behaves exactly as it
// did before.
//
// The gate between them is SUPPORT and CALM — is there ground or water
// directly underneath, and has it stopped falling. A wave, a crest and a
// spreading sheet are all height fields and all look right this way; only
// genuinely 3D water (a droplet in flight, an overhang, a pouring column)
// needs the blob.
//
// IT IS ALSO 8.9x FASTER, which was not the point but is the larger number.
// MEASURED on `--fluid-bench hill`, same tree, same 600 ticks, only fluidLevel
// and FLUID_SUB_Y changed, world+sky steady at 5.86-5.90 ms across all three
// runs as the internal control:
//
//   blob field + half-cell lattice (HEAD before this)   fluid march 55.02 ms
//   blob field + eighth lattice                                     54.60 ms
//   level field + eighth lattice (this)                              6.19 ms
//
// So the lattice was never the cost — the blob field was. On thin or spreading
// water the blob hovers NEAR iso without crossing it cleanly, so the blocky
// refine keeps failing to find an occupied sub-cell centre, returns its
// sentinel, and hands the coarse loop back a crossing it will rediscover on the
// next step — grinding through the 320-step budget for one pixel. The level
// field crosses iso on a clean plane, so the first refine resolves it and the
// march terminates. This is very probably the same mechanism as the "blocky
// depth and smooth depth disagree somewhere" artefact noted below, whose three
// refuted explanations all assumed the refine was FINDING something.
fn fluidFieldAt(p : vec3f) -> f32 {
  let iso = max(TUNE_FLUID_ISO, 0.05);

  // XZ lattice: nodes at cell centres, hence the -0.5 — same convention as
  // liquidFieldAt. Shared by both models so the surface cannot step as one
  // hands over to the other.
  let gxz = p.xz - vec2f(0.5);
  let bxz = floor(gxz);
  let fxz = gxz - bxz;
  let bi = vec2<i32>(bxz);

  // The level model indexes rows CELL-ALIGNED — fill is measured from the cell
  // FLOOR, exactly as the state nibble is — not centre-aligned like the blob.
  let cy = i32(floor(p.y));
  let fy = p.y - f32(cy);
  let here  = fluidRowAt(bi, fxz, cy);
  let below = fluidRowAt(bi, fxz, cy - 1);

  // SUPPORT. One eighth of water below is already full support: an eighth is
  // the CA's entire quantum, and a film resting on a film is still a film.
  // Ground counts the same way. Bilinear, so the anchor fades out across a
  // ledge edge instead of switching on a cell boundary.
  let sup = clamp(below.x * 8.0 + below.y, 0.0, 1.0);
  // WET. Is there any water in this column at all? Without this the ramp below
  // reads exactly `iso` at fy == 0 of a DRY row standing on rock — a dry row's
  // fill line is its own floor — and paints a sheet of water across every
  // ground plane next to a puddle.
  let wet = clamp(max(here.x, below.x) * 64.0, 0.0, 1.0);
  // CALM — the half of the gate `sup` cannot supply. A falling column of water
  // IS supported, by the water falling underneath it, so support alone drew a
  // pouring stream as a stack of flat discs, one per cell, hanging in the air.
  // Vertical speed separates the two cases, and it has to be VERTICAL only:
  // water spreading across the ground is fast, and drawing THAT as a film is
  // the entire point of this model.
  //
  // The threshold is the solver's own fluidSettleEps — draw water like settled
  // water exactly when the sim would call it settled enough to settle — with a
  // 3x band so the impact zone under a pour crossfades rather than switching.
  let vy = (here.z + below.z) / max(here.x + below.x, 1e-4);
  let eps = max(TUNE_FLUID_SETTLE_EPS, 0.5);
  let calm = 1.0 - smoothstep(eps, eps * 3.0, vy);
  let lw = clamp(TUNE_FLUID_LEVEL, 0.0, 1.0) * sup * wet * calm;

  // The row above is fetched at most once and only when something needs it:
  // the blob's upper half-cell, or a nearly-full row whose surface may stand
  // above its own ceiling. Every thin film — the whole case this exists for —
  // skips it, so the level model costs exactly what the blob model cost: two
  // rows, eight taps.
  let needAbove = (here.x > 0.75) || (lw < 0.999 && fy >= 0.5);
  var above = vec3f(0.0);
  if (needAbove) { above = fluidRowAt(bi, fxz, cy + 1); }

  var field = 0.0;
  if (lw < 0.999) {
    // BLOB — the original trilinear density, unchanged. Its Y nodes sit at cell
    // CENTRES, so the pair of rows it needs is (cy-1, cy) in the lower half of
    // a cell and (cy, cy+1) in the upper.
    field = select(mix(here.x, above.x, fy - 0.5),
                   mix(below.x, here.x, fy + 0.5), fy < 0.5);
  }
  if (lw > 0.001) {
    // LEVEL — the CA's geometry. `L` is the water level in this row's frame:
    // the row's own fill, plus, once the row is full, whatever stands in the
    // row above. `here.x` gates that second term, so a detached blob overhead
    // (3D water, the blob's job) cannot lift a surface off an empty row. It is
    // also what keeps the field CONTINUOUS through a deep pool: at a cell
    // boundary the row below reads (L-1) and the row above reads its own L, and
    // for the only configuration where that boundary is near the crossing — a
    // free surface sitting on a full cell — both are 0.
    let L = here.x + here.x * above.x;
    // The ramp is centred on `iso`, not on 0.5, so it crosses the threshold at
    // EXACTLY fy == L whatever the surface-threshold slider says. That equality
    // is the whole point: it is the CA's `cellWaterY = cell.y + (state+1)/8`,
    // evaluated on a bilinearly smoothed fill instead of on one cell.
    //
    // The ramp WIDTH is the smoothing radius, and that is load-bearing for the
    // NORMAL. fluidNormalAt takes four taps at radius fluidSmooth/sqrt(3); a
    // tighter ramp would saturate every one of them at 0 or 1, collapse the
    // gradient to a coarse difference and facet a gently sloping pond. At this
    // width the taps land inside the band, where -grad((L - y)/soft) is exactly
    // (-dL/dx, 1, -dL/dz) — the height-field normal, which is the same quantity
    // waterNormal builds from the CA's fullness gradient. Both surfaces then
    // take their shading normal from the same formula, which is most of why
    // they stop reading as two different liquids.
    let soft = max(TUNE_FLUID_SMOOTH, 0.4) * 0.5773503;
    field = mix(field, clamp((L - fy) / soft + iso, 0.0, 1.0), lw);
  }
  return field;
}

// Gradient of the field = the smooth outward surface normal.
//
// TETRAHEDRAL, not central differences. A central difference needs 6 field
// samples (2 per axis) and each field sample is 8 trilinear taps: 48 buffer
// reads for one normal, per fluid pixel. The four vertices of a regular
// tetrahedron span the same 3 dimensions with 4 samples — 32 reads, a third
// off — and the estimate is the same first-order gradient, because
// sum(k_i * f(p + k_i*h)) over the tetrahedron's k_i is 4h * grad(f) + O(h^2)
// exactly as the central pair is 2h * grad(f) + O(h^2).
//
// The taps are placed at RADIUS `e`, not at offset e per axis: the k_i are
// unit-cube diagonals of length sqrt(3), so h = e/sqrt(3) keeps the smoothing
// radius the tuner's `fluidSmooth` slider is calibrated against. Without that
// scaling the surface would quietly get 73% softer the day this landed.
fn fluidNormalAt(p : vec3f) -> vec3f {
  let h = max(TUNE_FLUID_SMOOTH, 0.4) * 0.5773503;   // 1/sqrt(3)
  let k0 = vec3f( 1.0, -1.0, -1.0);
  let k1 = vec3f(-1.0, -1.0,  1.0);
  let k2 = vec3f(-1.0,  1.0, -1.0);
  let k3 = vec3f( 1.0,  1.0,  1.0);
  // Negated: the field grows INTO the fluid, the surface normal points out.
  let g = -(k0 * fluidFieldAt(p + k0 * h) + k1 * fluidFieldAt(p + k1 * h) +
            k2 * fluidFieldAt(p + k2 * h) + k3 * fluidFieldAt(p + k3 * h));
  let len = length(g);
  if (len < 1e-4) { return vec3f(0.0, 1.0, 0.0); }
  return g / len;
}

// Mass-weighted velocity (voxels/SECOND — human units for the foam knobs),
// species-blended albedo and the advected FOAM FIELD at a point, in one gather
// so the shade pays the eight node lookups once.
//
// The foam field is grid word 7, written by sim_fluid.wgsl's g2p from the
// Ihmsen trapped-air / wave-crest / kinetic-energy potentials. It is a real
// simulated quantity that persists and decays, which is why foam TRAILS a
// breaking wave here instead of blinking on and off with the local velocity
// the way the churn term alone does.
struct FluidSample {
  vel  : vec3f,
  col  : vec3f,
  foam : f32,
  // Share of the sampled density that is SETTLED voxel water rather than live
  // MPM particles: 0 = all particles, 1 = the march is drawing a surface over
  // water the CA owns. See the SEAM SHADING block below — this is the weight
  // that decides how much of the shade comes from the fluid's own look and how
  // much from the CA water's, and it is the only thing that stops the marched
  // region reading as a chunk-shaped patch of a different liquid.
  settled : f32,
};

fn fluidSampleAt(p : vec3f) -> FluidSample {
  let g = p - vec3f(0.5);
  let b = floor(g);
  let f = g - b;
  let c0 = vec3<i32>(b);
  var mass = 0.0;      // live MPM particle mass
  var vmass = 0.0;     // virtual mass contributed by settled voxel water
  var vel = vec3f(0.0);
  var sp = vec4f(0.0);
  var foam = 0.0;
  for (var i = 0; i < 8; i++) {
    let o = vec3<i32>(i & 1, (i >> 1) & 1, (i >> 2) & 1);
    let wv = mix(1.0 - f, f, vec3f(o));
    let w = wv.x * wv.y * wv.z;
    let c = c0 + o;
    let nb = fluidNodeBase(c);
    var m = 0.0;
    if (nb >= 0) {
      m = f32(fluidGridR[u32(nb)]) * (1.0 / 1024.0);
    }
    if (m > 0.0) {
      mass += w * m;
      // Word 7 = the foam field, Q16 saturated at 1.0. Trilinear like the rest,
      // so the foam has a continuous gradient and does not show the node
      // lattice.
      foam += w * clamp(f32(fluidGridR[u32(nb) + 7u]) * (1.0 / 65536.0),
                        0.0, 1.0);
      // words 1..3 hold node VELOCITY after the last grid update, Q16.16
      // cells/tick; weight by mass so near-empty nodes cannot swing the average.
      vel += vec3f(f32(fluidGridR[u32(nb) + 1u]),
                   f32(fluidGridR[u32(nb) + 2u]),
                   f32(fluidGridR[u32(nb) + 3u])) * (w * m * (1.0 / 65536.0));
      let m1 = max(f32(fluidGridR[u32(nb) + 4u]) * (1.0 / 1024.0), 0.0);
      let m2 = max(f32(fluidGridR[u32(nb) + 5u]) * (1.0 / 1024.0), 0.0);
      let m3 = max(f32(fluidGridR[u32(nb) + 6u]) * (1.0 / 1024.0), 0.0);
      sp += vec4f(max(m - m1 - m2 - m3, 0.0), m1, m2, m3) * w;
    }
    // THE SEAM'S OTHER HALF. fluidCellAt already lets settled liquid voxels
    // contribute virtual mass to the FIELD, so the isosurface reaches over
    // them and the two surfaces meet instead of leaving a gap — but nothing
    // used to give that mass a COLOUR. A node with virtual mass and no
    // particles fell through the `continue` above, left `sp` at zero, and the
    // species blend divided by its 1e-4 floor: the shade came back BLACK.
    // That is the dark rim around every marched region that touches a pond.
    //
    // Settled water is species 0 — it is the same substance TUNE_FLUID_COLOR
    // names — so it accumulates into sp.x. max(), not +, exactly as
    // fluidCellAt: a cell mid-conversion briefly holds both representations of
    // the SAME water and adding them would double its density.
    let vw = voxWordAt(c);
    let vmat = voxMat(vw);
    if (vmat != MAT_AIR && materials[vmat].klass == CLASS_LIQUID) {
      let full = f32(voxState(vw) + 1u) * (1.0 / 8.0) *
                 max(TUNE_FLUID_REST_DENSITY, 1.0);
      let extra = max(full - m, 0.0);
      vmass += w * extra;
      sp.x += w * extra;
    }
  }
  var out : FluidSample;
  // Velocity and foam are weighted by PARTICLE mass alone — settled water is
  // by definition not moving and not aerated, so a surface drawn mostly over
  // it gets no churn foam and no wobble, which is what it should look like.
  out.vel = vec3f(0.0);
  if (mass > 1e-4) { out.vel = vel * (30.0 / mass); }   // cells/tick -> vox/s
  let tot = max(sp.x + sp.y + sp.z + sp.w, 1e-4);
  out.col = (TUNE_FLUID_COLOR * sp.x + TUNE_FLUID_COLOR1 * sp.y +
             TUNE_FLUID_COLOR2 * sp.z + TUNE_FLUID_COLOR3 * sp.w) / tot;
  out.foam = clamp(foam, 0.0, 1.0);
  out.settled = vmass / max(mass + vmass, 1e-4);
  return out;
}

// ---- DEPTH GRADIENT --------------------------------------------------------
// The species albedo says WHAT the liquid is; this says how its colour changes
// with how much of it you are looking through. Real water is not one colour
// attenuated — the shallow edge of a pool and its deep middle differ in HUE,
// because absorption is strongly wavelength-dependent (red goes first) and the
// short path simply never removes enough red to matter.
//
// Rather than fake that with a tint, the ramped colour is fed BACK into the
// Beer-Lambert coefficients, so shallow water genuinely absorbs like the
// shallow tint and deep water like the deep one. Setting fluidGradient to 0
// collapses this to the flat species albedo (the pre-gradient look).
//
// The ramp is exponential, not linear: it matches the shape of the absorption
// it is driving, so the hue shift tracks the brightness falloff instead of
// crossing it. `depth` is the metres over which the ramp substantially
// completes.
fn fluidGradientColor(base : vec3f, thickM : f32) -> vec3f {
  let g = clamp(TUNE_FLUID_GRADIENT, 0.0, 1.0);
  if (g <= 0.001) { return base; }
  let d = max(TUNE_FLUID_DEPTH, 0.05);
  let t = 1.0 - exp(-thickM / d);
  // The species albedo modulates the ramp rather than being replaced by it:
  // pouring the green species still reads green, but it now has a shallow
  // edge and a deep body. Species 1 (the water default) sits at ~(0.2,0.42,
  // 0.85), so this preserves the authored identity while adding the depth cue.
  let ramp = mix(TUNE_FLUID_SHALLOW, TUNE_FLUID_DEEP, t);
  return mix(base, base * ramp * 2.0, g);
}

// How much of a chunk a node block's isosurface can reach into its NEIGHBOUR.
// The B-spline support is 1.5 cells and the trilinear tap cube adds half a
// cell, so 2 is conservative. This is the whole reason the seam ring exists.
const FLUID_SEAM_SHELL : i32 = 2;

// Chunk classification for the march: 0 = nothing here, 1 = the chunk owns a
// node block, 2 = SEAM RING — no block of its own, but a face neighbour has
// one, so the isosurface can reach FLUID_SEAM_SHELL cells in from that face.
//
// The ring used to be classified as plain "active", which made the march fine-
// step through a whole 16-cell chunk of air for a surface that can only reach
// 2 cells into it. Over open scenes that ring is most of the marched volume:
// a camera looking along a poured sheet crosses several ring chunks per ray.
// Does this chunk hold anything the isosurface can be built from? A block
// allocation is NOT the same question: `mark` pads the block set by
// FLUID_MARK_PAD cells so the map can be built once per tick, so an allocated
// block is routinely all air. The Y-occupancy mask (common.wgsl) is the answer,
// and a zero mask means the block is empty.
fn fluidChunkWater(wc : vec3<i32>) -> bool {
  if (!chunkInWindow(wc, R.origin)) { return false; }
  let slot = chunkSlotIndex(wc);
  return fluidBlockMapR[slot] != 0u &&
         fluidBlockMapR[fbmYMaskIndex(slot)] != 0u;
}

fn fluidYMaskOf(wc : vec3<i32>) -> u32 {
  if (!chunkInWindow(wc, R.origin)) { return 0u; }
  return fluidBlockMapR[fbmYMaskIndex(chunkSlotIndex(wc))];
}

fn fluidChunkClass(wc : vec3<i32>) -> u32 {
  if (!chunkInWindow(wc, R.origin)) { return 0u; }
  if (fluidChunkWater(wc)) { return 1u; }
  // A chunk with only settled CA water has no MPM block allocation, but
  // fluidCellAt still returns non-zero density there (virtual mass from
  // voxel fullness).  If a face-neighbor IS active, the isosurface may
  // extend into this chunk and must not be clipped at the boundary.
  for (var a = 0; a < 3; a++) {
    for (var s = -1; s <= 1; s += 2) {
      var nc = wc;
      nc[a] += s;
      if (fluidChunkWater(nc)) { return 2u; }
    }
  }
  return 0u;
}

// t at which the ray leaves the 1-cell Y SLAB it is in. The fluid is
// gravity-fed, so it lives in a thin horizontal layer inside a 16-cell chunk:
// a y level with no bit in the mask is empty ACROSS THE WHOLE CHUNK, and a
// near-horizontal ray can cross the entire chunk on one buffer read instead of
// ~13 trilinear field samples (8 taps each) discovering it is air.
fn fluidYExit(ro : vec3f, inv : vec3f, py : f32) -> f32 {
  let plane = select(floor(py), floor(py) + 1.0, inv.y > 0.0);
  return (plane - ro.y) * inv.y;
}

// Is cell c inside the seam shell — within FLUID_SEAM_SHELL cells of a face
// whose neighbour chunk owns a block? Only tests the faces c is actually near,
// so a cell in the middle of a ring chunk costs zero buffer reads.
fn fluidSeamShell(c : vec3<i32>) -> bool {
  let wc = worldChunkOf(c);
  let lo = c & vec3<i32>(CHUNK_MASK);
  for (var a = 0; a < 3; a++) {
    if (lo[a] < FLUID_SEAM_SHELL) {
      var nc = wc;
      nc[a] -= 1;
      if (fluidChunkWater(nc)) { return true; }
    }
    if (lo[a] >= i32(CHUNK) - FLUID_SEAM_SHELL) {
      var nc = wc;
      nc[a] += 1;
      if (fluidChunkWater(nc)) { return true; }
    }
  }
  return false;
}

// NOTE: `fluidCellMarched` lived here — "is cell c inside the region the march
// actually samples", used by fs() to decide whether the MPM surface owned a CA
// liquid cell. It is GONE, deliberately: it answered a question about the
// MARCH's sampling region when the shade needed one about the PIXEL's nearest
// interface, and the two disagree wherever the Y-occupancy mask stops above the
// CA waterline — which is every pour into a pond. See the `mpmOwned` /
// `caShadedLiquid` block in fs(). `mf.hit` is already the per-pixel answer.

// t at which the ray leaves the CHUNK containing cell c — the chunk-stride
// skip for empty space. `inv` is the caller's precomputed 1/rd.
fn fluidChunkExit(ro : vec3f, inv : vec3f, c : vec3<i32>, t : f32) -> f32 {
  let base = vec3f(c & vec3<i32>(~CHUNK_MASK));
  let t0 = (base - ro) * inv;
  let t1 = (base + f32(CHUNK) - ro) * inv;
  let tf = max(t0, t1);
  return max(min(min(tf.x, tf.y), tf.z), t + 0.05);
}

struct FluidHit {
  hit    : bool,
  t      : f32,    // entry distance along the ray (0 when the camera is inside)
  thick  : f32,    // in-fluid path length behind the entry, fine voxels
  inside : bool,   // camera started submerged in the fluid
  // The VOXELIZED draw modes (2 and 3) know their normal exactly — it is the
  // cube face the ray entered through — so they carry it here instead of
  // paying the 32-tap field gradient, which would round the cubes back off.
  blocky : bool,
  nrm    : vec3f,
};

// How much fluid the ray crosses BEHIND the entry point, bounded by the scene
// surface (a pool on a bed is exactly bed-deep), by the fluid AABB (there is no
// fluid past it, by construction) and by an absorption horizon past which more
// water cannot change the pixel.
//
// GEOMETRIC STRIDE. This walk was 28 fixed 1.25-cell samples — on a pixel that
// hits water it was HALF of every field sample the pixel ever took, for a
// quantity that feeds exp(-absorb * thickness). That exponential is exquisitely
// sensitive to the first couple of cells (a thin film vs a sheet) and almost
// blind past ten (the light is gone either way), so the step grows
// 1.25 -> 1.25 -> 1.5 -> 1.8 ... and reaches the same ~40-cell absorption
// horizon in 12 samples instead of 28. Thin films keep full precision.
//
// Shared by the smooth and voxelized marches ON PURPOSE: absorption, the depth
// gradient and the refraction gate all key off this number, so sharing it is
// what makes an A/B between draw modes a comparison of surface SHAPE rather
// than of colour.
//
// `tEnd` IS THE SCENE BOUND, NOT THE FLUID AABB. The AABB (R.fluidLo/fluidHi)
// is built from the live particle blocks, but the field this walk samples also
// contains SETTLED voxel water (fluidCellAt's virtual mass), which extends
// arbitrarily far past those blocks. Clipping the walk to the AABB made a pour
// into a lake absorb like a puddle: the column stopped two chunks down and the
// surface rendered pale and see-through against a pond that was correctly deep.
// Cost is unchanged — the walk was already bounded by 14 samples and the
// 40-cell absorption horizon, not by the box.
fn fluidThickness(ro : vec3f, rd : vec3f, tHit : f32, tEnd : f32,
                  iso : f32) -> f32 {
  var tt = tHit + 0.5;
  var thick = 0.5;
  var step = 1.25;
  for (var i = 0; i < 14; i++) {
    if (tt >= tEnd || thick > 40.0) { break; }
    if (fluidFieldAt(ro + rd * tt) >= iso * 0.75) { thick += step; }
    tt += step;
    if (i >= 1) { step = min(step * 1.35, 8.0); }
  }
  return min(thick, max(tEnd - tHit, 0.25));
}

// ---- THE FLUID AABB (plan §7 item 5) ---------------------------------------
// R.fluidLo/fluidHi is the inclusive world-voxel box of everything the march
// can possibly hit this frame (world.h RenderParams; built on the CPU from the
// snapshot's active block list plus the tick's spawns, dilated two chunks).
//
// Why this is THE render fix and not a micro-optimisation: without it every
// one of the ~2M pixels ran the chunk-stride loop, and a SKY pixel is the
// worst case — it has no terrain hit, so tMax is the window exit and the loop
// strides ~32 chunks reading `fluidChunkActive` (up to 7 buffer reads) at each
// one, for a pool that occupies a hundredth of the screen. Measured at 11-27 ms
// of an 15-37 ms lab frame (plan §9 WP1 baselines).
//
// Returns (tEnter, tExit) along the ray, both unclamped; tExit < tEnter or
// tExit <= 0 means the ray misses the box. The empty box (lo > hi, the no-fluid
// state) makes tEnter > tExit on every ray by construction, so "no fluid" and
// "ray points away from the fluid" take the same early-out.
fn fluidBoundsSpan(ro : vec3f, inv : vec3f) -> vec2f {
  let lo = vec3f(R.fluidLo);
  let hi = vec3f(R.fluidHi) + vec3f(1.0);   // inclusive cell -> its far face
  let t0 = (lo - ro) * inv;
  let t1 = (hi - ro) * inv;
  let tn = min(t0, t1);
  let tf = max(t0, t1);
  return vec2f(max(max(tn.x, tn.y), tn.z), min(min(tf.x, tf.y), tf.z));
}

fn fluidMarch(ro : vec3f, rdIn : vec3f, tMax : f32) -> FluidHit {
  var out : FluidHit;
  out.hit = false;
  out.t = 0.0;
  out.thick = 0.0;
  out.inside = false;
  out.blocky = false;
  out.nrm = vec3f(0.0, 1.0, 0.0);
  if (tMax <= 0.0) { return out; }

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;
  let iso = max(TUNE_FLUID_ISO, 0.05);

  // Clip the whole march to the fluid AABB. `span.x <= 0` is exactly "the
  // camera is inside the box", which is also the only case in which the
  // submerged test below can be true — so a camera nowhere near the water no
  // longer pays a field sample for it either.
  let span = fluidBoundsSpan(ro, inv);
  if (span.y <= 0.0 || span.x > span.y) { return out; }
  let tStart = max(span.x, 0.0);
  let tEnd = min(span.y, tMax);
  if (tStart >= tEnd) { return out; }

  // Camera inside the fluid: no interface in front, the whole view absorbs.
  if (span.x <= 0.0 && fluidFieldAt(ro) >= iso) {
    out.hit = true;
    out.inside = true;
  } else {
    var t = tStart;
    var tPrev = tStart;
    var found = false;
    // The chunk class only changes at chunk boundaries, but the march steps
    // every 0.5-1.25 cells — so it was being recomputed (up to 7 buffer reads)
    // about 13 times per chunk for an answer that could not have changed.
    var heldSlot : u32 = 0xFFFFFFFFu;
    var heldClass : u32 = 0u;
    var heldYMask : u32 = 0u;
    // Step budget = worst case fine-marching straight down a full window
    // diagonal of solid fluid; the chunk skip means open scenes never get
    // close. A budget miss renders no surface for one frame of one pixel —
    // invisible — rather than a hitch.
    for (var i = 0; i < 320; i++) {
      if (t >= tEnd) { break; }
      let p = ro + rd * t;
      let c = vec3<i32>(floor(p));
      let wc = worldChunkOf(c);
      let slot = chunkSlotIndex(wc);
      if (slot != heldSlot) {
        heldSlot = slot;
        heldClass = fluidChunkClass(wc);
        heldYMask = fluidYMaskOf(wc);
      }
      if (heldClass == 0u) {
        tPrev = t;
        t = fluidChunkExit(ro, inv, c, t);
        continue;
      }
      if (heldClass == 1u &&
          (heldYMask & (1u << u32(c.y & i32(CHUNK_MASK)))) == 0u) {
        // Empty y slab: skip to the next y level, or to the chunk exit for a
        // near-horizontal ray (the mask is per-chunk, so it stops being an
        // answer at the boundary). This is the fine-grained empty-space skip a
        // 16-cell chunk classification cannot give.
        tPrev = t;
        t = max(min(fluidYExit(ro, inv, p.y),
                    fluidChunkExit(ro, inv, c, t)), t + 0.25);
        continue;
      }
      if (heldClass == 2u && !fluidSeamShell(c)) {
        // Seam ring, away from the active face: no node within the tap cube
        // can carry mass, so a field sample here is guaranteed zero. Stride to
        // just inside the chunk's far shell rather than to its exit, so the
        // shell facing the NEXT chunk still gets its fine steps.
        tPrev = t;
        t = max(fluidChunkExit(ro, inv, c, t) - f32(FLUID_SEAM_SHELL),
                t + 0.5);
        continue;
      }
      let d = fluidFieldAt(p);
      if (d >= iso) { found = true; break; }
      tPrev = t;
      // Adaptive stride: far below the iso the field cannot reach it within
      // one cell (B-spline support is 1.5 cells), so stride harder.
      t += select(0.5, 1.25, d < iso * 0.25);
    }
    if (!found) { return out; }
    // Bisect the crossing to sub-step precision — this is what keeps the
    // surface from shimmering as the camera moves.
    var lo = tPrev;
    var hi = t;
    for (var i = 0; i < 5; i++) {
      let mid = (lo + hi) * 0.5;
      if (fluidFieldAt(ro + rd * mid) >= iso) { hi = mid; } else { lo = mid; }
    }
    out.hit = true;
    out.t = hi;
  }

  out.thick = fluidThickness(ro, rd, out.t, tMax, iso);
  return out;
}

// ---- VOXELIZED MPM FLUID (draw modes 2 and 3) -------------------------------
// The same density field, resolved as CUBES instead of a smooth isosurface.
//
// WHY THIS EXISTS. The smooth march answers "what does MLS-MPM water look
// like"; this answers a different question the engine cares about just as
// much — "what does it look like if the water still reads as VOXELS?" Mode 3
// quantizes to the sim lattice exactly, one cube per world cell, so a pour is
// indistinguishable from CA water at a glance while the motion underneath is
// still the full MPM solve. Mode 2 subdivides each cell into 2x2x2 half-cells,
// which is the natural resolution for THIS solver: rest density is 8 particles
// per cell seeded on exactly that 2x2x2 lattice, so one sub-cell is one
// particle's worth of water.
//
// RENDER-ONLY, and deliberately so. Nothing here writes a voxel. The voxel word
// is 32 bits of hashed, saved, deterministic state (rules 1 and 3), and
// splatting a per-frame render preview into it would put a float decision
// inside the world hash — and a half-size grid is not representable there at
// all (the state nibble is fullness, not occupancy of eight sub-cells). The
// field being sampled is `fluidFieldAt`, the SAME field the smooth march uses,
// so the three draw modes agree about where the water IS and disagree only
// about how its boundary is drawn.
//
// SUB-CELL SAMPLING. A sub-cell is occupied when the field at its CENTRE is at
// or above the iso threshold. Settled CA water enters through the same
// virtual-mass blend in fluidCellAt, so a full settled cell quantizes to a full
// cube: the seam stays closed in every mode.
//
// THE LATTICE IS ANISOTROPIC: `shift` sets the XZ subdivision, but Y is always
// FLUID_SUB_Y = 8, and that asymmetry is the point rather than an oversight.
// Voxel water's own vertical resolution is EIGHTHS — the state nibble is
// fullness in eighths, and the CA renderer draws a partial cell's surface at
// `cell.y + (state+1)/8`. A uniform 2x2x2 lattice therefore cannot draw what
// the CA draws: its smallest step is HALF a voxel, four times the CA's, so a
// one-eighth film either vanished (its centre sample fell below iso) or stood
// up half a voxel tall. Both were visible as the pour "hovering" and then
// snapping flat the instant the seam settled it.
//
// Eighths in Y and whole cells in XZ (mode 3) is EXACTLY the CA's own geometry,
// which is why mode 3 now really does look like voxel water rather than merely
// close. Mode 2 keeps 2x2x2 in XZ for the finer horizontal shape.
//
// The refine budget has to cover the taller stack: a 1.35-cell coarse window
// straight down is ~11 eighth-sub-cells, plus the XZ crossings along the way.
//
// COST — COARSE SEARCH, LOCAL REFINE, and this is the whole design.
//
// The obvious implementation is a straight DDA that visits every sub-cell and
// tests the field at its centre. That was the first version and it MEASURED
// 74.85 ms of fluid march on the `hill` bench against the smooth march's 8.90
// ms — an 8.4x regression, which took frame p50 from 23.1 to 37.5 ms. The
// reason is that a DDA cannot stride: the smooth march spends most of its
// samples far below the iso threshold and jumps 1.25 cells at a time through
// them, while an exhaustive DDA pays 2 field samples per voxel (8 trilinear
// taps each) through exactly the same empty space to learn the same nothing.
//
// So this runs the smooth march's coarse loop verbatim — same three
// empty-space skips, same adaptive 0.5/1.25 stride — and only once that finds a
// crossing does it DDA the sub-cell lattice, across the one stride the crossing
// is known to lie in. The refine is bounded at 12 sub-cells (a 1.25-cell stride
// is 2.5 of them at mode 2), so a hit pixel pays the smooth march plus a
// handful of samples rather than a different order of cost.
//
// The refine window is [tPrev, t + one sub-cell]: tPrev is the last sample
// KNOWN below iso and t the first known at or above it, and the extra sub-cell
// covers the case where the crossing point sits inside a sub-cell whose centre
// is a little further along. A coarse stride that crosses iso but contains no
// occupied sub-cell CENTRE simply resumes the coarse search — hence refine
// returning a sentinel rather than a bool plus an out-param.
//
// KNOWN ARTEFACT, accepted deliberately (owner's call, 2026-08-24), MECHANISM
// NOT ESTABLISHED. In the blocky modes some of the ballistic spray droplets
// that the smooth march hides become visible, reading as hard white sprite
// triangles over the pool. The droplets themselves are not the bug: they are a
// deliberate feature, drawn by DrawParticles in EVERY mode, and they are simply
// what makes the difference visible. They are occluded by whatever depth this
// march writes (see the `mf.hit && mf.t > 0.05` nearest-wins rule in fs()), so
// the artefact means blocky depth and smooth depth disagree somewhere.
//
// THREE explanations were tried and all three are WRONG — do not re-derive
// them:
//   1. "the 1.25-cell far stride skips isolated spray." Refuted: the smooth
//      march takes the identical stride and shows no slivers.
//   2. "refine fails on thin water, the pixel gets no surface, and the droplet
//      shows through the hole." Refuted: a fallback that quantized the crossing
//      point whenever refine came back empty changed pixels but removed not one
//      sliver.
//   3. "the centre test lands the surface deeper than the smooth crossing, so
//      droplets in between are exposed — snap the bisected crossing to its
//      sub-cell instead." Refuted, and much worse: adjacent pixels snap to
//      different sub-cells, so the pool gains a regular grid of bright seams.
//
// The exhaustive DDA this replaced showed far fewer of them, at 8.4x the cost.
// Whoever picks this up: get the mechanism from a depth visualisation before
// writing any more code — three plausible stories cost more than one look at
// the actual depth buffer would have.
const FLUID_BLOCKY_STEPS : i32 = 320;
const FLUID_REFINE_STEPS : i32 = 20;
// Vertical sub-cells per voxel. EIGHT, to match the liquid state nibble — see
// the anisotropic-lattice note above. This is the one number that makes blocky
// MPM water and CA voxel water the same shape.
const FLUID_SUB_Y : f32 = 8.0;

// Exact ray entry into sub-cell `qc` of a lattice with 1/invSub cells per
// voxel per axis: the distance along the ray and the face normal. Slab
// intersection — the axis whose NEAR plane is crossed last is the face the ray
// came in through, the same argument the terrain DDA uses for its face normals.
// Returned as (normal.xyz, t) so the caller takes one value.
fn fluidSubCellEntry(ro : vec3f, rd : vec3f, inv : vec3f,
                     qc : vec3<i32>, invSub : vec3f) -> vec4f {
  let lo = vec3f(qc) * invSub;
  let hi = (vec3f(qc) + vec3f(1.0)) * invSub;
  let tn = min((lo - ro) * inv, (hi - ro) * inv);
  // Written as three explicit branches rather than an argmax + dynamic vector
  // index: WGSL only guarantees dynamic indexing on references, and `rd` is a
  // parameter value here.
  if (tn.x >= tn.y && tn.x >= tn.z) {
    return vec4f(select(1.0, -1.0, rd.x > 0.0), 0.0, 0.0, tn.x);
  }
  if (tn.y >= tn.z) {
    return vec4f(0.0, select(1.0, -1.0, rd.y > 0.0), 0.0, tn.y);
  }
  return vec4f(0.0, 0.0, select(1.0, -1.0, rd.z > 0.0), tn.z);
}

// Walk the sub-cell lattice across [tFrom, tTo] and return the entry
// (normal.xyz, t) of the first sub-cell whose CENTRE is at or above iso.
// w = -1 means the window held no occupied centre, which is why this reports a
// sentinel instead of a bool plus an out-param: the found case already needs to
// carry a normal and a distance, and a real entry t is clamped to >= 0.
fn fluidRefineSubCell(ro : vec3f, rd : vec3f, inv : vec3f, tFrom : f32,
                      tTo : f32, fsub : vec3f, invSub : vec3f,
                      iso : f32) -> vec4f {
  var t = max(tFrom, 0.0);
  var q = vec3<i32>(floor((ro + rd * t) * fsub));
  let stepQ = vec3<i32>(sign(rd));
  let bound = (vec3f(q) + max(vec3f(stepQ), vec3f(0.0))) * invSub;
  var tNext = (bound - ro) * inv;
  let tStep = abs(inv) * invSub;
  for (var i = 0; i < FLUID_REFINE_STEPS; i++) {
    if (t > tTo) { break; }
    if (fluidFieldAt((vec3f(q) + vec3f(0.5)) * invSub) >= iso) {
      let e = fluidSubCellEntry(ro, rd, inv, q, invSub);
      // A negative entry means the ray began inside this cube; clamp rather
      // than let the shade sample behind the eye — and it keeps -1 unambiguous.
      return vec4f(e.xyz, max(e.w, 0.0));
    }
    // Advance exactly one sub-cell.
    let m = min(min(tNext.x, tNext.y), tNext.z);
    if (tNext.x == m) {
      q.x += stepQ.x;
      tNext.x += tStep.x;
    } else if (tNext.y == m) {
      q.y += stepQ.y;
      tNext.y += tStep.y;
    } else {
      q.z += stepQ.z;
      tNext.z += tStep.z;
    }
    t = m;
  }
  return vec4f(0.0, 0.0, 0.0, -1.0);
}

// `shift` is log2 of the sub-cells per voxel edge IN XZ: 0 = one cube per sim
// cell (mode 3), 1 = half-cells (mode 2). Adding a 4x mode is `shift = 2u` and
// nothing else. Y is always eighths (FLUID_SUB_Y) in both modes — see the
// anisotropic-lattice note above.
fn fluidMarchBlocky(ro : vec3f, rdIn : vec3f, tMax : f32,
                    shift : u32) -> FluidHit {
  var out : FluidHit;
  out.hit = false;
  out.t = 0.0;
  out.thick = 0.0;
  out.inside = false;
  out.blocky = true;
  out.nrm = vec3f(0.0, 1.0, 0.0);
  if (tMax <= 0.0) { return out; }

  var rd = rdIn;
  if (abs(rd.x) < 1e-6) { rd.x = select(-1e-6, 1e-6, rd.x >= 0.0); }
  if (abs(rd.y) < 1e-6) { rd.y = select(-1e-6, 1e-6, rd.y >= 0.0); }
  if (abs(rd.z) < 1e-6) { rd.z = select(-1e-6, 1e-6, rd.z >= 0.0); }
  let inv = 1.0 / rd;
  let iso = max(TUNE_FLUID_ISO, 0.05);
  let fsub = vec3f(f32(1u << shift), FLUID_SUB_Y, f32(1u << shift));
  let invSub = vec3f(1.0) / fsub;

  // Same AABB clip as the smooth march: a ray that misses the live fluid box
  // pays one slab test and nothing else.
  let span = fluidBoundsSpan(ro, inv);
  if (span.y <= 0.0 || span.x > span.y) { return out; }
  let tStart = max(span.x, 0.0);
  let tEnd = min(span.y, tMax);
  if (tStart >= tEnd) { return out; }

  // Camera submerged — asked of the SUB-CELL the eye sits in, so "am I
  // underwater" agrees with what the mode actually drew.
  if (span.x <= 0.0 &&
      fluidFieldAt((floor(ro * fsub) + vec3f(0.5)) * invSub) >= iso) {
    out.hit = true;
    out.inside = true;
  } else {
    // COARSE search — byte for byte the smooth march's loop, including the
    // adaptive stride. It is looking for the same thing the smooth march looks
    // for (the first sample at or above iso); the only difference is what
    // happens when it finds one.
    var t = tStart;
    var tPrev = tStart;
    var hit = vec4f(0.0, 0.0, 0.0, -1.0);
    // First crossing that refine could not resolve to a sub-cell — the hole
    // guard below. w < 0 means "none recorded".
    var fallback = vec4f(0.0, 0.0, 0.0, -1.0);
    var heldSlot : u32 = 0xFFFFFFFFu;
    var heldClass : u32 = 0u;
    var heldYMask : u32 = 0u;

    for (var i = 0; i < FLUID_BLOCKY_STEPS; i++) {
      if (t >= tEnd) { break; }
      let p = ro + rd * t;
      let c = vec3<i32>(floor(p));
      let wc = worldChunkOf(c);
      let slot = chunkSlotIndex(wc);
      if (slot != heldSlot) {
        heldSlot = slot;
        heldClass = fluidChunkClass(wc);
        heldYMask = fluidYMaskOf(wc);
      }
      if (heldClass == 0u) {
        tPrev = t;
        t = fluidChunkExit(ro, inv, c, t);
        continue;
      }
      if (heldClass == 1u &&
          (heldYMask & (1u << u32(c.y & i32(CHUNK_MASK)))) == 0u) {
        tPrev = t;
        t = max(min(fluidYExit(ro, inv, p.y),
                    fluidChunkExit(ro, inv, c, t)), t + 0.25);
        continue;
      }
      if (heldClass == 2u && !fluidSeamShell(c)) {
        tPrev = t;
        t = max(fluidChunkExit(ro, inv, c, t) - f32(FLUID_SEAM_SHELL),
                t + 0.5);
        continue;
      }
      let d = fluidFieldAt(p);
      if (d >= iso) {
        // The crossing is somewhere in [tPrev, t]; resolve which sub-cell it
        // lands in. A window that holds no occupied CENTRE (the field crossed
        // iso between two sub-cell centres) falls through and the coarse
        // search simply carries on.
        //
        // tPrev is clamped to one coarse stride back, and that clamp is
        // load-bearing: the empty-space skips set tPrev BEFORE teleporting t,
        // so straight after a chunk skip tPrev can be a whole chunk behind.
        // Refining from there would spend the entire 12-step budget walking
        // sub-cells through space the skip just proved empty, never reach the
        // crossing, report a miss, and leave the coarse loop to do it again on
        // the next step — burning the budget and dropping the surface.
        hit = fluidRefineSubCell(ro, rd, inv, max(tPrev, t - 1.35),
                                 t + invSub.x, fsub, invSub, iso);
        if (hit.w >= 0.0) { break; }
      }
      tPrev = t;
      // Adaptive stride: far below the iso the field cannot reach it within
      // one cell (B-spline support is 1.5 cells), so stride harder.
      t += select(0.5, 1.25, d < iso * 0.25);
    }
    // A recorded crossing beats no surface at all — see the hole guard.
    if (hit.w < 0.0) { hit = fallback; }
    if (hit.w < 0.0) { return out; }
    out.hit = true;
    out.nrm = hit.xyz;
    out.t = hit.w;
  }

  out.thick = fluidThickness(ro, rd, out.t, tMax, iso);
  return out;
}

// A refracted ray re-marches the WORLD: this is what makes the shore bend at
// the surface and the bed swim when the water sloshes — the one effect a
// background-copy approximation can never give. Misses fall back to the
// straight-ray scene (thin films) or the sky (rays bent upward).
//
// `waterVox` is how much of this ray's path is inside the liquid — the column
// length the caller measured. AERIAL PERSPECTIVE IS AIR, and the submerged
// stretch of a refracted ray is not air: it is water, whose attenuation the
// caller applies afterwards as Beer-Lambert. Fogging it as well is the exact
// double-count the primary path's `if (h.liqT <= 0.0)` guard exists to avoid —
// and it is what washed a deep pond's bed out to sky grey the moment an MPM
// surface was drawn over it, while the CA-shaded half of the same pond stayed
// saturated. Only the part of the path BEYOND the column gets fogged, so a thin
// film on dry rock (column ~1 voxel, bed metres away) is unchanged.
fn traceRefraction(p : vec3f, rdr : vec3f, waterVox : f32,
                   fallback : vec3f) -> vec3f {
  let h = trace(p, rdr, TUNE_REFLECTION_STEPS, false);
  if (!h.hit) {
    if (rdr.y > 0.05) { return reflectionSky(rdr); }
    return fallback;
  }
  return applyAerial(shadeSecondaryHit(h), rdr, max(h.t - waterVox, 0.0));
}

// ============================================================================
// SEAM SHADING — why this function takes the CA water's material and path
// ============================================================================
// The march that feeds this shade does not stop at the live particles. Via
// fluidCellAt's virtual mass it also draws a surface over SETTLED voxel water,
// which is what closes the gap between a pour and the pond it lands in. The
// consequence nobody accounted for is that this function is then shading water
// the CA owns, with a completely unrelated model:
//
//     CA water  : absorb = TUNE_WATER_ABSORB  (1.85, 0.42, 0.20) per metre
//                 in-scatter = TUNE_WATER_SCATTER (0.045, 0.16, 0.20), flat
//     MPM fluid : absorb = (1.06 - depth-ramped species albedo) / clarity
//                 in-scatter = that albedo, squared and lit
//
// At 2.6 m of pond those two disagree by roughly (60,120,130) against
// (142,159,177) — measured, not estimated. So the instant a pour excites one
// chunk of a lake, the marched region of that lake changed representation and
// therefore CHANGED COLOUR, in a hard chunk-aligned rectangle that flickered as
// blocks were allocated and freed. That is the whole reported bug.
//
// THE FIX IS NOT TO PICK ONE MODEL. It is to notice that the two coefficients
// describe the same column, so the column's absorption is the PATH-WEIGHTED
// MIX of them:
//
//   * `caPath` is how much settled CA liquid the primary ray actually crossed
//     behind this surface (RayHit.liqPath — already measured, bed-bounded, and
//     free);
//   * the column behind the surface is at least that long, so its length is
//     max(fh.thick, caPath) and the settled SHARE of it is caPath/that;
//   * blend absorption, in-scatter, caustics and ripples by that share.
//
// At share 1 (a surface drawn over a lake) this reproduces shadeWater's body
// term exactly, so the ownership boundary is invisible. At share 0 (a pour in
// mid-air, or a sheet on dry rock) every term collapses to the authored MPM
// look, unchanged — which is what keeps --shot-fluid the picture it was.
//
// `caMat` is MAT_AIR when the ray crossed no CA liquid, and non-water liquids
// derive their coefficients here the same way shadeWater derives them, so an
// MPM pour into an oil pond blends toward OIL rather than toward water.
fn shadeMpmFluid(ro : vec3f, rd : vec3f, fh : FluidHit, caMat : u32,
                 caPath : f32, sceneBehind : vec3f) -> vec3f {
  let hitP = ro + rd * fh.t;
  // Colour/velocity sampled half a cell INTO the body so a grazing entry does
  // not read the empty half of the boundary cells.
  let s = fluidSampleAt(hitP + rd * 0.5);
  let speed = length(s.vel);                        // vox/s
  let churn = clamp(speed / max(TUNE_FLUID_FOAM_SPEED, 1.0), 0.0, 1.0);

  // ---- the column, and how much of it the CA owns ----
  // The march's own thickness walk stops at the absorption horizon; the CA's
  // path stops at the bed. Whichever is longer is the honest column length,
  // and taking the max also repairs the case where the fluid march found only
  // a thin excited film floating on water metres deep.
  let caLen = select(0.0, max(caPath, 0.0), caMat != MAT_AIR);
  let colVox = max(fh.thick, caLen);
  // SATURATING, not linear. The two coefficient sets describe the SAME
  // substance, so their disagreement is a modelling artefact, not a property of
  // the water — and a linear share lets a four-voxel excited film on a 26-voxel
  // pond keep a fifth of the MPM in-scatter, which is enough to read as a
  // brighter rectangle laid over the lake (measured: the settled region came
  // back ~1.4x the surrounding pond). Once most of the column is settled the
  // answer is simply "this is a lake, shade it like one". The low end is left
  // alone so a thick MPM body standing in a shallow puddle still shades as MPM
  // fluid, and caLen == 0 is still exactly the authored look.
  let caFrac = smoothstep(0.05, 0.55, caLen / max(colVox, 0.5));

  // ---- absorption + body (shared by the submerged and surface paths) ----
  // Beer-Lambert per channel, coefficients derived from the species albedo:
  // what the fluid does NOT reflect it absorbs, at a rate set by the clarity
  // knob (metres to roughly 1/e). The body term is the light the water itself
  // scatters back — it replaces the absorbed fraction so deep water goes to
  // the fluid's colour, never to black.
  let thickM = colVox * VOXEL_METERS;
  let clar = max(TUNE_FLUID_CLARITY, 0.05);
  // The depth-ramped colour, not the raw species albedo, is what drives the
  // absorption — see the DEPTH GRADIENT block. This is the whole gradient:
  // a thin film absorbs like the shallow tint, a deep body like the deep one.
  let gcol = fluidGradientColor(s.col, thickM);
  var absorb = (vec3f(1.06) - gcol) * (1.0 / clar);
  // Squaring the albedo into the scatter tint is what keeps deep water
  // SATURATED: single-scatter with a flat albedo washes toward the light's
  // own (near-white) colour, and the pool read as grey milk.
  var body = gcol * mix(gcol, vec3f(1.0), 0.25) *
             (ambientAt(vec3f(0.0, 1.0, 0.0)) * 0.85 + keyLightColor() * 0.35);
  if (caFrac > 0.001) {
    // The CA water's own coefficients, derived exactly as shadeWater derives
    // them — including its non-water branch, so an oil or acid pool keeps its
    // substance instead of being repainted as water by whatever fell in it.
    let cm = materials[caMat];
    let isWater = (cm.tagMask != 0u) && (f32(cm.opacity) / 255.0 < 0.45);
    var caAbsorb = WATER_ABSORB;
    var caScatter = WATER_SCATTER;
    if (!isWater) {
      let base = (unpackColor(cm.color0) + unpackColor(cm.color1)) * 0.5;
      let k = (f32(cm.opacity) / 255.0) * 9.0;
      caAbsorb = (vec3f(1.0) - base) * k + vec3f(0.05);
      caScatter = base * 0.22;
    }
    absorb = mix(absorb, caAbsorb, caFrac);
    body = mix(body, caScatter, caFrac);
  }
  let trans = exp(-absorb * thickM);

  if (fh.inside) {
    // Submerged: volumetric only — no interface, no Fresnel split. The
    // thickness already stopped at the scene surface, so this is the whole
    // underwater stretch of the view ray.
    return sceneBehind * trans + body * (1.0 - trans);
  }

  // ---- normal ----
  // The voxelized modes already carry an EXACT normal — the cube face the ray
  // entered through — and both the field gradient and the shimmer would round
  // those faces back off, which is the one thing those modes exist to avoid.
  // So they take neither: a flat face that boils is not a voxel.
  var n : vec3f;
  if (fh.blocky) {
    n = fh.nrm;
  } else {
    n = fluidNormalAt(hitP);
    // Sub-voxel shimmer, scaled by how hard the fluid is moving: a still pool
    // holds a glassy surface, a sloshing one boils. The REAL waves come from
    // the sim; this only supplies the sub-grid frequency the 10 cm lattice
    // cannot.
    let pm = hitP * VOXEL_METERS;
    let wob = TUNE_FLUID_WOBBLE * (0.06 + 0.30 * churn);
    n = normalize(n + vec3f(
        sin(pm.x * 21.0 + R.time * 2.9) + 0.5 * sin(pm.z * 33.0 - R.time * 4.1),
        0.0,
        cos(pm.z * 24.0 + R.time * 3.3) + 0.5 * cos(pm.x * 29.0 + R.time * 3.7)) *
        (wob * 0.12));
  }

  // ---- a still surface on a lake still belongs to the lake ----
  // The shape half of the seam the colour blend above fixes. Two ways in:
  //
  //   * s.settled — the surface here IS settled voxel water the march drew over
  //     (the seam ring). Obviously the lake's.
  //   * caFrac * calm — the surface is live MPM particles, but they are FLOATING
  //     ON a settled column and they have stopped moving. That is a raft of
  //     lake water that happens to be represented as particles this tick, and
  //     the same wind is blowing over it.
  //
  // Why it matters more than it sounds: a settled MPM slab has an EXACTLY
  // constant normal — (0,1,0) on every top face in the voxelized modes, and
  // near enough in the smooth one — so the whole slab sits at one specular
  // angle and the sun glint comes back as a single uniform sheet instead of the
  // sparkle the rippled CA lake around it breaks into. That reads as a
  // chunk-shaped patch of brighter water even when every colour term already
  // agrees; it was worth ~25/255 across the excited footprint.
  //
  // `calm` is what keeps this off the authored MPM look: a pour, a splash or
  // any sheet with real speed in it takes its shape from the solver, and wind
  // chop has no business on it. On dry ground caFrac is 0, so --shot-fluid is
  // untouched either way.
  //
  // Same rippleSlope and same screen-space damping as waterNormal
  // (waterRippleFootprint), so the two surfaces carry ONE wave field across the
  // boundary rather than two that merely resemble each other. Up-facing only,
  // exactly as waterNormal restricts it: the wave field describes a top
  // surface, and tilting a wall with it pushes the normal into the terrain.
  let calm = 1.0 - clamp(churn * 3.0, 0.0, 1.0);
  let rippleW = clamp(max(s.settled, caFrac * calm), 0.0, 1.0);
  if (rippleW > 0.01 && n.y > 0.25) {
    let slope = rippleSlope(vec2f(hitP.x, hitP.z) * VOXEL_METERS, R.time,
                            waterRippleFootprint(hitP)) * rippleW;
    n = normalize(n + vec3f(-slope.x, 0.0, -slope.y));
  }

  let v = -rd;
  let cosI = clamp(dot(n, v), 0.0, 1.0);

  // ---- Fresnel (Schlick, F0 from the tuner's IOR) ----
  let ior = max(TUNE_FLUID_IOR, 1.01);
  let f0 = ((ior - 1.0) / (ior + 1.0)) * ((ior - 1.0) / (ior + 1.0));
  var fres = f0 + (1.0 - f0) * pow(1.0 - cosI, 5.0);
  // A film thinner than a couple of cells is not a coherent mirror. Measured on
  // the COLUMN, not on the march's own thickness: a one-cell excited film on a
  // deep lake is the surface of that lake and reflects like one.
  fres *= clamp(colVox * 0.65, 0.3, 1.0);

  // ---- refraction ----
  // Faded out over a settled column, and this is a CORRECTNESS fix, not a
  // saving. shadeWater does not trace a refracted ray at all — its bed is
  // whatever the PRIMARY march resolved, with that march's shadows and ambient
  // occlusion on it. traceRefraction's bed comes from shadeSecondaryHit, which
  // has neither, so a pond bottom sampled through the MPM surface came back
  // brighter than the identical bottom sampled two pixels away through the CA
  // surface, and the boundary between them was a visible step with no colour
  // shift to explain it. Over a lake the two must agree, so over a lake this
  // takes the CA's answer. Free bonus: no secondary march on lake pixels.
  //
  // A pour on dry rock keeps the full traced refraction (caFrac == 0) — the
  // bent shore and the swimming bed are the whole point of it there.
  let refr = refract(rd, n, 1.0 / ior);
  var behind = sceneBehind;
  let refrW = 1.0 - caFrac;
  if (colVox > 0.75 && refrW > 0.02 && length(refr) > 0.5) {
    behind = mix(sceneBehind,
                 traceRefraction(hitP + refr * 0.75, refr, colVox, sceneBehind),
                 refrW);
  }
  // The bed under a settled column gets the CA's caustic web, faded in by the
  // same settled share the absorption uses. Without it the marched patch of a
  // lake is the one rectangle of bed with no light playing on it — as loud a
  // seam as the colour was, and free to fix because both surfaces now agree
  // about the wave field that casts it (waterCaustics / rippleSlope).
  if (caFrac > 0.001 && thickM > 0.02) {
    behind *= mix(1.0, waterCaustics(hitP, rd, colVox, thickM), caFrac);
  }
  var refracted = behind * trans + body * (1.0 - trans);

  // ---- reflection ----
  var reflection : vec3f;
  if (TUNE_FLUID_REFLECT > 0.01 && n.y > 0.25 &&
      fres > TUNE_REFLECTION_CUTOFF && colVox > 1.0) {
    // A real traced bounce — the shore, the tower, the debris beside the pool.
    reflection = traceReflection(hitP, n, rd);
  } else {
    reflection = reflectionSky(reflect(rd, n));
  }
  reflection *= TUNE_FLUID_REFLECT;

  var color = mix(refracted, reflection, clamp(fres, 0.0, 1.0));

  // ---- sun glint ----
  // Tight Blinn-Phong lobe; the wobble normal breaks it into the moving
  // sparkle field that says "liquid" from any distance.
  let kd = keyLightDir();
  let hv = normalize(kd + v);
  let spec = pow(max(dot(n, hv), 0.0), 380.0);
  color += keyLightColor() * spec * TUNE_FLUID_SPECULAR * (3.0 * fres + 0.15);

  // ---- foam ----
  // TWO sources, and they answer different questions:
  //
  //   * the FOAM FIELD (s.foam) is the simulated one — sim_fluid.wgsl's g2p
  //     accumulates the Ihmsen trapped-air/wave-crest/energy potentials into
  //     grid word 7, and that field DECAYS rather than tracking velocity. It
  //     is what makes foam persist in the wake of a breaking wave and drift
  //     with the water after the impact that made it has passed. This is the
  //     physically-motivated term.
  //   * CHURN is the cheap instantaneous one that was here before: fast
  //     surface = white. Kept because it responds with zero latency and reads
  //     well on thin fast sheets that never live long enough to build a field.
  //
  // They are combined by max(), not added: both are already "how white is
  // this", and summing them double-counts a breaking crest (which scores high
  // on both) into flat white paint.
  // ...and CHURN NEEDS A THRESHOLD ONCE IT IS FLOATING ON A LAKE. `churn` is
  // linear in speed from zero, so any drift at all whitens the surface a
  // little. On a free sheet that is the intent — it is the zero-latency
  // stand-in "for thin fast sheets that never live long enough to build a
  // field", per the note above. A raft of excited water sitting on a pond is
  // the opposite case: it has had every tick it needs to build a field, and its
  // field says there is no foam here. Left linear, a lake circulating at three
  // or four voxels a second came back with a constant ~0.08 of foam across the
  // whole excited footprint — a uniform warm-white wash in a perfect
  // chunk-shaped rectangle, and the last of the reported "plain blue chunk"
  // after every colour term already matched to within 2/255 (measured).
  //
  // So over a settled column the churn term has to earn it: nothing below about
  // a third of the foam speed, full by four fifths. A jet still ploughing into
  // the pond clears that easily and keeps its whitewater. caFrac == 0 (a pour
  // on dry rock) is the authored curve, untouched.
  let fieldFoam = s.foam * max(TUNE_FLUID_FOAM_FIELD, 0.0);
  let churnFoam = mix(churn, smoothstep(0.35, 0.80, churn), caFrac);
  var foam = TUNE_FLUID_FOAM * max(churnFoam, fieldFoam);
  if (foam > 0.003) {
    // Animated fbm break-up so it reads as bubbles and streaks being dragged
    // along rather than as white paint. Scaled by the texture knob: at 0 the
    // foam is flat coverage, at 1 it is fully broken into structure.
    let tex = clamp(TUNE_FLUID_FOAM_TEXTURE, 0.0, 1.0);
    let fn1 = fbm(hitP * 0.85 + vec3f(R.time * 1.9, 0.0, -R.time * 1.4), 3u);
    foam *= (1.0 - tex) + tex * (0.30 + 0.70 * fn1);
    // Foam is not just white paint: it is a rough, air-filled scattering layer.
    // Killing the specular under it (the mix below already replaces the shaded
    // colour) and lifting it with ambient is what stops it reading as a
    // gloss decal on top of the water.
    let foamCol = vec3f(0.93, 0.96, 0.99) *
                  (ambientAt(n) + keyLightColor() * 0.55);
    color = mix(color, foamCol, clamp(foam, 0.0, 0.90));
  }
  return color;
}

@fragment
fn fs(in : VSOut) -> FSOut {
  let ndc = in.uv;
  let rd = normalize(R.camFwd
                   + R.camRight * (ndc.x * R.tanHalfFov * R.aspect)
                   + R.camUp    * (ndc.y * R.tanHalfFov));

  let h = trace(R.camPos, rd, TUNE_PRIMARY_STEPS, true);

  // Rays that leave the window without a surface hit (and weren't absorbed by
  // media) continue into the far-field cascades from the window's exit point.
  var far : FarHit;
  far.hit = false;
  if (!h.hit && !h.saturated) {
    // in.pos.xy is the fragment's pixel coordinate — the dither key (see
    // farDither: screen-space, time-free, stable per pixel)
    far = traceFar(R.camPos, rd, h.tExit, in.pos.xy);
  }

  // ---- MPM fluid march (see the MPM FLUID SURFACE / VOXELIZED blocks) ----
  // Bounded by the terrain hit, or by the window exit for rays that leave
  // (the fluid only exists inside the window). Zero fluid anywhere, or the
  // tuner's per-particle cube-debug mode, skips all of it.
  //
  // TUNE_FLUID_SURFACE is the DRAW MODE, not a boolean — it kept its name
  // because the value lives in tuning.json and 0/1 still mean exactly what
  // they always did, so no migration is needed:
  //   0 = one raster cube per particle (the solver-debug view; drawn by
  //       Simulation::DrawFluid on the CPU side, which is why mode 0 is the
  //       one case this march does not run at all)
  //   1 = smooth isosurface — reflections, refraction, foam. The default.
  //   2 = voxelized at HALF a cell: 2x2x2 sub-voxels per sim cell, which is
  //       one sub-voxel per particle at rest density.
  //   3 = voxelized on the sim lattice: one cube per world cell, so MPM water
  //       reads as ordinary voxel water while moving as a fluid.
  // Rounded, not truncated, so a slider parked between two stops still picks
  // the nearer one instead of silently falling back to cubes.
  var mf : FluidHit;
  mf.hit = false;
  mf.blocky = false;
  if (R.fluidCount > 0u) {
    let sceneT = select(h.tExit, h.t, h.hit || h.saturated);
    let mode = i32(round(TUNE_FLUID_SURFACE));
    if (mode == 1) {
      mf = fluidMarch(R.camPos, rd, sceneT);
    } else if (mode == 2) {
      mf = fluidMarchBlocky(R.camPos, rd, sceneT, 1u);
    } else if (mode >= 3) {
      mf = fluidMarchBlocky(R.camPos, rd, sceneT, 0u);
    }
  }

  // reversed-Z depth so raster geometry (particles/debris) composites in.
  // A saturated media march writes depth at its stop point: the smoke is
  // opaque there, and raster geometry behind it must not draw through.
  //
  // ---- WHY MEDIA MUST WRITE DEPTH TOO ----
  // Translucent liquids and gases never set `hit` — trace() accumulates them
  // and keeps marching (see the CLASS_GAS / non-OPAQUE CLASS_LIQUID branch).
  // Keying depth on `hit` alone therefore reports the SOLID BEHIND the water,
  // or the far plane for a ray that crosses a lake and exits to sky. Raster
  // geometry (debris bodies, particles, sprites) then passes the reversed-Z
  // GreaterEqual test against that far value and draws ON TOP of water, lava
  // glow and fire it is genuinely behind — a body sunk in a pool floats over
  // the surface, and one on the far side of a plume punches through it.
  //
  // The fix is to take the NEAREST covering event along the ray. A liquid
  // interface at liqT is shaded as a real surface (Fresnel + reflection, see
  // shadeWater), so it covers whatever is behind it as far as raster geometry
  // is concerned, even though the march continued past it to light the bed.
  // Depth is a single value per pixel, so a partially transparent surface has
  // to pick a side: putting it at the interface is right, because that is
  // where the pixel's reflection and specular come from.
  //
  // KNOWN LIMIT — thin gas. Fire and smoke that never reach the saturation
  // early-out still write far-plane depth, so debris inside a WISPY plume is
  // not occluded by it. That is deliberate: a thin plume is genuinely
  // see-through, and writing depth at its first cell would hard-CLIP any body
  // standing in smoke instead of letting it show through — a worse artifact
  // than the one it fixes. Dense plumes already resolve via `saturated`.
  // Doing better needs order-independent transparency, not a depth tweak.
  var tDepth = -1.0;
  if (h.hit || h.saturated) {
    tDepth = h.t;
  } else if (far.hit) {
    tDepth = far.t;
  }
  // Liquid interface: nearest wins. `> 0.0` skips the underwater case (liqT
  // ~0 means the camera is already submerged, so there is no interface in
  // front of anything and depth belongs to whatever the ray actually found).
  if (h.liqT > 0.05 && (tDepth < 0.0 || h.liqT < tDepth)) {
    tDepth = h.liqT;
  }
  // MPM fluid interface: same nearest-wins rule as the CA liquid above, and
  // the same "> 0.05" skip for a submerged camera. Raster geometry (droplet
  // spray, debris) behind the surface is covered by it; spray in front of it
  // draws over it — which is exactly what a splash should do.
  if (mf.hit && mf.t > 0.05 && (tDepth < 0.0 || mf.t < tDepth)) {
    tDepth = mf.t;
  }
  var depth = 0.0;  // sky = far
  if (tDepth >= 0.0) {
    let viewZ = tDepth * dot(rd, R.camFwd);
    depth = clamp(KNEAR / max(viewZ, KNEAR), 0.0, 1.0);
  }

  var color : vec3f;
  if (!h.hit) {
    if (far.hit) {
      // far-field shading (phase 4): same palette, face term, sun tint, and
      // ambient floor as the near field, plus cascade-marched sun shadows and
      // a one-sample AO — lighting mismatch at the window seam is what makes
      // LOD terrain read as a different world.
      let m = materials[far.mat];
      // palette jitter at a FIXED world frequency (~0.5 m patches) instead of
      // per level cell: coarse cells otherwise flatten into single-color slabs
      // and the texture contrast visibly drops at every LOD handoff.
      let jc = (far.cell << vec3<u32>(farCellShift(far.level))) >> vec3<u32>(3u);
      let jit = pcg(u32(jc.x * 7 + jc.y * 131 + jc.z * 2917));
      var albedo = paletteColor(m, jit);
      var n = vec3f(0.0);
      n[far.axis] = -far.sgn;
      if (m.klass == CLASS_LIQUID) {
        albedo = unpackColor(m.color0);
        // distant water: a touch of sky reflection on up-facing surfaces so
        // lakes read as water instead of flat blue paint. Airglow only — a
        // full skyColor() here reflects individual stars off LOD-scale water
        // cells, which is both wrong (a star is far below a cascade cell's
        // angular footprint) and reads as sparkling noise on every far lake.
        if (n.y > 0.5) { albedo = mix(albedo, skyAirglow(reflect(rd, n)), 0.35); }
      }
      // Match the near field's face weights exactly — a different constant
      // here is a visible brightness step at the window seam.
      var face = 1.0;
      if (far.axis == 0) { face = TUNE_FACE_X; }
      else if (far.axis == 2) { face = TUNE_FACE_Z; }
      albedo *= surfaceGrain(far.cell << vec3<u32>(farCellShift(far.level)), TUNE_GRAIN_AMP_FAR);
      // Same wrapped diffuse as the near field — a different falloff here is a
      // visible brightness step at the window seam.
      var lambert = wrapDiffuse(dot(n, keyLightDir()), TUNE_DIFFUSE_WRAP);
      if (lambert > 0.0 && (R.flags & 1u) != 0u) {
        // start the shadow march just off the hit face, in fine-voxel coords
        let hp = R.camPos + rd * (far.t - 1e-3) +
                 n * (0.55 * f32(1u << farCellShift(far.level)));
        // SOFT, not hard-zero: at cascade resolution most shadow casters are
        // single-cell terrace steps and canopy rings, and a hard shadow term
        // turns them into high-frequency dark speckle ("ant trails") across
        // every hillside. 0.3 keeps the form cue without the noise.
        if (farShadowed(far.level, hp)) { lambert *= TUNE_SHADOW_FAR_LIFT; }
      }
      // one-sample AO: an occupied cell directly above darkens — valley floors
      // and ground under flattened canopy stop rendering at full sky ambient
      var ao = 1.0;
      let up = far.cell + vec3<i32>(0, 1, 0);
      if (farInBox(up, F.origins[far.level - 1u].xyz) &&
          farMatAt(far.level, up) != 0u) { ao = TUNE_AO_FAR; }
      // Same lighting model as the near field (hemisphere ambient x AO, plus
      // direct sun) so the two representations agree across the seam.
      let fsun = keyLightColor() * lambert;
      color = albedo * face * (ambientAt(n) * ao + fsun);
      let emis = f32(m.emission) / 255.0;
      if (emis > 0.0) { color += albedo * emis * TUNE_EMISSIVE_STRENGTH; }
      if (h.liqT <= 0.0) { color = applyAerial(color, rd, far.t); }
      else { color = applyAerial(color, rd, far.t - h.liqT); }
    } else {
      color = skyColor(rd);
    }
  } else {
    // A micro hit reports the SUB-VOXEL's material (h.micMat) while the cell's
    // own word still carries state/stamp/stain. Shading the sub-voxel's
    // material through the ordinary path here is exactly what the "palette
    // index == material id" convention buys: a red petal and a green stem in
    // one cell each light like the material they are, with no new shading code
    // and no per-micro colour table.
    let isMicro = h.micMat != 0u;
    let mat = select(voxMat(h.word), h.micMat, isMicro);
    let m = materials[mat];
    // Palette variant: a micro voxel has no state nibble of its own, so key it
    // on the sub-voxel's material and the cell, which keeps a tuft's blades
    // from all landing on the same palette entry while staying stable in time.
    let paletteState = select(voxState(h.word),
                              hash3(R.seed, 1u, cellIndexW(h.cell) ^ h.micMat),
                              isMicro);
    var albedo = paletteColor(m, paletteState);
    if (m.klass == CLASS_LIQUID) {
      // liquid state nibble is fullness, not a palette variant: fuller = deeper
      let fullness = f32(voxState(h.word) + 1u) / 8.0;
      albedo = mix(unpackColor(m.color2), unpackColor(m.color0), fullness);
    }

    var n = vec3f(0.0);
    n[h.axis] = -h.sgn;
    if (isMicro) {
      // The nested DDA ran in MODEL space, after the per-cell quarter-turn
      // swizzle, so its face normal has to be rotated back or a yaw-varied
      // tuft would light as though the sun had turned with it.
      n = microNormalToWorld(n, microBricks[voxMat(h.word)].flags,
                             microCellHash(microBricks[voxMat(h.word)].flags,
                                           h.cell));
    }

    // Voxel-scale grain: breaks up the white-noise palette confetti into
    // correlated patches (see surfaceGrain). Liquids are excluded — their state
    // nibble is fullness, not a palette variant, and graining a lake surface
    // fights the ripple normals.
    if (m.klass != CLASS_LIQUID) {
      albedo *= surfaceGrain(h.cell, TUNE_GRAIN_AMP);
    }

    // Stain overlay, applied to the ALBEDO so it takes the same light, shadow
    // and AO as the surface it soaked into (see applyStain). `wet` is the
    // coverage, used for the sheen below.
    var wet = 0.0;
    albedo = applyStain(albedo, h.word, h.cell, &wet);

    // ---- ambient occlusion ----
    // Needs the hit point's position within the face, so build it from the
    // exact hit and take the two axes tangent to the face normal.
    let hp = R.camPos + rd * h.t;
    let ni = vec3<i32>(round(n));
    let a1 = select(0, 1, h.axis == 0);            // first tangent axis
    let a2 = select(2, 1, h.axis == 2);            // second tangent axis
    let uv = vec2f(fract(hp[a1]), fract(hp[a2]));
    // voxelAO samples the eight CELL-scale neighbours around the hit face,
    // which is meaningless for a hit that happened INSIDE a cell: the face it
    // would sample is up to a whole cell away from the blade that was struck,
    // and the cell's own neighbours (open air, on all sides of a tuft) report
    // no occlusion anyway. Unoccluded instead of wrongly occluded — the micro
    // model's own self-shadowing would need a second march, which is exactly
    // the cost this feature exists to avoid, and grass genuinely does sit in
    // open sky.
    let ao = select(voxelAO(h.cell, ni, a1, a2, uv), 1.0, isMicro);

    // Per-face constant: kept, but much gentler than the old 0.55/0.75/0.85
    // spread. That spread was doing the job real ambient should do, and doing
    // it wrong — it darkened every north-facing wall by a fixed 25% regardless
    // of where the sky actually was. Now the hemisphere ambient carries the
    // directional term and this only breaks the tie between the two horizontal
    // axes so parallel walls don't fuse.
    //
    // Keyed on the WORLD normal rather than on h.axis, because a micro hit's
    // axis is in model space: after the quarter-turn yaw swizzle, x and z can
    // be swapped, and using the raw axis would give two identically-oriented
    // tufts different face weights.
    var face = 1.0;
    if (abs(n.x) > 0.5) { face = TUNE_FACE_X; }
    else if (abs(n.z) > 0.5) { face = TUNE_FACE_Z; }

    // Wrapped diffuse (see wrapDiffuse): keeps the risers of the terrain
    // staircase within a few percent of their tops instead of 1.8x apart.
    // wrap = 1.0 is deliberately wide. Measured on a grass hillside, the old
    // hard Lambert produced a BIMODAL luminance histogram — a cluster at 88-112
    // (risers) and a separate spike at 208 (sun-facing tops) with the 144-176
    // range completely empty. Two disjoint populations interleaved at voxel
    // frequency is what the eye reports as "noise on the floor"; there was no
    // gradient between them to read as slope. Widening the wrap fills that gap.
    var lambert = wrapDiffuse(dot(n, keyLightDir()), TUNE_DIFFUSE_WRAP);
    if (lambert > 0.0 && (R.flags & 1u) != 0u) {
      // h.t is the receiver's distance from the camera in fine voxels, which
      // is what picks the near (per-voxel) or cascade shadow — see sunShadowAt.
      lambert *= sunShadowAt(R.camPos + rd * (h.t - 1e-3), n, in.pos.xy, h.t);
    }
    // Direct sun + hemisphere ambient. Ambient is occluded by AO (it is sky
    // light, and AO measures how much sky the point can see); direct sun is
    // NOT — it already has its own shadow ray, and multiplying it by AO too
    // double-darkens contact regions into black smears.
    let sun = keyLightColor() * lambert;
    color = albedo * face * (ambientAt(n) * ao + sun);

    // ---- caustics on a submerged surface ----
    // The rippling web of focused sunlight on anything under water. This is
    // the term that makes a pond bed read as being underwater rather than as
    // dry ground with a blue sheet over it, and it applies to every lit
    // surface below a waterline — the bed, a boulder, a reed, the shore's
    // underwater slope — seen from ABOVE or BELOW the surface alike.
    //
    // Distinct from the caustic inside shadeWater, which is a multiplier
    // applied where a ray from dry land crosses the surface. That one cannot
    // fire at all when the camera is already submerged (no crossing happens),
    // which is why looking around underwater used to show no caustics on
    // anything. See bedCaustic for the projection difference.
    //
    // MULTIPLICATIVE, for the same reason as the other caustic path: caustics
    // redistribute the sunlight already landing on a surface, so they scale
    // what is there. Bright sand goes brighter, dark stone stays dark. Adding
    // a constant instead makes unlit rock glow, which reads as the surface
    // emitting light rather than as light playing over it.
    //
    // COST: gated hard. waterAbove() walks up to 40 cells, so it must not run
    // on every terrain pixel in the frame. `lambert > 0` skips everything in
    // shadow (a shadowed surface has no direct sun to redistribute anyway),
    // and the cell directly above must be a liquid before the walk starts —
    // which is one buffer read, false for essentially the whole world.
    if (lambert > 0.0) {
      let up1 = h.cell + vec3<i32>(0, 1, 0);
      if (inBounds(up1)) {
        let uw = voxWordAt(up1);
        let um = voxMat(uw);
        if (um != MAT_AIR && materials[um].klass == CLASS_LIQUID &&
            (materials[um].flags & MATF_OPAQUE) == 0u) {
          // Probe from just OFF the face, not from the hit point itself: a hit
          // point sits exactly on a cell boundary, and floor() there lands
          // inside the solid half the time, which reads the surface's own
          // material as the column and returns zero depth in a speckled
          // pattern. Nudging along the normal puts the probe unambiguously in
          // the water.
          let dAbove = waterAbove(hp + n * 0.5);

          // ---- the sunlight reaching this surface came THROUGH the water ----
          // and it is neither white nor at full strength when it arrives. This
          // is the term whose absence made the pond bed render BRIGHTER than
          // the water above it — a lit slab of pale stone, shaded as though it
          // were sitting in open air, then only tinted on the way back to the
          // eye. The bed has to be darkened and colour-shifted by the column
          // standing on it BEFORE anything else, or no amount of tuning on the
          // return path will stop it reading as white sand under blue fog.
          //
          // Beer-Lambert down the sun's slant path (longer than the vertical
          // depth at any angle off noon), using the same coefficients the
          // return trip uses, so a bed 2 m down loses most of its red exactly
          // as the water does.
          let kdc = keyLightDir();
          let slant = dAbove / max(kdc.y, 0.15);
          let downTrans = exp(-TUNE_SUB_ABSORB * slant);
          color *= mix(vec3f(1.0), downTrans, clamp(dAbove * 4.0, 0.0, 1.0));

          // Scaled by `lambert`, NOT gated on it. The enclosing `lambert > 0`
          // is only an early-out for fully shadowed pixels; using it as an
          // on/off switch for the caustic is what turned the soft shadow
          // gradient on the pool's rim wall into hard vertical stripes. That
          // wall is a stack of voxels whose shadow ray alternately clears and
          // clips the terrace lip above it, so `lambert` there is a fine
          // gradient — and multiplying a smooth gradient by a binary mask
          // quantises it into a barcode. Riding the same gradient keeps the
          // caustic continuous across it.
          let cw = bedCaustic(hp, n, dAbove) * lambert;
          color *= 1.0 + min(cw * TUNE_BED_CAUSTIC_GAIN, TUNE_BED_CAUSTIC_CAP);
        }
      }
    }

    // ---- wet sheen on a fresh stain ----
    // A stain is WET, and the specular highlight is what says so. Without it a
    // blood-soaked floor is just a floor with a red patch on it; with it the
    // patch reads as something spilled. Same Blinn-Phong lobe as the blood
    // surface itself so a pool and the stain around it are continuous.
    //
    // Modulated by the shadow-tested `lambert`, so a stain in shadow does not
    // catch a highlight from a sun it cannot see.
    if (wet > 0.0) {
      let hv = normalize(keyLightDir() - rd);
      let spec = pow(max(dot(n, hv), 0.0), TUNE_STAIN_SHEEN_POWER);
      color += keyLightColor() * spec * lambert * wet * TUNE_STAIN_SHEEN;
    }

    // ---- emissive surfaces ----
    // MOLTEN materials (an emissive OPAQUE liquid — lava, molten glass) take
    // the crust treatment: they REPLACE the diffuse shade rather than adding
    // to it, because an incandescent surface is not lit by the sun in any
    // meaningful sense, it is its own light source. Adding emission on top of
    // a sun-lit albedo is exactly what saturated every channel and turned a
    // lava pool into a white slab.
    //
    // Detected by class + flags + emission, never by material ID: any modder's
    // emissive opaque liquid gets this for free (CLAUDE.md conventions).
    let emis = f32(m.emission) / 255.0;
    let isMolten = m.klass == CLASS_LIQUID &&
                   (m.flags & MATF_OPAQUE) != 0u && m.emission > 0u;
    if (isMolten) {
      color = shadeMolten(m, mat, h.cell, R.camPos + rd * h.t, n, rd);
    } else if (emis > 0.0) {
      // everything else emissive (fire, embers) keeps the fast random flicker
      let ch = pcg(u32(h.cell.x * 7 + h.cell.y * 131 + h.cell.z * 2917));
      let flick = TUNE_EMISSIVE_FLICKER_BASE + TUNE_EMISSIVE_FLICKER_AMP * sin(R.time * TUNE_EMISSIVE_FLICKER_RATE + f32(ch & 0xFFu) * 0.0245);
      color += albedo * emis * TUNE_EMISSIVE_STRENGTH * flick;
    } else {
      // Non-emissive surfaces pick up nearby molten light. heatSpill() itself
      // rejects per tap on the chunk occupancy, and the loop breaks the moment
      // it leaves the window, so a surface with no lava near it costs at most
      // a few reads and no arithmetic. Cheap enough to run unconditionally,
      // and any earlier gate would have to answer the same "is lava near"
      // question the taps already answer.
      // EARLY REJECT, and this gate is load-bearing: heatSpill runs on every
      // non-emissive surface pixel, and measured unguarded it cost ~13 ms of a
      // 30 ms frame — paid overwhelmingly by terrain nowhere near lava. Lava
      // is a liquid, so it is counted in a chunk's total but NOT in its
      // blocker count (isRayBlocker excludes non-opaque liquids... but lava IS
      // opaque, so it does count). The cheap discriminator that survives both
      // cases is simply: does the chunk one step along the normal hold
      // anything at all? Open air above a grass field does not, so the common
      // case is one buffer read and out.
      let probe = h.cell + vec3<i32>(floor(n * 3.0 + vec3f(0.5)));
      if (inBounds(probe) && occTotal(chunkOcc(probe)) != 0u) {
        color += albedo * heatSpill(h.cell, n);
      }
    }

    // distance fog (density per meter, so the look survives voxel-size
    // changes). Density is a uniform tracking the far field's currently
    // FILLED radius (FarField::SafeRadiusMeters, plan phase 3B) so the
    // cascade horizon fades out instead of ending in a cut — and so a
    // half-filled cascade fogs out before its empty bands become visible.
    //
    // SKIPPED when the ray crossed a water surface: below the surface it is
    // water absorbing the light, not air, and shadeWater() models that with
    // Beer-Lambert instead. The air path in front of the water still gets
    // fogged — once, at the surface distance, after shadeWater() runs. Fogging
    // here as well would double-count it and wash the lake bed out to sky
    // color, which is exactly the haze that hides the bottom.
    if (h.liqT <= 0.0) { color = applyAerial(color, rd, h.t); }

    // ---- dirty-voxel debug highlight (dev panel toggle) ----
    if ((R.flags & 2u) != 0u) {
      let chSlot = chunkSlotIndex(worldChunkOf(h.cell));
      let snapTick = dirtyViz[chSlot];
      if (snapTick != 0u) {
        let stamp = voxStamp(h.word);
        if (stamp == stampFor(snapTick, 0u) || stamp == stampFor(snapTick, 1u)) {
          let ed = min(uv, 1.0 - uv);
          let me = min(ed.x, ed.y);
          if (me < 0.08) {
            color = vec3f(1.0, 0.05, 0.05);
          }
        }
      }
    }
  }

  // ---- gas tint along the ray ----
  // Gases stay pure participating media: smoke and steam have no interface to
  // reflect off, so the accumulated tau/tint is the whole story. mediaTau and
  // mediaTint accumulate per cell in trace(), so a ray crossing fire INTO
  // smoke shades each stretch with its own material instead of painting the
  // whole path with the first one.
  //
  // Liquids used to be tinted here too, and that is precisely why water looked
  // like blue fog — an absorbing volume with no surface. They now take the
  // shadeWater() path below instead.
  if (h.mediaMat != 0u && materials[h.mediaMat].klass == CLASS_GAS) {
    let mm = materials[h.mediaMat];
    var mc = (unpackColor(mm.color0) + unpackColor(mm.color1)) * 0.5;
    if (h.mediaTau > 1e-5) { mc = h.mediaTint / h.mediaTau; }
    let tau = h.mediaTau * VOXEL_METERS * MEDIA_ABSORB;
    let a = 1.0 - exp(-tau);
    color = mix(color, mc, a);
  }

  // ---- water surface ----
  // Everything the primary march resolved so far (bed, terrain, or sky, with
  // gas tint applied) is what sits BEHIND the water; shadeWater decides how
  // much of it survives the trip back up and what covers the rest.
  //
  // SEAM RULE: when MPM fluid is active, its isosurface extends over settled
  // voxel water via the virtual-mass blend in fluidCellAt.  A non-viscous
  // liquid cell inside that isosurface is already represented by the MPM
  // surface, so shadeWater must NOT run for it — the two shading models have
  // incompatible normals and the per-pixel fight is the strobing-squares
  // artifact.  The MPM path (below) handles the shade instead.
  //
  // Did a CA liquid path already put an interface on this pixel?  ONE liquid
  // interface per pixel is the invariant, and the MPM block below reads this
  // rather than re-deriving the answer from its own test — two independent
  // tests for one fact is what let a whole excited footprint get shaded twice.
  var caShadedLiquid = false;
  if (h.liqT > 0.0) {
    let lm = voxMat(voxWordAt(h.liqCell));
    if (lm != MAT_AIR && materials[lm].klass == CLASS_LIQUID) {
      let hitP = R.camPos + rd * h.liqT;
      let underwater = h.liqT < 0.05;

      // Does the MPM surface own this pixel?  EXACTLY ONE of the two water
      // paths may shade a pixel, and the rule is NEAREST WINS — the same rule
      // the depth write uses a hundred lines up.
      //
      // This used to ask `fluidCellMarched(h.liqCell)`: is the CA's liquid cell
      // inside the region the fluid march samples?  That is a different
      // question, and getting a different answer is a DOUBLE SHADE. The march's
      // per-chunk Y-occupancy mask only has bits where node mass lives, so
      // where a pour has excited the top of a pond the CA's first liquid cell
      // sits a few voxels BELOW the lowest marked slab: the test says "not
      // marched", shadeWater runs and paints a full water surface, and then the
      // MPM path runs on top of that and paints a second one over it. The pond
      // came back uniformly ~25/255 brighter inside the excited footprint than
      // the identical bed outside it (measured against the CA-only control
      // frame) — a chunk-shaped bright patch with no black rim and no colour
      // shift to explain it, which is why it survived the colour fix.
      //
      // mf.hit is already per-pixel and already false for a lake nowhere near
      // MPM activity (the march never samples an unmarked chunk), so the cell
      // test bought nothing that `mf.hit` did not already say — it only
      // disagreed with it. What remains is the depth comparison, with one cell
      // of slack because the isosurface's iso crossing and the CA's fullness
      // plane are two definitions of the same waterline and land about that far
      // apart. `caShadedLiquid` below carries the answer to the MPM path, so
      // the two cannot drift back out of agreement the way two independent
      // tests did.
      let mpmOwned = mf.hit
                     && materials[lm].moveEvery <= 1u
                     && (mf.inside || mf.t <= h.liqT + 1.0);

      if (underwater && !mpmOwned) {
        let sawSky = !h.hit && !h.saturated && !far.hit;
        color = shadeSubmerged(R.camPos, rd, lm, h.liqPath, color, in.pos.xy,
                               sawSky);
        caShadedLiquid = true;
      } else if (isViscousLiquid(materials[lm])) {
        color = shadeViscous(hitP, rd, lm, h.liqCell, h.liqAxis, h.liqSgn,
                             h.liqPath, max(h.mediaSurf, 0.125), color,
                             underwater);
        color = applyAerial(color, rd, h.liqT);
        caShadedLiquid = true;
      } else if (!mpmOwned) {
        color = shadeWater(hitP, rd, lm, h.liqCell, h.liqAxis, h.liqSgn,
                           h.liqPath, max(h.mediaSurf, 0.125), color,
                           h.liqT, underwater);
        color = applyAerial(color, rd, h.liqT);
        caShadedLiquid = true;
      }
      // mpmOwned && !viscous && !underwater: the MPM path below will shade it.
    }
  }

  // ---- translucent solid surface (ice, glass) ----
  // Applied AFTER water on purpose. Compositing here runs back-to-front, and a
  // frozen pond is ice sitting ON water: the ice is nearer the eye, so it has
  // to be the last thing laid over everything it covers. Doing it before the
  // water block would let the water surface paint over its own ice lid.
  //
  // The guard is tsT vs liqT rather than tsT alone, so a ray that enters water
  // FIRST and only then meets ice (looking up from under a frozen pond) does
  // not get the lid drawn over the water it is actually looking through.
  if (h.tsT > 0.0 && (h.liqT <= 0.0 || h.tsT < h.liqT)) {
    // Re-read defensively, exactly as the water path does: a hot material
    // reload between trace and shade would otherwise index the wrong
    // absorption.
    let tm = voxMat(voxWordAt(h.tsCell));
    if (tm != MAT_AIR && isTranslucentSolid(materials[tm])) {
      let hitP = R.camPos + rd * h.tsT;
      color = shadeTranslucent(hitP, rd, tm, h.tsCell, h.tsAxis, h.tsSgn,
                               h.tsPath, color, h.tsT, in.pos.xy);
      // Fog from the ice surface, not from whatever is behind it.
      color = applyAerial(color, rd, h.tsT);
    }
  }

  // ---- MPM fluid surface ----
  // The MPM isosurface encompasses both active particles AND settled voxel
  // water (via fluidCellAt's virtual-mass blend).  The CA water block above
  // already defers to the MPM path for seam-eligible cells inside the
  // isosurface, so this path now runs unconditionally when the march hit —
  // except for viscous liquids (blood, oil) genuinely nearer than the MPM
  // surface, which the CA block still shaded.
  // ---- THE PANE GUARD --------------------------------------------------------
  // `caShadedLiquid` is the whole gate. It is true exactly when a CA liquid path
  // already drew this pixel's interface, and that covers two distinct failures
  // at once:
  //
  //  1. DOUBLE SHADE. Two water surfaces painted over each other on the same
  //     pixel — see the mpmOwned block above.
  //  2. THE PANE. The fluid march's empty-space skips classify a chunk as
  //     "nothing here" from the BLOCK MAP, but the field they skip through also
  //     contains settled voxel water (fluidCellAt's virtual mass). A chunk full
  //     of lake with no MPM block is skipped as empty, so when the ray enters a
  //     marched chunk it is ALREADY submerged: it takes one sample at or above
  //     iso and the crossing bisection collapses onto the chunk face it just
  //     came through. The march reports an interface on a CHUNK BOUNDARY PLANE
  //     and this shade draws it — a vertical pane of glass standing in the pond,
  //     Fresnel and specular glint and all, tens of voxels under the real
  //     surface. It cannot be a real interface: the ray was inside water before
  //     it got there, which is precisely what caShadedLiquid records.
  if (mf.hit && !caShadedLiquid) {
    let caMatRaw = select(MAT_AIR, voxMat(voxWordAt(h.liqCell)), h.liqT > 0.0);
    let viscousNearer = h.liqT > 0.05 && h.liqT < mf.t
                        && isViscousLiquid(materials[voxMat(voxWordAt(h.liqCell))]);
    if (!viscousNearer) {
      // The SETTLED water this surface is standing on (see the SEAM SHADING
      // block above shadeMpmFluid). Only a non-viscous CA liquid the primary
      // ray actually crossed counts: h.liqPath is the length it measured, and
      // it is what turns a pour into a lake from a pale blue chunk into the
      // lake's own colour. A viscous liquid nearer the eye already took the
      // pixel above, and one FURTHER than the fluid surface is behind it, so
      // its column does belong to this shade.
      var caMat = MAT_AIR;
      var caPath = 0.0;
      if (caMatRaw != MAT_AIR && materials[caMatRaw].klass == CLASS_LIQUID &&
          !isViscousLiquid(materials[caMatRaw])) {
        caMat = caMatRaw;
        caPath = h.liqPath;
      }
      color = shadeMpmFluid(R.camPos, rd, mf, caMat, caPath, color);
      if (!mf.inside) { color = applyAerial(color, rd, mf.t); }
    }
  }

  // NOTE: rising embers over lava were attempted here and REMOVED. The
  // approach (probe downward per ray step for a molten surface, then resolve
  // analytic spark points above it) is sound in principle but was never made
  // to render reliably, and it cost ~13 ms/frame at 1080p because the probe
  // runs per step for every pixel on screen. If revisited, do NOT rebuild it
  // as a per-pixel search: spawn real particles from a bounded event, or bake
  // an emitter list the shader can index, so the cost scales with the number
  // of pools rather than with screen area.


  // fire glow: additive, from the flicker-weighted emissive path. Intensity
  // drives a temperature ramp across the material palette — stray flame
  // voxels stay wispy deep-orange, plume cores saturate toward white-hot.
  if (h.fireMat != 0u && h.fireGlow > 0.0) {
    let fm = materials[h.fireMat];
    let x = 1.0 - exp(-h.fireGlow * TUNE_FIRE_GLOW_RATE);
    var fc = mix(unpackColor(fm.color2), unpackColor(fm.color0),
                 clamp(x * 2.0, 0.0, 1.0));
    fc = mix(fc, unpackColor(fm.color1) * 1.25 + vec3f(0.10, 0.06, 0.0),
             clamp(x * 2.0 - 1.0, 0.0, 1.0));
    // slow global breathing on top of the per-cell flicker baked into fireGlow
    let breathe = (1.0 - TUNE_FIRE_BREATHE_AMP) + TUNE_FIRE_BREATHE_AMP * sin(R.time * TUNE_FIRE_BREATHE_RATE);
    color += fc * x * TUNE_FIRE_INTENSITY * breathe;
  }

  // ---- tonemap ----
  // The renderer works in linear HDR and emissive surfaces legitimately exceed
  // 1.0 by a wide margin, so the output curve has to COMPRESS the highlights
  // rather than clip them. The previous `pow(color, 1/2.2)` was a bare gamma
  // curve: everything over 1.0 clamped flat, which meant a hot surface lost
  // all its colour AND all its structure at exactly the moment it got
  // interesting. It is half the reason lava rendered as a white slab (the
  // other half being that emission was added on top of a lit albedo).
  //
  // Reinhard-with-white-point on LUMINANCE (per-channel desaturates: ember
  // orange turns tan and every warm emissive goes gold), with a late cubed
  // blend of per-channel at the very top so genuinely hot cores bleach toward
  // white like real blackbody progression. Lives in common.wgsl (tonemapHdr)
  // because the raster body paths must compress through the SAME curve or
  // debris brightness drifts from the terrain it landed on.
  color = tonemapHdr(color);
  var out : FSOut;
  out.color = vec4f(color, 1.0);
  out.depth = depth;
  return out;
}
