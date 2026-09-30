// rain_map.wgsl — THE RAIN SHADOW MAP (2026-09-30; DESIGN.md §9.w "Where the
// rain lands"). Two per-frame compute entries on the shadow layout
// (pass_table.def rain_map_prep / rain_map_build, the ShadowCache table), read
// by raymarch.wgsl's rain overlay (which drops are drawn) and near-field wet
// shading (which surfaces darken) through rainMapHeight().
//
// ---- WHAT PROBLEM THIS SOLVES ----------------------------------------------
// Rain used to be gated on the OPENNESS grid, which answers a different
// question: how much of a face's hemisphere sees the sky, 5 rays with the up
// ray weighted 2/6. That is wrong three ways for rain:
//   * it has no opinion in empty 4^3 blocks (only blocks holding a blocker
//     are measured), so any drop more than ~40 cm from a surface read
//     "unknown = open" and fell through the middle of every room;
//   * a roof with open sides still reads ~0.67 open, which the drop gate
//     turned into 93% visible;
//   * it ignores which way the rain is FALLING, so a windward doorway and a
//     lee one were the same doorway.
// Precipitation is a directional light: it arrives along one direction
// (fallDir, the lean rainOverlay already draws with), and "is this point wet"
// is "is this point the first thing on its fall line". So this is a SHADOW
// MAP along fallDir, built the way a shadow map is.
//
// ---- THE MAP ------------------------------------------------------------------
// A 2-D grid in the SHEARED horizontal coordinate
//     key = xz + s * y          s = wind / fallV  (drift per unit fallen)
// in world VOXELS. Every point of one fall line has the same key (a drop at
// height y sits at xz = key - s*y, which is rainColumns' `xz = qd - wind *
// (hc / fallV)` with the eye's key folded in), so a fall line is one texel.
// Each texel holds the world voxel y of the TOP of the first ray blocker the
// line meets coming down (RMAP_OPEN when it meets none in the window). A point
// at height y is under cover iff y < height(key(point)).
//
// RMAP_N^2 texels of one voxel each (51.2 m at 10 cm), TOROIDAL: texel =
// key & RMAP_MASK, covering keys [centre - N/2, centre + N/2) round the eye's
// own key. Moving re-centres by changing which key each texel stands for, and
// only texels whose key CHANGED are re-marched — a strip, not the map.
//
// ---- VALIDITY (tags, not trust) -----------------------------------------------
// Each texel carries a TAG: the low 12 bits of its key's x and z and the
// map's GENERATION. The reader recomputes the tag it expects and treats a
// mismatch as "no opinion" (the caller decides what that means; both callers
// say "open", which is the pre-map look and never hides rain that is there).
// The generation bumps when the lean moves by more than RMAP_REBUILD_COS
// (~2 degrees): every texel's fall line changed, so every tag goes stale at
// once and this frame re-marches the whole map.
//
// ---- THE BUILD ---------------------------------------------------------------
// One thread per texel. A texel is marched this frame if its tag is stale
// (new key after a re-centre, new generation after a lean change, never
// written) or its row is in the ROLLING band (RMAP_ROLL_ROWS a frame, so the
// whole map is re-marched every RMAP_N / RMAP_ROLL_ROWS frames and an edit —
// a roof dug through, a wall built — reaches the rain within ~1 s). Everything
// else returns after one load.
//
// THE MARCH IS traceOpaque, NOT A NEW DDA — sim_openness.wgsl's header says why
// (one media-blind opaque DDA shared by every secondary ray). Two calls: a
// COARSE one from the sky, which steps 4^3 blocks over the blocker mask and
// stops at the first block holding any blocker, then a FINE one from just
// before that block to find the voxel inside it. A coarse hit is only ever
// HIGHER than the true one (a set bit may stop the ray early, never late), so
// if the fine march runs out of steps (the line slipped past the block's
// blockers into a long fine stretch) the coarse height stands: the answer errs
// DRY, i.e. toward today's behaviour under a roof.
//
// The ray starts at the SKY BOUND (sky_top.wgsl: the highest occupied level-1
// cascade cell, recomputed every frame, conservative-high) plus a two-chunk
// margin, capped at the window top. The margin is for what the cascade's
// centre sampling can miss: a level-1 cell is ONE sample of a 2^3 region, so a
// one-voxel roof above everything else in a kilometre may not be in the bound.
// Leaving the window sideways or reaching its floor without a hit = open.
//
// Render-only derived data: nothing is hashed or saved, and the sim never
// binds this buffer (the sim's own exposure map is sim_rain_expo.wgsl, integer
// and on the tick stream — this one is float, per frame and camera-centred).

