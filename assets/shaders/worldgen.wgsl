// worldgen.wgsl — compute procgen (M7), integer-only and seed-deterministic.
// genCell() is a pure function of WORLD coordinates + seed, so a chunk that is
// generated, evicted unmodified, and re-entered regenerates bit-identically.
// The height function is mirrored exactly in C++ (world.cpp TerrainHeight) so
// the CPU can compute spawn points — keep the two in sync, including the
// floor-division fixes for negative coordinates.
//
// Two entries, both one workgroup (64 threads) per chunk:
//   main — full residency window (NUM_CHUNKS workgroups), startup / regen
//   list — T.genCount slot indices from genList, streamed-in chunks
// Each workgroup fills its chunk, computes its occupancy count in-kernel (the
// renderer skips occ==0 chunks, so a late count would flicker the horizon),
// and wakes the chunk once so loose material settles and then sleeps.
//
// Terrain: a forest overworld. A low-frequency biome field picks forest
// (dominant), rare desert, and snow above the treeline; the surface caps with
// grass over dirt, sand in the desert. Ponds are hash-placed discs, carved and
// filled to a rim-derived level so they cannot spill (see pondAt). Trees are
// placed per-tile by tile hash (5 species) and sampled from the 5x5 tile
// neighborhood so canopies overhang tile borders. Below it all: column-band
// caves, lava pockets at depth, and per-256^2-tile ruin POIs.
// The authored origin-area set pieces (water/oil/lava pools, wood platform)
// live at their absolute coordinates and appear when those chunks generate.
//
// EVERYTHING here is placed as INERT material (wood/leaves/grass/petal, never
// stem/sprout/vine). Worldgen paints foliage by the million and the reactive
// garden materials grow — a generated forest of `stem` would keep every chunk
// in the world awake and blow the settled-world budget (CLAUDE.md rule 2).
// Reactive `seed` is still scattered, but sparsely, exactly as before.

@group(0) @binding(0) var<storage, read_write> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read_write> dirtyIn   : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> dirtyOut  : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(7) var<storage, read_write> occupancy : array<u32>;
@group(0) @binding(16) var<storage, read> genList : array<u32>;

const M_STONE : u32 = 1u;
const M_WOOD  : u32 = 2u;
const M_SAND  : u32 = 3u;
const M_WATER : u32 = 5u;
const M_OIL   : u32 = 6u;
const M_LAVA  : u32 = 12u;
const M_SNOW  : u32 = 15u;
const M_DIRT  : u32 = 16u;
const M_SEED  : u32 = 18u;
const M_GRASS  : u32 = 33u;
const M_LEAVES : u32 = 34u;
const M_PINE   : u32 = 35u;
const M_AUTUMN : u32 = 36u;
const M_BIRCH  : u32 = 37u;
const M_PETAL  : u32 = 38u;

// Biomes, from the low-frequency biome field (see biomeAt).
const B_FOREST : u32 = 0u;   // dominant: grass over dirt, dense trees
const B_MEADOW : u32 = 1u;   // forest clearings: flowers, few trees
const B_PINE   : u32 = 2u;   // conifer stands on the higher slopes
const B_DESERT : u32 = 3u;   // rare: the old sand world, kept as a destination

// Floor division / positive modulo: WGSL `/` and `%` truncate toward zero,
// which breaks value-noise lattices at negative coordinates (gx would repeat
// around 0 and fx would go negative).
fn fdiv(a : i32, b : i32) -> i32 {
  var q = a / b;
  if ((a % b) != 0 && ((a < 0) != (b < 0))) { q -= 1; }
  return q;
}
fn fmodp(a : i32, b : i32) -> i32 {
  let m = a % b;
  return m + select(0, b, m < 0);
}

fn vnoise(x : i32, z : i32, cs : i32, seed : u32) -> i32 {
  let gx = fdiv(x, cs);
  let gz = fdiv(z, cs);
  let fx = fmodp(x, cs);
  let fz = fmodp(z, cs);
  let h00 = i32(hash3(seed, bitcast<u32>(gx),      bitcast<u32>(gz))      & 0xFFu);
  let h10 = i32(hash3(seed, bitcast<u32>(gx + 1),  bitcast<u32>(gz))      & 0xFFu);
  let h01 = i32(hash3(seed, bitcast<u32>(gx),      bitcast<u32>(gz + 1))  & 0xFFu);
  let h11 = i32(hash3(seed, bitcast<u32>(gx + 1),  bitcast<u32>(gz + 1))  & 0xFFu);
  let v0 = h00 * (cs - fx) + h10 * fx;
  let v1 = h01 * (cs - fx) + h11 * fx;
  return (v0 * (cs - fz) + v1 * fz) / (cs * cs);
}

// ---- world scale ----
// One voxel is VOXEL_METERS = 6.25 cm: 16 voxels to the metre, and the player
// capsule is 27 voxels tall. The original desert worldgen was written as if a
// voxel were about a metre, so every feature came out as a tabletop model of
// itself — 4 m hills, a 2 m "lake", 0.9 m ruins, knee-high trees.
//
// HORIZONTAL features scale by HSCALE. X/Z stream infinitely, so widening them
// costs nothing but noise-cell size: a lake becomes a lake, a biome becomes a
// region you walk across rather than step over.
//
// VERTICAL relief deliberately does NOT scale with it. The residency window is
// 256 voxels = 16 m tall and does not stream in Y (caves already run from y-24
// to the surface), so height is a hard budget, not a free parameter.
//
// Third scale pass: the first cut was tabletop-tiny, the second (HSCALE=2,
// ~3x hills) overshot. Halved back down — everything EXCEPT the trees, which
// keep their metre-true sizes (TREE_* / treeInfo below are untouched).
const HSCALE : i32 = 1;

// Snow/treeline. Terrain spans y32..y86, so this sits in the top ~quarter of
// the range: high ridges go bare and white, everything below is forest. It was
// a bare `80` in four places when the band was y44..y90 — retune it whenever
// the band moves.
const TREELINE : i32 = TUNE_TREELINE;

fn baseHeight(x : i32, z : i32, seed : u32) -> i32 {
  // Two octaves. Wavelength and amplitude were both halved in the third scale
  // pass (128/32-cell, 84+24 -> 64/16-cell, 42+12), so slopes are unchanged —
  // the hills are the same shape, half the size. Band y32..y86: the low end
  // leaves room for lake basins to cut down into, and the low ceiling buys the
  // headroom that lets a 12 m great-oak crown on a ridge fit under y256.
  return TUNE_BASE_HEIGHT
       + (vnoise(x, z, TUNE_HILL_WAVELENGTH * HSCALE, seed ^ 1u) * TUNE_HILL_AMPLITUDE) / 255
       + (vnoise(x, z, TUNE_DETAIL_WAVELENGTH * HSCALE, seed ^ 2u) * TUNE_DETAIL_AMPLITUDE) / 255;
}

// ---- biome field ----
// One low-frequency noise picks the biome, a second breaks up the boundary so
// biomes interlock instead of meeting on a smooth contour. Desert is gated to
// the top of the range (~12% of the field) so it reads as a rare destination
// you walk to rather than the default world. Height still overrides at the top:
// snow caps above 80 regardless of biome (handled in genCell).
// The biome cell is pinned at 384 voxels (24 m) — deliberately NOT halved with
// the rest of the third scale pass. Trees kept their size and their 9 m
// spacing, so a biome region has to stay many tree-tiles wide or a "meadow"
// holds one bush and the field reads as per-tree noise. The break-up octave
// scales with it so edges stay proportionally ragged.
fn biomeAt(x : i32, z : i32, seed : u32) -> u32 {
  let b = vnoise(x, z, TUNE_BIOME_SCALE * HSCALE, seed ^ 0x1Bu)
        + (vnoise(x, z, (TUNE_BIOME_SCALE / 4) * HSCALE, seed ^ 0x1Cu) - 128) / 3;   // edge break-up
  if (b > i32(TUNE_DESERT_THRESHOLD)) { return B_DESERT; }
  if (b > i32(TUNE_PINE_THRESHOLD)) { return B_PINE; }
  if (b < i32(TUNE_MEADOW_THRESHOLD))  { return B_MEADOW; }
  return B_FOREST;
}

