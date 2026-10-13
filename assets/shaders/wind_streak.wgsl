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
// SPAWN WHERE THE WIND IS STRONG (2026-10-11). A dead slot tries to respawn a
// few times a second at a random point in a VOLUME round the camera: a disc
// of the pool's radius, from just over the terrain table's ground up to one
// radius over the eye (and down to one radius under it when flying). It is
// born with probability smoothstep(threshold, threshold + span, |windAt|) --
// the SPEED of the field there, shelter, primitives and heat included. So
// streaks are seen wherever the field runs fast: aloft, where the profile
// makes it fastest, as well as over the grass. Until this date the band was
// the 0.2-6.2 m surface layer (most of them under 2 m) gated on the gust
// EXCESS over the mean; with the ground boundary below that glued every
// streak to the terrain and none were ever seen in the sky (owner: "they just
// race along surfaces and seem bounded to those surfaces").
//
// THEY FOLLOW THE FIELD. Each streak is advected by windAt(p, t) — the same
// function the debug arrows sample, primitives and (since 2026-10-02) the heat
// term included, so a streak over a fire rises — the streaks ARE the field,
// drawn. (The grass sway samples it too, without the heat term: raymarch.wgsl
// does not bind the pool.) A trail is the last `trail` positions,
// pushed every `spacing` seconds into a ring, drawn as a camera-facing ribbon
// tapering and fading from head to tail, fading in and out over the particle's
// life, near the camera, at the pool's radius and into the fog.
//
// WATER. A gust streak is AIR: it is never born in a liquid cell and dies the
// step it enters one. The coarse terrain table counts a lake's surface as
// ground, but not a pond under its bilinear facets, a river, or a basin the
// player filled -- and under the water veil the surface writes no depth, so a
// streak under it drew straight through. While the camera's own cell is
// liquid the gust streaks are not drawn at all: from below, the air is seen
// through a moving interface, not as crisp wisps.
//
// CURRENT TRACERS (2026-10-10). The slots after the gust streaks are the
// CURRENT field drawn the same way: born a hand's depth under a water surface
// where currentAt() runs faster than CUR_SPAWN_LO, carried along the surface
// by its horizontal part, dead when they leave the liquid. currentAt is the
// function the surface waves advect with and the current arrows sample
// (debug_current.wgsl), so these ARE that field. Still water, and a world with
// no current primitives (currentRenderOn 0), spawns none.
//
// LAYOUT, array<vec4f>: row 0 = (camera cell is liquid ? 1 : 0, 0, 0, 0),
// written by the update's thread 0 for the draw; then per slot STREAK_STRIDE rows —
//   [0] position (world voxels), age (s)
//   [1] lifetime (s, 0 = never spawned), strength (0..1), ring head, push timer
//   [2 .. 2 + STREAK_TRAIL_MAX) the trail ring: position, age at push
// STREAK_TRAIL_MAX / STREAK_STRIDE must match kWindStreakTrail /
// kWindStreakStride in src/sim/world.h (check_invariants `windstreak`).

@group(0) @binding(3) var<uniform> R : RenderParams;
// The cell under a particle (the update's layout, shadowBGL_ 0/2/4): a gust
// streak dies in liquid and a current tracer lives only in it. The draw never
// reads them. `> voxels` with EXACTLY ONE SPACE: resources.cpp keys the
// page-table block on it.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
// The wind-draft shelter volume (shadowBGL_ 24, the UPDATE's layout): a streak
// advected by windAt stops at a wall and threads through a doorway. The draw
// never reads it, so it is not in renderBGL_'s view of this module.
@group(0) @binding(24) var<storage, read> draftField : array<u32>;
// The heat layer (shadowBGL_ 25/26, the UPDATE's layout): a streak over a fire
// or a lava pool rises with the heat term windAt adds (common.wgsl windHeatF,
// the BOUND_RO reader). Read-only views of the sim's words, written on the
// tick command buffer; the wind_streak row's R(HeatPool) R(HeatMeta) order it
// after the tick's heat passes. The draw never reads them.
@group(0) @binding(25) var<storage, read> heatPool : array<u32>;
@group(0) @binding(26) var<storage, read> heatMeta : array<u32>;
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
// The ground as the flow's boundary (the advection step's note): the depth
// over which a streak's vertical speed follows the terrain table's slope,
// fading linearly to none (voxels: 12 m -- the layer the ground actually
// deflects; at 40 m every streak in the low band rode the terrain's shape and
// read as bound to it); the steepest slope it follows (a 56-degree face; a cliff
// cell's gradient is clamped to this); and the clearance a step that ends
// under the table's ground is put back to (the spawn band's own floor).
const STREAK_FOLLOW_DEPTH : f32 = 120.0;
const STREAK_FOLLOW_MAX_SLOPE : f32 = 1.5;
const STREAK_FLOOR : f32 = 2.0;

