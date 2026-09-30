// GUST STREAKS — faint white air-streaks that trace the wind, visible mainly in
// strong gusts (docs/RESEARCH_wind.md §13.6). RENDER-ONLY: nothing here
// touches TickParams, a voxel or anything hashed.
//
// THE POOL. A fixed ring of kWindStreakCap particles near the camera
// (Simulation::windStreakBuf_), updated once a frame by `update` on the
// per-frame ShadowCache table and drawn by `vsStreak`/`fsStreak` in the world
// pass. Both are bounded by the pool, not by the world: the compute pass is
// one thread per live slot (render.streakCount, <= the cap), the draw one
// instance per slot, and at master alpha 0 neither is recorded at all.
//
// SPAWN ONLY IN GUSTS. A dead slot tries to respawn a few times a second at a
// random point in a disc around the camera, low over the terrain table's
// ground. It is born only with probability smoothstep(threshold, threshold +
// span, excess), where the EXCESS is the gust bands' component along the local
// mean plus the magnitude of any wind primitive there — the part of the wind
// above its mean. So a calm day spawns nothing, a breezy one the odd streak in
// its strongest gusts, and a gale a sky full of them.
//
// THEY FOLLOW THE FIELD. Each streak is advected by windAt(p, t) — the same
// function the grass sway and the debug arrows sample, primitives included —
// so the streaks ARE the field, drawn. A trail is the last `trail` positions,
// pushed every `spacing` seconds into a ring, drawn as a camera-facing ribbon
// tapering and fading from head to tail, fading in and out over the particle's
// life, near the camera, at the pool's radius and into the fog.
//
// LAYOUT, array<vec4f>: word 0 unused, then per slot STREAK_STRIDE rows —
//   [0] position (world voxels), age (s)
//   [1] lifetime (s, 0 = never spawned), strength (0..1), ring head, push timer
//   [2 .. 2 + STREAK_TRAIL_MAX) the trail ring: position, age at push
// STREAK_TRAIL_MAX / STREAK_STRIDE must match kWindStreakTrail /
// kWindStreakStride in src/sim/world.h (check_invariants `windstreak`).

@group(0) @binding(3) var<uniform> R : RenderParams;
// The update's view (shadowBGL_ binding 22, compute).
@group(0) @binding(22) var<storage, read_write> streaks : array<vec4f>;
// The draw's view of the same buffer (renderBGL_ binding 33, vertex).
@group(0) @binding(33) var<storage, read> streaksR : array<vec4f>;

const STREAK_TRAIL_MAX : u32 = 16u;
const STREAK_STRIDE : u32 = 18u;
// Distinct salt: never a slice of another stream (render-only, but a streak
// field correlated with some other hashed pattern would still read as one).
const STREAK_SALT : u32 = 0x57EAu;
// Respawn attempts per second per dead slot. The spawn PROBABILITY is the gust
// gate below; this only sets how quickly a gust fills with streaks.
const STREAK_SPAWN_RATE : f32 = 5.0;

fn streakU01(a : u32, b : u32) -> f32 {
  return f32(hash3(STREAK_SALT ^ a, R.streakN.z, b) & 0xFFFFFFu) * (1.0 / 16777216.0);
}

fn streakAlive(h0 : vec4f, h1 : vec4f) -> bool { return h1.x > 0.0 && h0.w < h1.x; }

@compute @workgroup_size(64)
fn update(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i >= R.streakN.x) { return; }
  let base = 1u + i * STREAK_STRIDE;
  var h0 = streaks[base];
  var h1 = streaks[base + 1u];
  let dt = R.streakB.w;
  if (dt <= 0.0) { return; }                  // paused, or an aux view's frame
  let trail = clamp(R.streakN.y, 2u, STREAK_TRAIL_MAX);
  let radius = R.streakA.w;

  if (!streakAlive(h0, h1)) {
    // ---- respawn attempt ----
    if (streakU01(1u, i) >= min(dt * STREAK_SPAWN_RATE, 1.0)) { return; }
    let ang = streakU01(2u, i) * 6.28318531;
    let rr = sqrt(streakU01(3u, i)) * radius;
    var p = vec3f(R.camPos.x + cos(ang) * rr, 0.0, R.camPos.z + sin(ang) * rr);
    let tr = windTerrF(p, &R);
    let ground = select(R.camPos.y - 17.0, tr.h, tr.edge > 0.0);
    let hq = streakU01(4u, i);
    p.y = ground + 2.0 + hq * hq * 60.0;       // 0.2 .. 6.2 m, most of them low
    let s = windSampleAt(p, R.time, 0.0, &R);
    let bands = windBandWS(s, s.b1) * WIND_BAND_W1 + windBandWS(s, s.b2) * WIND_BAND_W2;
    let m = windMeanWS(s);
    let ml = length(m.xz);
    var ex = length(windPrimAt(p, &R));
    if (ml > 1e-3) { ex += dot(bands.xz, m.xz) / ml; }
    let pr = smoothstep(R.streakA.y, R.streakA.y + max(R.streakA.z, 1e-3), ex);
    if (streakU01(5u, i) >= pr) { return; }
    h0 = vec4f(p, 0.0);
    h1 = vec4f(R.streakB.x * (0.7 + 0.6 * streakU01(6u, i)), pr, 0.0, 0.0);
    for (var k = 0u; k < STREAK_TRAIL_MAX; k++) {
      streaks[base + 2u + k] = vec4f(p, 0.0);
    }
    streaks[base] = h0;
    streaks[base + 1u] = h1;
    return;
  }

  // ---- advect by THE field ----
  let w = windAt(h0.xyz, R.time, &R);
  let p = h0.xyz + w * dt;
  h0 = vec4f(p, h0.w + dt);
  var head = u32(h1.z) % trail;
  var timer = h1.w + dt;
  let spacing = max(R.streakB.y, 1e-3);
  if (timer >= spacing) {
    timer = min(timer - spacing, spacing);
    head = (head + 1u) % trail;
    streaks[base + 2u + head] = vec4f(p, h0.w);
  }
  // Out of the pool's reach, or into the ground: done (age = life).
  let d = p.xz - R.camPos.xz;
  var dead = dot(d, d) > radius * radius * 1.69;
  let tr = windTerrF(p, &R);
  if (tr.edge > 0.0 && p.y < tr.h) { dead = true; }
  if (dead) { h0.w = h1.x; }
  streaks[base] = h0;
  streaks[base + 1u] = vec4f(h1.x, h1.y, f32(head), timer);
}

