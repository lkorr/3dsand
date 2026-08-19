// sim_compact.wgsl — compacts the dirty-chunk flags into a dense index list
// plus indirect dispatch args, so the 54 CA color passes each dispatch exactly
// one workgroup per dirty chunk (DESIGN.md §11: sim cost scales with activity,
// not world size; a settled world dispatches ~nothing).
//
// Determinism note: the list ORDER is scheduling-dependent (atomicAdd append),
// but each dirty chunk appears exactly once and the color scheme makes all
// same-pass writes disjoint, so processing order cannot affect sim state —
// bit-determinism holds (DESIGN.md §4).
//
// Dispatch: (NUM_CHUNKS / 64, 1, 1) workgroups of 64 threads.

@group(0) @binding(1) var<storage, read_write> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<u32>;
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
@group(0) @binding(13) var<storage, read_write> args : array<atomic<u32>>;  // x=count, y=1, z=1

// main: compacts dirtyIn (this tick's active set) for the CA color passes.
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i == 0u) {
    atomicStore(&args[1], 1u);
    atomicStore(&args[2], 1u);
  }
  if (i >= NUM_CHUNKS) { return; }
  if (dirtyIn[i] != 0u) {
    let slot = atomicAdd(&args[0], 1u);
    dirtyList[slot] = i;
  }
}

// mainNext: compacts dirtyOut (every chunk written this tick — markDirty marks
// the containing chunk of every voxel write) so the occupancy update can run
// over only the chunks whose contents changed.
@compute @workgroup_size(64)
fn mainNext(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i == 0u) {
    atomicStore(&args[1], 1u);
    atomicStore(&args[2], 1u);
  }
  if (i >= NUM_CHUNKS) { return; }
  if (dirtyOut[i] != 0u) {
    let slot = atomicAdd(&args[0], 1u);
    dirtyList[slot] = i;
  }
}