// ---- the spawn clearing ----
// The selftest plants fixtures at fixed origin-area columns — the walk test at
// (140,140), debris/prefab/mob drops at (60,60), (80,80), (90,90), (100,100) —
// and every one of them assumes TerrainHeight() is the top of the world there.
// A pond or an oak canopy over any of those turns a passing test into a
// mystery. Keep the block clear of both; it also gives the player somewhere to
// stand at spawn instead of waking up inside a trunk.
// Widened past the old 40..160 box: trunks are only suppressed when the TRUNK
// site is inside the clearing, and a great oak rooted just outside now reaches
// ~67 voxels in, so the old margin no longer kept the fixtures clear.
fn inSpawnClearing(x : i32, z : i32) -> bool {
  return x >= 0 && x <= 220 && z >= 0 && z <= 220;
}

// Inside the clearing, the columns the selftest actually drops bodies onto keep
// the ORIGINAL bare sand cap instead of the grass-over-stone forest floor.
// Those fixtures (debris islands at (60,60), the prefab at (80,80), the burn
// plank and shatter dumbbell at (90,90)) have margins tuned against that
// surface: bodies sink into the loose powder rather than resting on top of a
// solid grass skin — and a single grass tuft one voxel above the ground is
// enough to hold a burning body higher, which changes which of its voxels the
// emitted fire reaches. That cost the shatter test its 8-voxel plate.
// Small pads, so the forest still closes in around them.
fn onFixturePad(x : i32, z : i32) -> bool {
  let a = abs(x - 60) <= 10 && abs(z - 60) <= 10;
  let b = abs(x - 80) <= 10 && abs(z - 80) <= 10;
  let c = abs(x - 90) <= 10 && abs(z - 90) <= 10;
  return a || b || c;
}

// ---- ponds ----
// Bounded DISC ponds, one per POND_TILE XZ tile by tile hash — the same
// placement scheme as the trees, and for the same reason: a pond is an object
// with a knowable boundary, not a field contour. The previous design filled
// basin-noise contours up to a noise water table, and wherever the basin MASK
// edge crossed ground that sat below the local table, the pond poured onto
// dry lower land and crept downhill — the sleep gate caught exactly that (82
// chunks around one pond still awake after 600 settle ticks; the CPU-mirror
// scan found 175 such spill edges in that region alone). A disc pond cannot
// leak by construction: its water surface is set 2 below the LOWEST terrain
// sample on its own rim, so the shore stands above the water all the way
// around, and the bowl is carved into the terrain beneath it.
// Still deliberately independent of the cave system (caves stop 40 voxels
// under the surface, bowls reach ~10) — a pond can never drain into a tunnel.
const POND_TILE : i32 = TUNE_POND_TILE;   // 14 m between pond sites
// 24 rim directions, cos/sin in 1/256ths (15 degree steps). 24 samples on the
// largest (r=36) pond puts one every ~9 voxels of arc — dense enough that the
// 16-voxel fine terrain octave cannot hide a below-water notch between two
// samples, which is what the -2 margin then absorbs.
const POND_RIM : array<vec2<i32>, 24> = array<vec2<i32>, 24>(
  vec2<i32>( 256,    0), vec2<i32>( 247,   66), vec2<i32>( 222,  128),
  vec2<i32>( 181,  181), vec2<i32>( 128,  222), vec2<i32>(  66,  247),
  vec2<i32>(   0,  256), vec2<i32>( -66,  247), vec2<i32>(-128,  222),
  vec2<i32>(-181,  181), vec2<i32>(-222,  128), vec2<i32>(-247,   66),
  vec2<i32>(-256,    0), vec2<i32>(-247,  -66), vec2<i32>(-222, -128),
  vec2<i32>(-181, -181), vec2<i32>(-128, -222), vec2<i32>( -66, -247),
  vec2<i32>(   0, -256), vec2<i32>(  66, -247), vec2<i32>( 128, -222),
  vec2<i32>( 181, -181), vec2<i32>( 222, -128), vec2<i32>( 247,  -66));

// Returns (bowl floor, water surface) at this column, or (-1,-1) outside any
// pond. genCell carves the terrain to the floor and fills (floor, surface]
// with water. Pure function of (coords, seed), exactly like treeInfo.
fn pondAt(x : i32, z : i32, seed : u32) -> vec2<i32> {
  let none = vec2<i32>(-1, -1);
  let pt = fdiv(x, POND_TILE);
  let pz = fdiv(z, POND_TILE);
  let rh = hash3(seed ^ 0xB0A7u, bitcast<u32>(pt), bitcast<u32>(pz));
  if (rh % TUNE_POND_CHANCE != 0u) { return none; }              // ~1 pond per 4 tiles
  let r = TUNE_POND_RADIUS_MIN + i32((rh >> 4u) % TUNE_POND_RADIUS_SPAN);              // radius 20..36 (2.5-4.5 m)
  // Center insets by 60 > max radius + margin: the disc never leaves its own
  // tile, so callers only ever consult ONE tile (no neighborhood scan).
  let span = u32(POND_TILE - 120);
  let cx = pt * POND_TILE + 60 + i32((rh >> 9u) % span);
  let cz = pz * POND_TILE + 60 + i32((rh >> 17u) % span);
  // Keep-out zones, by DISC (center + radius), not by column: the spawn
  // clearing + fixture pads, the streaming ball column (408,128) whose test
  // assumes TerrainHeight() is the surface, and the three authored pools
  // (128 covers the widest rim 80 + max radius 36 + slack).
  if (cx >= -44 && cx <= 264 && cz >= -44 && cz <= 264) { return none; }
  if (abs(cx - 408) < r + 24 && abs(cz - 128) < r + 24) { return none; }
  let q1x = cx - 420; let q1z = cz - 420;
  let q2x = cx - 260; let q2z = cz - 300;
  let q3x = cx - 220; let q3z = cz - 520;
  if (q1x * q1x + q1z * q1z < 128 * 128) { return none; }
  if (q2x * q2x + q2z * q2z < 128 * 128) { return none; }
  if (q3x * q3x + q3z * q3z < 128 * 128) { return none; }
  let dx = x - cx;
  let dz = z - cz;
  let d2 = dx * dx + dz * dz;
  if (d2 > r * r) { return none; }
  // water level: 2 under the lowest ground on the rim circle
  var wmin = 0x7FFFFFFF;
  for (var i = 0; i < 24; i++) {
    let s = POND_RIM[i];
    wmin = min(wmin, baseHeight(cx + (s.x * r) / 256, cz + (s.y * r) / 256, seed));
  }
  let surf = wmin - 2;
  // parabolic bowl: 2 voxels deep at the rim, 8 at the center, carved below
  // the water surface (terrain that is already lower stays — water just fills
  // deeper there, still capped by the rim-derived surface)
  let depth = 2 + ((r * r - d2) * 6) / (r * r);
  return vec2<i32>(surf - depth, surf);
}

// ---- trees ----
// SCALE: one voxel is VOXEL_METERS = 6.25 cm, so there are 16 voxels to the
// metre and the player capsule is 27 voxels tall. Every dimension below is
// therefore written as METRES * VOX_PER_M, never as a bare voxel count — the
// first cut of this system used bare counts and produced 10-voxel "oaks" that
// were 60 cm tall, i.e. knee-high shrubs. If you tune these, tune the metres.
//
// Heights are held to roughly half real scale (a 12 m oak would be 190 voxels)
// because the residency window is 256 voxels tall and does not stream in Y:
// terrain tops out at y86, and a great oak there already pushes its crown
// against the window ceiling; anything taller is beheaded. Crown radius,
// being horizontal, is free to be generous.
// These dimensions survived the third scale pass untouched — the world
// halved around the trees, per the "trees were the one thing at a good size"
// verdict. Their METRE sizes are the spec; don't scale them with HSCALE.
const VOX_PER_M : i32 = 16;

// Placement is per TREE_TILE XZ tile: hash the tile, and it either holds one
// tree or none. The tile has to be at least as wide as a canopy or trees
// overlap into mush; at 10 m great oaks that means a 6 m tile, not the 1 m
// (16-voxel) tile the shrub-sized first cut used.
// Sized against the widest canopy: a great oak is radius ~67 voxels (4.2 m),
// so trunks need ~9 m of spacing or every crown swallows its neighbours and
// the forest becomes one undifferentiated green ceiling. Some overlap is good
// — that is what closes the canopy — but it has to be overlap, not merger.
const TREE_TILE : i32 = TUNE_TREE_TILE;         // 9 m between trunk sites
// How many tiles out to search: a canopy can overhang its own tile by
// (radius + in-tile jitter), here 67 + 72 = 139 voxels, just under one tile.
const TREE_SCAN : i32 = 2;           // +-2 tiles, comfortably covers it
//
// Everything is a pure function of (tile coords, seed) — no state, no
// scattering pass — so a tree straddling a chunk border generates identically
// from either chunk, and a chunk evicted and re-entered regrows the same tree.

