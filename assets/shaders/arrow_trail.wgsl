// ARROW TRAILS — a faint ribbon behind every arrow in flight, so the arc it
// flies reads (owner, 2026-10-15). RENDER-ONLY: nothing here touches
// TickParams, a voxel or anything hashed. Layout and ownership:
// src/sim/arrowtrail.h.
//
// Its own module on purpose. It started as a second entry point in
// wind_streak.wgsl, reading rows parked after the gust-streak pool; the wind's
// look is expected to change, and the arrows' must not change with it. So
// nothing here reads a wind knob or the streak pool: the width, colour and
// near fade are this file's.
//
// THE ROWS (renderPartBGL_ binding 6, vertex), CPU-written each frame by
// game/arrows.h ArrowTrails: per trail ARROW_TRAIL_PTS vec4f of (position,
// alpha), point 0 the arrow's drawn point and on back through where it was,
// so the ribbon IS the arc it flew. The alpha carries the fade with age; a
// point past the trail's end has alpha 0 and sits on the one before it (no
// segment). ARROW_TRAIL_PTS must match kArrowTrailPts (check_invariants
// `arrowtrail`).
//
// One instance per trail, (ARROW_TRAIL_PTS - 1) segments x 6 vertices, drawn
// as a camera-facing quad strip: hidden by the world in front of it, writing
// no depth, straight alpha over the scene.

@group(0) @binding(3) var<uniform> R : RenderParams;
@group(1) @binding(6) var<storage, read> trails : array<vec4f>;

const ARROW_TRAIL_PTS : u32 = 16u;
// Ribbon half-width at the head (world voxels: 1.5 cm), tapering to 40% at
// the tail.
const TRAIL_WIDTH : f32 = 0.15;
const TRAIL_TAIL_WIDTH : f32 = 0.4;
// Faded out within this many voxels of the eye and full by the second: your
// own shot leaving the string is not a slab across the screen.
const TRAIL_NEAR0 : f32 = 15.0;
const TRAIL_NEAR1 : f32 = 40.0;
// A translucent white, the same by day and night (a dark colour at low alpha
// reads as a black line over a moonlit scene, not as a streak of air).
const TRAIL_COLOR : vec3f = vec3f(0.92, 0.95, 1.0);

struct TrailOut {
  @builtin(position) pos : vec4f,
  @location(0) alpha : f32,
};

@vertex
fn vsTrail(@builtin(vertex_index) vi : u32,
           @builtin(instance_index) ii : u32) -> TrailOut {
  var out : TrailOut;
  out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
  out.alpha = 0.0;
  let seg = vi / 6u;
  let v = vi % 6u;
  if (seg + 1u >= ARROW_TRAIL_PTS) { return out; }
  let base = ii * ARROW_TRAIL_PTS;
  let a0 = trails[base + seg];
  let a1 = trails[base + seg + 1u];
  if (max(a0.w, a1.w) <= 0.0) { return out; }
  let s0 = a0.xyz;
  let s1 = a1.xyz;
  let sdir = s1 - s0;
  let slen = length(sdir);
  if (slen < 1e-4) { return out; }
  // Every cull is decided for the WHOLE segment, from both ends, before any
  // vertex is placed: a per-vertex cull puts one corner at clip (0, 0, 0, 1)
  // while the others stay put -- a sliver to the middle of the screen.
  let rel0 = s0 - R.camPos;
  let rel1 = s1 - R.camPos;
  if (min(dot(rel0, R.camFwd), dot(rel1, R.camFwd)) <= 2.0) { return out; }  // behind / at the eye
  let sd = sdir / slen;
  let pl0 = length(cross(sd, normalize(rel0)));
  let pl1 = length(cross(sd, normalize(rel1)));
  if (min(pl0, pl1) < 1e-4) { return out; }   // seen end-on: no width to give it
  // Two triangles per segment: (s0-, s0+, s1-), (s1-, s0+, s1+).
  let atEnd = (v == 2u || v == 3u || v == 5u);
  let side = select(-1.0, 1.0, (v == 1u || v == 3u || v == 4u));
  let q = select(s0, s1, atEnd);
  let rel = select(rel0, rel1, atEnd);
  let dist = length(rel);
  let perp = cross(sd, rel / dist) / select(pl0, pl1, atEnd);
  // 0 at the head, 1 at the tail.
  let kf = f32(select(seg, seg + 1u, atEnd)) / f32(ARROW_TRAIL_PTS - 1u);
  // Never thinner than ~a pixel (a thinner ribbon drops in and out of the
  // raster as it moves) -- but a ribbon WIDENED to that floor is dimmed by the
  // same factor, so a distant trail keeps the brightness its real width earns.
  let pxWorld = dist * R.tanHalfFov * 2.0 / max(R.viewPx, 1.0);
  let want = TRAIL_WIDTH * mix(1.0, TRAIL_TAIL_WIDTH, kf);
  let width = max(want, pxWorld * 0.6);
  let cover = want / width;
  out.pos = projectView(rel + perp * (side * width), R);
  let near = smoothstep(TRAIL_NEAR0, TRAIL_NEAR1, dist);
  // The one air law (common.wgsl airFogTauS), eye to the trail.
  let fog = exp(-airFogTauS(R.camPos.y, R.hfogDensity, R.hfogFalloff, R.hfogBaseY,
                            R.fogDensity, dist * VOXEL_METERS,
                            (q.y - R.camPos.y) / max(dist, 1e-3)));
  out.alpha = clamp(select(a0.w, a1.w, atEnd) * cover * near * fog, 0.0, 1.0);
  return out;
}

@fragment
fn fsTrail(in : TrailOut) -> @location(0) vec4f {
  return vec4f(TRAIL_COLOR, in.alpha);
}