// ---- current tracers. RenderParams has no room for more knobs: alpha and
// slot count ride streakN.w (support.cpp packs it), the rest is here ----
// Respawn attempts per second per dead slot.
const CUR_SPAWN_RATE : f32 = 3.0;
// Surface speed (world cells/s) below which nothing is born, and the span over
// which a spawn becomes certain: 0.05 m/s and +0.5 m/s. A river reach runs
// ~0.5-1.5 m/s; a pond with no primitive is exactly 0.
const CUR_SPAWN_LO : f32 = 0.5;
const CUR_SPAWN_SPAN : f32 = 5.0;
// Lifetime (s, +-30%); trail spacing as a multiple of the gust streaks' (water
// is ~10x slower than air: the same spacing would be a 2 cm smear); ribbon
// width as a multiple of theirs.
const CUR_LIFE : f32 = 4.0;
const CUR_SPACING_MUL : f32 = 4.0;
const CUR_WIDTH_MUL : f32 = 1.6;
// Depth under the surface a tracer rides at, voxels: 0.15 .. 1.15 m, most of
// them shallow, so from the bank they read as on the water.
const CUR_DEPTH_MIN : f32 = 1.5;
const CUR_DEPTH_SPAN : f32 = 10.0;
// The surface search: from this far over the terrain table's ground (which
// counts standing water) down at most this many cells.
const CUR_SCAN_ABOVE : i32 = 24;
const CUR_SCAN_CELLS : i32 = 72;

fn curSlots() -> u32 { return (R.streakN.w >> 8u) & 0xFFFu; }
fn curAlpha() -> f32 { return f32(R.streakN.w >> 24u) * (1.0 / 255.0); }
fn streakSlots() -> u32 { return R.streakN.x + curSlots(); }

// Is the cell at p a liquid? Outside the residency window = no (the masked
// read would alias another chunk).
fn streakCellLiquid(p : vec3f) -> bool {
  let c = vec3<i32>(floor(p));
  if (!chunkInWindow(worldChunkOf(c), ptOrigin())) { return false; }
  return materials[voxMat(voxWordAt(c))].klass == CLASS_LIQUID;
}

fn streakU01(a : u32, b : u32) -> f32 {
  return f32(hash3(STREAK_SALT ^ a, R.streakN.z, b) & 0xFFFFFFu) * (1.0 / 16777216.0);
}

fn streakAlive(h0 : vec4f, h1 : vec4f) -> bool { return h1.x > 0.0 && h0.w < h1.x; }


