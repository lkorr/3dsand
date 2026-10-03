// gi_gather.wgsl — THE GI GATHER CACHE'S REFRESH PASS (2026-10-03,
// raymarch-gi-grass). Two per-frame compute entries on the shadow layout
// (pass_table.def gi_prepare / gi_gather, the ShadowCache table); the fragment
// shader (raymarch.wgsl giBounceAt) only READS the cache and REQUESTS faces.
//
// ---- WHAT IT REPLACES ---------------------------------------------------------
// The one-bounce gather (nine coarse traceOpaque rays per block-face, see
// giGatherRays below) is cached per block-face in irradiance's second plane
// (common.wgsl GI_CACHE_BASE). Until this pass existed the REFRESH ran inside
// raymarch.wgsl's fragment shader: on a slot's scheduled frame, or for a word
// that read 0, an elected pixel cast the nine rays and wrote the word. Round 1
// of the raymarch perf work (b2e5883) measured what was left of that after the
// election: 0.2-0.3 ms of rays in the warps that still gathered, plus ~0.1 ms
// of register footprint for every pixel of every frame (the nine inlined
// traceOpaque loops are part of the fragment shader's 128-register budget
// whether a pixel gathers or not). The gather is a function of the FACE, not
// of the pixel, so it belongs in a compute pass over the faces that are due —
// work that scales with the faces on screen that need it, not with the screen.
//
// ---- THE PROTOCOL (one frame of latency, the shadow cache's shape) -----------
//   fs, frame f:   a hit on a block-face whose cache word is not FRESH (0 =
//                  never gathered, or the low bit clear = stale since the
//                  openness walk's last full visit) or whose slot is due this
//                  frame (render.giCachePeriod, slots staggered exactly as
//                  before) sets the face's bit in the request BITMAP; the lane
//                  that flips the bit appends one packed record to the LIST.
//                  The pixel shades with whatever the word holds now.
//   gi_prepare, f+1: moves the count into the saved slot, reopens the list,
//                  writes the indirect args.
//   gi_gather,  f+1: nine lanes per record: release its bit, cast the nine rays
//                  from the face centre and write the word FRESH (low bit on).
//                  BEFORE shadow_resolve: see pass_table.def for why the
//                  order decides how much multi-bounce light it sees.
//   fs, frame f+1: reads it.
// So a due face refreshes one frame after the frame that saw it was due — the
// cadence (once per period per visible face) is unchanged, it is shifted by a
// frame. What DID change, for the better: every visible face of a due slot now
// refreshes, where the fragment-shader election refreshed the faces whose centre
// pixel (or a 1/32 lottery winner) happened to be on screen.
//
// THE ONE PLACE THE PICTURE CAN DIFFER: a face seen for the first time. The
// fragment shader used to gather it inline on that frame; now it shades from
// the bilinear neighbours (a never-gathered tap drops out of the filter) for
// one frame and is filled before the next. The openness walk no longer ZEROES
// the word on a full visit (that was the common case — every dirty chunk), it
// clears the low bit, so a re-walked face keeps its previous answer for the one
// frame it waits; only a face with no previous answer at all (a slot the window
// just reused, a block that just grew a surface) has a one-frame gap.
//
// ---- THE LIST ------------------------------------------------------------------
// giReq (Buf::GiReq; renderBGL_ 39 fragment, shadowBGL_ 28 compute):
//   [0] atomic append count (fs)        [1] saved count (prepare -> gather)
//   [2] raw count last frame (stats)    [3] refused past the cap (stats)
//   [4..6] indirect args (prepare writes, copy_giArgs moves to Buf::GiArgs)
//   [7] pad
//   [GI_REQ_HEADER .. + GI_REQ_CAP)     one record per requested face:
//                                       shadowPackCell(block min cell, face)
//   [GI_REQ_BITS ..)                    one bit per irradiance-plane word:
//                                       "this face is already in the list"
// A face is listed at most once per frame (the bitmap), a refused face (list
// full) clears its own bit and asks again next frame. The cell is packed
// TOROIDAL, as the shadow request is, so a record means the same block-face
// whichever window the gather unwraps it into; the stamp test at the gather is
// what drops a record whose slot the window has since handed to another chunk.
//
// Render-only derived data: never hashed, never saved, no sim binding.

