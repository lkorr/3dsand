// sky_top.wgsl — THE FAR CASCADE'S PER-LEVEL SKY BOUND (2026-09-28).
// Two per-frame compute entries on the shadow layout (pass_table.def
// sky_top_clear / sky_top_reduce, the ShadowCache table). Split out of
// shadow_resolve.wgsl when that pass started reading farOcc read-only.

// ============================ THE SKY BOUND =================================
// (2026-09-28, raymarch perf.) One word per far level at farOcc's tail: the
// highest occupied LEVEL CELL in that level's whole box, biased so that 0 means
// "the level holds nothing". raymarch.wgsl's traceFar and farShadowDist clip
// every ray to the slab below it: a sky ray used to enter all FAR_LEVELS
// shells and chunk-walk each one (8..16 chunk probes a shell) to learn there
// was nothing up there — measured as the largest single piece of the far
// march on every camera with sky in it.
//
// CONSERVATIVE BY CONSTRUCTION. It is a max over EVERY slot, including slots a
// pending face still holds stale bytes in (farBox excludes those from the
// march, but they are counted here under their new absolute row), and the
// per-chunk top row it reads is itself conservative-high (farOccTop's note).
// Extra slots can only raise a max, so the bound is never below a cell the
// march could hit. Recomputed from scratch every frame, so it also comes back
// DOWN when the terrain that raised it streams out.
//
// Render-only derived data; the sim never reads farOcc.
// The shadow layout's far bindings (simulation.cpp shadowBGL_): 19 is farOcc,
// declared READ-ONLY by shadow_resolve.wgsl (its far shadow march) and
// read_write here, which is why this is its own module — one module cannot
// declare one binding twice. The layout entry is plain Storage for this.
@group(0) @binding(19) var<storage, read_write> farOcc : array<atomic<u32>>;
@group(0) @binding(20) var<uniform> F : FarParams;

const SKY_TOP_BASE : u32 = FAR_LEVELS * FAR_NUM_CHUNKS;   // raymarch.wgsl agrees
const SKY_TOP_BIAS : i32 = 1 << 24;                       // raymarch.wgsl agrees

// ---- THE SKY TILES (map v2 P1) ---------------------------------------------
// The same bound per TILE of SKY_TILE_CHUNKS x SKY_TILE_CHUNKS chunk columns,
// SKY_TILE_N x SKY_TILE_N tiles a level, after the 16 per-level words: on a
// map with a 1,400 m summit the per-level word is the summit for every ray in
// the level, and a ray over the sea or the foothills marched its chunks under
// a ceiling it could never meet. Keyed by SLOT column (slot x and z >> 2), so
// one word covers every world chunk column that maps into it -- within the
// level box that is one world tile, or the two partial tiles at the box's
// opposite faces when the origin is not tile-aligned, and the max covers both.
// Same encoding as the level word (0 = nothing; else the top + 1, biased).
// ray_start.wgsl's rayStartFar reads it (SKY_TILE_* agree there); world.cpp
// sizes farOcc's tail for it (kFarSkyTopWords).
const SKY_TILE_BASE : u32 = SKY_TOP_BASE + 16u;
const SKY_TILE_CHUNKS : u32 = 4u;
const SKY_TILE_N : u32 = FAR_NCHUNK / SKY_TILE_CHUNKS;   // 8
const SKY_TILE_WORDS : u32 = FAR_LEVELS * SKY_TILE_N * SKY_TILE_N;

@compute @workgroup_size(16)
fn skyTopClear(@builtin(local_invocation_index) li : u32) {
  for (var i = li; i < 16u + SKY_TILE_WORDS; i += 16u) {
    atomicStore(&farOcc[SKY_TOP_BASE + i], 0u);
  }
}

var<workgroup> skyWgMax : atomic<u32>;
// A workgroup's 64 slots are two x-rows (32 each) of one z (FAR_NUM_CHUNKS is
// a multiple of 64 and the order is (z * N + y) * N + x, so the pair shares z
// whenever N is even), i.e. SKY_TILE_N tiles: reduced here, one global
// atomicMax per tile.
var<workgroup> skyWgTile : array<atomic<u32>, SKY_TILE_N>;

@compute @workgroup_size(64)
fn skyTopReduce(@builtin(local_invocation_index) li : u32,
                @builtin(workgroup_id) wg : vec3<u32>) {
  if (li == 0u) { atomicStore(&skyWgMax, 0u); }
  if (li < SKY_TILE_N) { atomicStore(&skyWgTile[li], 0u); }
  workgroupBarrier();
  // FAR_NUM_CHUNKS is a multiple of 64 (pass_table.cpp asserts it), so a
  // workgroup's 64 words all belong to one level.
  let idx = wg.x * 64u + li;
  let level0 = idx / FAR_NUM_CHUNKS;
  let slot = idx % FAR_NUM_CHUNKS;
  let occ = atomicLoad(&farOcc[idx]);
  if ((occ & 0xFFFFu) != 0u) {
    // farChunkIndexG's order is (z * N + y) * N + x.
    let sy = i32((slot / FAR_NCHUNK) % FAR_NCHUNK);
    let o = F.origins[level0].xyz;
    let cy = o.y + ((sy - o.y) & i32(FAR_NCHUNK_MASK));   // farSlotToChunk
    let top = farOccTop(occ);
    // top 0 = "unknown, assume full": the chunk's top row.
    let row = select(i32(top) - 1, i32(CHUNK) - 1, top == 0u);
    let yTop = cy * i32(CHUNK) + row;
    atomicMax(&skyWgMax, u32(yTop + SKY_TOP_BIAS + 1));
    atomicMax(&skyWgTile[(slot % FAR_NCHUNK) / SKY_TILE_CHUNKS], u32(yTop + SKY_TOP_BIAS + 1));
  }
  workgroupBarrier();
  if (li == 0u) {
    let m = atomicLoad(&skyWgMax);
    if (m != 0u) { atomicMax(&farOcc[SKY_TOP_BASE + level0], m); }
  }
  if (li < SKY_TILE_N) {
    let m = atomicLoad(&skyWgTile[li]);
    let tz = (slot / (FAR_NCHUNK * FAR_NCHUNK)) / SKY_TILE_CHUNKS;
    if (m != 0u) {
      atomicMax(&farOcc[SKY_TILE_BASE + (level0 * SKY_TILE_N + tz) * SKY_TILE_N + li], m);
    }
  }
}