// Per-tile tree descriptor, unpacked from one hash.
struct Tree {
  present : bool,
  species : u32,   // 0 oak, 1 pine, 2 birch, 3 great oak, 4 bush
  wx      : i32,   // trunk world x/z
  wz      : i32,
  base    : i32,   // ground height at the trunk
  trunk   : i32,   // trunk height in voxels
  radius  : i32,   // canopy radius
  rnd     : u32,   // spare bits for per-tree jitter
};

fn treeInfo(tx : i32, tz : i32, seed : u32) -> Tree {
  var t : Tree;
  t.present = false;
  t.species = 0u; t.wx = 0; t.wz = 0; t.base = 0;
  t.trunk = 0; t.radius = 0; t.rnd = 0u;

  let hsh = hash3(seed ^ 0x7BEE5u, bitcast<u32>(tx), bitcast<u32>(tz));
  t.rnd = hsh;
  // Trunk sits somewhere in the middle half of the tile — jittering the site
  // within the tile is what stops a forest from reading as a planted grid.
  let inset = TREE_TILE / 4;
  let span = u32(TREE_TILE / 2);
  t.wx = tx * TREE_TILE + inset + i32((hsh >> 3u) % span);
  t.wz = tz * TREE_TILE + inset + i32((hsh >> 9u) % span);

  let biome = biomeAt(t.wx, t.wz, seed);
  let h = baseHeight(t.wx, t.wz, seed);
  t.base = h;

  // No trees on snowfields, in ponds, or over the selftest fixture sites.
  if (h >= TREELINE) { return t; }
  if (inSpawnClearing(t.wx, t.wz)) { return t; }
  if (pondAt(t.wx, t.wz, seed).y >= 0) { return t; }

  // density by biome: forest is nearly every tile, meadow is sparse clearing,
  // desert gets the occasional dead bush.
  let roll = (hsh >> 17u) % 100u;
  var chance = 0u;
  if (biome == B_FOREST)      { chance = TUNE_TREE_CHANCE_FOREST; }
  else if (biome == B_PINE)   { chance = TUNE_TREE_CHANCE_PINE; }
  else if (biome == B_MEADOW) { chance = TUNE_TREE_CHANCE_MEADOW; }
  else                        { chance = TUNE_TREE_CHANCE_DESERT; }   // desert
  if (roll >= chance) { return t; }

  // species by biome
  let sroll = (hsh >> 24u) % 100u;
  if (biome == B_DESERT) {
    t.species = 4u;                                    // bush
  } else if (biome == B_PINE) {
    t.species = select(1u, 0u, sroll < 18u);           // mostly pine
  } else if (biome == B_MEADOW) {
    if (sroll < 45u)      { t.species = 4u; }          // bushes
    else if (sroll < 80u) { t.species = 2u; }          // birch
    else                  { t.species = 0u; }
  } else {                                             // forest
    if (sroll < 46u)      { t.species = 0u; }          // oak
    else if (sroll < 62u) { t.species = 2u; }          // birch
    else if (sroll < 76u) { t.species = 1u; }          // pine
    else if (sroll < 84u) { t.species = 3u; }          // great oak
    else                  { t.species = 4u; }          // bush
  }

  // Dimensions in TENTHS OF A METRE, converted below — the whole point is that
  // these read as physical sizes next to a 1.7 m player, not as voxel counts.
  // Crown radius stays near half the trunk height: taller and the tree reads as
  // a pole, wider and neighbouring canopies merge into one ceiling.
  let j = i32((hsh >> 12u) % 5u);   // per-tree size jitter, in 0.1 m steps
  var trunkDm = 0;
  var radDm = 0;
  switch (t.species) {
    //                        trunk           crown radius
    case 0u: { trunkDm = 55 + j * 5;  radDm = 28 + j * 2; }   // oak      5.5-7.5 m
    case 1u: { trunkDm = 70 + j * 8;  radDm = 24 + j * 2; }   // pine     7.0-10 m
    // Birch is a branch SKELETON, not a crown: `radius` here is the reach of a
    // primary limb, not the extent of a leaf ball, so it can be generous
    // without the canopy-merger problem that constrains the round species.
    case 2u: { trunkDm = 65 + j * 6;  radDm = 22 + j * 2; }   // birch    6.5-9.0 m
    case 3u: { trunkDm = 95 + j * 6;  radDm = 42 + j * 3; }   // great oak 9.5-12 m
    default: { trunkDm = 8;           radDm = 9 + j; }        // bush     ~0.8 m
  }
  t.trunk = trunkDm * VOX_PER_M / 10;
  t.radius = radDm * VOX_PER_M / 10;
  t.present = true;
  return t;
}

// Integer sine on a 256-step circle, returning -256..256. Bhaskara-style
// parabolic approximation, exact in integers — NO f32, because this feeds voxel
// placement and the whole sim/worldgen determinism argument (rule 1) rests on
// avoiding vendor-divergent float math. Max error vs. true sine is ~1.5%, which
// is a fraction of a voxel over a branch and identical on every machine.
fn isin(a : i32) -> i32 {
  let p = a & 255;                     // 0..255 == 0..2pi
  let half = p & 127;                  // 0..127 == 0..pi
  // parabola 4h(128-h)/128^2 peaks at 1 for h=64; scale to 256
  let v = (4 * half * (128 - half) * 256) / (128 * 128);
  return select(v, -v, p >= 128);
}

// ---- implicit branch skeleton (birch) ----
// Worldgen is a PURE PER-CELL FUNCTION: there is no place to grow a tree with a
// turtle and write voxels as it walks, because every voxel is evaluated on its
// own and a chunk may be generated in isolation. So branching is implicit —
// treeBranch() re-derives the same fixed skeleton from the tree's hash for
// every cell, and the cell tests its distance to each segment. Cost is bounded
// by construction (BIRCH_LIMBS * (1 + BIRCH_SUBS) segments, no recursion), and
// the whole thing is integer-only so it stays deterministic across vendors.
//
// Squared distance from point p to the segment a->b, all in voxels, times
// (len^2) to keep it integer: returns (d2 * denom, denom) so the caller can
// compare against a radius without dividing. i64 isn't available, so segments
// are kept short enough (< ~200 voxels) that the products stay inside i32:
// the worst term is len2 * len2 ~ (3*200^2)^2 — too big, so we instead project
// with a normalized-to-1024 parameter and accept the rounding. Rounding a
// branch axis by a fraction of a voxel is invisible and, crucially, identical
// on every machine.
fn segDist2(px : i32, py : i32, pz : i32,
            ax : i32, ay : i32, az : i32,
            bx : i32, by : i32, bz : i32) -> i32 {
  let vx = bx - ax; let vy = by - ay; let vz = bz - az;
  let wx = px - ax; let wy = py - ay; let wz = pz - az;
  let len2 = vx * vx + vy * vy + vz * vz;
  if (len2 <= 0) { return wx * wx + wy * wy + wz * wz; }
  // t in [0,1024]; dot can overflow only for absurdly long segments
  var tq = ((wx * vx + wy * vy + wz * vz) * 1024) / len2;
  tq = clamp(tq, 0, 1024);
  let cx = ax + (vx * tq) / 1024;
  let cy = ay + (vy * tq) / 1024;
  let cz = az + (vz * tq) / 1024;
  let ex = px - cx; let ey = py - cy; let ez = pz - cz;
  return ex * ex + ey * ey + ez * ez;
}

// A birch carries BIRCH_LIMBS primary limbs off the upper bole, each of which
// forks into BIRCH_SUBS twigs. Leaves live ONLY in a small blob at each twig
// tip — that is the whole look: bare white bark structure, green only at the
// extremities, nothing like a sphere on a stick.
const BIRCH_LIMBS : i32 = 5;
const BIRCH_SUBS  : i32 = 3;

