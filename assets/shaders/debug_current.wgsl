// CURRENT-FIELD DEBUG OVERLAY — the arrows (water plan component 8).
//
// A CLONE of debug_wind.wgsl, and it has to be one: the arrow geometry, the
// axial fade and the near-plane cull below are three bugs already paid for
// once, and re-deriving them for a second field would pay for them again.
// What is different is one line — it samples `currentAt` instead of `windAt`
// — plus the ramp's full-scale speed, because a current is a tenth of a wind.
//
// THE WHOLE POINT is that it calls `currentAt`, the same function in
// common.wgsl that the surface waves advect with and the foam reads its
// convergence from. A visualiser with its own copy of the field would be a
// picture of a DIFFERENT current, agreeing with the world only until someone
// edited one of the two, and it would be exactly as convincing while wrong.
//
// ZERO CPU WORK PER ARROW: no arrow buffer and nothing uploaded, the vertex
// shader derives its lattice point from the instance index and R.camPos alone.
// And zero cost when off — main.cpp skips the draw entirely.

@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec3f,
  @location(1) alpha : f32,
};

// Lattice points per axis, from the two knobs. MIRRORED in
// CurrentDebugArrowsPerAxis (src/sim/currentprim.h), which is what decides how many
// instances get drawn — the CPU picks the count, this picks where each one
// goes, and the two derive it the same way from the same constants. The clamp
// is the same clamp, for the same reason.
fn arrowsPerAxis() -> i32 {
  let half = clamp(i32(TUNE_CUR_DBG_RADIUS / TUNE_CUR_DBG_SPACING), 0, 24);
  return 2 * half + 1;
}

// Cool -> hot on 0..1. Blue for still air through cyan, green and yellow to
// red at the top of the range, which is the ramp a slope field is normally
// read with. Piecewise-linear on purpose: a smooth analytic ramp makes the
// magnitude harder to eyeball, and eyeballing it is the job.
fn currentRamp(x : f32) -> vec3f {
  let u = clamp(x, 0.0, 1.0);
  if (u < 0.25) { return mix(vec3f(0.15, 0.25, 0.85), vec3f(0.10, 0.80, 0.90), u * 4.0); }
  if (u < 0.50) { return mix(vec3f(0.10, 0.80, 0.90), vec3f(0.25, 0.90, 0.30), (u - 0.25) * 4.0); }
  if (u < 0.75) { return mix(vec3f(0.25, 0.90, 0.30), vec3f(0.95, 0.85, 0.15), (u - 0.50) * 4.0); }
  return mix(vec3f(0.95, 0.85, 0.15), vec3f(1.0, 0.20, 0.12), (u - 0.75) * 4.0);
}

// Reference speed the colour ramp and the arrow length saturate at, world
// cells/s. 40 = 4 m/s, which is a violent drain's throat — a river reach sits
// in the blue-green half and there is headroom left to SEE a whirlpool. NOT
// the wind overlay's 240: at that scale every current in the world is one
// shade of blue, which is a picture that says nothing.
const CUR_DBG_FULL_SCALE : f32 = 40.0;