// `> voxels` with EXACTLY ONE SPACE: resources.cpp BodyAddressesVoxels keeps
// the page-table block (and traceOpaque in it) only for bodies containing it.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(8) var<storage, read_write> irradiance : array<u32>;
@group(0) @binding(9) var<storage, read> opennessGen : array<u32>;
@group(0) @binding(28) var<storage, read_write> giReq : array<atomic<u32>>;

// ---- layout (raymarch.wgsl agrees; pass_table.h kGiReq*, check_invariants) ----
const GI_REQ_HEADER : u32 = 8u;             // raymarch.wgsl agrees
const GI_REQ_CAP : u32 = 131072u;           // raymarch.wgsl agrees
const GI_REQ_BITS : u32 = GI_REQ_HEADER + GI_REQ_CAP;   // raymarch.wgsl agrees

// ---- the gather (moved here from raymarch.wgsl giGatherRays, unchanged) -------
// The full design notes — the quadrature weights, the nine directions, which
// face of the emitter, the receiver's own block, the step budget — stay with
// the cache's reader in raymarch.wgsl (see "one-bounce gather over the
// irradiance grid" there). The function is here, and ONLY here, so there is
// one gather and nothing can make two copies disagree.
const GI_W_NORMAL : f32 = 0.25;
const GI_W_RING : f32 = 0.09375;
// Ray `r` of the nine (r = 0 the normal; 1..4 the tangent diagonals; 5..8 the
// corners): its weighted contribution, zero on a miss.
fn giGatherRay(ro : vec3f, n : vec3f, face : u32, r : u32) -> vec3f {
  let axis = i32(face >> 1u);
  let t0 = vec3f(select(0.0, 1.0, axis == 1), select(0.0, 1.0, axis == 2),
                 select(0.0, 1.0, axis == 0));
  let t1 = vec3f(select(0.0, 1.0, axis == 2), select(0.0, 1.0, axis == 0),
                 select(0.0, 1.0, axis == 1));
  let su = select(select(0.0, 1.0, r == 1u || r == 5u || r == 6u), -1.0,
                  r == 2u || r == 7u || r == 8u);
  let sv = select(select(0.0, 1.0, r == 3u || r == 5u || r == 7u), -1.0,
                  r == 4u || r == 6u || r == 8u);
  let d = normalize(n + t0 * su + t1 * sv);
  let w = select(GI_W_RING, GI_W_NORMAL, r == 0u);
  // Coarse from the first step: block steps over the blockers mask, the same
  // march the openness pass and the shadow rays use, so a gather ray can
  // never see through a wall the shadow ray cannot.
  let s = traceOpaque(ro, d, TUNE_GI_GATHER_BLOCKS + 1, 0.0, &occupancy, &materials);
  if (!s.hit) { return vec3f(0.0); }
  let slot = chunkIndexW(s.cell);
  // An emitter's light is only meaningful under a matching stamp.
  if (opennessGen[slot] != opennessStamp(worldChunkOf(s.cell))) { return vec3f(0.0); }
  let lo = vec3<u32>(s.cell & vec3<i32>(i32(CHUNK) - 1));
  let base = irrIndex(slot, subOccBitLocal(lo), 0u);
  // EVERY FACE THE RAY LEANS ON, weighted by the direction's share on that
  // axis (raymarch.wgsl's notes say why the entry face alone gathers nothing
  // from a floor).
  let ax = abs(d.x);
  let ay = abs(d.y);
  let az = abs(d.z);
  var e = vec3f(0.0);
  if (ax > 0.1) { e += unpackRgb9e5(irradiance[base + 0u + select(0u, 1u, d.x < 0.0)]) * ax; }
  if (ay > 0.1) { e += unpackRgb9e5(irradiance[base + 2u + select(0u, 1u, d.y < 0.0)]) * ay; }
  if (az > 0.1) { e += unpackRgb9e5(irradiance[base + 4u + select(0u, 1u, d.z < 0.0)]) * az; }
  return e * (w / (ax + ay + az));
}

// face = axis * 2 + (normal sign > 0), common.wgsl openFaceOfNormal's encoding.
fn giFaceNormal(face : u32) -> vec3f {
  let axis = face >> 1u;
  let s = select(-1.0, 1.0, (face & 1u) != 0u);
  return vec3f(select(0.0, s, axis == 0u), select(0.0, s, axis == 1u),
               select(0.0, s, axis == 2u));
}

// --------------------------------------------------------------- passes ----