// Direction for limb `i` of tree `t`, normalized to length ~256. Spread around
// the compass by golden-angle-ish stepping (integer approximation) so limbs
// never stack, with per-tree and per-limb hash jitter on azimuth and pitch.
//
// The vector MUST be normalized: callers scale it by (length / 256), so an
// un-normalized direction makes branch length depend on direction. The first
// cut built (cos*horiz, rise, sin*horiz) with horiz = 256 - rise, which gave a
// vector dominated by `rise` and a horizontal reach of ~11 voxels — the limbs
// hugged the bole and the tree rendered as a bare pole with a fork on top.
fn birchLimbDir(t : Tree, i : i32, gen : i32) -> vec3<i32> {
  let h = hash3(t.rnd ^ 0x5B12u, bitcast<u32>(i), bitcast<u32>(gen));
  // azimuth in 0..255 (a 256-step circle), stepped by ~137/360 of a turn
  let az = (i * 97 + i32(h % 24u) + i32(t.rnd >> 19u) * 3 + gen * 41) & 255;
  // Pitch as a 0..256 "how much of the direction is upward" weight. Primaries
  // sit near 45 degrees (the angle that actually reads as a branch); twigs
  // climb a bit more steeply so the crown gathers rather than splays flat.
  var up = 110 + i32((h >> 7u) % 70u);       // ~0.43..0.70 of unit, ~25-45 deg
  if (gen == 1) { up = 140 + i32((h >> 13u) % 80u); }
  // horizontal magnitude = sqrt(256^2 - up^2), by integer Newton iteration
  var hm = 256 - up / 2;                     // seed ('target' is reserved in WGSL)
  let hm2 = 256 * 256 - up * up;
  for (var it = 0; it < 4; it++) { hm = (hm + hm2 / max(hm, 1)) / 2; }
  let c = isin((az + 64) & 255);   // cos = sin(az + 90deg), in -256..256
  let s = isin(az);
  return vec3<i32>((c * hm) / 256, up, (s * hm) / 256);
}

// Material this tree contributes at world cell (x,y,z), or MAT_AIR.
fn treeCell(t : Tree, x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let dx = x - t.wx;
  let dz = z - t.wz;
  let dy = y - t.base;               // height above the trunk's ground
  if (dy < 0) { return MAT_AIR; }

  // Crown centre sits BELOW the trunk top by about a third of the radius, so
  // the trunk runs up into the foliage instead of holding a ball above itself.
  // Centring on t.trunk exactly leaves a bare pole with a hat on it, which is
  // what makes a voxel forest read as lollipops from any distance.
  let topY = t.trunk - t.radius / 3;
  let r = t.radius;

  // leaf material per species
  var leaf = M_LEAVES;
  if (t.species == 1u) { leaf = M_PINE; }
  // a slice of broadleaf stands go autumn, by tree not by voxel
  else if ((t.rnd >> 5u) % TUNE_AUTUMN_FRACTION == 0u) { leaf = M_AUTUMN; }
  var bark = M_WOOD;
  if (t.species == 2u) { bark = M_BIRCH; }

  // ---- trunk ----
  // Trunk radius is PROPORTIONAL to height (~1/22, so a 6 m oak gets a ~0.5 m
  // thick bole) and tapers toward the crown. Fixed small radii were the other
  // half of the shrub bug: a 1-voxel stem under a 6 m tree is a wire that
  // vanishes at any distance and leaves the canopy apparently floating.
  var tr = max(t.trunk / 22, 1);
  if (t.species == 2u) { tr = max(tr * 3 / 4, 1); }   // birch: slimmer
  if (t.species == 4u) { tr = 0; }                    // bush: single stem
  // taper: full width at the base, ~60% by the crown
  let taper = tr - (tr * dy * 2) / max(t.trunk * 5, 1);
  var trNow = max(taper, select(1, 0, t.species == 4u));
  // flared root buttress on the great oaks
  if (t.species == 3u && dy < t.trunk / 6) { trNow = trNow + tr / 2; }
  // Birch skips the straight box column: its bole is part of the branch
  // skeleton below, so it can lean and taper as one continuous structure.
  if (t.species != 2u && dy <= t.trunk && abs(dx) <= trNow && abs(dz) <= trNow) {
    // round off the corners so it isn't a visible square column
    if (abs(dx) + abs(dz) <= trNow + trNow / 2 + 1) { return bark; }
  }

  switch (t.species) {
    // ---- oak / great oak: blobby round crown, wider than tall ----
    case 0u, 3u: {
      let cy = topY;
      let vy = (dy - cy) * 3 / 2;              // squash vertically
      let d2 = dx * dx + dz * dz + vy * vy;
      if (d2 <= r * r) {
        // hash-nibbled edge so the crown silhouette is ragged, not a sphere
        let n = hash3(seed ^ 0x1EAFu, bitcast<u32>(x) ^ (bitcast<u32>(z) << 11u),
                      bitcast<u32>(y)) % 100u;
        let edge = d2 * 100 / max(r * r, 1);   // 0 centre .. 100 rim
        if (edge < 62 || n > u32(edge - 40)) { return leaf; }
      }
      // great oaks get a second, lower crown lobe for a layered canopy
      if (t.species == 3u) {
        let vy2 = (dy - (cy - r)) * 2;
        let d3 = dx * dx + dz * dz + vy2 * vy2;
        let r2 = r * 3 / 4;
        if (d3 <= r2 * r2) { return leaf; }
      }
      return MAT_AIR;
    }
    // ---- pine: stacked conical skirts, narrowing to a tip ----
    case 1u: {
      let tip = t.trunk + t.trunk / 8;         // crown overshoots the bole
      let start = t.trunk / 4;                 // bare lower trunk
      if (dy < start || dy > tip) { return MAT_AIR; }
      let up = tip - dy;                       // distance below the tip
      let span = max(tip - start, 1);
      // Cone radius grows linearly downward to the full crown radius, with a
      // saw-tooth so the skirts read as layered boughs. Both the slope and the
      // skirt period are derived from the tree's own size — the old fixed
      // `up / 3` and `up % 4` only made a cone at all on a 16-voxel sapling.
      let skirt = max(t.trunk / 10, 2);
      var cr = (up * r) / span + (up % skirt) / 2 - skirt / 4;
      cr = clamp(cr, 0, r);
      let md = abs(dx) + abs(dz);              // diamond cross-section
      if (md <= cr) { return leaf; }
      return MAT_AIR;
    }
    // ---- birch: generative branching structure ----
    // Not a crown at all: a leaning bole that forks into limbs, each of which
    // forks again into twigs, with leaves ONLY as small clusters at the twig
    // tips. What you should see is white bark tracery with green confetti at
    // the extremities — the deliberate opposite of the lollipop the round
    // crown produced. Everything is re-derived from t.rnd per cell.
    case 2u: {
      // + r again for the leaf cluster carried on top of the highest twig tip
      if (dy > t.trunk + r * 2) { return MAT_AIR; }

      // Bole: a 3-segment polyline that drifts as it rises, so the trunk has a
      // natural lean and slight S-curve instead of being a plumb column.
      let leanH = hash3(t.rnd ^ 0xB01Eu, 1u, 0u);
      let lx = i32(leanH % 17u) - 8;           // total drift, voxels, over the bole
      let lz = i32((leanH >> 8u) % 17u) - 8;
      // Fork height: where the bole stops being a single stem. Kept low (just
      // under half) so the branch structure is most of the tree's visible mass
      // — a high fork leaves a bare pole, which is the silhouette this whole
      // rewrite exists to kill.
      let forkY = t.trunk * 9 / 20;
      let topBole = t.trunk;

      var btr = max(t.trunk / 30, 1);          // birch is a slim tree
      // bole point at height h (0..topBole), drifting quadratically
      // p(h) = base + lean * (h/topBole)^2
      let hq = (dy * 1024) / max(topBole, 1);
      let bxAt = (lx * hq * hq) / (1024 * 1024);
      let bzAt = (lz * hq * hq) / (1024 * 1024);
      // bole radius tapers from btr at the ground to ~1 at the top
      let boleR = max(btr - (btr * dy) / max(topBole, 1) + select(0, 1, dy < topBole / 8), 1);
      if (dy <= topBole) {
        let ex = dx - bxAt; let ez = dz - bzAt;
        if (ex * ex + ez * ez <= boleR * boleR) { return bark; }
      }

      // Limbs branch off between forkY and the bole top, climbing outward.
      // Each limb is one segment; each spawns BIRCH_SUBS twigs from its far
      // end. Leaves are tested first at the twig tips, then the wood — so a
      // tip cluster reads as foliage rather than bark poking through it.
      // Limb length. r + r/2 made limbs that shot out like scaffolding poles,
      // longer than the tree was wide; the crown has to stay narrower than its
      // height or the birch stops reading as a slender tree.
      let limbLen = r;
      for (var i = 0; i < BIRCH_LIMBS; i++) {
        let lh = hash3(t.rnd ^ 0xC0DEu, bitcast<u32>(i), 7u);
        // attachment height, spread up the upper bole
        let ah = forkY + ((topBole - forkY) * i) / BIRCH_LIMBS
                 + i32(lh % u32(max((topBole - forkY) / BIRCH_LIMBS, 1)));
        let ahq = (ah * 1024) / max(topBole, 1);
        let ax = (lx * ahq * ahq) / (1024 * 1024);
        let az = (lz * ahq * ahq) / (1024 * 1024);
        let d0 = birchLimbDir(t, i, 0);
        // limb length shrinks with attachment height: lower limbs are longest
        let ll = limbLen - (limbLen * (ah - forkY)) / max((topBole - forkY) * 2, 1);
        // A limb is TWO segments, not one: it leaves the bole climbing and then
        // bends over toward horizontal at the elbow. A single straight segment
        // is what made the first working version read as scaffolding poles —
        // real branches curve, and the bend is most of what sells it.
        let mx = ax + (d0.x * ll) / (256 * 2);
        let my = ah + (d0.y * ll) / (256 * 2);
        let mz = az + (d0.z * ll) / (256 * 2);
        // outer half keeps the horizontal run but sheds most of the climb
        let ex = mx + (d0.x * ll) / (256 * 2);
        let ey = my + (d0.y * ll) / (256 * 5);
        let ez = mz + (d0.z * ll) / (256 * 2);

        // cheap AABB reject for the whole limb + its twigs before any distance
        // work: twigs extend at most twigLen past the limb end, plus a cluster
        let pad = ll * 3 / 4 + r / 5 + 4;
        if (dx < min(ax, ex) - pad || dx > max(ax, ex) + pad ||
            dz < min(az, ez) - pad || dz > max(az, ez) + pad ||
            dy < min(ah, ey) - pad || dy > max(ah, ey) + pad) { continue; }

        // Twigs. One set sprouts from the ELBOW and one from the TIP, so
        // foliage is distributed along the limb instead of bunching in a knot
        // at the far end and leaving a long bare arm behind it.
        let twigLen = ll / 2 + ll / 4;
        for (var k = 0; k < BIRCH_SUBS * 2; k++) {
          let sub = k % BIRCH_SUBS;
          let fromTip = k >= BIRCH_SUBS;
          let d1 = birchLimbDir(t, i * 8 + sub + 1, 1);
          // blend the twig direction toward the parent limb so it continues
          // the branch rather than starting a new random spray
          let tdx = (d1.x + d0.x) / 2;
          let tdy = (d1.y + d0.y) / 2;
          let tdz = (d1.z + d0.z) / 2;
          // elbow twigs are shorter — they are lower-order branches
          let tl = select(twigLen * 2 / 3, twigLen, fromTip);
          let sx = select(mx, ex, fromTip);
          let sy = select(my, ey, fromTip);
          let sz = select(mz, ez, fromTip);
          let tx2 = sx + (tdx * tl) / 256;
          let ty2 = sy + (tdy * tl) / 256;
          let tz2 = sz + (tdz * tl) / 256;

          // Leaf cluster at the twig tip — a small hash-eroded blob, the ONLY
          // place this species puts foliage. Kept SMALL on purpose: at r*2/5
          // (~1 m) the fifteen clusters merged back into the solid ball this
          // rewrite exists to avoid. ~0.4 m reads as a tuft on a branch tip.
          let cr = max(r / 5, 4);
          let ddx = dx - tx2; let ddy = dy - ty2; let ddz = dz - tz2;
          let cd2 = ddx * ddx + ddy * ddy + ddz * ddz;
          if (cd2 <= cr * cr) {
            let n = hash3(seed ^ 0x81C4u,
                          bitcast<u32>(x) ^ (bitcast<u32>(z) << 11u),
                          bitcast<u32>(y)) % 100u;
            // Solid at the blob core, ragged only at its rim. Eroding the core
            // too (the first cut cut ~25% everywhere) made the clusters read as
            // green dust at any distance instead of as foliage.
            let edge = (cd2 * 100) / max(cr * cr, 1);
            if (edge < 45 || n > u32(edge - 20)) { return leaf; }
          }
          // The twig itself. A 1-voxel radius twig is a wire that aliases into
          // a dotted line and disappears a few metres out; 2 keeps it readable.
          if (segDist2(dx, dy, dz, sx, sy, sz, tx2, ty2, tz2) <= 4) {
            return bark;
          }
        }

        // The limb, as its two segments: thicker on the inner half near the
        // bole, thinner past the elbow, so it visibly tapers outward.
        if (segDist2(dx, dy, dz, ax, ah, az, mx, my, mz) <= 4) { return bark; }
        if (segDist2(dx, dy, dz, mx, my, mz, ex, ey, ez) <= 4) { return bark; }
      }
      return MAT_AIR;
    }
    // ---- bush: a low leafy dome ----
    default: {
      let vy = (dy - 1) * 2;
      if (dx * dx + dz * dz + vy * vy <= r * r) { return leaf; }
      return MAT_AIR;
    }
  }
}

