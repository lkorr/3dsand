// COLLISION-BOX DEBUG OVERLAY — the green wireframes.
//
// Draws one oriented box per physics body: avatar/mob limbs, held items and
// rigidbody debris. This is a DEBUG view, so it is deliberately unlit, unfogged
// and drawn with depth testing OFF (see the pipeline in simulation.cpp): a
// collider you can only see when it happens to be unoccluded is useless for
// working out why a limb is catching on something, and the whole reason to look
// at one is that the body it belongs to is in the way.
//
// WHY QUADS AND NOT LineList. WebGPU guarantees no line width beyond 1 px, so a
// true line list vanishes at any distance and aliases badly up close. Each edge
// is therefore expanded into a thin CAMERA-FACING quad whose thickness is set
// in world units but floored in screen space, so a distant box stays legible
// without a near one turning into a slab. Six vertices per edge, twelve edges,
// 72 vertices per box — all procedural, matching the no-vertex-buffer
// convention every other draw here follows.

struct DebugBox {
  pos   : vec3f,   // box centre, world voxels
  pad0  : f32,
  half  : vec3f,   // half-extents along the box's OWN axes
  pad1  : f32,
  quat  : vec4f,   // x, y, z, w
  color : u32,     // 0xAABBGGRR
  pad2  : u32,
  pad3  : u32,
  pad4  : u32,
};

@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(1) @binding(4) var<storage, read> boxes : array<DebugBox>;

struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec3f,
  @location(1) alpha : f32,
};

fn qrot(q : vec4f, v : vec3f) -> vec3f {
  // v + 2w(qv x v) + 2 qv x (qv x v) — the standard sandwich, expanded.
  let u = q.xyz;
  return v + 2.0 * cross(u, cross(u, v) + q.w * v);
}

// The 8 corners of a unit box, indexed 0..7 as a 3-bit (x,y,z) sign pattern.
fn corner(i : u32, half : vec3f) -> vec3f {
  let sx = select(-1.0, 1.0, (i & 1u) != 0u);
  let sy = select(-1.0, 1.0, (i & 2u) != 0u);
  let sz = select(-1.0, 1.0, (i & 4u) != 0u);
  return vec3f(sx, sy, sz) * half;
}

// The 12 edges as corner-index pairs. Packed rather than declared as an array
// of vec2u so the table stays a compile-time constant expression.
fn edgeEnds(e : u32) -> vec2<u32> {
  // 4 edges along x, 4 along y, 4 along z.
  switch (e) {
    case 0u:  { return vec2u(0u, 1u); }
    case 1u:  { return vec2u(2u, 3u); }
    case 2u:  { return vec2u(4u, 5u); }
    case 3u:  { return vec2u(6u, 7u); }
    case 4u:  { return vec2u(0u, 2u); }
    case 5u:  { return vec2u(1u, 3u); }
    case 6u:  { return vec2u(4u, 6u); }
    case 7u:  { return vec2u(5u, 7u); }
    case 8u:  { return vec2u(0u, 4u); }
    case 9u:  { return vec2u(1u, 5u); }
    case 10u: { return vec2u(2u, 6u); }
    default:  { return vec2u(3u, 7u); }
  }
}

@vertex
fn vsBox(@builtin(vertex_index) vi : u32,
         @builtin(instance_index) inst : u32) -> VSOut {
  let b = boxes[inst];
  let e = vi / 6u;             // which of the 12 edges
  let v = vi % 6u;             // which vertex of that edge's quad
  let ends = edgeEnds(e);

  // Edge endpoints in world space, through the body's own rotation.
  let a = b.pos + qrot(b.quat, corner(ends.x, b.half));
  let c = b.pos + qrot(b.quat, corner(ends.y, b.half));

  // Two triangles: (a0,a1,c0) and (c0,a1,c1), where the 0/1 suffix is the
  // offset to either side of the line.
  let atEnd = (v == 2u || v == 3u || v == 5u);
  let side  = select(-1.0, 1.0, (v == 1u || v == 3u || v == 4u));
  let p = select(a, c, atEnd);

  // Thicken across the line, in the plane facing the camera. Using the view
  // direction to the SEGMENT (rather than the camera forward) keeps the quad
  // edge-on-proof when a box is off to the side of the screen.
  let toCam = normalize(R.camPos - p);
  let dir = normalize(c - a);
  var perp = cross(dir, toCam);
  let plen = length(perp);
  // Degenerate only when the edge points straight at the eye; any perpendicular
  // will do there, and picking one deterministically avoids a flickering quad.
  perp = select(normalize(cross(dir, R.camUp)), perp / plen, plen > 1e-4);

  // Thickness in world voxels, but never thinner than roughly a pixel and never
  // fatter than a few: distance-scaled so the wireframe reads the same whether
  // the body is at your feet or across the arena.
  let dist = max(length(R.camPos - p), 0.001);
  let pxWorld = dist * R.tanHalfFov * 2.0 / max(R.viewPx, 1.0);
  let thick = clamp(0.035, pxWorld * 1.1, pxWorld * 3.0);

  var out : VSOut;
  out.pos = projectView(p + perp * (side * thick) - R.camPos, R);
  let col = unpackColor(b.color);
  out.color = col;
  out.alpha = f32((b.color >> 24u) & 255u) / 255.0;
  return out;
}

@fragment
fn fsBox(in : VSOut) -> @location(0) vec4f {
  // Unlit and untonemapped ON PURPOSE. Every other pass here runs its colour
  // through litColor + tonemapHdr so it sits in the world; this one must NOT,
  // because a debug wireframe that dims at dusk or washes out in sunlight is
  // exactly as useless as one you cannot see through a wall. The colour that
  // was asked for is the colour that is drawn.
  return vec4f(in.color, in.alpha);
}
