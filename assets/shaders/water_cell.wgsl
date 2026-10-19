// water_cell.wgsl -- THE WATER CELL CACHE'S BAKE (2026-10-18).
//
// Two per-frame rows on the ShadowCache table (pass_table.def wc_prepare /
// copy_wcArgs / wc_bake), the GI gather's prepare/copy/indirect shape. The
// raymarch's water pixels used to rebuild their CELL's depth, openness and
// four neighbour columns (~32 dependent voxel loads)
// per PIXEL; those were 0.7-2.0 ms of a water-heavy frame. The
// fragment shader now lists the cells it drew (one lane per slot per frame,
// water_cell.wgsli's election) and this pass computes each listed cell ONCE,
// at the start of the next frame, against that frame's voxels. Layout and
// protocol: water_cell.wgsli.
//
// A first try did the same compute in the fragment shader (one elected pixel
// per cell): +0.4 ms on near water, -1.1 ms on a far fall, because a far cell
// is a few pixels and nearly every warp held an elected lane it then waited
// for. Out here no warp waits on another's cell.

// `> voxels` with EXACTLY ONE SPACE: resources.cpp BodyAddressesVoxels keeps
// the page-table block only for bodies containing it.
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(37) var<storage, read_write> wcache : array<atomic<u32>>;

fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, R.origin); }

// wc_prepare: last frame's request count -> a dispatch size; reopen the list
// for this frame's fragment shader.
@compute @workgroup_size(1)
fn wcPrepare() {
  let raw = atomicLoad(&wcache[0]);
  let n = min(raw, WC_REQ_CAP);
  atomicStore(&wcache[1], n);
  atomicStore(&wcache[2], raw);
  atomicStore(&wcache[3], select(0u, raw - WC_REQ_CAP, raw > WC_REQ_CAP));
  atomicStore(&wcache[0], 0u);
  atomicStore(&wcache[4], (n + 63u) / 64u);
  atomicStore(&wcache[5], 1u);
  atomicStore(&wcache[6], 1u);
}

// wc_bake: one lane per listed cell. The election made the list one entry per
// SLOT, so no two lanes write the same slot and no record can be torn. The
// stamp goes LAST, but nothing reads the slot before this pass ends anyway
// (the raymarch is ordered after it).
@compute @workgroup_size(64)
fn wcBake(@builtin(global_invocation_id) gid : vec3<u32>) {
  let n = atomicLoad(&wcache[1]);
  let i = gid.x;
  if (i >= n) { return; }
  let key = atomicLoad(&wcache[WC_HEADER + i]);
  let cell = shadowUnwrapCell(key, R.origin * i32(CHUNK));
  let base = wcSlotBase(key);
  let o = waterCellCompute(cell);
  atomicStore(&wcache[base + 2u], key);
  atomicStore(&wcache[base + 3u], pack2x16float(vec2f(o.depthM, o.open)));
  atomicStore(&wcache[base + 4u], pack2x16float(vec2f(o.hx0, o.hx1)));
  atomicStore(&wcache[base + 5u], pack2x16float(vec2f(o.hz0, o.hz1)));
  atomicStore(&wcache[base + 1u], R.frameIdx + 1u);
}