// Union of every tree whose canopy can reach (x,y,z): the (2*TREE_SCAN+1)^2
// tile neighborhood, which must cover the largest canopy radius (great oak,
// ~4.5 m = 72 voxels) plus the trunk's in-tile jitter. First non-air wins —
// order is by tile index, a fixed priority, never dispatch order (rule 1).
fn treeAt(x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN; oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN; ox <= TREE_SCAN; ox++) {
      let t = treeInfo(tx + ox, tz + oz, seed);
      if (!t.present) { continue; }
      // cheap reject before the per-species shape test. Birch is a branching
      // skeleton, not a crown: its limbs reach r + r/2 and their twigs extend
      // past that, so it needs a wider gate than the round-crown species or
      // the outer branches get sliced off at an invisible cylinder.
      var reach = t.radius + 2;
      if (t.species == 2u) { reach = t.radius * 5 / 2 + 4; }
      if (abs(x - t.wx) > reach || abs(z - t.wz) > reach) {
        continue;
      }
      // Vertical extent must cover the tallest thing the species can put above
      // its bole: the pine tip overshoots the trunk by trunk/8, and round
      // crowns reach topY + r. Clipping this is how canopies get flat tops.
      // Birch twig tips climb to ~trunk + r and then carry a leaf cluster on
      // top of that, so it needs the extra cluster radius or every birch gets
      // its uppermost foliage sheared off in a flat plane.
      var vtop = t.base + t.trunk + t.radius + 2;
      if (t.species == 2u) { vtop = vtop + t.radius; }
      if (y < t.base || y > vtop) { continue; }
      let m = treeCell(t, x, y, z, seed);
      if (m != MAT_AIR) { return m; }
    }
  }
  return MAT_AIR;
}

// Caves: COLUMN BANDS carved by 2D noise — for each (x,z) inside a cavern
// mask, one contiguous vertical span is removed. Unlike 3D-threshold carving
// this cannot create free-floating stone blobs (stone above/below a band is
// horizontally connected to full columns at the mask boundary), which matters
// because the island detector would correctly-but-noisily convert generated
// floaters into debris the moment anything moved nearby.
// Returns 0 = solid, 1 = carve to air, 2 = carve to lava (deep cavern floors).
// Cavern masks scale horizontally like everything else (a 40-voxel mask cell
// made 2.5 m caves); the vertical spans grow only ~2x, matching the hills, so
// a cavern is a passage you walk through rather than a crawl space. The
// 10-voxel surface shell becomes 40 (2.5 m) so caves can't breach the new,
// thicker soil layer from below.
fn caveAt(x : i32, y : i32, z : i32, h : i32, seed : u32) -> i32 {
  // band 1: near-surface caverns following the terrain
  let m1 = vnoise(x, z, 40 * HSCALE, seed ^ 5u);
  if (m1 > i32(TUNE_CAVE_THRESHOLD1)) {
    let f1 = h - 40 - (vnoise(x, z, 32 * HSCALE, seed ^ 6u) * 60) / 255;
    let c1 = min(f1 + 10 + (vnoise(x, z, 12 * HSCALE, seed ^ 7u) * 20) / 255,
                 h - 40);
    if (y >= f1 && y <= c1) { return 1; }
  }
  // band 2: deep caverns at absolute depth (streamed depth is real terrain)
  let m2 = vnoise(x + 7717, z - 4177, 48 * HSCALE, seed ^ 8u);
  if (m2 > i32(TUNE_CAVE_THRESHOLD2)) {
    let f2 = -40 - (vnoise(x, z, 40 * HSCALE, seed ^ 9u) * 70) / 255;
    let c2 = f2 + 12 + (vnoise(x, z, 16 * HSCALE, seed ^ 10u) * 26) / 255;
    if (y >= f2 && y <= c2 && y <= h - 40) {
      if (y <= f2 + 2 && m2 > 190) { return 2; }  // lava pools where mask peaks
      return 1;
    }
  }
  return 0;
}

