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
// Dispatch: (NUM_SLOTS / 64, 1, 1) workgroups of 64 threads — the whole slot
// space, window slots and ticket slots alike (docs/PLAN_chunk_tickets.md).

@group(0) @binding(1) var<storage, read_write> dirtyIn : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<u32>;
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
@group(0) @binding(13) var<storage, read_write> args : array<atomic<u32>>;  // x=count, y=1, z=1
// Read for the ticket table in its tail ONLY (common.wgsl ticketSlotActive):
// declaring it is what keeps common.wgsl's TICKET_BOUND block in this shader.
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;

// main: compacts dirtyIn (this tick's active set) for the CA color passes.
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i == 0u) {
    atomicStore(&args[1], 1u);
    atomicStore(&args[2], 1u);
  }
  if (i >= NUM_SLOTS) { return; }
  if (dirtyIn[i] != 0u) {
    // A TICKET'S SHELL IS RESIDENT, NOT ACTIVE (docs/PLAN_chunk_tickets.md
    // §2.3). Matter a ticket's interior pushes into its outer ring of chunks
    // is written there and stays — readable by the interior, frozen until the
    // ticket re-centres or releases — because dispatching the shell would
    // need ITS 26-neighbourhood resident, and that is the next ring out.
    // So a dirty shell slot never enters the CA's list. `mainNext` below
    // keeps it: the occupancy and digest of a written shell chunk still
    // update, which is what the release's store decision and the hash read.
    if (i >= NUM_CHUNKS && !ticketSlotActive(i)) { return; }
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
  if (i >= NUM_SLOTS) { return; }
  if (dirtyOut[i] != 0u) {
    let slot = atomicAdd(&args[0], 1u);
    dirtyList[slot] = i;
  }
}