@compute @workgroup_size(64)
fn update(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i >= streakSlots()) { return; }
  let base = 1u + i * STREAK_STRIDE;
  var h0 = streaks[base];
  var h1 = streaks[base + 1u];
  let dt = R.streakB.w;
  if (dt <= 0.0) { return; }                  // paused, or an aux view's frame
  // Row 0: is the eye under a liquid? The draw hides the gust streaks then.
  if (i == 0u) {
    streaks[0] = vec4f(select(0.0, 1.0, streakCellLiquid(R.camPos)), 0.0, 0.0, 0.0);
  }
  let trail = clamp(R.streakN.y, 2u, STREAK_TRAIL_MAX);
  let radius = R.streakA.w;
  if (i >= R.streakN.x) {
    currentUpdate(i, base, h0, h1, dt, trail, radius);
    return;
  }

  if (!streakAlive(h0, h1)) {
    // ---- respawn attempt ----
    if (streakU01(1u, i) >= min(dt * STREAK_SPAWN_RATE, 1.0)) { return; }
    let ang = streakU01(2u, i) * 6.28318531;
    let rr = sqrt(streakU01(3u, i)) * radius;
    var p = vec3f(R.camPos.x + cos(ang) * rr, 0.0, R.camPos.z + sin(ang) * rr);
    let tr = windTerrF(p, &R);
    let ground = select(R.camPos.y - 17.0, tr.h, tr.edge > 0.0);
    // HEIGHT. Uniform from just over the ground to one radius over the eye,
    // clipped to one radius under it: standing, the air up to ~20 m over the
    // head; flying, a box round the eye that never goes underground.
    let lo = max(ground + STREAK_FLOOR, R.camPos.y - radius);
    let hi = max(R.camPos.y + radius, lo);
    p.y = lo + streakU01(4u, i) * (hi - lo);
    if (streakCellLiquid(p)) { return; }        // air, not water (header)
    // THE SPEED of the field the streak will be advected by: windAt is the
    // draft-sheltered ambient plus primitives plus heat, so a still room or
    // the lee of a wall spawns nothing instead of streaks that hang in place.
    let ws = length(windAt(p, R.time, &R));
    let pr = smoothstep(R.streakA.y, R.streakA.y + max(R.streakA.z, 1e-3), ws);
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
  var w = windAt(h0.xyz, R.time, &R);
  let p0 = h0.xyz;
  let a0 = h0.w;
  // ---- ...WITH THE GROUND AS A BOUNDARY (ticket 2026-10-09_15-09-01) ----
  // The field's mean is HORIZONTAL: nothing in windAt turns it up a slope.
  // The CA never notices -- a voxel blocks whatever pushes into it -- but a
  // tracer does: a streak born 0.2-6 m over a WINDWARD slope ran straight
  // into the coarse ground within metres and died, mostly while still fading
  // in, so a hill's windward face showed no streaks at all and its lee (where
  // the ground falls away from the same horizontal motion) showed them
  // running full length. Owner: "on one side of the hill there's no wind
  // trail, on the other it goes on way longer".
  //
  // Real air in the surface layer cannot pass through the ground, so it
  // follows it: the vertical speed AT the surface is U . grad(h) (the
  // no-penetration condition), fading with height on the scale of the hill.
  // That is applied here, to the tracer, from the same terrain table the
  // field's ramp reads -- up a windward face, down a lee one, the same law
  // both ways. The slope is clamped (a cliff cell would otherwise fling a
  // streak straight up), and a step that still ends under the table's ground
  // (its bilinear facets, a clamped slope) slides along it instead of
  // killing the streak -- but only by about one step's own rise: a step that
  // ends DEEP under the ground hit a wall (a cliff, a ridge the table steps
  // up onto between cells) and dies there as before, or the slide would
  // teleport it up the face and draw a 40 m ribbon in one frame.
  let ground = (R.streakN.w & 1u) != 0u;   // wind.streakGround
  let trA = windTerrF(p0, &R);
  if (ground && trA.edge > 0.0) {
    var g = vec2f(trA.gx, trA.gz);
    let gl = length(g);
    if (gl > STREAK_FOLLOW_MAX_SLOPE) { g *= STREAK_FOLLOW_MAX_SLOPE / gl; }
    let hagl = max(p0.y - trA.h, 0.0);
    let follow = trA.edge * (1.0 - min(hagl / STREAK_FOLLOW_DEPTH, 1.0));
    w.y += dot(w.xz, g) * follow;
  }
  var p = p0 + w * dt;
  let trB = windTerrF(p, &R);
  var hitWall = false;
  // Off: the bare field's old rule, into the table's ground = dead.
  if (!ground && trB.edge > 0.0 && p.y < trB.h) { hitWall = true; }
  if (ground && trB.edge > 0.0) {
    let under = trB.h + STREAK_FLOOR - p.y;
    if (under > STREAK_FLOOR + STREAK_FOLLOW_MAX_SLOPE * length(w.xz) * dt) { hitWall = true; }
    else if (under > 0.0) { p.y += under; }
  }
  // Into a liquid: done, as into the ground (header, WATER).
  if (streakCellLiquid(p)) { hitWall = true; }
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
  var dead = hitWall || dot(d, d) > radius * radius * 1.69;
  // ...or out of it vertically: the spawn volume's own reach.
  if (abs(p.y - R.camPos.y) > radius * 1.3) { dead = true; }
  if (dead) { h0.w = h1.x; }
  streaks[base] = h0;
  streaks[base + 1u] = vec4f(h1.x, h1.y, f32(head), timer);
}