fn genCell(c : vec3<i32>, seed : u32) -> u32 {
  let x = c.x; let y = c.y; let z = c.z;
  var h = baseHeight(x, z, seed);
  var mat = MAT_AIR;
  var fluidTop = -1;     // top of any standing fluid at this column
  var fluid = MAT_AIR;

  // ---- authored origin-area set pieces (absolute world coords) ----
  // Halved in the third scale pass (radii 136/64/48 -> 68/32/24): a swimmable
  // ~8.5 m lake, a 4 m oil pond, a 3 m lava pool. Depths kept — halving depth
  // too would leave water too shallow to submerge in. Floor/surface heights
  // anchor to POOL_Y, and they sit outside the spawn clearing so they don't
  // disturb the fixtures.
  let poolY = 44;
  // Water lake at (420,420), ~8.5 m across
  let pdx = x - 420; let pdz = z - 420;
  let pd2 = pdx * pdx + pdz * pdz;
  if (pd2 < 68 * 68) {
    h = poolY;
    fluid = M_WATER; fluidTop = poolY + 24;
  } else if (pd2 < 80 * 80) {
    h = max(h, poolY + 26);    // containment rim
  }
  // Oil pond at (260,300), ~4 m across
  let odx = x - 260; let odz = z - 300;
  let od2 = odx * odx + odz * odz;
  if (od2 < 32 * 32) {
    h = poolY + 6;
    fluid = M_OIL; fluidTop = poolY + 24;
  } else if (od2 < 42 * 42) {
    h = max(h, poolY + 26);
  }
  // Lava pool at (220,520), ~3 m across
  let ldx = x - 220; let ldz = z - 520;
  let ld2 = ldx * ldx + ldz * ldz;
  if (ld2 < 24 * 24) {
    h = poolY + 2;
    fluid = M_LAVA; fluidTop = poolY + 20;
  } else if (ld2 < 34 * 34) {
    h = max(h, poolY + 22);
  }

  let inPoolFloor = pd2 < 68 * 68 || od2 < 32 * 32 || ld2 < 24 * 24;
  let inRim = pd2 < 80 * 80 || od2 < 42 * 42 || ld2 < 34 * 34;

  // ---- biome + pond (pondAt's keep-out list excludes the pool areas, so a
  // disc never overlaps a rim; the fluidTop<0 check is belt-and-braces) ----
  let biome = biomeAt(x, z, seed);
  var pond = -1;
  let pw = pondAt(x, z, seed);
  if (pw.y >= 0) {
    pond = pw.y;
    h = min(h, pw.x);                  // carve the bowl into the terrain
    if (fluidTop < 0) { fluid = M_WATER; fluidTop = pw.y; }
  }

  if (y <= h) {
    let submerged = pond >= 0;
    if (!inPoolFloor && h >= TREELINE && y > h - 2) {
      mat = M_SNOW;                        // snow caps on the high hills
    } else if (inPoolFloor) {
      mat = M_STONE;
    } else if (submerged && y > h - 3) {
      mat = M_SAND;                        // sandy pond bed
    } else if ((biome == B_DESERT || onFixturePad(x, z)) && y > h - 4) {
      mat = M_SAND;                        // loose cap — avalanches into repose piles
    } else if (y == h) {
      mat = M_GRASS;                       // forest floor: one grass skin (SOLID)
    } else {
      // NOTE: no loose-dirt layer under the grass. `dirt` is a powder, and a
      // 3-4 voxel powder shell under a solid skin avalanches out from under the
      // grass on every slope the moment the world wakes — the whole surface
      // creeps and chunks never sleep. Stone directly under the skin keeps the
      // forest floor static; digging still exposes stone, and the `rubble`
      // field turns grass into dirt when it IS broken.
      mat = M_STONE;
    }
    // depth is real now (no bedrock): caves carve the stone body, with lava
    // pooling on deep cavern floors. No caves under the authored pools/rims —
    // a cave breaching a rim column drains the pool through the tunnel system
    // and the world never settles.
    if (mat == M_STONE && !inRim) {
      let cv = caveAt(x, y, z, h, seed);
      if (cv == 1) { mat = MAT_AIR; }
      else if (cv == 2) { mat = M_LAVA; }
    }
  } else if (fluidTop >= 0 && y <= fluidTop) {
    mat = fluid;
  }

  // ---- surface cover: trees, then ground flora ----
  // Only above ground and out of the water, and never inside the authored rims
  // (a tree rooted on a pool rim would drop leaves into the pool).
  if (mat == MAT_AIR && !inRim && y > h && h < TREELINE && pond < 0) {
    let tm = treeAt(x, y, z, seed);
    if (tm != MAT_AIR) { mat = tm; }
  }

  // Ground flora on the grass skin: flower clumps in meadows, sparse elsewhere.
  // Inert petal/grass, placed only in the one voxel above the surface, so this
  // costs a settled world nothing.
  // The fixture pads stay bare for the same reason they stay sandy: a single
  // grass tuft above the surface is a SOLID voxel the selftest's dropped bodies
  // come to rest on, which lifts them a voxel and re-geometries the burn.
  if (mat == MAT_AIR && y == h + 1 && !inRim && pond < 0 && h < TREELINE &&
      biome != B_DESERT && !onFixturePad(x, z)) {
    let fr = hash3(seed ^ 0xF10Eu, bitcast<u32>(x), bitcast<u32>(z));
    // clump mask: flowers grow in patches, not as uniform static
    let clump = vnoise(x, z, 24 * HSCALE, seed ^ 0xF11Eu);
    // Tuned down deliberately: at ~15% coverage the flowers read as confetti
    // sprayed over the whole map rather than as patches in a meadow. Keeping
    // them inside the clump mask and rare outside it is what makes finding a
    // flowery clearing feel like finding something.
    var thresh = 0u;
    if (biome == B_MEADOW) { thresh = select(6u, 60u, clump > 165); }
    else                   { thresh = select(2u, 16u, clump > 190); }
    let roll = fr % 1000u;
    if (roll < thresh) {
      mat = select(M_PETAL, M_GRASS, (fr >> 11u) % 4u != 0u);  // mostly tufts
    }
  }

  // Reactive seeds are deliberately NOT scattered by worldgen any more.
  //
  // A seed sprouts a stem that races hardening against growth, branches, and
  // blooms — on the old open desert those read as occasional garden accents.
  // In a forest they don't: the stalks grow taller than the oaks, they are
  // brightly striped where everything else is green, and because they are the
  // only MOVING thing in view the eye goes straight to them. Even at 1/4000
  // they were what the world looked like. The garden is still fully intact and
  // one brush stroke away (and reactions.json is untouched) — it just isn't the
  // default overworld any more. Placing them is now a player/POI decision.
  //
  // This also removes the last worldgen-placed growth source, which is why a
  // settled world now reports 0 active chunks instead of a handful.

  // Wood platform on pillars near spawn (authored POI). ~2.5 m square deck on
  // 4 posts. Footprint halved in the third scale pass; deck HEIGHT stays 3 m
  // so the player (1.7 m) still walks under it. Anchored to the local terrain.
  let deckY = baseHeight(166, 166, seed) + 48;
  let onPillar = (abs(x - 148) <= 2 || abs(x - 184) <= 2) &&
                 (abs(z - 148) <= 2 || abs(z - 184) <= 2) && y <= deckY;
  let inSlab = x >= 146 && x <= 186 && z >= 146 && z <= 186 &&
               y >= deckY && y <= deckY + 3;
  if ((onPillar && y > h) || inSlab) {
    mat = M_WOOD;
  }

  // Procedural ruin POIs: one hollow stone building per ~5th tile, placed by
  // tile hash. Building halved with the world: ~3.5 m square, 3 m tall — a
  // hut, not a hall. The 2 m doorway is NOT halved: it has to clear the 1.7 m
  // player, which is exactly the mouse-hole mistake the first cut made.
  let ruinTile = 256 * HSCALE;
  let tx = fdiv(x, ruinTile); let tz = fdiv(z, ruinTile);
  if (tx != 0 || tz != 0) {
    let rh = hash3(seed ^ 0xA111CEu, bitcast<u32>(tx), bitcast<u32>(tz));
    if (rh % TUNE_RUIN_CHANCE == 0u) {
      let rw = 56;                        // 3.5 m footprint
      let rht = 48;                       // 3 m to the roof
      // keep the whole footprint inside the tile whatever HSCALE is
      let margin = 32;
      let jit = u32(max(ruinTile - rw - margin * 2, 1));
      let rx = tx * ruinTile + margin + i32((rh >> 8u) % jit);
      let rz = tz * ruinTile + margin + i32((rh >> 16u) % jit);
      // box test in XZ first: baseHeight for the centre only when close
      if (x >= rx && x < rx + rw && z >= rz && z < rz + rw) {
        let ry = baseHeight(rx + rw / 2, rz + rw / 2, seed);
        if (y >= ry && y < ry + rht) {
          let shellXZ = x < rx + 4 || x >= rx + rw - 4 ||
                        z < rz + 4 || z >= rz + rw - 4;
          let shellY = y >= ry + rht - 4;
          // doorway: 2 m tall, 1.5 m wide, centred on the -x wall
          let door = y < ry + 32 && abs(z - (rz + rw / 2)) <= 12 && x < rx + 4;
          if ((shellXZ || shellY) && !door) { mat = M_STONE; }
          else if (!shellXZ) { mat = select(mat, MAT_AIR, y > ry); }  // hollow
        }
      }
    }
  }

  if (mat == MAT_AIR) { return 0u; }
  let rnd = hash3(seed ^ 0xC0FFEEu,
                  bitcast<u32>(x) ^ (bitcast<u32>(z) << 12u), bitcast<u32>(y));
  // liquids are born full (state nibble = fullness); solids get palette jitter
  var state = rnd % 3u;
  if (mat == M_WATER || mat == M_OIL || mat == M_LAVA) { state = LIQ_FULL_STATE; }
  return packVox(mat, state, 0xFFu);
}

