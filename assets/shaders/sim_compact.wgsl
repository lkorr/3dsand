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
// TEMPORAL RATE LOD (common.wgsl rateLodLevel): `main` reads the tick and the
// declared players to decide whether a slow-only chunk is due this tick.
@group(0) @binding(4) var<uniform> T : TickParams;
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
    // TEMPORAL RATE LOD (DESIGN.md §4 "Temporal rate LOD"). A window chunk
    // whose ONLY pending work is compensable reactions held at its own level
    // (sim_step doReactions marks exactly DIRTY_R_SLOW for that and nothing
    // else) has nothing to do on a tick that is not its due tick: every one of
    // those rules would be skipped there without a roll. So it is left out of
    // the list, and its mark is CARRIED into dirtyOut (zeroed by the phase-0
    // fill before this pass, OR'd by mutate/explode/windWake before it, read
    // by the CA after it) so the chunk is still dirty on its due tick. Any
    // other bit -- a write, a move, an op, a neighbour's fan-out, heat,
    // charge -- means real work and the chunk is dispatched as before. Ticket
    // slots (i >= NUM_CHUNKS) are level 0 by definition (sim_step agrees).
    if (i < NUM_CHUNKS && dirtyIn[i] == DIRTY_R_SLOW) {
      let wc = slotWorldChunk(i, T.origin);
      if (!rateLodDue(T.tick, wc, rateLodLevel(wc, &T))) {
        dirtyOut[i] = dirtyOut[i] | DIRTY_R_SLOW;
        return;
      }
    }
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
  // A word that is EXACTLY DIRTY_R_SLOW (temporal rate LOD: a slow-only chunk
  // the CA ran without writing, or one `main` skipped and carried) names a
  // chunk whose contents did not change this tick -- every voxel write ORs a
  // write bit beside it -- so occupancy, farDown and the seam have nothing
  // new to read there. Left out, as an unwritten chunk always was.
  if (dirtyOut[i] != 0u && dirtyOut[i] != DIRTY_R_SLOW) {
    let slot = atomicAdd(&args[0], 1u);
    dirtyList[slot] = i;
  }
}