@vertex
fn vsCurArrow(@builtin(vertex_index) vi : u32,
           @builtin(instance_index) inst : u32) -> VSOut {
  var out : VSOut;

  // ---- lattice point from the instance index -----------------------------
  let n = arrowsPerAxis();
  let nu = u32(n);
  let ix = i32(inst % nu);
  let iy = i32((inst / nu) % nu);
  let iz = i32((inst / (nu * nu)) % nu);
  let half = (n - 1) / 2;

  // Anchor the lattice to a WORLD grid snapped to the spacing, not to the
  // camera itself. Anchoring to the camera makes every arrow slide along with
  // you, which reads as the field moving — the one thing the overlay exists to
  // let you judge. Snapped, the arrows stand still in the world and the set of
  // them visible rolls over as you walk.
  let sp = max(TUNE_CUR_DBG_SPACING, 1.0);
  let base = floor(R.camPos / sp) * sp;
  let p = base + vec3f(f32(ix - half), f32(iy - half), f32(iz - half)) * sp;

  // Cull to a SPHERE, not the lattice's cube: the corners of the cube are 1.7x
  // further out than the sides, so without this the field has a boxy edge that
  // looks like a property of the wind.
  let toArrow = p - R.camPos;
  let dist = length(toArrow);
  let vz = dot(toArrow, R.camFwd);   // view-space depth, = projectView's w
  if (dist > TUNE_CUR_DBG_RADIUS) {
    // Degenerate: collapse the whole quad to one clip-space point, so it has
    // zero area and rasterizes nothing. Cheaper and more certain than an alpha
    // of zero, which still shades every pixel it covers. Safe to do per-vertex
    // because every test that reaches here depends only on the INSTANCE, so
    // all six vertices of a quad agree — a triangle with one collapsed corner
    // and two real ones would be a shard stretched across the screen.
    out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
    out.color = vec3f(0.0);
    out.alpha = 0.0;
    return out;
  }

  // ---- THE FIELD ---------------------------------------------------------
  // The same currentAt() the wave advection and the foam call. The overlay is
  // EVIDENCE only because it is the identical function.
  let w = currentAt(p, &R);
  let speed = length(w);
  let mag01 = clamp(speed / CUR_DBG_FULL_SCALE, 0.0, 1.0);
  if (speed < 1e-3) {
    out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
    out.color = vec3f(0.0);
    out.alpha = 0.0;
    return out;
  }
  let dir = w / speed;

  // Arrow length: mostly fixed, modestly magnitude-scaled. Bounded by the
  // spacing so a strong gust can never grow an arrow into its neighbour —
  // overlapping arrows turn a field into a hairball and the ramp already
  // carries the magnitude.
  let len = sp * (0.18 + 0.42 * mag01);

  // AXIAL FADE — fade out arrows pointing at or away from the eye.
  //
  // This is not cosmetic. An arrow aligned with the view ray carries no
  // direction on screen: it projects to a streak running radially out of the
  // wind's vanishing point, LENGTHENING as it aligns (perspective divide — the
  // near end of the segment projects further from the vanishing point than the
  // far end). A field of them reads as a starburst of thin lines, which looks
  // exactly like a rasterizer bug and hides the arrows that are actually
  // informative. The geometry was right; the projection is just useless there.
  //
  // So: alpha by the SINE of the angle between the arrow and the view ray.
  // Full strength across the view, gone within ~20 degrees of the axis. The
  // small hole this leaves around the vanishing point is honest — that is the
  // region where a 2D arrow genuinely cannot say which way the wind blows —
  // and the tuner's direction knob is what you turn to look into it.
  let viewRay = toArrow / max(dist, 1e-4);
  let axial = abs(dot(dir, viewRay));
  let axialFade = smoothstep(0.0, 0.35, sqrt(max(0.0, 1.0 - axial * axial)));
  if (axialFade < 0.01) {
    out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
    out.color = vec3f(0.0);
    out.alpha = 0.0;
    return out;
  }

  // NEAR-PLANE CULL, and it has to happen HERE — after `len` is known — not up
  // with the sphere test. `projectView` returns w = view depth, so a vertex at
  // w <= 0 is behind the eye: the triangle it belongs to gets clipped and
  // renders as a long shard stretched clean across the frame. Testing the
  // arrow's CENTRE is not enough, because its vertices reach up to ~0.6 len
  // toward the camera from there — which is exactly the bug this replaced: a
  // fan of thin lines radiating from one point near the horizon, thrown by
  // arrows off to the side of the view whose leading vertex had crossed behind
  // the eye while their centre had not.
  if (vz <= len * 0.75 + 1.0) {
    out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
    out.color = vec3f(0.0);
    out.alpha = 0.0;
    return out;
  }

  let a = p - dir * (len * 0.5);
  let b = p + dir * (len * 0.5);

  // ---- three segments: shaft, two head barbs -----------------------------
  // A barb needs an axis to splay about. Any vector not parallel to `dir` will
  // do; picking the world axis `dir` is LEAST aligned with keeps the cross
  // product well conditioned at every orientation, including straight up.
  let ax = abs(dir);
  var upAxis = vec3f(0.0, 1.0, 0.0);
  if (ax.y >= ax.x && ax.y >= ax.z) { upAxis = vec3f(1.0, 0.0, 0.0); }
  let barbAxis = normalize(cross(dir, upAxis));
  let headLen = len * 0.34;
  let headW = len * 0.17;

  let seg = vi / 6u;      // 0 shaft, 1 and 2 the barbs
  let v = vi % 6u;        // which vertex of that segment's quad
  var s0 = a;
  var s1 = b;
  if (seg == 1u) { s0 = b; s1 = b - dir * headLen + barbAxis * headW; }
  else if (seg == 2u) { s0 = b; s1 = b - dir * headLen - barbAxis * headW; }

  // Two triangles per segment: (s0-, s0+, s1-) and (s1-, s0+, s1+), where the
  // sign is the offset to either side of the line. Same expansion
  // debug_lines.wgsl uses for box edges.
  let atEnd = (v == 2u || v == 3u || v == 5u);
  let side = select(-1.0, 1.0, (v == 1u || v == 3u || v == 4u));
  let q = select(s0, s1, atEnd);

  // Thicken across the line in the plane facing the camera. Using the view
  // direction to the SEGMENT rather than the camera forward keeps the quad
  // edge-on-proof when an arrow is off to the side of the screen.
  let toCam = normalize(R.camPos - q);
  let sdir = s1 - s0;
  let slen = length(sdir);
  // A zero-length segment (barbs of a vanishingly short arrow) has no
  // direction to be perpendicular to; drop it rather than emit a NaN quad.
  if (slen < 1e-5) {
    out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
    out.color = vec3f(0.0);
    out.alpha = 0.0;
    return out;
  }
  let sn = sdir / slen;
  var perp = cross(sn, toCam);
  let plen = length(perp);
  // Degenerate only when the segment points straight at the eye; any
  // perpendicular will do there, and choosing one deterministically avoids a
  // quad that flickers as you turn.
  perp = select(normalize(cross(sn, R.camUp)), perp / plen, plen > 1e-4);

  // Thickness in world voxels, floored at roughly a pixel and capped at a few:
  // a distant arrow stays legible without a near one turning into a slab.
  let pxWorld = max(dist, 0.001) * R.tanHalfFov * 2.0 / max(R.viewPx, 1.0);
  let thick = clamp(len * 0.035, pxWorld * 1.1, pxWorld * 3.0);

  out.pos = projectView(q + perp * (side * thick) - R.camPos, R);
  out.color = currentRamp(mag01);
  // Fade out toward the edge of the lattice so the field does not end on a
  // hard shell, and fade the nearest arrows too — a lattice point a metre from
  // your eye is a wall of colour, not information.
  let far = 1.0 - smoothstep(TUNE_CUR_DBG_RADIUS * 0.55,
                             TUNE_CUR_DBG_RADIUS, dist);
  let near = smoothstep(0.0, sp * 1.5, dist);
  out.alpha = 0.85 * far * near * axialFade;
  return out;
}

@fragment
fn fsCurArrow(in : VSOut) -> @location(0) vec4f {
  // Unlit and untonemapped, exactly as debug_lines.wgsl argues: a debug
  // overlay that dims at dusk is useless at dusk, and the colour that encodes
  // the magnitude has to be the colour that reaches the screen or the ramp
  // stops meaning anything. The colour that was asked for is the colour drawn.
  return vec4f(in.color, in.alpha);
}