// ---- far-field surface skin (phase 4: distance look) ----
// Terrain surface height for a column, mirroring genCell's `h` EXACTLY
// (baseHeight + the authored pool floor/rim overrides, same order). Keep the
// two in sync — a divergence shows up as mis-colored far terrain, not a crash.
fn surfHeightAt(x : i32, z : i32, seed : u32) -> i32 {
  var h = baseHeight(x, z, seed);
  let poolY = 44;
  let pdx = x - 420; let pdz = z - 420;
  let pd2 = pdx * pdx + pdz * pdz;
  if (pd2 < 68 * 68) { h = poolY; }
  else if (pd2 < 80 * 80) { h = max(h, poolY + 26); }
  let odx = x - 260; let odz = z - 300;
  let od2 = odx * odx + odz * odz;
  if (od2 < 32 * 32) { h = poolY + 6; }
  else if (od2 < 42 * 42) { h = max(h, poolY + 26); }
  let ldx = x - 220; let ldz = z - 520;
  let ld2 = ldx * ldx + ldz * ldz;
  if (ld2 < 24 * 24) { h = poolY + 2; }
  else if (ld2 < 34 * 34) { h = max(h, poolY + 22); }
  // natural disc ponds carve their bowl into the terrain (mirrors genCell)
  let pw = pondAt(x, z, seed);
  if (pw.y >= 0) { h = min(h, pw.x); }
  return h;
}

// XZ canopy footprint, ignoring height: which tree crown (if any) covers this
// column. At coarse cascade levels a whole tree is thinner than one cell, so
// the center sample loses it and distant forest degraded into bare grass; the
// far field flattens crowns into the terrain skin instead — the horizon keeps
// its canopy color even where no individual tree survives sampling. Pure
// function of (coords, seed), same as everything the sieve uses.
fn treeCanopyAt(x : i32, z : i32, seed : u32) -> u32 {
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN; oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN; ox <= TREE_SCAN; ox++) {
      let t = treeInfo(tx + ox, tz + oz, seed);
      if (!t.present || t.species == 4u) { continue; }   // bushes: too small
      let dx = x - t.wx; let dz = z - t.wz;
      // Birch spreads its leaf clusters out at the twig tips rather than
      // filling a disc, so its far-field footprint is a wider but sparser
      // ring; approximated as a larger disc with a hash punch-out so distant
      // birch stands stay airy instead of reading as solid canopy.
      if (t.species == 2u) {
        let br = t.radius * 2;
        if (dx * dx + dz * dz > br * br) { continue; }
        if (hash3(seed ^ 0x2B17u, bitcast<u32>(x), bitcast<u32>(z)) % 5u < 2u) {
          continue;
        }
      } else if (dx * dx + dz * dz > t.radius * t.radius) { continue; }
      if (t.species == 1u) { return M_PINE; }
      if ((t.rnd >> 5u) % TUNE_AUTUMN_FRACTION == 0u) { return M_AUTUMN; }
      return M_LEAVES;
    }
  }
  return MAT_AIR;
}

// Far-field cell material rule, shared VERBATIM by the sieve (`far`, pristine
// procgen) and the downsample (`fardown`, live grid). The center sample
// decides SHAPE (occupancy); this decides COLOR: a cell that straddles the
// terrain surface takes the surface SKIN material (grass/sand/snow — whatever
// genCell puts at y == h) instead of whatever body material the center sample
// happened to land on. Without it, a coarse cell whose center sits one voxel
// under the 1-voxel grass skin stores STONE, and every distant hillside reads
// as gray rock with green contour stripes where the sampling aligns — the
// dominant artifact in the v0.5.4 far view.
//
// Both entry points call this same pure function of (mat, coords, level,
// seed), so downsampled and pristine regions still agree exactly at their
// boundaries (the invariant the `far downsample` selftest gate protects).
fn farSurfaceMat(mat : u32, fine : vec3<i32>, shift : u32, seed : u32) -> u32 {
  let k = materials[mat].klass;
  if (k != CLASS_SOLID && k != CLASS_POWDER) { return mat; }  // fluids keep their ID
  let h = surfHeightAt(fine.x, fine.z, seed);
  // "Topmost solid cell of this column": solid means center <= h, and the cell
  // above (center + 2^shift) samples past h. NOT "cell span contains h" — when
  // h lands in a cell's lower half that cell's center samples air (the cell is
  // empty) and the visible top face belongs to the cell BELOW, whose span does
  // not contain h. The span version left half of all surface cells stone-gray.
  if (fine.y > h || h >= fine.y + (1 << shift)) { return mat; }
  // canopy flattening only where cells are 2 m+ (32+ fine voxels); finer
  // levels still resolve trees as shapes and double-painting would fatten them
  if (shift >= 5u) {
    let can = treeCanopyAt(fine.x, fine.z, seed);
    if (can != MAT_AIR) { return can; }
  }
  let skin = genCell(vec3<i32>(fine.x, h, fine.z), seed) & 0xFFFu;
  // hollow ruin interiors can return air at y == h; keep the body mat then
  if (skin == MAT_AIR || materials[skin].klass == CLASS_GAS) { return mat; }
  return skin;
}

var<workgroup> wgCount : atomic<u32>;
var<workgroup> wgBlock : atomic<u32>;

