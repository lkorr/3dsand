// far_select.wgsl — THE FAR DOWNSAMPLE CADENCE: which written chunks the far
// cascades re-downsample THIS tick (world.h farDownState; 2026-10-18, tickets
// 2026-10-16_11-14-41 and 2026-10-17_21-03-25).
//
// WHY. `fardown` (worldgen.wgsl) used to dispatch over every chunk written this
// tick (compactNext's dirtyOut list), and a chunk whose far-visible matter moved
// was re-downsampled into all eight cascade levels -- a genColumn per sample
// column per level. Running water keeps rewriting its chunks, so a pour down
// a hill or a breached dam paid the whole downsample of every wet chunk every
// few ticks (farField 4.6 ms of the slope pour's tick, 10.0 ms of the dam
// breach's) for cascade cells that are drawn past ~20 m, where a
// quarter-second-old water line is not a visible difference.
//
// THE RULE, per slot, every tick:
//   * written this tick (dirtyOut != 0)  -> PENDING;
//   * pending, and at least FAR_DOWN_EVERY ticks since its last downsample
//                                         -> selected: downsampled now, the
//                                            tick recorded, pending cleared.
// So a one-off edit (a dug hole, a felled tree, a blast) is downsampled the
// tick it happens, exactly as before; a chunk that keeps changing is
// downsampled every FAR_DOWN_EVERY ticks; and the LAST state of a chunk that
// stops changing lands at most FAR_DOWN_EVERY - 1 ticks after its last write,
// because the pending bit outlives the dirty flag. That last property is what
// a per-tick dirty list could never give a rate limit -- a chunk that went
// quiet on a skipped tick would never have come back -- and is why the
// selection reads the per-slot flags rather than a list.
//
// It also closes the old HASH-TICK hole (pass_table.def farDownHash note): a
// hash tick compacts no dirtyOut list, so the backstop rows ran over dirtyIn
// and missed a chunk first written from OUTSIDE the active set. dirtyOut is
// written on every tick, so this sees every write either way.
//
// Render-only derived data, like the downsample it schedules: nothing here
// writes a voxel. farVox is also what a particle outside residency collides
// with, so WHEN a cell's byte changes can move a far landing -- but the
// schedule is a pure function of (tick, which slots were written), so the run
// stays reproducible. The list ORDER is scheduling-dependent (atomicAdd), and
// fardown's phases are order-free by construction (worldgen.wgsl, the
// follow-up split note), exactly as they were over compactNext's list.
//
// Dispatch: (NUM_SLOTS / 64) workgroups of 64, every tick -- one load per
// slot for a world at rest, which is the whole cost.

@group(0) @binding(2) var<storage, read_write> dirtyOut : array<u32>;
@group(0) @binding(4) var<uniform> T : TickParams;
// The list fardown dispatches over (worldgen.wgsl `farDirty`, the same
// binding read-only there).
@group(1) @binding(4) var<storage, read_write> farDirtyW : array<u32>;
@group(1) @binding(8) var<storage, read_write> farDownState : array<u32>;
@group(1) @binding(9) var<storage, read_write> farDownArgs : array<atomic<u32>>;  // x=count, y=1, z=1

// Ticks between two downsamples of a chunk that keeps changing: 8 = 3.75 Hz
// at the 30 Hz tick, so the cascade's image of running water (drawn past the
// ~20 m handoff) lags it by at most ~0.25 s. Measured on --perf dam (ticket
// 21-03-25's breach, farField ms/tick): every tick 10.0, every 4th 7.8, every
// 16th 4.5 -- a wet chunk is rewritten every few ticks, not every tick, so a
// cadence of 4 barely bites. 8 is the middle, chosen for the handoff seam: a
// flood front moving ~1 m/s would stand ~0.25 m apart across it.
const FAR_DOWN_EVERY : u32 = 8u;
const FDS_PENDING : u32 = 0x80000000u;
const FDS_TICK : u32 = 0x7FFFFFFFu;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i == 0u) {
    atomicStore(&farDownArgs[1], 1u);
    atomicStore(&farDownArgs[2], 1u);
  }
  if (i >= NUM_SLOTS) { return; }
  let written = dirtyOut[i] != 0u;
  let st = farDownState[i];
  if (!written && (st & FDS_PENDING) == 0u) { return; }
  let now = T.tick & FDS_TICK;
  let last = st & FDS_TICK;
  // 31-bit wrap-safe age. A state from before a world reset (the tick counter
  // restarted) reads as a huge age, i.e. due -- the safe direction -- and so
  // does the zeroed buffer's "never downsampled" (last 0).
  let age = (now - last) & FDS_TICK;
  if (last != 0u && age < FAR_DOWN_EVERY) {
    if (written && (st & FDS_PENDING) == 0u) { farDownState[i] = st | FDS_PENDING; }
    return;
  }
  farDownState[i] = now;
  farDirtyW[atomicAdd(&farDownArgs[0], 1u)] = i;
}