// ---- one CURRENT TRACER slot (header, CURRENT TRACERS) ----
// The gust streak's update, re-cut for water: spawn under a surface instead of
// over the ground, advect by currentAt's horizontal part, die out of liquid.
// The trail push is the gust streak's loop, at CUR_SPACING_MUL its spacing.
fn currentUpdate(i : u32, base : u32, h0In : vec4f, h1In : vec4f, dt : f32,
                 trail : u32, radius : f32) {
  var h0 = h0In;
  var h1 = h1In;
  if (!streakAlive(h0, h1)) {
    // ---- respawn attempt ----
    if (R.currentRenderOn == 0u || R.currentPrimCount == 0u) { return; }
    if (streakU01(11u, i) >= min(dt * CUR_SPAWN_RATE, 1.0)) { return; }
    let ang = streakU01(12u, i) * 6.28318531;
    let rr = sqrt(streakU01(13u, i)) * radius;
    var p = vec3f(R.camPos.x + cos(ang) * rr, 0.0, R.camPos.z + sin(ang) * rr);
    let tr = windTerrF(p, &R);
    let ground = select(R.camPos.y, tr.h, tr.edge > 0.0);
    // A camera far above the water spawns nothing: the draw's vertical fade
    // would hide every tracer anyway.
    if (R.camPos.y - ground > 2.0 * radius + 62.0) { return; }
    // The water's surface in this column: down from over the table's ground
    // (or from just over a submerged eye) to the first cell that is neither
    // air nor gas. Liquid = found; anything else = dry here.
    let top = i32(floor(max(ground, min(R.camPos.y, ground + 62.0)))) + CUR_SCAN_ABOVE;
    var c = vec3<i32>(i32(floor(p.x)), top, i32(floor(p.z)));
    if (!chunkInWindow(worldChunkOf(c), ptOrigin()) ||
        !chunkInWindow(worldChunkOf(c - vec3<i32>(0, CUR_SCAN_CELLS, 0)), ptOrigin())) {
      return;
    }
    var surf = -1e9;
    for (var k = 0; k < CUR_SCAN_CELLS; k++) {
      let mat = voxMat(voxWordAt(c));
      if (mat != MAT_AIR) {
        let kl = materials[mat].klass;
        if (kl == CLASS_LIQUID) { surf = f32(c.y) + 1.0; break; }
        if (kl != CLASS_GAS) { break; }
      }
      c.y -= 1;
    }
    if (surf < -1e8) { return; }
    let dq = streakU01(14u, i);
    p.y = surf - CUR_DEPTH_MIN - dq * dq * CUR_DEPTH_SPAN;
    if (!streakCellLiquid(p)) { return; }       // shallower than that: skip
    let v = currentAt(p, &R);
    let pr = smoothstep(CUR_SPAWN_LO, CUR_SPAWN_LO + CUR_SPAWN_SPAN, length(v.xz));
    if (streakU01(15u, i) >= pr) { return; }
    h0 = vec4f(p, 0.0);
    h1 = vec4f(CUR_LIFE * (0.7 + 0.6 * streakU01(16u, i)), pr, 0.0, 0.0);
    for (var k = 0u; k < STREAK_TRAIL_MAX; k++) {
      streaks[base + 2u + k] = vec4f(p, 0.0);
    }
    streaks[base] = h0;
    streaks[base + 1u] = h1;
    return;
  }

  // ---- carried along the surface by THE field's horizontal part ----
  // The depth is held: the vertical term is a vortex's rise, which would sink
  // or surface a tracer that is drawing the SURFACE flow.
  let w = currentAt(h0.xyz, &R);
  let p0 = h0.xyz;
  let a0 = h0.w;
  let p = p0 + vec3f(w.x, 0.0, w.z) * dt;
  var head = u32(h1.z) % trail;
  let spacing = max(R.streakB.y * CUR_SPACING_MUL, 1e-3);
  let total = h1.w + dt;
  let cnt = u32(floor(total / spacing));
  for (var k = select(0u, cnt - trail, cnt > trail); k < cnt; k++) {
    let ago = total - spacing * f32(k + 1u);
    let f = clamp(1.0 - ago / dt, 0.0, 1.0);
    head = (head + 1u) % trail;
    streaks[base + 2u + head] = vec4f(mix(p0, p, f), a0 + dt * f);
  }
  let timer = min(total - spacing * f32(cnt), spacing);
  h0 = vec4f(p, a0 + dt);
  // Out of the water (a bank, the surface dropping under it) or out of the
  // pool's reach: done.
  let d = p.xz - R.camPos.xz;
  if (!streakCellLiquid(p) || dot(d, d) > radius * radius * 1.69) { h0.w = h1.x; }
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
  if (ii >= streakSlots()) { return out; }
  let isCur = ii >= R.streakN.x;
  // A submerged eye sees no gust streaks (header, WATER).
  if (!isCur && streaksR[0].x > 0.5) { return out; }
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
  let want = R.streakB.z * select(1.0, CUR_WIDTH_MUL, isCur) * (1.0 - 0.7 * kf);
  let width = max(want, pxWorld * 0.6);
  let cover = want / width;
  out.pos = projectView(rel + perp * (side * width), R);
  let lf = h0.w / h1.x;
  let life = smoothstep(0.0, 0.15, lf) * (1.0 - smoothstep(0.55, 1.0, lf));
  // Gone within a metre of the eye, full by four: a streak that close is a
  // slab across the screen, not a wisp.
  let near = smoothstep(10.0, 40.0, dist);
  // The pool's reach in 3-D: a streak far above or below the eye fades like
  // one far off to the side (the spawn volume is a radius tall either way).
  let far = 1.0 - smoothstep(R.streakA.w * 0.6, R.streakA.w, dist);
  // The one air law (common.wgsl airFogTauS): the height fog plus the
  // loading fog, along the ray from the eye to the streak.
  let fog = exp(-airFogTauS(R.camPos.y, R.hfogDensity, R.hfogFalloff, R.hfogBaseY,
                            R.fogDensity, dist * VOXEL_METERS,
                            (q.y - R.camPos.y) / max(dist, 1e-3)));
  // Tapered at BOTH ends: fading in over the first segment behind the head
  // and out toward the tail. A full-alpha head was a blunt cap, which reads
  // as a comet (a thing) rather than a wisp (moving air).
  let shape = min(kf * f32(trail - 1u), 1.0) * (1.0 - kf);
  let master = select(R.streakA.x, curAlpha(), isCur);
  out.alpha = clamp(master * h1.y * shape * cover * life * near * far * fog, 0.0, 1.0);
  // The same translucent white day and night (owner call, 2026-09-30). It was
  // dimmed with the light (down to 0.08 at night), and a dark colour blended
  // at the streak's alpha reads as a BLACK line over a moonlit scene, not as
  // faint air. Visibility is the alpha's job; the colour stays constant.
  out.color = vec3f(0.92, 0.95, 1.0);
  // A current tracer is light on water: a cooler white, so where a river
  // runs under the wind the two fields read as two things.
  if (isCur) { out.color = vec3f(0.80, 0.94, 1.0); }
  return out;
}

@fragment
fn fsStreak(in : StreakOut) -> @location(0) vec4f {
  return vec4f(in.color, in.alpha);
}