// NINE LANES PER FACE, not one. The list is short (a few hundred faces on the
// budget cameras: a sixteenth of what is on screen, plus what just went stale)
// and each ray is a chain of dependent block reads, so one thread per face was
// a handful of warps each walking nine rays one after another — a pass bound by
// LATENCY with the GPU nearly idle: measured 0.06-0.09 ms of `pre` for 100-500
// faces, which ate a third of what moving the gather out of the fragment
// shader saved. Nine lanes walk the nine rays side by side (0.00-0.01 ms over
// the fragment-shader version's `pre`), and lane 0 sums them IN RAY ORDER from
// zero — the serial loop's arithmetic exactly (a miss adds 0.0), so the word is
// bit-identical to a one-thread-per-face gather.
const GI_FACES_PER_GROUP : u32 = 7u;        // 7 x 9 = 63 lanes of 64
var<workgroup> giPart : array<vec3f, 64>;

// gi_prepare: last frame's request count -> a dispatch size; reopen the list
// for this frame's fragment shader. The shadow cache's prepare, for this list.
@compute @workgroup_size(1)
fn giPrepare() {
  let raw = atomicLoad(&giReq[0]);
  let n = min(raw, GI_REQ_CAP);
  atomicStore(&giReq[1], n);
  atomicStore(&giReq[2], raw);
  atomicStore(&giReq[3], select(0u, raw - GI_REQ_CAP, raw > GI_REQ_CAP));
  atomicStore(&giReq[0], 0u);
  atomicStore(&giReq[4], (n + GI_FACES_PER_GROUP - 1u) / GI_FACES_PER_GROUP);
  atomicStore(&giReq[5], 1u);
  atomicStore(&giReq[6], 1u);
}

// gi_gather: GI_FACES_PER_GROUP requested block-faces per workgroup, nine lanes
// each. The origin is the face centre half a voxel past the block's far plane
// along the normal (the origin the fragment shader's refresh used, and
// sim_openness.wgsl's rays use).
@compute @workgroup_size(64)
fn giGather(@builtin(local_invocation_index) lid : u32,
            @builtin(workgroup_id) wid : vec3<u32>) {
  let n = atomicLoad(&giReq[1]);
  let slotInGroup = lid / 9u;
  let r = lid - slotInGroup * 9u;
  let i = wid.x * GI_FACES_PER_GROUP + slotInGroup;
  var blockMin = vec3<i32>(0);
  var face = 0u;
  var idx = 0xFFFFFFFFu;
  if (slotInGroup < GI_FACES_PER_GROUP && i < n) {
    // Every lane of the face decodes the same record; lane 0 releases the
    // face's dedup bit (whatever happens below, the next frame may ask again;
    // the bit index is the plane index, so it is window-independent).
    let packed = atomicLoad(&giReq[GI_REQ_HEADER + i]);
    face = (packed >> (WORLD_SHIFT * 3u)) & 7u;
    blockMin = shadowUnwrapCell(packed, R.origin * i32(CHUNK));
    let slot = chunkIndexW(blockMin);
    let lo = vec3<u32>(blockMin & vec3<i32>(i32(CHUNK) - 1));
    let pidx = irrIndex(slot, subOccBitLocal(lo), face);
    if (r == 0u) {
      atomicAnd(&giReq[GI_REQ_BITS + (pidx >> 5u)], ~(1u << (pidx & 31u)));
    }
    // A slot the window handed to another chunk since the request, or one the
    // walk has not stamped: there is no cache here to fill.
    if (opennessGen[slot] == opennessStamp(worldChunkOf(blockMin))) { idx = pidx; }
  }
  var c = vec3f(0.0);
  if (idx != 0xFFFFFFFFu) {
    let nrm = giFaceNormal(face);
    let half = f32(SUBOCC_BLOCK) * 0.5;
    c = giGatherRay(vec3f(blockMin) + vec3f(half) + nrm * (half + 0.5), nrm, face, r);
  }
  giPart[lid] = c;
  workgroupBarrier();
  if (idx != 0xFFFFFFFFu && r == 0u) {
    var acc = vec3f(0.0);
    for (var k = 0u; k < 9u; k++) { acc += giPart[lid + k]; }
    // The low bit forced on: 0 is "never gathered", a clear low bit is
    // "stale", and a face in the dark gathers a true zero that must read as
    // FRESH.
    irradiance[GI_CACHE_BASE + idx] = packRgb9e5(acc) | 1u;
  }
}
