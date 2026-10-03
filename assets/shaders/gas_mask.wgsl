// gas_mask.wgsl — THE GAS EMPTY-BRICK MASK (2026-10-03, raymarch-smoke). One
// per-frame compute entry on the shadow layout (pass_table.def gas_mask, the
// ShadowCache table), read by raymarch.wgsl's gasOuterCountAt.
//
// ---- WHAT IT IS FOR -----------------------------------------------------------
// Whenever any gas exists (RFLAG_GAS), every pixel whose ray reaches ~10 m runs
// gasOuterFill: 16 samples of the in-window band plus 16 past the window face,
// each a smoothstepped trilinear fetch of EIGHT 16-bit counts out of the 4 MiB
// gasOuter box. Measured with --render-budget (fire / village cameras, one
// process): the whole fill was 1.18 / 0.55 ms of a 10.8 / 6.4 ms raymarch, and
// running its loops with the fetches removed and nothing else changed saved
// MORE than that (1.37 / 0.55) -- the loop arithmetic is free, the cost is the
// eight loads per sample. And almost every sample is of an EMPTY cell: smoke
// fills a few bricks of a box that spans +-51 m.
//
// So one bit per brick says whether a sample there can be anything but zero.
// The fragment side tests the bit (one word out of a 4 KiB buffer that stays in
// L1) and skips the fetch on a clear bit. That is EXACT, not an approximation:
// a clear bit means all eight corners of the filter footprint are 0, and the
// trilinear blend of eight zeros is exactly 0.0 -- the value the fetch would
// have returned. The picture is bit-identical.
//
// ---- THE LAYOUT ---------------------------------------------------------------
// gasOuter is GAS_OUTER_N^3 cells (128^3, 0.8 m each). A brick is
// (1 << GM_SHIFT)^3 = 4^3 cells, so the mask is 32^3 bits: word
// (bz * GM_N + by), bit bx -- one u32 per x-row of bricks.
//
// THE RIM. The sampler's footprint for a point is cells d .. d+1 on each axis
// (d = floor of the half-cell-biased position), and the fragment side tests the
// brick of d. When d is the LAST cell of its brick, d+1 is the first cell of
// the next one, so a brick's bit must also cover the one-cell rim past its high
// faces: the scan below runs cells [4b, 4b + 4] inclusive, clamped to the box
// (cells past the box read as 0 in the sampler, so there is nothing to cover).
// Reading whole words can also take in the neighbouring half of a pair; that
// only ever sets a bit that could have been clear -- conservative, never wrong.
//
// ---- VALIDITY ------------------------------------------------------------------
// Rebuilt every frame RFLAG_GAS is set, BEFORE the raymarch in the same command
// buffer (BeginRendering's flush orders the write before the fragment read), and
// the raymarch consults it only on frames that flag is set. On a frame it is
// clear this returns without writing and the raymarch does not look, so a stale
// mask is never read. gasOuter is written on the TICK command buffer; this row
// declares R(GasOuter), which orders it after the tick's splats.
//
// Cost: GM_N^3 = 32,768 threads, each OR-ing ~75 words; ~0.02-0.05 ms on the
// frames that have gas, one empty dispatch on the frames that do not.
//
// Render-only derived data: never hashed, never saved, no sim binding.

@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(30) var<storage, read> gasOuter : array<u32>;
@group(0) @binding(31) var<storage, read_write> gasMask : array<u32>;

// world.h kGasOuterN, mirrored like sim_gas / sim_step / raymarch's own copies
// (check_invariants.py pins all four): a prelude constant would miss the SPIR-V
// cache for every shader.
const GAS_OUTER_N : u32 = 128u;
// EXPERIMENT ARMS: brick shift 2 / 1 / 0 by debug.perfExp.
const GAS_MASK_SHIFT : u32 = 2u;
const GM_N : u32 = GAS_OUTER_N >> GAS_MASK_SHIFT;   // bricks per axis
const GM_ROW_WORDS : u32 = GM_N / 32u;              // u32 words per x-row
const GM_WORDS : u32 = GM_N * GM_N * GM_ROW_WORDS;
const_assert GM_N >= 32u;
const RFLAG_GAS : u32 = 8u;                   // raymarch.wgsl RFLAG_GAS agrees

var<workgroup> gmRow : atomic<u32>;

@compute @workgroup_size(32)
fn gasMaskBuild(@builtin(local_invocation_id) lid : vec3<u32>,
                @builtin(workgroup_id) wid : vec3<u32>) {
  // R is a uniform buffer and wid is the group's own id, so both returns are
  // uniform: every invocation of the group leaves together and the barriers
  // below stay in uniform control flow.
  if ((R.flags & RFLAG_GAS) == 0u) { return; }
  if (wid.x >= GM_WORDS) { return; }
  if (lid.x == 0u) { atomicStore(&gmRow, 0u); }
  workgroupBarrier();
  // wid.x = (bz * GM_N + by) * GM_ROW_WORDS + word-in-row, lid.x = the bit.
  let rowI = wid.x / GM_ROW_WORDS;
  let b = vec3<u32>((wid.x % GM_ROW_WORDS) * 32u + lid.x, rowI % GM_N, rowI / GM_N);
  let lo = b << vec3<u32>(GAS_MASK_SHIFT);
  let hi = min(lo + vec3<u32>(1u << GAS_MASK_SHIFT), vec3<u32>(GAS_OUTER_N - 1u));
  var hit = 0u;
  for (var z = lo.z; z <= hi.z; z++) {
    for (var y = lo.y; y <= hi.y; y++) {
      let row = (z * GAS_OUTER_N + y) * GAS_OUTER_N;
      for (var w = (row + lo.x) >> 1u; w <= (row + hi.x) >> 1u; w++) {
        hit |= gasOuter[w];
      }
    }
  }
  if (hit != 0u) { atomicOr(&gmRow, 1u << lid.x); }
  workgroupBarrier();
  if (lid.x == 0u) { gasMask[wid.x] = atomicLoad(&gmRow); }
}
