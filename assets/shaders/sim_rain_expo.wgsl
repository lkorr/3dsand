// sim_rain_expo.wgsl — THE RAIN EXPOSURE MAP (2026-09-30; DESIGN.md §9.w
// "Where the rain lands"; src/sim/rainexpo.h is the CPU mirror and the
// reference this file is written against).
//
// WHAT PROBLEM THIS SOLVES. sim_step's rainExposed used to be a one-cell probe:
// "the cell above me is not a ray blocker", or the same for a side neighbour.
// Anything longer inside the CA breaks determinism (the seesSky note: a
// 48-cell column read races the writes of the pass it runs in). So a room's
// floor under a ceiling 2 m up was exposed, any air cell beside an indoor wall
// splashed rain on it, and the rain could not lean. That note named the fix:
// a separate pass over an exposure buffer, before the CA, which the CA only
// reads. This is that pass.
//
// THE MAP. Rain falls along TickParams' slope (rainexpo.h: the lattice of fall
// lines, key(c) = c.xz + (n c.y + 8) >> 4). Per 4-key TEXEL on both axes, one
// thread marches the texel's REPRESENTATIVE line (key 4t + 2) down from the
// window top and stores the level of the first ray-blocking cell on its path,
// or RX_OPEN. The domain is every texel a window cell's key can land in: the
// window plus the upwind strip its lines enter through, sized for the steepest
// slope the weather can hand the sim (weather.h kRainSlopeMaxN).
//
// DETERMINISM, by construction. Each thread reads voxels and materials only and
// writes only its own texel; the CA reads the map read-only; the map is built
// IN FULL on every tick that can read it (rain or wetness in the rain word),
// before the CA and after the tick's mutations, from nothing but the grid and
// TickParams. No history: a save, a load, a replay start or a window move has
// nothing to carry. The page-table sentinel skip is exact (an EMPTY chunk IS
// air), so where the table happens to hold a page instead changes the cost of
// the walk, never its answer.
//
// COST. One thread per texel: 128 x 128 = 16k lines at a vertical fall, up to
// ~480 x 128 at the steepest lean along an axis (~377^2 on a diagonal). A line
// through sky is a chunk skip per chunk; zero on a dry tick (the row is not
// recorded, rule 2).

// `> voxels` with EXACTLY ONE SPACE (resources.cpp BodyAddressesVoxels).
@group(0) @binding(0) var<storage, read> voxels : array<u32>;
@group(0) @binding(3) var<storage, read> materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(45) var<storage, read_write> rainExpo : array<u32>;

// ---- the lattice (rainexpo.h rainlat::*; sim_step and sim_mutate agree) ------
const RX_TEX_SHIFT : u32 = 2u;              // rainlat::kExpoTexShift
const RX_REP : i32 = 2;                     // rainlat::kExpoRepOffset
const RX_OPEN : i32 = -2147483647 - 1;      // rainlat::kExpoOpen
const RX_MAX_AXIS : i32 = 482;              // rainlat::kExpoMaxAxis

fn rxFloorDiv(a : i32, b : i32) -> i32 {    // b > 0
  return select(-((-a + b - 1) / b), a / b, a >= 0);
}
fn rxDrift(n : i32, y : i32) -> i32 { return rxFloorDiv(n * y + 8, 16); }
// rainlat::AxisSpan: the levels at which line k's main cell is in [lo, hi].
fn rxAxisSpan(k : i32, n : i32, lo : i32, hi : i32) -> vec2<i32> {
  let A = k - hi;
  let B = k - lo;
  if (n == 0) {
    return select(vec2<i32>(1, 0), vec2<i32>(-1073741824, 1073741823), A <= 0 && 0 <= B);
  }
  if (n > 0) {
    return vec2<i32>(-rxFloorDiv(-(16 * A - 8), n), rxFloorDiv(16 * B + 7, n));
  }
  let m = -n;
  return vec2<i32>(rxFloorDiv(8 - 16 * B - 16, m) + 1, rxFloorDiv(8 - 16 * A, m));
}
fn rxSlope() -> vec2<i32> { return vec2<i32>(T.rainSlopeQx >> 12u, T.rainSlopeQz >> 12u); }
// rainlat::ExpoDomain: texel lo (xy) and extent (zw).
fn rxDomain(n : vec2<i32>) -> vec4<i32> {
  let b = T.origin * i32(CHUNK);
  let yb = b.y;
  let yt = b.y + i32(WORLD_N) - 1;
  let d0 = vec2<i32>(rxDrift(n.x, yb), rxDrift(n.y, yb));
  let d1 = vec2<i32>(rxDrift(n.x, yt), rxDrift(n.y, yt));
  let klo = b.xz + min(d0, d1);
  let khi = b.xz + vec2<i32>(i32(WORLD_N) - 1) + max(d0, d1);
  let lo = klo >> vec2<u32>(RX_TEX_SHIFT);
  return vec4<i32>(lo, (khi >> vec2<u32>(RX_TEX_SHIFT)) - lo + 1);
}