// `> voxels` with EXACTLY ONE SPACE: resources.cpp BodyAddressesVoxels keeps
// the page-table block (and traceOpaque in it) only for bodies containing it.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(12) var<uniform> C : CloudParams;
// read-only here: cloud.wgsl's `weather` / `env` write it (the probe words).
@group(0) @binding(15) var<storage, read> cloudMaps : array<u32>;
@group(0) @binding(19) var<storage, read> farOcc : array<u32>;
@group(0) @binding(22) var<storage, read_write> rainMap : array<u32>;

// ---- layout (raymarch.wgsl rainMapHeight reads the same) ----------------------
// [0] frame stamp (C.frame of the prep that wrote it)   [1] flags (1 = live)
// [2,3] slope s (f32 bits)   [4] generation (1..255)   [5,6] centre key (i32)
// [7] rolling cursor   [8] this frame's ray start height (f32 voxels): no
// blocker is above it, so a reader skips the texel for anything higher
// [9..15] unused
// then RMAP_N^2 x 2 words: (height i32, tag).
const RMAP_N : u32 = 512u;                  // raymarch.wgsl agrees
const RMAP_MASK : i32 = 511;                // raymarch.wgsl agrees
const RMAP_HEADER : u32 = 16u;              // raymarch.wgsl agrees
const RMAP_OPEN : i32 = -2147483647 - 1;    // raymarch.wgsl agrees
const RMAP_ROLL_ROWS : u32 = 8u;
// cos(2 degrees): the lean may drift this far before the map is rebuilt.
const RMAP_REBUILD_COS : f32 = 0.99939083;
// The fine refinement: start this many voxels before the coarse block, spend
// at most this many DDA steps.
const RMAP_FINE_BACK : f32 = 6.0;
const RMAP_FINE_STEPS : i32 = 48;
const RMAP_COARSE_STEPS : i32 = 1024;
const SKY_TOP_BASE : u32 = FAR_LEVELS * FAR_NUM_CHUNKS;   // sky_top.wgsl agrees
const SKY_TOP_BIAS : i32 = 1 << 24;                       // sky_top.wgsl agrees

// The texel's tag. raymarch.wgsl carries a byte-identical copy.
fn rainMapTag(key : vec2<i32>, gen : u32) -> u32 {
  return (u32(key.x) & 0xFFFu) | ((u32(key.y) & 0xFFFu) << 12u) | ((gen & 0xFFu) << 24u);
}

// THE LEAN: the wind a drop drifts with, m/s, from the 20 m averaged camera
// wind (probe words 4..6, cloud.wgsl writeRainWindProbe) — the preset's share
// of it, capped at its lean. raymarch.wgsl rainLeanWind is the same function
// and must stay so: the map is built along the fall the streaks are drawn at.
fn rainLeanWindHere(fallV : f32) -> vec2f {
  let wField = vec2f(bitcast<f32>(cloudMaps[CLOUD_PROBE_BASE + 4u]),
                     bitcast<f32>(cloudMaps[CLOUD_PROBE_BASE + 6u]));
  let wLean = wField * C.rainWindShare;
  let wLen = length(wLean);
  let wMax = fallV * C.rainLeanTan;
  return wLean * select(1.0, wMax / max(wLen, 1e-4), wLen > wMax);
}

