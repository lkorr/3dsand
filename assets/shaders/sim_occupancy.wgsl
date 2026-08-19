// sim_occupancy.wgsl — per-chunk non-air voxel counts (renderer empty-space
// skipping) + optional whole-world hash (determinism check / desync detector).
// The hash is a wrapping sum of per-cell hashes: commutative, so the atomic
// accumulation order cannot affect the result — deterministic by construction.
//
// Dispatch: (NUM_CHUNKS, 1, 1) workgroups; each workgroup reduces one chunk.

@group(0) @binding(0) var<storage, read_write> voxels    : array<u32>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(7) var<storage, read_write> occupancy : array<u32>;
@group(0) @binding(8) var<storage, read_write> worldHash : array<atomic<u32>>;

var<workgroup> wgCount : atomic<u32>;
var<workgroup> wgHash  : atomic<u32>;

@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgHash, 0u);
  }
  workgroupBarrier();

  let base = wg.x * CHUNK_VOL;
  var count = 0u;
  var h = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    let v = voxels[base + i] & 0xFFFFu;   // stamp byte excluded from state identity
    if ((v & 0xFFFu) != MAT_AIR) {
      count += 1u;
      if (T.hashEnable != 0u) {
        h += pcg((base + i) ^ (v * 0x9E3779B9u));
      }
    }
  }
  atomicAdd(&wgCount, count);
  if (T.hashEnable != 0u) { atomicAdd(&wgHash, h); }
  workgroupBarrier();

  if (li == 0u) {
    occupancy[wg.x] = atomicLoad(&wgCount);
    if (T.hashEnable != 0u) {
      atomicAdd(&worldHash[0], atomicLoad(&wgHash));
    }
  }
}