fn rxInWindow(c : vec3<i32>) -> bool {
  let d = c - T.origin * i32(CHUNK);
  return all(d >= vec3<i32>(0)) && all(d < vec3<i32>(i32(WORLD_N)));
}
// The rain stops here: a ray blocker (sim_step rainOpen's predicate, material
// level). Outside the window: air.
fn rxBlocks(c : vec3<i32>) -> bool {
  if (!rxInWindow(c)) { return false; }
  let m = voxMat(voxWordAt(c));
  return m != MAT_AIR && isRayBlocker(materials[m]);
}

// rainlat::FirstBlockLevel for line (kx, kz), with the exact sentinel skip.
fn rxMarch(k : vec2<i32>, n : vec2<i32>) -> i32 {
  let b = T.origin * i32(CHUNK);
  let yb = b.y;
  let yt = b.y + i32(WORLD_N) - 1;
  let hi = b.xz + vec2<i32>(i32(WORLD_N) - 1);
  let sx = rxAxisSpan(k.x, n.x, b.x, hi.x);
  let sz = rxAxisSpan(k.y, n.y, b.z, hi.y);
  // One level either side of the in-window span: past that every path cell is
  // outside the window on some axis, i.e. air (rainexpo.h).
  var y = min(yt, min(sx.y, sz.y) + 1);
  let yEnd = max(yb, max(sx.x, sz.x) - 1);
  var p = k - vec2<i32>(rxDrift(n.x, y + 1), rxDrift(n.y, y + 1));
  loop {
    if (y < yEnd) { break; }
    let m = k - vec2<i32>(rxDrift(n.x, y), rxDrift(n.y, y));
    if (y == yt) {
      if (rxBlocks(vec3<i32>(m.x, y, m.y))) { return y; }
    } else {
      // The L: down, along x at the previous z, along z at the new x.
      let st = select(vec2<i32>(-1), vec2<i32>(1), m >= p);
      var ax = p.x;
      loop {
        if (rxBlocks(vec3<i32>(ax, y, p.y))) { return y; }
        if (ax == m.x) { break; }
        ax += st.x;
      }
      var az = p.y;
      loop {
        if (az == m.y) { break; }
        az += st.y;
        if (rxBlocks(vec3<i32>(m.x, y, az))) { return y; }
      }
    }
    // THE SENTINEL SKIP: the main cell's chunk is one non-blocking material
    // throughout, so every path cell while the main stays in this chunk is
    // clear. Jump to the lowest level that keeps it in.
    let mc = vec3<i32>(m.x, y, m.y);
    if (rxInWindow(mc)) {
      let e = pageEntryOf(voxSlotOfCell(mc));
      if ((e & PT_SENTINEL_BIT) != 0u) {
        let sm = e & PT_MAT_MASK;
        if (sm == MAT_AIR || !isRayBlocker(materials[sm])) {
          let c0 = (mc >> vec3<u32>(CHUNK_SHIFT)) * i32(CHUNK);
          let cx = rxAxisSpan(k.x, n.x, c0.x, c0.x + i32(CHUNK) - 1);
          let cz = rxAxisSpan(k.y, n.y, c0.z, c0.z + i32(CHUNK) - 1);
          let yl = max(c0.y, max(cx.x, cz.x));
          if (yl < y) {
            p = k - vec2<i32>(rxDrift(n.x, yl), rxDrift(n.y, yl));
            y = yl - 1;
            continue;
          }
        }
      }
    }
    p = m;
    y -= 1;
  }
  return RX_OPEN;
}

@compute @workgroup_size(64)
fn build(@builtin(global_invocation_id) gid : vec3<u32>) {
  let n = rxSlope();
  let d = rxDomain(n);
  let count = u32(d.z * d.w);
  if (gid.x >= count) { return; }
  // The buffer holds RX_MAX_AXIS^2 words, sized for |slope| <= kRainSlopeMaxN,
  // which weather.cpp clamps. A TickParams from anywhere else (an op record, a
  // peer) is not clamped by that code, and past the end Tint's robustness
  // clamp would fold every excess thread onto the LAST word — many writers,
  // different values, a scheduling-dependent survivor (cross-vendor audit).
  if (gid.x >= u32(RX_MAX_AXIS * RX_MAX_AXIS)) { return; }
  let t = vec2<i32>(i32(gid.x % u32(d.z)), i32(gid.x / u32(d.z)));
  let k = ((d.xy + t) << vec2<u32>(RX_TEX_SHIFT)) + vec2<i32>(RX_REP);
  rainExpo[gid.x] = bitcast<u32>(rxMarch(k, n));
}