// ---- prep: one thread decides this frame's slope, generation and centre ----
@compute @workgroup_size(1)
fn rainMapPrep() {
  let snow = clamp(C.precipType, 0.0, 1.0);
  let fallV = mix(8.5, 1.1, snow);
  let sNew = rainLeanWindHere(fallV) / fallV;
  var s = vec2f(bitcast<f32>(rainMap[2]), bitcast<f32>(rainMap[3]));
  var gen = rainMap[4];
  let live = rainMap[1] == 1u && gen != 0u;
  let fNew = normalize(vec3f(sNew.x, -1.0, sNew.y));
  let fOld = normalize(vec3f(s.x, -1.0, s.y));
  if (!live || dot(fNew, fOld) < RMAP_REBUILD_COS) {
    s = sNew;
    gen = (gen % 255u) + 1u;   // 1..255: a zeroed texel's tag never matches
  }
  let key = vec2<i32>(floor(R.camPos.xz + s * R.camPos.y));
  rainMap[0] = C.frame;
  rainMap[1] = 1u;
  rainMap[2] = bitcast<u32>(s.x);
  rainMap[3] = bitcast<u32>(s.y);
  rainMap[4] = gen;
  rainMap[5] = bitcast<u32>(key.x);
  rainMap[6] = bitcast<u32>(key.y);
  rainMap[7] = (rainMap[7] + RMAP_ROLL_ROWS) & u32(RMAP_MASK);
  rainMap[8] = bitcast<u32>(rainMapStartY());
}

// The highest y a ray needs to start from: the level-1 sky bound plus the
// margin, never above the window.
fn rainMapStartY() -> f32 {
  let top = f32(R.origin.y * i32(CHUNK) + i32(WORLD_N));
  let w = farOcc[SKY_TOP_BASE];
  if (w == 0u) { return top; }
  let cellVox = f32(1u << farCellShift(1u));
  let ceilVox = f32(i32(w) - SKY_TOP_BIAS) * cellVox;
  return min(top, ceilVox + 2.0 * f32(CHUNK));
}

// ---- build: one thread per texel, stale or rolling texels march ----
@compute @workgroup_size(8, 8)
fn rainMapBuild(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= RMAP_N || gid.y >= RMAP_N) { return; }
  let centre = vec2<i32>(bitcast<i32>(rainMap[5]), bitcast<i32>(rainMap[6]));
  let gen = rainMap[4];
  // The one key in [centre - N/2, centre + N/2) this texel stands for.
  let lo = centre - vec2<i32>(i32(RMAP_N / 2u));
  let key = lo + ((vec2<i32>(gid.xy) - lo) & vec2<i32>(RMAP_MASK));
  let at = RMAP_HEADER + (gid.y * RMAP_N + gid.x) * 2u;
  let want = rainMapTag(key, gen);
  let rolling = ((gid.y - rainMap[7]) & u32(RMAP_MASK)) < RMAP_ROLL_ROWS;
  if (rainMap[at + 1u] == want && !rolling) { return; }

  let s = vec2f(bitcast<f32>(rainMap[2]), bitcast<f32>(rainMap[3]));
  // The fall line through the texel's centre: xz = key + 0.5 - s * y, going
  // DOWN (y decreasing moves xz by +s per voxel), i.e. along fallDir.
  let y0 = rainMapStartY();
  let kc = vec2f(key) + 0.5;
  let ro = vec3f(kc.x - s.x * y0, y0, kc.y - s.y * y0);
  let rd = normalize(vec3f(s.x, -1.0, s.y));
  var height = RMAP_OPEN;
  let hc = traceOpaque(ro, rd, RMAP_COARSE_STEPS, 0.0, &occupancy, &materials);
  if (hc.hit) {
    height = hc.cell.y + 1;
    let t0 = max(hc.t - RMAP_FINE_BACK, 0.0);
    let hf = traceOpaque(ro + rd * t0, rd, RMAP_FINE_STEPS, 1e30, &occupancy, &materials);
    // A fine hit BELOW the coarse block's entry is the voxel the block held;
    // one above it cannot happen (the coarse march saw no blocker there), so
    // min() only guards float noise at the block face.
    if (hf.hit) { height = min(height, hf.cell.y + 1); }
  }
  rainMap[at] = bitcast<u32>(height);
  rainMap[at + 1u] = want;
}