fn genChunk(slot : u32, li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
  }
  workgroupBarrier();

  let sc = vec3<i32>(vec3<u32>(slot % NCHUNK, (slot / NCHUNK) % NCHUNK,
                               slot / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  var count = 0u;
  var block = 0u;
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    let l = vec3<i32>(vec3<u32>(i % CHUNK, (i / CHUNK) % CHUNK, i / (CHUNK * CHUNK)));
    let w = genCell(base + l, T.seed);
    voxels[slot * CHUNK_VOL + i] = w;
    let m = w & 0xFFFu;
    if (m != MAT_AIR) {
      count += 1u;
      if (isRayBlocker(materials[m])) { block += 1u; }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  workgroupBarrier();

  if (li == 0u) {
    let n = atomicLoad(&wgCount);
    // in-kernel so the renderer never sees a stale 0
    occupancy[slot] = packOcc(n, atomicLoad(&wgBlock));
    if (n > 0u) {          // wake once; loose material settles, then sleeps
      atomicStore(&dirtyIn[slot], 1u);
      atomicStore(&dirtyOut[slot], 1u);
    } else {
      atomicStore(&dirtyIn[slot], 0u);
      atomicStore(&dirtyOut[slot], 0u);
    }
  }
}

// Full residency window: NUM_CHUNKS workgroups.
@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  genChunk(wg.x, li);
}

// Streamed-in chunks: T.genCount slot indices from genList.
@compute @workgroup_size(64)
fn list(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  if (wg.x >= T.genCount) { return; }
  genChunk(genList[wg.x], li);
}

// ---- far-field cascade fill: the worldgen "sieve" ----
// (render-only LOD — DESIGN.md §9, docs/PLAN_far_field_cascades.md)
// Lives in this file to share genCell(): a level-k cascade cell is filled by
// sampling genCell at the FINE-voxel center of the 2^k-wide region it covers,
// so cascades regenerate bit-identically from (coords, seed) at any stride.
// Gases are dropped (no media in the far field); liquids keep their ID and
// render as opaque surfaces at distance. Features thinner than a coarse cell
// vanish — correct LOD behavior, not data loss.
//
// One workgroup per farList entry: (level-1) << 12 | chunk slot. Each thread
// owns 64 CONSECUTIVE cells = 16 whole u32 words of the byte-packed farVox,
// so there are no partial-word writes and no atomics on the voxel data.
// farVox/farOcc are declared ATOMIC because the phase-2 downsample entry
// (`fardown`, below) does partial-WORD updates: a level-k word packs 4 material
// bytes spanning 4*2^k fine voxels, which at k >= 2 is wider than one 16-voxel
// fine chunk, so two workgroups can target different bytes of one word. WGSL
// forbids declaring one buffer both atomic and non-atomic in a module, so the
// full-word stores here became atomicStore (uncontended — free in practice).
// Legal because cascades are render-only derived data: no determinism
// requirement attaches to them (DESIGN.md §9).
@group(1) @binding(0) var<storage, read_write> farVox : array<atomic<u32>>;
@group(1) @binding(1) var<storage, read_write> farOcc : array<atomic<u32>>;
@group(1) @binding(2) var<storage, read> farList : array<u32>;
@group(1) @binding(3) var<uniform> F : FarParams;
@group(1) @binding(4) var<storage, read> farDirty : array<u32>;

var<workgroup> wgFarCount : atomic<u32>;

@compute @workgroup_size(64)
fn far(@builtin(workgroup_id) wg : vec3<u32>,
       @builtin(local_invocation_index) li : u32) {
  if (wg.x >= T.farCount) { return; }
  if (li == 0u) { atomicStore(&wgFarCount, 0u); }
  workgroupBarrier();

  let packed = farList[wg.x];
  let level = (packed >> 12u) + 1u;   // 1-based
  let slot = packed & 0xFFFu;
  let sc = vec3<i32>(vec3<u32>(slot % FAR_NCHUNK, (slot / FAR_NCHUNK) % FAR_NCHUNK,
                               slot / (FAR_NCHUNK * FAR_NCHUNK)));
  // base LEVEL-cell coord of this level chunk (origins are level-chunk units)
  let base = farSlotToChunk(sc, F.origins[level - 1u].xyz) * i32(CHUNK);
  let shift = farCellShift(level);   // fine voxels per cell, as a shift

  var count = 0u;
  let wordBase = ((level - 1u) * FAR_VOX + slot * CHUNK_VOL) / 4u + li * 16u;
  for (var wi = 0u; wi < 16u; wi++) {
    var word = 0u;
    for (var b = 0u; b < 4u; b++) {
      let i = li * 64u + wi * 4u + b;
      let l = vec3<i32>(vec3<u32>(i % CHUNK, (i / CHUNK) % CHUNK,
                                  i / (CHUNK * CHUNK)));
      let cc = base + l;   // level-cell coords
      // the sieve: fine-voxel center of the 2^shift-wide region this cell covers
      let fine = (cc << vec3<u32>(shift)) + vec3<i32>(1 << (shift - 1u));
      let mat = genCell(fine, T.seed) & 0xFFFu;
      var byteV = 0u;
      if (mat != MAT_AIR && materials[mat].klass != CLASS_GAS) {
        // shape from the center sample, color from the surface skin (phase 4)
        byteV = min(farSurfaceMat(mat, fine, shift, T.seed), 255u);
        count += 1u;
      }
      word |= byteV << (b * 8u);
    }
    atomicStore(&farVox[wordBase + wi], word);
  }
  atomicAdd(&wgFarCount, count);
  workgroupBarrier();
  if (li == 0u) {
    atomicStore(&farOcc[(level - 1u) * FAR_NUM_CHUNKS + slot],
                atomicLoad(&wgFarCount));
  }
}

// ---- far-field cascade downsample: edits at distance (plan phase 2) ----
// The sieve above fills cascades from PRISTINE procgen, so a crater dug inside
// the residency window vanished the moment the player walked away. This entry
// re-derives, from the LIVE voxel grid, every cascade cell whose sample point
// lies inside a fine chunk that changed this tick — so edits leave a
// downsampled ghost in the cascades and eviction needs no special handling.
//
// Same sample rule as the sieve, deliberately: a level-k cell is the material
// of the single fine voxel at the CENTER of the 2^k region it covers. Sampling
// the same point from live data means edited and pristine regions agree
// exactly at their boundary (no seam where a refilled plane meets a
// downsampled chunk). Only cells whose center voxel lands in this chunk are
// touched; at k >= 5 one level cell is wider than a chunk, so most chunks
// contribute to no cell at those levels — correct, the owning chunk does it.
//
// Dispatch: indirect, one workgroup per entry of the compacted dirty list
// (farDirty == world.dirtyList after compactNext) — cost scales with activity,
// a settled world runs zero workgroups (CLAUDE.md rule 2).
//
// Races: a level-k word packs 4 cells = 4*2^k fine voxels of x-extent, wider
// than a 16-voxel chunk for k >= 2, so neighboring chunks' workgroups write
// different bytes of one word concurrently. Byte writes are therefore
// atomicAnd(clear) + atomicOr(set); farOcc gets atomicMax(.,1) — deliberately
// conservative, never falsely zero (a stale over-estimate only costs marching
// an empty level chunk; a stale zero would make new terrain invisible).
// Smallest sample point >= b on one axis: centers are c = m*step + half, so
// m = ceil((b - half) / step) — floor division because b goes negative.
fn farFirstCenter(b : i32, step : i32, half : i32) -> i32 {
  return fdiv(b - half + step - 1, step) * step + half;
}
// How many of those centers land in [b, b + CHUNK). c0 >= b by construction,
// so the span is never negative before the clamp.
fn farCenterCount(b : i32, c0 : i32, step : i32) -> i32 {
  let span = b + i32(CHUNK) - c0;
  if (span <= 0) { return 0; }
  return (span + step - 1) / step;
}

@compute @workgroup_size(64)
fn fardown(@builtin(workgroup_id) wg : vec3<u32>,
           @builtin(local_invocation_index) li : u32) {
  // world fine-voxel base of the dirty chunk this workgroup owns
  let slot = farDirty[wg.x];
  let sc = vec3<i32>(vec3<u32>(slot % NCHUNK, (slot / NCHUNK) % NCHUNK,
                               slot / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);

  for (var level = 1u; level <= FAR_LEVELS; level++) {
    let shift = farCellShift(level);
    let step = 1 << shift;          // fine voxels per level cell, per axis
    let half = 1 << (shift - 1u);   // center offset inside the cell
    // First sample point >= base on each axis (centers sit at m*step + half),
    // and how many of them fall inside this chunk's 16-voxel span.
    let first = vec3<i32>(farFirstCenter(base.x, step, half),
                          farFirstCenter(base.y, step, half),
                          farFirstCenter(base.z, step, half));
    let n = vec3<i32>(farCenterCount(base.x, first.x, step),
                      farCenterCount(base.y, first.y, step),
                      farCenterCount(base.z, first.z, step));
    let total = u32(n.x * n.y * n.z);
    let origin = F.origins[level - 1u].xyz;
    for (var i = li; i < total; i += 64u) {
      let ix = i32(i) % n.x;
      let iy = (i32(i) / n.x) % n.y;
      let iz = i32(i) / (n.x * n.y);
      // fine-voxel sample point, and the level cell it belongs to
      let fine = first + vec3<i32>(ix, iy, iz) * step;
      let cc = fine >> vec3<u32>(shift);
      if (!farInBox(cc, origin)) { continue; }   // outside this cascade level

      // live grid (the sample point is inside this chunk, hence resident)
      let mat = voxels[cellIndexW(fine)] & 0xFFFu;
      var byteV = 0u;
      if (mat != MAT_AIR && materials[mat].klass != CLASS_GAS) {
        // Same skin rule as the sieve — the skin is looked up from PRISTINE
        // procgen (genCell), so a pristine chunk downsamples bit-identically
        // to the sieve's fill. An edited surface keeps its pristine skin color
        // while the cell's center voxel survives; the moment the center voxel
        // is dug away the cell empties for real. A slightly stale rim color is
        // invisible at cascade distances; a seam between refilled planes and
        // downsampled chunks is not.
        byteV = min(farSurfaceMat(mat, fine, shift, T.seed), 255u);
      }
      let bi = farVoxByteIndex(level, cc);
      let shift = (bi & 3u) * 8u;
      atomicAnd(&farVox[bi >> 2u], ~(0xFFu << shift));
      atomicOr(&farVox[bi >> 2u], byteV << shift);
      if (byteV != 0u) {
        atomicMax(&farOcc[farOccIndex(level, cc)], 1u);
      }
    }
  }
}
