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
// The wind-draft shelter volume (shadowBGL_ 24, the UPDATE's layout): a streak
// advected by windAt stops at a wall and threads through a doorway. The draw
// never reads it, so it is not in renderBGL_'s view of this module.
@group(0) @binding(24) var<storage, read> draftField : array<u32>;
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

// How far into "flying" the camera is: 0 within one radius of the ground,
// 1 from two radii up. Drives the spawn band's blend from the surface layer to
// a box round the eye.
fn streakFly(ground : f32, radius : f32) -> f32 {
  return clamp((R.camPos.y - ground) / max(radius, 1.0) - 1.0, 0.0, 1.0);
}
// The vertical half-reach of the band at this flying fraction: the surface
// layer's 6.2 m on the ground, the radius high up (whichever is larger).
fn streakVertReach(ground : f32, radius : f32) -> f32 {
  let fly = streakFly(ground, radius);
  return max(mix(R.camPos.y - ground + 62.0, radius, fly), 62.0);
}

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
    // HEIGHT. Near the ground the band is the surface layer, 0.2 .. 6.2 m up
    // and most of them low, because that is where the gusts are seen against
    // the grass. Climb and it follows the camera: past one radius above the
    // ground it blends into a box +-radius round the eye, fully by two, so a
    // flier high up still sees streaks around them instead of a carpet of
    // them far below.
    let hq = streakU01(4u, i);
    let fly = streakFly(ground, radius);
    let lo = mix(ground + 2.0, max(ground + 2.0, R.camPos.y - radius), fly);
    let hi = mix(ground + 62.0, max(ground + 62.0, R.camPos.y + radius), fly);
    p.y = lo + mix(hq * hq, hq, fly) * (hi - lo);
    let s = windSampleAt(p, R.time, 0.0, &R);
    let bands = windBandWS(s, s.b1) * WIND_BAND_W1 + windBandWS(s, s.b2) * WIND_BAND_W2;
    let m = windMeanWS(s);
    let ml = length(m.xz);
    var ex = length(windPrimAt(p, &R));
    // Sheltered by the draft volume like the advection below: indoors, in a
    // cave or in the lee of a wall the gust excess shrinks with the wind, so
    // a still room spawns nothing instead of streaks that hang in place.
    let full = m + bands;
    let fl = length(full.xz);
    let shelter = select(1.0, length(draftApplyF(p, full, &R).xz) / fl, fl > 1e-3);
    if (ml > 1e-3) { ex += dot(bands.xz, m.xz) / ml * min(shelter, 1.5); }
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
  let p0 = h0.xyz;
  let a0 = h0.w;
  let p = p0 + w * dt;
  h0 = vec4f(p, a0 + dt);
  var head = u32(h1.z) % trail;
  // Push a trail point every `spacing` seconds — AS MANY as fell inside this
  // frame, each placed where the streak was at that instant along the frame's
  // step. One push per frame made a frame longer than `spacing` (anything
  // under 1 / spacing = 40 fps at the default) stretch the ribbon to
  // trail x dt: the same gust drew a longer streak on a slower machine.
  // Only the newest `trail` of them are written: older ones would be
  // overwritten within this same frame anyway.
  let spacing = max(R.streakB.y, 1e-3);
  let total = h1.w + dt;
  let cnt = u32(floor(total / spacing));
  for (var k = select(0u, cnt - trail, cnt > trail); k < cnt; k++) {
    // How long before the frame's end the streak passed push k.
    let ago = total - spacing * f32(k + 1u);
    let f = clamp(1.0 - ago / dt, 0.0, 1.0);
    head = (head + 1u) % trail;
    streaks[base + 2u + head] = vec4f(mix(p0, p, f), a0 + dt * f);
  }
  let timer = min(total - spacing * f32(cnt), spacing);
  // Out of the pool's reach, or into the ground: done (age = life).
  let d = p.xz - R.camPos.xz;
  var dead = dot(d, d) > radius * radius * 1.69;
  // ...or out of it vertically: the band's own reach (streakVertReach).
  let tr = windTerrF(p, &R);
  let groundHere = select(R.camPos.y - 17.0, tr.h, tr.edge > 0.0);
  if (abs(p.y - R.camPos.y) > streakVertReach(groundHere, radius) * 1.3) { dead = true; }
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
  // Every cull below is decided for the WHOLE segment, from both ends, before
  // any vertex is placed. Per-vertex culls put a culled corner at clip
  // (0, 0, 0, 1) while its triangle's other corners stay put — a sliver from
  // the streak to the middle of the screen.
  let rel0 = s0 - R.camPos;
  let rel1 = s1 - R.camPos;
  if (min(dot(rel0, R.camFwd), dot(rel1, R.camFwd)) <= 2.0) { return out; }  // behind / at the eye
  let sd = sdir / slen;
  let pl0 = length(cross(sd, normalize(rel0)));
  let pl1 = length(cross(sd, normalize(rel1)));
  if (min(pl0, pl1) < 1e-4) { return out; }   // seen end-on: no width to give it
  // Same expansion as debug_wind.wgsl: (s0-, s0+, s1-), (s1-, s0+, s1+).
  let atEnd = (v == 2u || v == 3u || v == 5u);
  let side = select(-1.0, 1.0, (v == 1u || v == 3u || v == 4u));
  let q = select(s0, s1, atEnd);
  let rel = select(rel0, rel1, atEnd);
  let dist = length(rel);
  let perp = cross(sd, rel / dist) / select(pl0, pl1, atEnd);
  // 0 at the head, 1 at the tail.
  let kf = f32(select(seg, seg + 1u, atEnd)) / f32(trail - 1u);
  let pxWorld = dist * R.tanHalfFov * 2.0 / max(R.viewPx, 1.0);
  // Never thinner than ~a pixel (a thinner ribbon drops in and out of the
  // raster as it moves) — but a ribbon WIDENED to that floor is dimmed by the
  // same factor, so a distant wisp keeps the brightness its real width earns
  // instead of reading as a bold line.
  let want = R.streakB.z * (1.0 - 0.7 * kf);
  let width = max(want, pxWorld * 0.6);
  let cover = want / width;
  out.pos = projectView(rel + perp * (side * width), R);
  let lf = h0.w / h1.x;
  let life = smoothstep(0.0, 0.15, lf) * (1.0 - smoothstep(0.55, 1.0, lf));
  // Gone within a metre of the eye, full by four: a streak that close is a
  // slab across the screen, not a wisp.
  let near = smoothstep(10.0, 40.0, dist);
  // Horizontal reach, plus any vertical distance past the surface band's own
  // 6.2 m: on the ground nothing changes, high up a streak far above or below
  // the eye fades like one far off to the side.
  let dyEx = max(abs(q.y - R.camPos.y) - 62.0, 0.0);
  let far = 1.0 - smoothstep(R.streakA.w * 0.6, R.streakA.w,
                             length(vec2f(length(q.xz - R.camPos.xz), dyEx)));
  let fog = exp(-dist * VOXEL_METERS * R.fogDensity);
  // Tapered at BOTH ends: fading in over the first segment behind the head
  // and out toward the tail. A full-alpha head was a blunt cap, which reads
  // as a comet (a thing) rather than a wisp (moving air).
  let shape = min(kf * f32(trail - 1u), 1.0) * (1.0 - kf);
  out.alpha = clamp(R.streakA.x * h1.y * shape * cover * life * near * far * fog, 0.0, 1.0);
  // The same translucent white day and night (owner call, 2026-09-30). It was
  // dimmed with the light (down to 0.08 at night), and a dark colour blended
  // at the streak's alpha reads as a BLACK line over a moonlit scene, not as
  // faint air. Visibility is the alpha's job; the colour stays constant.
  out.color = vec3f(0.92, 0.95, 1.0);
  return out;
}

@fragment
fn fsStreak(in : StreakOut) -> @location(0) vec4f {
  return vec4f(in.color, in.alpha);
}
