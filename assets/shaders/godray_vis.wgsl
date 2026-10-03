// godray_vis.wgsl — THE GOD-RAY SUN VISIBILITY VOLUME (2026-10-03,
// raymarch-shadow-water). One per-frame compute entry on the shadow layout
// (pass_table.def godray_vis, the ShadowCache table), read by raymarch.wgsl's
// godRays() through godVisAt().
//
// ---- WHAT IT REPLACES ---------------------------------------------------------
// Under water every pixel marches TUNE_GODRAY_STEPS samples along its view ray
// and asks, at each, whether the sun reaches it. Since Lin follow-ups T5.2
// that question is one COARSE occlusion ray (traceOpaque over the 4^3 blocker
// mask, coarse from its first step) per 4^3 BLOCK a pixel's samples visit —
// still ~7 rays a pixel, and measured with --render-budget's `godshadow0` arm
// (submerged camera, harness lake, 1080p) they were 6.3 ms of a 20.4 ms frame:
// the largest single term in the submerged view. Adjacent pixels' samples sit
// in the SAME blocks and cast the same rays toward the same sun, two million
// times over for what is a few hundred thousand distinct blocks.
//
// So the answer is computed ONCE PER BLOCK, here, for the blocks round the
// eye, and godRays reads a word. It is the same question asked the same way —
// one coarse traceOpaque toward the sun with the same block budget — from the
// block's CENTRE instead of from whichever sample of the pixel first entered
// it. Because that march is coarse from its first step (it tests 4^3 blocks of
// the mask, never a voxel), the start point inside the block changes only
// which neighbouring blocks a grazing ray clips, and the answer was already
// shared by every sample of a pixel in that block. What it adds over the
// per-pixel ray is that neighbouring pixels can no longer disagree about one
// block, i.e. the shaft edges no longer carry a per-pixel start-point dither.
//
// ---- THE VOLUME ---------------------------------------------------------------
// GV_NX x GV_NY x GV_NZ blocks, centred on the eye's block, recomputed every
// frame the eye is in a liquid (camera-relative, so there is nothing to
// invalidate). It covers TUNE_GODRAY_RANGE (14 m = 35 blocks) in XZ either
// side of the eye and +-8 m vertically; a sample outside it falls back to the
// per-pixel ray, so the volume is a cache and never a clip.
//
// [0]      stamp: R.frameIdx | GV_VALID when this frame's volume was built,
//          0 when the eye was not in a liquid (raymarch.wgsl falls back)
// [1..]    one word per block: GV_LIT, GV_DARK (0 is never written for a
//          block inside the volume on a built frame)
//
// Cost: GV_N threads, one coarse ray of at most TUNE_GODRAY_SHADOW_STEPS
// blocks each, all toward the same sun from adjacent blocks — the coherent
// version of the rays the fragment shader cast incoherently. When the eye is
// dry every thread returns after one voxel read.
//
// Render-only derived data: never hashed, never saved, no sim binding.

// `> voxels` with EXACTLY ONE SPACE: resources.cpp BodyAddressesVoxels keeps
// the page-table block (and traceOpaque in it) only for bodies containing it.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(27) var<storage, read_write> godVis : array<u32>;

// ---- layout (raymarch.wgsl godVisAt reads the same; world-independent) -------
const GV_NX : i32 = 72;                     // raymarch.wgsl agrees
const GV_NY : i32 = 40;                     // raymarch.wgsl agrees
const GV_NZ : i32 = 72;                     // raymarch.wgsl agrees
const GV_HEADER : u32 = 1u;                 // raymarch.wgsl agrees
const GV_VALID : u32 = 0x80000000u;         // raymarch.wgsl agrees
const GV_LIT : u32 = 1u;                    // raymarch.wgsl agrees
const GV_DARK : u32 = 2u;                   // raymarch.wgsl agrees

// The volume's lowest block, from the eye. raymarch.wgsl godVisOrigin is the
// same expression on the same uniform, so both sides index one grid.
fn godVisOrigin() -> vec3<i32> {
  let eb = vec3<i32>(floor(R.camPos)) >> vec3<u32>(SUBOCC_SHIFT);
  return eb - vec3<i32>(GV_NX / 2, GV_NY / 2, GV_NZ / 2);
}

// Is the eye in a liquid this frame? The fragment side's own test is its
// primary ray's (shadeSubmerged), which this cannot see; the stamp below is
// what makes a disagreement harmless — a frame this skipped reads 0 there and
// the fragment shader casts its own rays, exactly as before this existed.
fn godVisEyeWet() -> bool {
  let c = vec3<i32>(floor(R.camPos));
  if (!inWindow(c, R.origin)) { return false; }
  let m = voxMat(voxWordAt(c));
  if (m == MAT_AIR) { return false; }
  return materials[m].klass == CLASS_LIQUID;
}

@compute @workgroup_size(64)
fn godrayVis(@builtin(global_invocation_id) gid : vec3<u32>) {
  let n = u32(GV_NX * GV_NY * GV_NZ);
  let i = gid.x;
  if (i >= n) { return; }
  let kd = keyLightDirP(R);
  // godRays' own early-outs, so a frame that casts no shaft builds nothing.
  let live = TUNE_GODRAY_STEPS > 0 && kd.y >= 0.08 && godVisEyeWet();
  if (i == 0u) { godVis[0] = select(0u, (R.frameIdx & 0x7FFFFFFFu) | GV_VALID, live); }
  if (!live) { return; }
  let ii = i32(i);
  let lb = vec3<i32>(ii % GV_NX, (ii / GV_NX) % GV_NY, ii / (GV_NX * GV_NY));
  let blk = godVisOrigin() + lb;
  let p = vec3f(blk * i32(SUBOCC_BLOCK)) + vec3f(f32(SUBOCC_BLOCK) * 0.5);
  var vis = GV_LIT;
  if (inWindow(vec3<i32>(floor(p)), R.origin)) {
    let s = traceOpaque(p, kd, TUNE_GODRAY_SHADOW_STEPS, 0.0, &occupancy, &materials);
    vis = select(GV_LIT, GV_DARK, s.hit);
  }
  godVis[GV_HEADER + i] = vis;
}