struct StreakOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec3f,
  @location(1) alpha : f32,
};

fn streakPoint(base : u32, head : u32, trail : u32, k : u32, cur : vec3f) -> vec3f {
  if (k == 0u) { return cur; }
  return streaksR[base + 2u + (head + trail - (k - 1u)) % trail].xyz;
}

// One ribbon per instance: (trail - 1) segments x 6 vertices, head first.
@vertex
fn vsStreak(@builtin(vertex_index) vi : u32,
            @builtin(instance_index) ii : u32) -> StreakOut {
  var out : StreakOut;
  out.pos = vec4f(0.0, 0.0, 0.0, 1.0);
  out.color = vec3f(0.0);
  out.alpha = 0.0;
  let trail = clamp(R.streakN.y, 2u, STREAK_TRAIL_MAX);
  if (ii >= R.streakN.x) { return out; }
  let base = 1u + ii * STREAK_STRIDE;
  let h0 = streaksR[base];
  let h1 = streaksR[base + 1u];
  if (!streakAlive(h0, h1)) { return out; }
  let seg = vi / 6u;
  let v = vi % 6u;
  if (seg + 1u >= trail) { return out; }
  let head = u32(h1.z) % trail;
  let s0 = streakPoint(base, head, trail, seg, h0.xyz);
  let s1 = streakPoint(base, head, trail, seg + 1u, h0.xyz);
  let sdir = s1 - s0;
  let slen = length(sdir);
  if (slen < 1e-4) { return out; }            // not pushed yet: no segment
  // Same expansion as debug_wind.wgsl: (s0-, s0+, s1-), (s1-, s0+, s1+).
  let atEnd = (v == 2u || v == 3u || v == 5u);
  let side = select(-1.0, 1.0, (v == 1u || v == 3u || v == 4u));
  let q = select(s0, s1, atEnd);
  let rel = q - R.camPos;
  let dist = length(rel);
  if (dot(rel, R.camFwd) <= 2.0) { return out; }   // behind / at the eye
  var perp = cross(sdir / slen, rel / max(dist, 1e-4));
  let pl = length(perp);
  if (pl < 1e-4) { return out; }
  perp = perp / pl;
  // 0 at the head, 1 at the tail.
  let kf = f32(select(seg, seg + 1u, atEnd)) / f32(trail - 1u);
  let pxWorld = max(dist, 0.001) * R.tanHalfFov * 2.0 / max(R.viewPx, 1.0);
  let width = max(R.streakB.z * (1.0 - 0.7 * kf), pxWorld * 0.6);
  out.pos = projectView(rel + perp * (side * width), R);
  let lf = h0.w / h1.x;
  let life = smoothstep(0.0, 0.15, lf) * (1.0 - smoothstep(0.55, 1.0, lf));
  // Gone within a metre of the eye, full by four: a streak that close is a
  // slab across the screen, not a wisp.
  let near = smoothstep(10.0, 40.0, dist);
  let far = 1.0 - smoothstep(R.streakA.w * 0.6, R.streakA.w, length(q.xz - R.camPos.xz));
  let fog = exp(-dist * VOXEL_METERS * R.fogDensity);
  out.alpha = clamp(R.streakA.x * h1.y * (1.0 - kf) * life * near * far * fog, 0.0, 1.0);
  // White, dimmed with the light: a streak is lit air, not a lamp.
  let lit = clamp(R.dayWeight * 0.85 + R.moonLit * 0.25 + 0.08, 0.08, 1.0);
  out.color = vec3f(0.92, 0.95, 1.0) * lit;
  return out;
}

@fragment
fn fsStreak(in : StreakOut) -> @location(0) vec4f {
  return vec4f(in.color, in.alpha);
}
