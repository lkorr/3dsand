// sim_occupancy.wgsl — per-chunk occupancy words (renderer empty-space
// skipping) + optional whole-world hash (determinism check / desync detector).
// Each occupancy word packs two counts (packOcc in common.wgsl): low 16 bits =
// non-air voxels, high 16 = ray blockers (solid/powder/opaque-liquid). Shadow
// rays chunk-skip on the blocker count, so gas plumes don't defeat the skip.
// The hash is a wrapping sum of per-cell hashes: commutative, so the atomic
// accumulation order cannot affect the result — deterministic by construction.
//
// Dispatch: (NUM_CHUNKS, 1, 1) workgroups; each workgroup reduces one chunk.

@group(0) @binding(0) var<storage, read_write> voxels    : array<u32>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(7) var<storage, read_write> occupancy : array<u32>;
@group(0) @binding(8) var<storage, read_write> worldHash : array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;

var<workgroup> wgCount : atomic<u32>;
var<workgroup> wgBlock : atomic<u32>;
var<workgroup> wgHash  : atomic<u32>;

// mainDirty: occupancy update over only the chunks written this tick
// (indirect over the compacted dirtyOut list) — the per-tick sim cost stays
// proportional to activity, not world size (DESIGN.md §11). No hash: the
// whole-world hash needs every chunk, so hash ticks use the full pass below.
@compute @workgroup_size(64)
fn mainDirty(@builtin(workgroup_id) wg : vec3<u32>,
             @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
  }
  workgroupBarrier();

  let base = dirtyList[wg.x] * CHUNK_VOL;
  var count = 0u;
  var block = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    let m = voxels[base + i] & 0xFFFu;
    if (m != MAT_AIR) {
      count += 1u;
      if (isRayBlocker(materials[m])) { block += 1u; }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  workgroupBarrier();

  if (li == 0u) {
    occupancy[dirtyList[wg.x]] = packOcc(atomicLoad(&wgCount), atomicLoad(&wgBlock));
  }
}

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
    atomicStore(&wgHash, 0u);
  }
  workgroupBarrier();

  let base = wg.x * CHUNK_VOL;
  var count = 0u;
  var block = 0u;
  var h = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    let v = voxels[base + i] & 0xFFFFu;   // stamp byte excluded from state identity
    let m = v & 0xFFFu;
    if (m != MAT_AIR) {
      count += 1u;
      if (isRayBlocker(materials[m])) { block += 1u; }
      if (T.hashEnable != 0u) {
        h += pcg((base + i) ^ (v * 0x9E3779B9u));
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  if (T.hashEnable != 0u) { atomicAdd(&wgHash, h); }
  workgroupBarrier();

  if (li == 0u) {
    occupancy[wg.x] = packOcc(atomicLoad(&wgCount), atomicLoad(&wgBlock));
    if (T.hashEnable != 0u) {
      atomicAdd(&worldHash[0], atomicLoad(&wgHash));
    }
  }
}
