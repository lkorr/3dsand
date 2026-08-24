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
// JITTER materialization list: (slot, sentinel entry) pairs. Its OWN buffer,
// never genList — Stream::FillSlots writes genList mid-frame while a page fill
// drains at the head of the next command buffer, and the deferred writes
// interleave (see world.cpp).
@group(0) @binding(19) var<storage, read> pageFillList : array<u32>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;

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
// ---- aquatic plants (materials.json ids 59..62) ----
const M_LILYPAD : u32 = 59u;
const M_LILYFLR : u32 = 60u;
const M_REED    : u32 = 61u;
const M_KELP    : u32 = 62u;
// ---- desert / pine-highland / alpine flora (materials.json ids 70..76) ----
// The three biomes that generated as bare ground: desert (bare sand plus the
// occasional dead bush), the pine highlands (bare needles over stone) and the
// snowline above TREELINE (bare snow). Placed by cactusAt() and the three
// biome ground-cover blocks at the end of genCell.
//
// These numbers are ARRAY POSITIONS in materials.json (id == index + 1), read
// back out of the file AFTER appending — this block was reserved 91..97 and
// landed at 70..76 because it appended before the other agents' blocks did.
// Recompute from positions, never from what was reserved:
//   python -c "import json;[print(i+1,m['id']) for i,m in
//              enumerate(json.load(open('assets/materials/materials.json'))['materials'])]"
const M_CACTUS       : u32 = 70u;   // saguaro/barrel flesh: SOLID, blocking
const M_CACTUS_RIB   : u32 = 71u;   // ribbed skin + spines: SOLID, blocking
const M_CACTUS_BLOOM : u32 = 72u;   // crown flower: passable
const M_SCRUB        : u32 = 73u;   // creosote/sage: passable
const M_TUSSOCK      : u32 = 74u;   // dry bunchgrass: passable
const M_HEATH        : u32 = 75u;   // huckleberry/juniper: passable
const M_CUSHION      : u32 = 76u;   // alpine cushion / lichen crust: passable
// ---- vines / climbers / hanging moss (materials.json ids 77..80) ----
// These numbers are ARRAY POSITIONS in materials.json (id == index + 1), and
// they are not the ids this block was authored against: it was reserved 70..76
// and landed at 77..80 because another agent's block committed in between.
// That is the append-only contract working as intended — recompute from
// positions, never from what was reserved:
//   python -c "import json;[print(i+1,m['id']) for i,m in
//              enumerate(json.load(open('assets/materials/materials.json'))['materials'])]"
const M_VINE_HANG      : u32 = 77u;
const M_CREEPER_FLOWER : u32 = 78u;
const M_MOSS_HANG      : u32 = 79u;
const M_IVY            : u32 = 80u;
// ---- meadow wildflowers (materials.json ids 65..69) ----
// Five micro-model species that vary by CLUMP, not per cell: see the ground
// flora block in genCell. petal_blue (63) and petal_pink (64) are colour
// materials the .vox models paint with and are never placed by worldgen, which
// is why they have no constant here.
const M_BLUEBELL  : u32 = 65u;
const M_FOXGLOVE  : u32 = 66u;
const M_BUTTERCUP : u32 = 67u;
const M_CLOVER    : u32 = 68u;
const M_WILDROSE  : u32 = 69u;
// ---- tall meadow grass (materials.json ids 95..96) ----
// Dense stands of blade grass up to 8 cells (80 cm — about half the player).
// Placed by flowerAt() like the flowers, because it IS the flower machinery:
// a per-column species + height answer that the base cell and the stalk
// branch re-derive identically. The head material caps the stack so a stand
// has dried tan tips at ragged heights instead of a mown flat top. Same
// array-position caveat as every block above: these are POSITIONS in
// materials.json, recompute after any append lands ahead of this one.
const M_TALLGRASS      : u32 = 95u;
const M_TALLGRASS_HEAD : u32 = 96u;
// Tallest a meadow flower can be, in CELLS — must be >= the largest value
// flowerHeight() can return (now tall grass, 4 + 4 = 8; foxglove reaches 5).
// It bounds the Y range the stalk branch scans, so an under-count silently
// beheads the tall species and an over-count just costs a few wasted
// evaluations per column.
const FLOWER_MAX_H : i32 = 8;
// ---- shoreline: the wet fringe outside a pond (materials.json ids 81..87) ----
// Placed by the shore-cover block in genCell against shoreAt(). Like the vine
// block above, these landed at ids other than the ones reserved for them
// (77..83) because other agents' blocks committed first — the numbers below are
// ARRAY POSITIONS read back out of materials.json, not what was asked for.
const M_SHORE_MUD    : u32 = 81u;
const M_MARSH_GRASS  : u32 = 82u;
const M_CATTAIL      : u32 = 83u;
const M_CATTAIL_HEAD : u32 = 84u;
const M_HORSETAIL    : u32 = 85u;
const M_WATER_IRIS   : u32 = 86u;
const M_WET_MOSS     : u32 = 87u;
// ---- forest undergrowth (materials.json ids 88..94) ----
// The layer that lives UNDER a closed canopy, as opposed to the grass and
// flowers that live in the gaps. Placement is driven by canopy cover, not by
// biome — see undergrowthSite() and the ground-cover block in genCell.
// These ids are ARRAY POSITIONS in materials.json (id == index + 1). Re-derive
// after any append with:
//   python -c "import json;[print(i+1,m['id']) for i,m in
//              enumerate(json.load(open('assets/materials/materials.json'))['materials'])]"
const M_FERN      : u32 = 88u;
const M_MUSHROOM  : u32 = 89u;
const M_TOADSTOOL : u32 = 90u;
const M_MOSS      : u32 = 91u;
const M_SAPLING   : u32 = 92u;
const M_BRAMBLE   : u32 = 93u;
const M_LITTER    : u32 = 94u;

// Undergrowth placement constants. Plain WGSL consts rather than TUNE_* knobs,
// following the TREE_TILE / TREE_SCAN / POND_RIM precedent in this file: these
// are PLACEMENT CONTENT (which plant grows where), not look/feel, and the
// tuning pipeline's five-file round trip is reserved for the latter. They also
// change the world hash, so they are rule-1 state and belong with the rest of
// the integer procgen rather than behind an F5 reload.
//
// The two COVER thresholds are the whole design, so they are worth reading as a
// unit. undergrowthSite() returns 0 (open sky) .. 255 (deep under a crown):
//   < UG_COVER_EDGE   open ground:  grass and flowers, nothing else
//   >= UG_COVER_EDGE  canopy edge:  flowers, plus a thinning scatter of litter
//   >= UG_COVER_MIN   under cover:  the shade set takes over from the flowers
//   >= UG_COVER_DEEP  deep shade:   brambles drop out, fern/moss/litter remain
// UG_COVER_MIN sits near the middle of a single crown's ramp, NOT at its rim:
// a rim-aligned threshold draws a visible circle of fern around every tree.
const UG_COVER_EDGE : i32 = 40;
const UG_COVER_MIN  : i32 = 96;
const UG_COVER_DEEP : i32 = 190;

// 1-in-N per column, inside the relevant patch mask. These are the densities
// that make the floor read as dense without paving it: a fern every ~7 columns
// inside a fern bank is a bank you push through, one every 2 is a hedge.
const UG_FERN_CHANCE    : u32 = 7u;
const UG_FERN_PATCH     : i32 = 140;    // vnoise 0..255; ~40% of the area
const UG_MOSS_CHANCE    : u32 = 3u;
const UG_MOSS_PATCH     : i32 = 150;
const UG_BRAMBLE_CHANCE : u32 = 23u;
const UG_SAPLING_CHANCE : u32 = 900u;   // rare on purpose: it reads as a TREE
const UG_LITTER_CHANCE  : u32 = 4u;     // the default floor of a wood
const UG_LITTER_EDGE_CHANCE : u32 = 11u;  // thinner, past the crown rim

// Mushrooms ring the BOLE. Radius in voxels (a great oak's ring is wider, via
// the per-tree jitter added at the call site); the inner d2 > 9 keeps them off
// the trunk cells themselves.
const UG_SHROOM_RING : i32 = 11;
const UG_SHROOM_BASE_CHANCE : u32 = 3u;

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

// Per-tile pond descriptor, unpacked from one tile hash — the pond analogue of
// treeInfo. Split out of pondAt so the SHORE band (shoreAt, below) can ask
// "where is the nearest pond rim" for a column that is OUTSIDE every disc, and
// therefore gets `none` back from pondAt. Both callers must see exactly the
// same disc, so there is one place that decides it.
struct Pond {
  present : bool,
  cx      : i32,   // disc centre, world coords
  cz      : i32,
  r       : i32,   // disc radius
};

fn pondInfo(pt : i32, pz : i32, seed : u32) -> Pond {
  var p : Pond;
  p.present = false; p.cx = 0; p.cz = 0; p.r = 0;

  let rh = hash3(seed ^ 0xB0A7u, bitcast<u32>(pt), bitcast<u32>(pz));
  if (rh % TUNE_POND_CHANCE != 0u) { return p; }                 // ~1 pond per 4 tiles
  let r = TUNE_POND_RADIUS_MIN + i32((rh >> 4u) % TUNE_POND_RADIUS_SPAN);
  // The disc must never leave its own tile: pondAt is consulted for ONE tile
  // per column (no neighbourhood scan), so a pond that overhung its tile edge
  // would simply vanish from the columns on the other side — half a bowl,
  // carved terrain with no water in it.
  //
  // The inset is therefore DERIVED from the largest radius this tuning can
  // produce, not a hardcoded constant. It used to be a literal 60, which was
  // correct only for the original radius 20..36; the moment the radii grew
  // past it the guarantee silently broke. `maxR + 4` keeps a small margin for
  // the rim samples.
  let maxR = TUNE_POND_RADIUS_MIN + i32(TUNE_POND_RADIUS_SPAN) - 1;
  let inset = maxR + 4;
  // A tile that cannot contain the biggest possible disc holds no pond at all,
  // rather than one that silently clips. max(1) keeps the modulo legal.
  let span = u32(max(POND_TILE - 2 * inset, 1));
  if (POND_TILE - 2 * inset < 1) { return p; }
  let cx = pt * POND_TILE + inset + i32((rh >> 9u) % span);
  let cz = pz * POND_TILE + inset + i32((rh >> 17u) % span);
  // Keep-out zones, by DISC (center + radius), not by column: the spawn
  // clearing + fixture pads, the streaming ball column (408,128) whose test
  // assumes TerrainHeight() is the surface, and the three authored pools
  // (128 covers the widest rim 80 + max radius 36 + slack).
  if (cx >= -44 && cx <= 264 && cz >= -44 && cz <= 264) { return p; }
  if (abs(cx - 408) < r + 24 && abs(cz - 128) < r + 24) { return p; }
  let q1x = cx - 420; let q1z = cz - 420;
  let q2x = cx - 260; let q2z = cz - 300;
  let q3x = cx - 220; let q3z = cz - 520;
  if (q1x * q1x + q1z * q1z < 128 * 128) { return p; }
  if (q2x * q2x + q2z * q2z < 128 * 128) { return p; }
  if (q3x * q3x + q3z * q3z < 128 * 128) { return p; }
  p.present = true; p.cx = cx; p.cz = cz; p.r = r;
  return p;
}

// The pond's water surface, from its rim. Split out of pondAt for the same
// reason pondInfo was: the shore band needs the waterline of a pond it is
// standing OUTSIDE of, to pick a depth-banded plant and to know how far above
// the water it is. 24 baseHeight samples, so callers should ask once.
fn pondSurface(p : Pond, seed : u32) -> i32 {
  // water level: 2 under the lowest ground on the rim circle
  var wmin = 0x7FFFFFFF;
  for (var i = 0; i < 24; i++) {
    let s = POND_RIM[i];
    wmin = min(wmin, baseHeight(p.cx + (s.x * p.r) / 256, p.cz + (s.y * p.r) / 256, seed));
  }
  return wmin - 2;
}

// Returns (bowl floor, water surface) at this column, or (-1,-1) outside any
// pond. genCell carves the terrain to the floor and fills (floor, surface]
// with water. Pure function of (coords, seed), exactly like treeInfo.
fn pondAt(x : i32, z : i32, seed : u32) -> vec2<i32> {
  let none = vec2<i32>(-1, -1);
  let p = pondInfo(fdiv(x, POND_TILE), fdiv(z, POND_TILE), seed);
  if (!p.present) { return none; }
  let dx = x - p.cx;
  let dz = z - p.cz;
  let d2 = dx * dx + dz * dz;
  if (d2 > p.r * p.r) { return none; }
  let surf = pondSurface(p, seed);
  // Parabolic bowl, carved below the water surface (terrain that is already
  // lower stays — water just fills deeper there, still capped by the
  // rim-derived surface).
  //
  // DEPTH IS THE WHOLE POINT: at kVoxelMeters 0.10 the player capsule is 17
  // voxels tall, so the original 8-voxel centre depth was 0.8 m and a pond
  // could only ever be waded through. TUNE_POND_DEPTH now puts the centre well
  // over the player's head while TUNE_POND_DEPTH_RIM keeps the edge shallow,
  // so you walk in off a beach rather than stepping off a wall.
  // LoadTuning clamps the depth under the cave layer — a bowl that breaches a
  // tunnel drains the pond and the world never settles.
  let depth = TUNE_POND_DEPTH_RIM +
              ((p.r * p.r - d2) * (TUNE_POND_DEPTH - TUNE_POND_DEPTH_RIM)) / (p.r * p.r);
  return vec2<i32>(surf - depth, surf);
}

// ---- the shore band ----
// The wet fringe OUTSIDE the disc. Everything up to here treated a pond as a
// binary — inside the disc you get water and pond life, one voxel outside you
// get the same plain grass as a hillside a kilometre away — so walking up to a
// pond had no approach: the marsh, the mud, the reed bed you push through are
// what make arriving at water read as arriving somewhere.
//
// COST (rule 2). This is a per-column query on the worldgen path, which runs
// for every cell of every generated chunk, so it must be O(1) and cheap in the
// overwhelmingly common case of "nowhere near a pond":
//
//   * At most FOUR pondInfo calls, never a 5x5 scan like the trees. A pond disc
//     is guaranteed to lie inside its own tile (see the inset above), so a
//     column can only be within `band` of a disc belonging to its own tile or
//     to a tile whose EDGE is within `band` of the column — and a column is
//     within `band` of at most one tile edge per axis. The loop is over
//     {0, sx} x {0, sz} where sx/sz are 0 unless the column is inside `band` of
//     that axis' tile boundary, so it collapses to ONE call away from the
//     boundaries and the duplicate (0,0) entry is skipped.
//   * pondSurface (24 baseHeight samples) is evaluated only once a disc has
//     actually been found within the band — i.e. only for shore columns.
//
// Returns (distance PAST the rim in voxels, water surface Y), or (-1,-1) when
// this column is not on any shore. Distance 0 is the first column outside the
// disc; the inside of the disc returns none (that is pondAt's job).
//
// Why the band cannot simply be read off `pondAt` returning none: the disc's
// clearance inside its own tile can be as little as 4 voxels for the largest
// radius, so a wider band derived from one tile alone would be sliced off flat
// along a tile edge — a straight-line haircut through a marsh, which is exactly
// the artifact the tile scan buys us out of.
struct Shore {
  onShore : bool,
  past    : i32,   // voxels beyond the rim (0 = first dry column)
  surf    : i32,   // the pond's water surface Y
};

fn shoreAt(x : i32, z : i32, seed : u32) -> Shore {
  var s : Shore;
  s.onShore = false; s.past = 0; s.surf = -1;

  let band = TUNE_SHORE_BAND;
  if (band <= 0) { return s; }

  let pt = fdiv(x, POND_TILE);
  let pz = fdiv(z, POND_TILE);
  // Which neighbouring tile (if any) has an edge close enough that its disc
  // could reach this column. -1/+1/0 per axis, so at most 2x2 tiles total.
  let lx = fmodp(x, POND_TILE);
  let lz = fmodp(z, POND_TILE);
  let sx = select(select(0, 1, lx >= POND_TILE - band), -1, lx < band);
  let sz = select(select(0, 1, lz >= POND_TILE - band), -1, lz < band);

  var best = 0x7FFFFFFF;
  var bestP : Pond;
  bestP.present = false; bestP.cx = 0; bestP.cz = 0; bestP.r = 0;
  for (var iz = 0; iz < 2; iz++) {
    let oz = select(0, sz, iz == 1);
    if (iz == 1 && sz == 0) { continue; }        // no second row to check
    for (var ix = 0; ix < 2; ix++) {
      let ox = select(0, sx, ix == 1);
      if (ix == 1 && sx == 0) { continue; }      // no second column to check
      let p = pondInfo(pt + ox, pz + oz, seed);
      if (!p.present) { continue; }
      let dx = x - p.cx;
      let dz = z - p.cz;
      let d2 = dx * dx + dz * dz;
      // Inside the disc is the pond, not the shore.
      if (d2 <= p.r * p.r) { return s; }
      // Compare in SQUARED distance to keep this integer-exact (no isqrt), then
      // resolve `past` once, on the winner only.
      let outer = p.r + band;
      if (d2 > outer * outer) { continue; }
      if (d2 < best) { best = d2; bestP = p; }
    }
  }
  if (!bestP.present) { return s; }

  // Integer distance past the rim, by bisection on the squared radius — 8 steps
  // over the band, no sqrt and no f32 (rule 1). `past` is the smallest k with
  // d2 <= (r+k)^2, minus one, i.e. the number of whole voxels of dry ground
  // between this column and the waterline.
  var lo = 0;
  var hi = band;
  for (var i = 0; i < 8; i++) {
    if (lo >= hi) { break; }
    let mid = (lo + hi) / 2;
    let rr = bestP.r + mid;
    if (best <= rr * rr) { hi = mid; } else { lo = mid + 1; }
  }
  s.onShore = true;
  s.past = max(lo - 1, 0);
  s.surf = pondSurface(bestP, seed);
  return s;
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

// ---- BOUNDS on the size table below, for treeAt's cheap rejects ------------
//
// These are NOT the rule. Every tile that survives them still goes through the
// exact per-tile `reach` and `vtop` tests in treeAt, so a bound that is too
// LOOSE costs a few wasted tile lookups and changes no output. A bound that is
// too TIGHT shears canopies and MOVES THE WORLD HASH — so if the size table in
// treeInfo grows a species or a dimension, these must be re-derived upward with
// it. check_invariants.py asserts they still dominate the table.
//
// Why they exist, measured: treeAt ran its 25-tile scan for EVERY air cell
// above ground, and each tile's treeInfo costs a biomeAt + a baseHeight + a
// pondAt (~20 hashes) — all of it paid before the `y > vtop` test that rejects
// it. A cell 300 voxels up in open sky therefore paid ~500 hashes to conclude
// "no tree here". Under --autofly-surface that was the whole of genChunk's
// 21 ms per window shift, which the paged streaming path then fences on
// (docs/PLAN_surface_flight_perf.md B2).
//
// The jitter is `j = (hsh >> 12) % 5`, so every maximum below is at j = 4.
const TREE_MAX_TRUNK_DM : i32 = 95 + 4 * 6;   // great oak, the tallest trunk
const TREE_MAX_RAD_DM   : i32 = 42 + 4 * 3;   // great oak, the widest crown
const TREE_BIRCH_RAD_DM : i32 = 22 + 4 * 2;   // birch: the one species whose
                                              // gate adds a second radius
const TREE_PINE_TRUNK_DM : i32 = 70 + 4 * 8;  // pine, the second-tallest
// The widest horizontal `reach` any species can ask for. Birch wins it despite
// its narrow crown, because its gate is `radius * 5/2 + 4` (a branch skeleton,
// not a leaf ball).
const TREE_MAX_REACH : i32 =
    max(TREE_MAX_RAD_DM * VOX_PER_M / 10 + 2,
        (TREE_BIRCH_RAD_DM * VOX_PER_M / 10) * 5 / 2 + 4);
// The tallest a tree cell can sit ABOVE its own trunk's ground, per species —
// `vtop - base` at maximum jitter. Taken as a max over species rather than by
// summing the biggest of each dimension, which would be ~35% looser and stop
// the sky short-circuit from firing at all.
const TREE_MAX_ABOVE : i32 =
    max(TREE_MAX_TRUNK_DM * VOX_PER_M / 10 + TREE_MAX_RAD_DM * VOX_PER_M / 10 + 2,
        max((65 + 4 * 6) * VOX_PER_M / 10
              + 2 * (TREE_BIRCH_RAD_DM * VOX_PER_M / 10) + 2,
            TREE_PINE_TRUNK_DM * VOX_PER_M / 10
              + (24 + 4 * 2) * VOX_PER_M / 10 + 2));
// A trunk only exists below the treeline, and the terrain band caps its ground
// independently, so the highest ground any trunk can stand on is the smaller of
// the two. Above TREE_MAX_BASE + TREE_MAX_ABOVE there is no tree anywhere in
// the world, at any seed — one compare replaces the whole scan.
const TREE_MAX_BASE : i32 = min(TUNE_TREELINE - 1,
                                TUNE_BASE_HEIGHT + TUNE_HILL_AMPLITUDE +
                                    TUNE_DETAIL_AMPLITUDE);
const TREE_MAX_TOP : i32 = TREE_MAX_BASE + TREE_MAX_ABOVE;

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

// The HASH-ONLY half of treeInfo: WHERE the trunk stands, and nothing that
// costs a noise lookup. Split out so treeAt can reject a tile on distance —
// and then on ground height alone — before paying for the site's biome and
// pond queries. One hash3 for the whole thing.
struct TreeSite {
  hsh : u32,
  wx  : i32,
  wz  : i32,
};

fn treeSite(tx : i32, tz : i32, seed : u32) -> TreeSite {
  var s : TreeSite;
  s.hsh = hash3(seed ^ 0x7BEE5u, bitcast<u32>(tx), bitcast<u32>(tz));
  // Trunk sits somewhere in the middle half of the tile — jittering the site
  // within the tile is what stops a forest from reading as a planted grid.
  let inset = TREE_TILE / 4;
  let span = u32(TREE_TILE / 2);
  s.wx = tx * TREE_TILE + inset + i32((s.hsh >> 3u) % span);
  s.wz = tz * TREE_TILE + inset + i32((s.hsh >> 9u) % span);
  return s;
}

// The rest of treeInfo, given a site and the ground height AT that site.
//
// `base` is passed in rather than sampled here because treeAt has already had
// to know it for its vertical reject, and baseHeight is eight hashes — sampling
// it twice would be the most expensive thing this function does. It is the same
// value either way (baseHeight is a pure function of the site), so the split
// changes no output; treeInfo below is the unchanged one-shot form for the
// callers that have no reject to do.
fn treeInfoAt(s : TreeSite, base : i32, seed : u32) -> Tree {
  var t : Tree;
  t.present = false;
  t.species = 0u; t.wx = s.wx; t.wz = s.wz; t.base = base;
  t.trunk = 0; t.radius = 0; t.rnd = s.hsh;

  let hsh = s.hsh;
  let biome = biomeAt(t.wx, t.wz, seed);
  let h = base;

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

// The one-shot form, for callers with no reject of their own to do first
// (undergrowthSite, treeCanopyAt). Identical to what this function was before
// the site split.
fn treeInfo(tx : i32, tz : i32, seed : u32) -> Tree {
  let s = treeSite(tx, tz, seed);
  return treeInfoAt(s, baseHeight(s.wx, s.wz, seed), seed);
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

// ---- hanging vines, moss beards and trunk ivy (implicit, per-cell) ----
//
// THE PROBLEM THIS SOLVES. A vine is the one plant whose real-world form is a
// PATH: it starts somewhere and travels. Worldgen has no turtle to walk it —
// genCell sees one cell and must answer for that cell alone, with no memory of
// the cells above it and no ability to write into them. So a vine cannot be
// grown; it has to be a CLOSED-FORM PREDICATE that every cell along the strand
// independently agrees on.
//
// The trick is that a hanging vine has exactly one degree of freedom: the
// column it hangs in. Fix the column and the whole strand is determined by two
// numbers — where it starts (the canopy underside directly above) and how far
// it falls (a per-column hash). Both are pure functions of (column, tree), so
// every cell in the strand derives the identical pair and the strand is
// continuous by construction rather than by being drawn.
//
// That is why the canopy underside is computed ANALYTICALLY below instead of
// by marching upward looking for leaves. Marching would be the obvious port of
// the turtle idea and it is exactly wrong here: it costs O(vine length) leaf
// evaluations per cell, and treeCell is not cheap (a birch alone is 5 limbs x
// 6 twigs of segment distance). Each species' crown is an implicit surface we
// already have the parameters for, so its underside is one integer sqrt.
//
// Rule 2: everything here is INERT, placed once. A growing vine is the textbook
// version of the thing the file header warns about — it would keep every forest
// chunk awake forever. `vine` (material 22) is the REACTIVE garden vine and is
// deliberately NOT what this places.

// Underside of the tree's foliage in the column (dx,dz) relative to the trunk,
// as a height above t.base, or -1 if this column carries no canopy to hang
// from. This is the inverse of the crown tests in treeCell: same parameters,
// solved for the lowest y instead of tested at a given y.
fn canopyUnderside(t : Tree, dx : i32, dz : i32) -> i32 {
  let d2 = dx * dx + dz * dz;
  let r = t.radius;
  let topY = t.trunk - t.radius / 3;
  switch (t.species) {
    // Round crowns: the treeCell test is dx^2 + dz^2 + ((dy-cy)*3/2)^2 <= r^2,
    // so the lowest dy in this column is cy - (2/3)*sqrt(r^2 - d2). Great oaks
    // carry a second, lower lobe centred at cy - r with a 2x vertical squash
    // and radius 3r/4 — whichever hangs lower is the real underside.
    case 0u, 3u: {
      var low = 0x7FFFFFFF;
      if (d2 <= r * r) {
        let s = i32(isqrt(u32(r * r - d2)));
        low = topY - (s * 2) / 3;
      }
      if (t.species == 3u) {
        let r2 = r * 3 / 4;
        if (d2 <= r2 * r2) {
          let s2 = i32(isqrt(u32(r2 * r2 - d2)));
          low = min(low, (topY - r) - s2 / 2);
        }
      }
      if (low == 0x7FFFFFFF) { return -1; }
      return low;
    }
    // Pine: a downward-widening cone, so its underside in a column is the
    // height at which the cone radius (diamond metric, as in treeCell) equals
    // that column's distance. Solving cr = (up*r)/span for `up` and converting
    // back: dy = tip - (md*span)/r. The saw-tooth skirt is ignored here — it
    // moves the boundary by a voxel or two and a vine hanging from a needle
    // rather than from the bough beneath it is not a distinction anyone sees.
    case 1u: {
      let tip = t.trunk + t.trunk / 8;
      let start = t.trunk / 4;
      let span = max(tip - start, 1);
      let md = abs(dx) + abs(dz);
      if (md > r) { return -1; }
      let dy = tip - (md * span) / max(r, 1);
      if (dy < start) { return -1; }
      return dy;
    }
    // Birch has no crown surface at all — its foliage is fifteen small blobs at
    // twig tips, and there is no closed form for "the lowest one over this
    // column". Birches get moss beards off their LIMBS instead (handled by the
    // caller, which already has the limb geometry in hand), never a curtain.
    // Bushes are too low to hang anything from.
    default: { return -1; }
  }
}

// Vine material contributed by tree `t` at world cell (x,y,z), or MAT_AIR.
// Called from treeAt's existing tile loop, so it adds NO new world scan: the
// 25 tiles were already visited and `t` is already in registers.
fn treeVine(t : Tree, x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let dx = x - t.wx;
  let dz = z - t.wz;
  let dy = y - t.base;
  if (dy < 0) { return MAT_AIR; }

  // Bushes carry nothing; birch is handled as a moss beard further down.
  if (t.species == 4u) { return MAT_AIR; }

  // ---- 1. curtain vines under a round/conic canopy ----
  // One hash per COLUMN (not per cell): the column either hosts a strand or it
  // does not, and every cell of that strand reads the same roll. A per-cell
  // roll would give dashed vines, which is the same bug the pond plants call
  // out — and the salt is distinct per feature, never a bit-slice of a shared
  // hash, because slices of one hash correlate (see the pond-life note).
  let hv = hash3(seed ^ 0x71E5u, bitcast<u32>(x), bitcast<u32>(z));
  // Great oaks are the trees that read as ancient, so they drape hardest.
  var chance = TUNE_VINE_CHANCE;
  if (t.species == 3u) { chance = max(TUNE_VINE_CHANCE / 2u, 1u); }
  if ((hv % chance) == 0u) {
    let under = canopyUnderside(t, dx, dz);
    if (under >= 0) {
      // Strand length, jittered per column so the curtain has a ragged hem
      // instead of a machine-cut edge — the single most obvious tell that a
      // procedural vine is procedural.
      let len = TUNE_VINE_LEN_MIN + i32((hv >> 7u) % u32(max(TUNE_VINE_LEN_SPAN, 1)));
      // The strand occupies (under - len, under]: it starts INSIDE the foliage
      // by one cell so there is no visible gap between leaf and vine, and runs
      // down from there.
      if (dy <= under && dy > under - len) {
        // Never let a strand reach the ground: a vine that touches down reads
        // as a pillar and, worse, is something the player walks into where
        // they expected floor. Held clear of the trunk's own ground height,
        // which is the only ground height this function knows.
        if (dy > 2) {
          // A minority of strands flower. Gated on the SAME column roll, so a
          // blossom can only appear on a column that actually grew a vine —
          // the lilypad/blossom precedent.
          if (((hv >> 17u) % TUNE_CREEPER_FLOWER_CHANCE) == 0u &&
              ((dy + i32(hv >> 24u)) % 7) == 0) {
            return M_CREEPER_FLOWER;
          }
          return M_VINE_HANG;
        }
      }
    }
  }

  // ---- 2. moss beards on the great oaks and birches ----
  // Spanish-moss style: not a strand from the canopy underside but a short,
  // fuzzy skirt clinging to the outer canopy rim and to birch limbs, which is
  // what makes an old forest read as damp rather than merely green.
  // Restricted to the species that carry it so the whole forest does not fur
  // over: great oaks (the ancient ones) and birch (whose bare limbs are what
  // the beard is legible against).
  if (t.species == 3u || t.species == 2u) {
    let hm = hash3(seed ^ 0x3055u, bitcast<u32>(x), bitcast<u32>(z));
    if ((hm % TUNE_MOSS_CHANCE) == 0u) {
      // For the great oak, hang from the canopy underside like a short vine.
      // For the birch there is no underside, so the beard hangs from the
      // BOLE-TOP plane instead, thinned toward the middle so it reads as
      // hanging off the limb structure rather than as a disc.
      // 'from' is a RESERVED KEYWORD in WGSL — hence the awkward name.
      var anchor = -1;
      if (t.species == 3u) {
        anchor = canopyUnderside(t, dx, dz);
      } else {
        // Birch: limbs occupy the band between the fork and the bole top and
        // reach `radius` outward. A beard cell is legal inside that annulus,
        // hanging from a height that falls off with distance so the skirt
        // follows the limbs' outward-and-downward sweep.
        let d2 = dx * dx + dz * dz;
        let rr = t.radius;
        if (d2 <= rr * rr && d2 > (rr / 3) * (rr / 3)) {
          let d = i32(isqrt(u32(d2)));
          anchor = t.trunk - (d * t.trunk) / max(rr * 3, 1);
        }
      }
      if (anchor >= 0) {
        let mlen = TUNE_MOSS_LEN_MIN + i32((hm >> 9u) % u32(max(TUNE_MOSS_LEN_SPAN, 1)));
        if (dy <= anchor && dy > anchor - mlen && dy > 2) {
          return M_MOSS_HANG;
        }
      }
    }
  }

  // ---- 3. ivy climbing the bole ----
  // The one climber that is not a hanging strand. Derived as a thin shell
  // AROUND the trunk cylinder treeCell already defines, so it hugs whatever
  // the trunk actually is (including the great oak's flared buttress) without
  // restating the trunk shape: same taper expression, evaluated at +1.
  // Angular gating by an isin() lobe makes the ivy climb in a couple of ropes
  // up one side rather than sheathing the trunk uniformly.
  if (t.species != 2u && dy <= t.trunk * 3 / 4) {
    var tr = max(t.trunk / 22, 1);
    let taper = tr - (tr * dy * 2) / max(t.trunk * 5, 1);
    var trNow = max(taper, 1);
    if (t.species == 3u && dy < t.trunk / 6) { trNow = trNow + tr / 2; }
    // the shell: just outside the bark the trunk test claims
    let ad = abs(dx) + abs(dz);
    let onShell = max(abs(dx), abs(dz)) <= trNow + 1 && ad > trNow + trNow / 2 + 1 &&
                  ad <= trNow + trNow / 2 + 3;
    if (onShell) {
      // Which side of the trunk, as a 256-step angle, from the sign-corrected
      // octant — cheap and integer, no atan needed: the ivy only has to pick a
      // consistent side, not a precise bearing.
      let ang = (dx * 32) / max(abs(dx) + abs(dz), 1) + select(128, 0, dx >= 0);
      // Two ropes that spiral: the favoured angle drifts with height.
      let phase = (i32(t.rnd >> 11u) & 255) + dy * TUNE_IVY_TWIST / 16;
      let off = ((ang - phase) & 127) - 64;
      let hi = hash3(seed ^ 0x1E9Au, bitcast<u32>(x), bitcast<u32>(z));
      if (abs(off) < 26 && (hi % TUNE_IVY_CHANCE) == 0u) {
        return M_IVY;
      }
    }
  }

  return MAT_AIR;
}

// Union of every tree whose canopy can reach (x,y,z): the (2*TREE_SCAN+1)^2
// tile neighborhood, which must cover the largest canopy radius (great oak,
// ~4.5 m = 72 voxels) plus the trunk's in-tile jitter. First non-air wins —
// order is by tile index, a fixed priority, never dispatch order (rule 1).
fn treeAt(x : i32, y : i32, z : i32, seed : u32) -> u32 {
  // ---- REJECT CHEAPEST-FIRST (see the TREE_MAX_* block) -------------------
  //
  // This function is called for EVERY air cell above ground, which in a
  // streamed-in vertical slab means most of the chunk — the window is 512
  // voxels tall and the terrain band is 54 of them. The rejects below are
  // ordered by what they cost, and none of them decides anything: the exact
  // `reach` and `vtop` tests further down are unchanged and still have the
  // final say.
  //
  // 1. Above every canopy in the world -> one compare, no scan at all.
  if (y > TREE_MAX_TOP) { return MAT_AIR; }
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN; oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN; ox <= TREE_SCAN; ox++) {
      // 2. Horizontal, on the trunk site alone (one hash). A +-2 tile's trunk
      //    is at least 181 voxels away and TREE_MAX_REACH is 124, so the outer
      //    ring of the scan is rejected outright and only ~4 of the 25 tiles
      //    reach the noise queries below. TREE_SCAN stays 2 because the exact
      //    per-species reach still decides; this only stops us PAYING for the
      //    tiles it was always going to refuse.
      let s = treeSite(tx + ox, tz + oz, seed);
      if (abs(x - s.wx) > TREE_MAX_REACH || abs(z - s.wz) > TREE_MAX_REACH) {
        continue;
      }
      // 3. Vertical, on baseHeight alone (eight hashes) — before biomeAt and
      //    pondAt, which a tile only needs once a cell can actually be in it.
      //    `y < sbase` is the exact test the loop already made; the upper one
      //    is TREE_MAX_ABOVE's bound on the same `vtop` computed below.
      let sbase = baseHeight(s.wx, s.wz, seed);
      if (y < sbase || y > sbase + TREE_MAX_ABOVE) { continue; }
      let t = treeInfoAt(s, sbase, seed);
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
      // Vines/moss/ivy fill cells the tree itself left EMPTY, so they are
      // tested second and can never displace bark or foliage. No extra scan:
      // this is the same tile, the same `t`, one more predicate.
      // The horizontal gate above already bounds them — every strand hangs
      // inside the canopy footprint or on the bole — and the vertical gate is
      // bounded below by t.base, which is where a strand is cut off anyway.
      let vm = treeVine(t, x, y, z, seed);
      if (vm != MAT_AIR) { return vm; }
    }
  }
  return MAT_AIR;
}

// ---- cacti: the desert's implicit tall shape --------------------------------
// A cactus is built exactly the way a tree is — per-tile hash placement, a pure
// per-cell shape test, no state — and for the same reason: worldgen evaluates
// one voxel at a time with no place to walk a turtle, so anything metre-scale
// has to be an implicit function of the cell.
//
// WHY NOT A MICRO MODEL. A micro model is ONE world cell, which at
// VOXEL_METERS is 10 cm. A saguaro is 3-5 m. The soft desert ground cover
// (scrub, tussock) is micro; anything that stands over the player is a shape.
//
// SCALE. Dimensions are METRES * VOX_PER_M like the trees above, never bare
// voxel counts — that is the mistake that produced knee-high "oaks" the first
// time this file was written, and a 40-voxel saguaro would be 2.5 m of
// waist-high stump rather than the thing you see across a desert.
//
// Two species, because they read completely differently and the contrast is
// what sells the biome:
//   0 SAGUARO — a tall ribbed column, 3.2-5.0 m, with 0-2 upcurved arms. The
//               silhouette everyone already has in their head.
//   1 BARREL  — a squat ribbed drum, 0.5-0.9 m, crowned with flowers. Ground
//               furniture; it is what stops the desert floor being empty
//               between the columns.
//
// COST. One tile lookup plus a bounded per-arm loop (CACTUS_ARMS = 2, no
// recursion). The scan is +-1 tile rather than the trees' +-2 because a cactus
// is narrow: the widest thing here is a saguaro with both arms out, ~1.1 m of
// half-width, against a 2.5 m tile. See CACTUS_SCAN.
const CACTUS_TILE : i32 = 40;    // 2.5 m between cactus sites
// How many tiles out to search. A cactus can overhang its own tile by
// (arm reach + in-tile jitter) = ~18 + 20 = 38 voxels, which is inside one
// tile, so +-1 covers it. Deliberately NOT the trees' +-2: this scan runs for
// every air cell above the desert floor and a 9-tile scan is 9/25 the cost of
// a 25-tile one for a shape that cannot reach that far.
const CACTUS_SCAN : i32 = 1;
const CACTUS_ARMS : i32 = 2;     // hard cap; bounds the per-cell loop

struct Cactus {
  present : bool,
  species : u32,   // 0 saguaro, 1 barrel
  wx      : i32,   // world x/z of the column centre
  wz      : i32,
  base    : i32,   // ground height at the root
  height  : i32,   // column height in voxels
  radius  : i32,   // column radius in voxels
  arms    : i32,   // 0..CACTUS_ARMS (saguaro only)
  rnd     : u32,
};

fn cactusInfo(tx : i32, tz : i32, seed : u32) -> Cactus {
  var c : Cactus;
  c.present = false;
  c.species = 0u; c.wx = 0; c.wz = 0; c.base = 0;
  c.height = 0; c.radius = 0; c.arms = 0; c.rnd = 0u;

  // DISTINCT SALT. Not a bit-slice of the tree hash and not the tree salt with
  // a different shift: the pond-life comment in genCell documents exactly why
  // slices of one hash correlate, and a cactus that only ever grew where a
  // dead bush also rolled would read as a planted grid.
  let hsh = hash3(seed ^ 0xCAC71u, bitcast<u32>(tx), bitcast<u32>(tz));
  c.rnd = hsh;
  let inset = CACTUS_TILE / 4;
  let span = u32(CACTUS_TILE / 2);
  c.wx = tx * CACTUS_TILE + inset + i32((hsh >> 3u) % span);
  c.wz = tz * CACTUS_TILE + inset + i32((hsh >> 9u) % span);

  // Desert only, and never on the keep-out ground every other feature avoids:
  // the spawn clearing, the selftest fixture pads, or a pond.
  if (biomeAt(c.wx, c.wz, seed) != B_DESERT) { return c; }
  let h = baseHeight(c.wx, c.wz, seed);
  c.base = h;
  if (h >= TREELINE) { return c; }
  if (inSpawnClearing(c.wx, c.wz)) { return c; }
  if (onFixturePad(c.wx, c.wz)) { return c; }
  if (pondAt(c.wx, c.wz, seed).y >= 0) { return c; }

  let roll = (hsh >> 17u) % 100u;
  if (roll >= TUNE_CACTUS_CHANCE) { return c; }

  // Barrels outnumber saguaros heavily. A desert with a saguaro every 2.5 m is
  // a plantation; the columns have to be occasional or they stop being
  // landmarks, which is the entire job they do here.
  let sroll = (hsh >> 24u) % 100u;
  c.species = select(1u, 0u, sroll < TUNE_SAGUARO_FRACTION);

  // Dimensions in TENTHS OF A METRE, converted below — same convention as
  // treeInfo, and the reason a saguaro comes out at a real 3.2-5.0 m instead
  // of as a tabletop model of one.
  let j = i32((hsh >> 12u) % 5u);
  var hDm = 0;
  var rDm = 0;
  if (c.species == 0u) {
    hDm = 32 + j * 5;    // saguaro 3.2 - 5.2 m
    rDm = 3;             // ~0.3 m radius: a 0.6 m thick column
    // Arms only on the taller half: a young saguaro has none, and putting arms
    // on a short one is the single most obviously wrong thing this shape can
    // do. 0, 1 or 2, never more — the loop below is bounded by CACTUS_ARMS.
    c.arms = select(0, i32((hsh >> 21u) % 3u), j >= 2);
  } else {
    hDm = 5 + j;         // barrel 0.5 - 0.9 m
    rDm = 3 + j / 2;     // squat: radius comparable to height
  }
  c.height = hDm * VOX_PER_M / 10;
  c.radius = max(rDm * VOX_PER_M / 10, 2);
  c.present = true;
  return c;
}

// Where arm `i` leaves the bole, and how far it reaches. Returns
// (attach height, horizontal dx, horizontal dz, arm length), all in voxels.
// Split out so the AABB in cactusAt can bound the arms without duplicating the
// geometry — an AABB that disagrees with the shape is how a limb gets sliced
// off at an invisible plane.
fn cactusArm(c : Cactus, i : i32) -> vec4<i32> {
  let ah = hash3(c.rnd ^ 0x4A12u, bitcast<u32>(i), 3u);
  // Arms leave the bole between 40% and 65% of its height. Lower than that and
  // the arm looks like a second plant; higher and the classic candelabra
  // silhouette collapses into a fork at the tip.
  let attach = c.height * 2 / 5 + i32(ah % u32(max(c.height / 4, 1)));
  // Azimuth on the 256-step integer circle. Arms are pushed to opposite sides
  // (i * 128) so two arms never grow into each other, with per-arm jitter.
  let az = (i * 128 + i32(ah >> 7u) % 90) & 255;
  let reach = c.radius * 3 + i32((ah >> 15u) % u32(max(c.radius * 2, 1)));
  return vec4<i32>(attach, (isin((az + 64) & 255) * reach) / 256,
                   (isin(az) * reach) / 256, reach);
}

// Material this cactus contributes at world cell (x,y,z), or MAT_AIR.
fn cactusCell(c : Cactus, x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let dx = x - c.wx;
  let dz = z - c.wz;
  let dy = y - c.base;
  if (dy < 0) { return MAT_AIR; }

  // ---- the bole ----
  // A cactus is a RIBBED column, and the ribs are the whole reason it reads as
  // a cactus rather than as a green pipe. The rib is derived from the integer
  // azimuth of the cell about the axis, so it is a real vertical flute rather
  // than a hash speckle — speckle reads as damage, flutes read as anatomy.
  let d2 = dx * dx + dz * dz;
  let r = c.radius;
  if (c.species == 0u) {
    if (dy <= c.height && d2 <= r * r) {
      // Rib test: 12 flutes around the column. `dx*8/max(...)` is a cheap
      // integer stand-in for the azimuth — exact angles are not needed, only a
      // repeating function of direction that is identical on every machine.
      let flute = (abs(dx) * 7 + abs(dz) * 11 + dy / 24) % 5;
      // The rim of the column is skin; the middle is flesh. That split is what
      // makes a cut cactus show pale flesh inside a darker wall.
      let rim = d2 * 4 >= r * r * 3;
      if (rim || flute == 0) { return M_CACTUS_RIB; }
      return M_CACTUS;
    }
    // ---- arms ----
    // Each arm is TWO segments: out from the bole, then straight up. That
    // right-angle elbow IS the saguaro silhouette; a single sloping segment
    // reads as a broken branch.
    for (var i = 0; i < CACTUS_ARMS; i++) {
      if (i >= c.arms) { break; }
      let a = cactusArm(c, i);
      let attach = a.x;
      let ex = a.y;
      let ez = a.z;
      let ar = max(r * 2 / 3, 2);
      // horizontal run, at the attach height
      if (segDist2(dx, dy, dz, 0, attach, 0, ex, attach, ez) <= ar * ar) {
        return M_CACTUS_RIB;
      }
      // vertical rise from the elbow, stopping short of the bole tip so the
      // main column stays the tallest point
      let riseTop = attach + (c.height - attach) * 3 / 4;
      if (segDist2(dx, dy, dz, ex, attach, ez, ex, riseTop, ez) <= ar * ar) {
        return M_CACTUS_RIB;
      }
      // a bloom on the arm tip, on some arms
      if (((c.rnd >> u32(4 + i)) % 3u) == 0u) {
        let bx = dx - ex; let by = dy - (riseTop + 1); let bz = dz - ez;
        if (bx * bx + by * by + bz * bz <= 4) { return M_CACTUS_BLOOM; }
      }
    }
    // Crown of flowers on the bole tip. A blooming saguaro is the thing that
    // makes one column in a field read as the subject of the frame.
    if (((c.rnd >> 11u) % 3u) == 0u && dy == c.height + 1 && d2 <= r * r) {
      return M_CACTUS_BLOOM;
    }
    return MAT_AIR;
  }

  // ---- barrel cactus: a squat ribbed drum ----
  // Domed rather than flat-topped: the top third pulls in, so it reads as a
  // barrel and not as a cylinder someone cut off.
  if (dy > c.height) {
    // the flower crown sits one voxel above the dome
    if (dy == c.height + 1 && d2 <= (r / 2) * (r / 2) &&
        ((c.rnd >> 13u) % 2u) == 0u) {
      return M_CACTUS_BLOOM;
    }
    return MAT_AIR;
  }
  // radius shrinks over the top third
  var br = r;
  let shoulder = c.height * 2 / 3;
  if (dy > shoulder) {
    br = r - (r * (dy - shoulder)) / max(c.height - shoulder, 1);
  }
  if (d2 <= br * br) {
    let flute = (abs(dx) * 7 + abs(dz) * 11) % 4;
    let rim = d2 * 4 >= br * br * 3;
    if (rim || flute == 0) { return M_CACTUS_RIB; }
    return M_CACTUS;
  }
  return MAT_AIR;
}

// Union of every cactus whose shape can reach (x,y,z), over the 3x3 tile
// neighbourhood. First non-air wins — order is by tile index, a fixed priority,
// never dispatch order (rule 1). Same structure as treeAt, including the AABB
// reject before any shape work.
fn cactusAt(x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let tx = fdiv(x, CACTUS_TILE);
  let tz = fdiv(z, CACTUS_TILE);
  for (var oz = -CACTUS_SCAN; oz <= CACTUS_SCAN; oz++) {
    for (var ox = -CACTUS_SCAN; ox <= CACTUS_SCAN; ox++) {
      let c = cactusInfo(tx + ox, tz + oz, seed);
      if (!c.present) { continue; }
      // HORIZONTAL reject. Must cover the widest thing the species can produce
      // or the outer arm gets sliced off at an invisible cylinder — the same
      // trap the birch's `reach` comment documents. A saguaro arm reaches
      // radius*3 + jitter from the axis, plus its own thickness.
      var reach = c.radius + 2;
      if (c.species == 0u) { reach = c.radius * 6 + 4; }
      if (abs(x - c.wx) > reach || abs(z - c.wz) > reach) { continue; }
      // VERTICAL extent must cover the tallest thing the species can put above
      // its base. Clipping this is how canopies get flat tops (treeAt's vtop
      // comment); here it would behead the saguaro and its crown of flowers.
      // + 2 covers the bloom sitting one voxel above the tip.
      let vtop = c.base + c.height + 2;
      if (y < c.base || y > vtop) { continue; }
      let m = cactusCell(c, x, y, z, seed);
      if (m != MAT_AIR) { return m; }
    }
  }
  return MAT_AIR;
}

// ---- forest undergrowth: what the canopy decides -----------------------------
// A real forest floor is not a uniform lawn with flowers on it. Under a closed
// crown almost no light reaches the ground, so the plants that live there are
// the shade specialists — ferns, mushrooms, moss, brambles, leaf litter, and
// the seedlings waiting for a gap. In the gaps between crowns you get the
// opposite: grass and flowers, which need the light.
//
// So undergrowth is placed as a function of CANOPY COVER rather than of biome,
// and that single inversion is what makes the forest read as LAYERED instead of
// as one green skin with confetti on it. Cover is the input; the existing
// grass/flower block is now gated on the complement of it.
//
// COST. Answering "how covered is this column" is the same 25-tile scan
// treeAt/treeCanopyAt already run, so this function does it ONCE and returns
// everything the placement rule needs — cover, and the distance to the nearest
// trunk (mushrooms ring tree bases, which is the cheapest high-value detail
// available here). Calling treeCanopyAt separately would have doubled the scan
// for the same answer. The scan is bounded at (2*TREE_SCAN+1)^2 = 25 tiles and
// runs for exactly one Y per column (the y == h + 1 gate), so it costs the same
// order as the flower block it sits next to.
//
// Everything placed is INERT (rule 2): no reaction in reactions.json uses any
// of these as `self` with an emit, so a generated forest floor settles and
// sleeps exactly as the bare one did.
// ---- meadow flowers: which species, and how tall --------------------------
// A micro model is ONE world cell, and a cell is VOXEL_METERS = 10 cm. So a
// single-cell flower is 10 cm tall whatever its model does, and every species
// is the same height as every other — a "foxglove" (1-2 m in life) came out the
// same size as clover. That is the tabletop-model-of-itself failure the tree
// block above documents, in miniature.
//
// The fix is the reed pattern: a flower is a STACK of cells, and the model in
// each cell is the same micro model repeated. Height is per-species (a briar is
// not a clover) with a per-plant hash jitter on top, so a patch has a natural
// height spread instead of being a mown lawn of identical stems.
//
// flowerSpecies() is the single source of truth for "what grows in this
// column", called by BOTH the base-cell branch and the upper-stalk branch.
// Sharing it is what makes a stalk one continuous plant rather than two
// unrelated halves that happen to be adjacent — the same reason the reed block
// tests the same hashes above and below the waterline.
struct Flower {
  mat    : u32,   // MAT_AIR when this column grows no flower
  height : i32,   // total cells, >= 1
};

// Per-species base height in CELLS, jittered per plant. Ranges are chosen
// against the 10 cm cell: clover is ground cover and stays 1 cell (10 cm),
// while a foxglove spire reaches 4 (40 cm). These are deliberately at the low
// end of life-size — a true 1.5 m foxglove is 15 cells, which at meadow density
// would be a wall of stems the player cannot see over.
fn flowerHeight(sp : u32, h : u32) -> i32 {
  switch (sp) {
    case M_CLOVER:    { return 1; }                        // 10 cm mat
    case M_BUTTERCUP: { return 2 + i32(h % 2u); }           // 20-30 cm
    case M_BLUEBELL:  { return 2 + i32(h % 2u); }           // 20-30 cm
    case M_WILDROSE:  { return 3 + i32(h % 2u); }           // 30-40 cm briar
    default:          { return 3 + i32(h % 3u); }           // foxglove 30-50 cm
  }
}

// Which flower this column grows, and how tall. `cover` is the canopy cover
// from undergrowthSite (wild rose is a woodland-margin plant, so it is placed
// by cover rather than by the species field).
//
// Pure function of (x, z, seed, cover): the upper-stalk branch re-derives it
// per cell WITHOUT re-running the 25-tile scan, by passing the cover it already
// knows is irrelevant there (see the call site) — so a taller flower costs a
// few hashes per extra cell, never another scan.
fn flowerAt(x : i32, z : i32, seed : u32, cover : i32) -> Flower {
  var f : Flower;
  f.mat = MAT_AIR;
  f.height = 0;

  let fr = hash3(seed ^ 0xF10Eu, bitcast<u32>(x), bitcast<u32>(z));
  let clump = vnoise(x, z, 24 * HSCALE, seed ^ 0xF11Eu);
  let biome = biomeAt(x, z, seed);

  // ---- tall grass stands: the reed-bed of the open meadow ------------------
  // Checked BEFORE the flower threshold because a stand is dense where flowers
  // are sparse: up to ~90% of columns in a patch core grow a blade, which no
  // per-mille flower rate reaches. The patch mask ramps both density and
  // height from the fringe to the core, so a stand rises out of the lawn as a
  // dome of blades rather than standing on a hard edge — the same reasoning
  // as the crown-cover ramp in undergrowthSite. Columns inside a stand that
  // roll NO blade fall through to the normal grass/flower chain, so a stand
  // has an understory instead of bare dirt between the stems.
  if (biome == B_MEADOW) {
    let tg = vnoise(x + 501, z - 267, 15 * HSCALE, seed ^ 0x7A55u);
    if (tg > 176) {
      let hTall = hash3(seed ^ 0x7A56u, bitcast<u32>(x), bitcast<u32>(z));
      let dens = min(u32(tg - 176) >> 3u, 8u);   // 0..8 in tenths of columns
      if ((hTall % 10u) < dens + 1u) {
        f.mat = M_TALLGRASS;
        // 4..8 cells (40-80 cm): the cap ramps with patch depth so the core
        // of a stand overtops its fringe, and the per-plant jitter under the
        // cap is what keeps the top ragged — a bed cut to one height reads as
        // a fence (the cattail block learned this first).
        let hi = 4 + min((tg - 176) / 12, 4);
        f.height = 4 + i32((hTall >> 8u) % u32(max(hi - 3, 1)));
        return f;
      }
    }
  }

  var thresh = 0u;
  if (biome == B_MEADOW) { thresh = select(6u, 60u, clump > 165); }
  else                   { thresh = select(2u, 16u, clump > 190); }
  if ((fr % 1000u) >= thresh) { return f; }

  let sp = vnoise(x + 911, z - 733, 40 * HSCALE, seed ^ 0xF1A5u);
  let spj = sp + (vnoise(x, z, 11 * HSCALE, seed ^ 0xF1A6u) - 128) / 4;
  let hBell = hash3(seed ^ 0xB1E7u, bitcast<u32>(x), bitcast<u32>(z));
  let hFoxg = hash3(seed ^ 0xF0C9u, bitcast<u32>(x), bitcast<u32>(z));
  let hButt = hash3(seed ^ 0x8B77u, bitcast<u32>(x), bitcast<u32>(z));
  let hClov = hash3(seed ^ 0xC10Fu, bitcast<u32>(x), bitcast<u32>(z));
  let hRose = hash3(seed ^ 0x8053u, bitcast<u32>(x), bitcast<u32>(z));

  // Grass is the default: a meadow is grass WITH flowers in it. Grass and petal
  // stay ONE cell — they are the ground layer the flowers rise out of.
  var m = select(M_PETAL, M_GRASS, (fr >> 11u) % 4u != 0u);
  if (spj < 55) {
    if ((hBell % 3u) == 0u) { m = M_BLUEBELL; }
  } else if (spj < 100) {
    if ((hButt % 2u) == 0u) { m = M_BUTTERCUP; }
  } else if (spj < 140) {
    if ((hClov % 3u) != 0u) { m = M_CLOVER; }
  } else if (spj < 175) {
    if ((hFoxg % 7u) == 0u) { m = M_FOXGLOVE; }
    else if ((hButt % 3u) == 0u) { m = M_BUTTERCUP; }
  }
  if (cover >= UG_COVER_EDGE && (hRose % 9u) == 0u) { m = M_WILDROSE; }

  f.mat = m;
  // Only the five flowers stack; grass and petal are the one-cell ground layer.
  if (m == M_GRASS || m == M_PETAL) { f.height = 1; }
  else { f.height = flowerHeight(m, hFoxg >> 7u); }
  return f;
}

struct Undergrowth {
  cover   : i32,   // 0 = open sky, 255 = deep under a crown
  trunkD2 : i32,   // squared XZ distance to the nearest trunk, or a large value
  species : u32,   // species of that nearest tree (0..4)
  rnd     : u32,   // that tree's hash, for per-tree variation of its own ring
};

// Cover contribution falls off from the crown CENTRE to its rim rather than
// being a hard disc, because the interesting structure is the half-lit margin
// where fern gives way to grass. A hard disc puts a visible circle on the
// ground under every tree; a ramp puts a gradient there, and gradients are what
// the eye reads as depth.
//
// Contributions ADD across overlapping crowns and saturate at 255: two crowns
// overlapping is genuinely darker than one, and that is what makes a dense
// stand of oaks grow a different floor from an isolated tree.
fn undergrowthSite(x : i32, z : i32, seed : u32) -> Undergrowth {
  var u : Undergrowth;
  u.cover = 0;
  u.trunkD2 = 1 << 24;      // "no trunk anywhere near", larger than any reach
  u.species = 0u;
  u.rnd = 0u;

  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN; oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN; ox <= TREE_SCAN; ox++) {
      let t = treeInfo(tx + ox, tz + oz, seed);
      if (!t.present) { continue; }
      let dx = x - t.wx;
      let dz = z - t.wz;
      let d2 = dx * dx + dz * dz;

      // Nearest trunk, for the mushroom ring. Ties broken by tile ORDER, which
      // is fixed (rule 1) — never by dispatch order.
      if (d2 < u.trunkD2) {
        u.trunkD2 = d2;
        u.species = t.species;
        u.rnd = t.rnd;
      }

      // Canopy cover. A bush (species 4) is knee-high and shades nothing, so it
      // contributes none — including it made every meadow read as closed forest
      // because bushes are the commonest meadow tile.
      if (t.species == 4u) { continue; }
      // Birch is a branch skeleton with leaf clusters at the twig tips, so it
      // covers a wider circle far more thinly. Same approximation the far field
      // makes in treeCanopyAt: a bigger radius, much less weight.
      var r = t.radius;
      var peak = 200;
      if (t.species == 2u) { r = t.radius * 2; peak = 90; }
      else if (t.species == 1u) { peak = 230; }   // pine: dense, dark
      else if (t.species == 3u) { peak = 255; }   // great oak: the darkest floor
      if (d2 > r * r) { continue; }
      // Linear ramp in the RADIUS (not in d2), so the falloff is even across
      // the crown instead of hugging the rim. Integer sqrt-free: compare d2
      // against r2 scaled by the fraction, which is the same ordering.
      // cover = peak * (1 - d/r), computed as peak * (r2 - d2) / r2 would bias
      // toward the centre; the halfway point of that ramp is where fern stops
      // and grass starts, so it is worth getting the shape right.
      let rr = max(r, 1);
      // d/r in 1/256ths. Computed as isqrt(d2 << 16 / r^2) rather than as
      // 256 * isqrt(d2) / r: the latter takes the square root FIRST and so
      // throws away its fractional part before the scale, which quantises the
      // ramp into visible concentric steps at small radii.
      // Done in u32 deliberately. d2 reaches (2 * 67)^2 = 17956 for the widest
      // birch footprint, and 17956 << 16 is 1.18e9 — inside i32, but close
      // enough to 2^31 that a future wider crown would silently wrap. u32 has
      // the headroom and every operand here is non-negative by construction.
      let frac = i32(isqrt((u32(d2) << 16u) / u32(rr * rr)));   // 0..256
      u.cover = min(255, u.cover + (peak * (256 - min(frac, 256))) / 256);
    }
  }
  return u;
}

// Caves: COLUMN BANDS carved by 2D noise — for each (x,z) inside a cavern
// mask, one contiguous vertical span is removed. Unlike 3D-threshold carving
// this cannot create free-floating stone blobs (stone above/below a band is
// horizontally connected to full columns at the mask boundary), which matters
// because the island detector would correctly-but-noisily convert generated
// floaters into debris the moment anything moved nearby.
// Returns 0 = solid, 1 = carve to air, 2 = carve to lava (below the magma
// table — see caveFill).
// Cavern masks scale horizontally like everything else (a 40-voxel mask cell
// made 2.5 m caves); the vertical spans grow only ~2x, matching the hills, so
// a cavern is a passage you walk through rather than a crawl space. The
// 10-voxel surface shell becomes 40 (2.5 m) so caves can't breach the new,
// thicker soil layer from below.
//
// ---- THE MAGMA TABLE: generated matter has to be generated AT REST ---------
//
// Worldgen used to lay the deep lava down as a 3-voxel FULL-fullness slab on
// the raw cavern floor (`y <= f2 + 2 && m2 > 190`). That is not a rest state:
// f2 swings 70 voxels across a 40-voxel noise cell, so the slab was a sheet of
// full lava on a ~25-degree hillside. It took ~2,700 ticks (~90 s of sim) to
// flow level, and EVERY WINDOW SHIFT regenerated a fresh unsettled band — so
// under sustained flight the world was permanently mid-settle. That band was
// 97% of the still-active chunks in the `--autofly-park` histogram and about
// half the whole surface-flight active set (docs/PLAN_surface_flight_perf.md,
// corrections 4-5). Cost scales with activity (CLAUDE.md rule 2), so matter
// that takes 90 seconds to stop moving is a rule-2 bug in the AUTHORING, not
// in the CA.
//
// The hard part is that genCell is a PURE PER-CELL FUNCTION of (world coords,
// seed): no flood fill, no neighbourhood walk, no way to find the rim of a
// basin. "At rest" for a liquid normally means "flat, at whatever level its
// basin sets", and the basin is precisely what a per-cell function cannot see.
//
// The way out is that a flat cut does not NEED to find the basin, because the
// cave's own complement already is one. Fill every carved cell at or below
// LAVA_LEVEL with lava and every carved cell above it with air, and then at
// each y <= LAVA_LEVEL a cell is lava exactly when it is carved and stone
// otherwise. So:
//
//   * laterally, every lava cell's neighbours are lava or STONE, at every
//     level — containment is a property of the carve, not of the fill;
//   * vertically, everything under a lava cell is lava or stone;
//   * the only lava/air interface in the world is the single plane
//     y == LAVA_LEVEL.
//
// Full cells, full cells beneath them, no lateral fullness difference and
// nowhere to spread — stepLiquid (sim_step.wgsl) falls through all three of
// its rules to the "settled: no markDirty" tail, and the chunk sleeps after
// one tick like the stone around it. A pure per-cell test buys a globally
// correct rest state because the FLATNESS comes from the constant and the
// CONTAINMENT comes from the geometry that was already there.
//
// Two properties that must not be broken:
//
//   * The cut applies to BOTH BANDS, which is why it lives in caveFill and not
//     in band 2. Band 1's floor reaches h - 100, which dips below LAVA_LEVEL
//     wherever a pond bowl has carved h down, and a band-1 AIR cell beside a
//     band-2 LAVA cell at the same y would be a hole in the container. Routing
//     every carve through one function makes containment independent of which
//     band cut the hole — today, and for any band added later.
//   * The level must be a CONSTANT. Any per-column or per-noise-cell level
//     reintroduces a step in the surface, and a step in a liquid is a flow.
//     This is also the honest cost of the rule: the magma table is at the same
//     height everywhere, which you could notice by comparing two distant
//     caverns. A basin-local level is not computable here at any price.
const LAVA_LEVEL : i32 = -80;

// What a carved cell is filled with. The one place the magma table is applied.
fn caveFill(y : i32) -> i32 {
  if (y <= LAVA_LEVEL) { return 2; }   // flooded, and flat, and therefore still
  return 1;                            // open cave
}

fn caveAt(x : i32, y : i32, z : i32, h : i32, seed : u32) -> i32 {
  // band 1: near-surface caverns following the terrain
  let m1 = vnoise(x, z, 40 * HSCALE, seed ^ 5u);
  if (m1 > i32(TUNE_CAVE_THRESHOLD1)) {
    let f1 = h - 40 - (vnoise(x, z, 32 * HSCALE, seed ^ 6u) * 60) / 255;
    let c1 = min(f1 + 10 + (vnoise(x, z, 12 * HSCALE, seed ^ 7u) * 20) / 255,
                 h - 40);
    if (y >= f1 && y <= c1) { return caveFill(y); }
  }
  // band 2: deep caverns at absolute depth (streamed depth is real terrain)
  let m2 = vnoise(x + 7717, z - 4177, 48 * HSCALE, seed ^ 8u);
  if (m2 > i32(TUNE_CAVE_THRESHOLD2)) {
    let f2 = -40 - (vnoise(x, z, 40 * HSCALE, seed ^ 9u) * 70) / 255;
    let c2 = f2 + 12 + (vnoise(x, z, 16 * HSCALE, seed ^ 10u) * 26) / 255;
    // No `m2 > 190` lava test here any more: gating the fill on the cavern
    // MASK put a vertical lava wall against open air wherever m2 crossed 190,
    // which is a flow the moment the world ticks. Depth is the only thing the
    // fill may depend on.
    if (y >= f2 && y <= c2 && y <= h - 40) { return caveFill(y); }
  }
  return 0;
}

// ---- THE COLUMN HALF, hoisted out of the per-cell path --------------------
//
// Everything from baseHeight down to the shore band is a pure function of
// (x, z) — no `y` appears anywhere in it — and none of it is cheap: baseHeight
// and biomeAt are eight hashes each, pondAt is a tile lookup, and shoreAt can
// cost a pondSurface, which is 24 more baseHeight samples. genCell recomputed
// the lot for every one of the 16 cells in a chunk column, so a chunk paid it
// 4,096 times for 256 distinct answers.
//
// Split out so genChunk can evaluate it ONCE per column and hand the result to
// the 16 cells that share it. The arithmetic is untouched and in the same
// order, so the words produced are identical — the world hash is the gate on
// that, and genCell below still composes the two halves for the callers that
// evaluate one isolated cell.
struct Col {
  h           : i32,         // ground height, after pool and pond carving
  biome       : u32,
  pond        : i32,         // disc-pond water surface Y, or -1
  pw          : vec2<i32>,   // pondAt's (bowl floor, surface)
  fluid       : u32,         // standing fluid material at this column
  fluidTop    : i32,         // its surface Y, or -1
  inPoolFloor : bool,
  inRim       : bool,
  shore       : Shore,
};

fn genColumn(x : i32, z : i32, seed : u32) -> Col {
  // ---- the fluid lab's flat slab (world.h kLabSlabY; PLAN_fluid_overhaul §4)
  // One guard, HERE, covers every worldgen consumer — genChunk (full + list),
  // genCell and with it the far cascades — because they all come through this
  // column function. The Col it returns is chosen so genCellIn's existing
  // gates suppress every feature without a second tap there:
  //   inPoolFloor = true  -> plain stone body, no snow cap, no grass skin
  //   inRim       = true  -> no caves, no trees, no undergrowth
  //   pond = -1, fluid = air, shore off -> no water, no pond/shore life
  // World::TerrainHeight takes the same branch on the CPU, so collision,
  // spawns and mob probes see exactly this slab. T.labMode is 0 on every
  // non-lab path, making this a dead branch for the pinned world hash.
  if (T.labMode != 0u) {
    var lab : Col;
    lab.h = LAB_SLAB_Y;
    lab.biome = 0u;
    lab.pond = -1;
    lab.pw = vec2<i32>(-1, -1);
    lab.fluid = MAT_AIR;
    lab.fluidTop = -1;
    lab.inPoolFloor = true;
    lab.inRim = true;
    lab.shore.onShore = false;
    lab.shore.past = 0;
    lab.shore.surf = -1;
    return lab;
  }
  var h = baseHeight(x, z, seed);
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

  // ---- the shore band (see shoreAt) ----
  // Queried ONCE per column, here, and reused by both the ground-skin swap
  // below and the shore-cover block further down — it is the one call in this
  // function that can cost a pondSurface (24 baseHeight samples), so asking
  // twice would double it for every shore column.
  //
  // Suppressed wherever the pond block itself is suppressed: never inside the
  // authored pool rims (the lava and oil pools must not grow weeds — the same
  // reason the pond-life block gates on `pond >= 0`), never on a fixture pad or
  // in the desert, and never above the treeline, so a shore is always a shore
  // and never a marsh growing out of a snowfield.
  var shore : Shore;
  shore.onShore = false; shore.past = 0; shore.surf = -1;
  if (pond < 0 && !inRim && !onFixturePad(x, z) && biome != B_DESERT &&
      h < TREELINE) {
    shore = shoreAt(x, z, seed);
    // A column whose ground stands well above the waterline is a BLUFF, not a
    // shore. This is the single most load-bearing test in the feature, and it
    // is a HEIGHT test rather than a second radius on purpose:
    //
    // pondSurface is `min(24 rim samples) - 2`, so the waterline sits under the
    // LOWEST point of the rim and most of the rim stands well above it — at the
    // default tuning the median column even at past=0 is ~14 voxels up. A band
    // defined by radius alone therefore paints marsh up whatever hillside
    // happens to abut the disc, which is exactly the artifact that gives away
    // that the fringe is a radius and not a wetness.
    //
    // Cutting on height instead makes the marsh follow the LOW parts of the
    // rim, so a pond in rolling ground gets reed beds in its shallow bays and
    // dry bank on its steep sides — which is what a real pond does, and it
    // costs one comparison. `shoreLift` is the knob: it is a fraction of the
    // raw band that survives, and it moves it a LOT (at the default pond it
    // takes 5% of columns at 4 and 99% at 24), so it is worth having as its own
    // parameter rather than derived from the band width.
    if (shore.onShore && h > shore.surf + TUNE_SHORE_LIFT) {
      shore.onShore = false;
    }
  }

  var col : Col;
  col.h = h;
  col.biome = biome;
  col.pond = pond;
  col.pw = pw;
  col.fluid = fluid;
  col.fluidTop = fluidTop;
  col.inPoolFloor = inPoolFloor;
  col.inRim = inRim;
  col.shore = shore;
  return col;
}

// ---- THE CELL HALF: everything that actually depends on y -----------------
//
// Unpacks the column into exactly the local names the body below has always
// used, so the body is unchanged line for line. Every one of these is read-only
// from here down — the only thing this half writes is `mat`.
fn genCellIn(col : Col, x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let h = col.h;
  let biome = col.biome;
  let pond = col.pond;
  let pw = col.pw;
  let fluid = col.fluid;
  let fluidTop = col.fluidTop;
  let inPoolFloor = col.inPoolFloor;
  let inRim = col.inRim;
  let shore = col.shore;
  var mat = MAT_AIR;

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
    } else if (shore.onShore && shore.past < TUNE_SHORE_MUD_WIDTH &&
               y > h - 2) {
      // WET MUD, in the inner ring only. This is the transition the whole
      // feature exists for: the bed inside the disc is sand and the bank
      // outside it was the same grass as a hillside a kilometre inland, so the
      // waterline was a hard colour edge with nothing in between.
      //
      // Two voxels deep rather than one, unlike the grass skin, because you
      // dig into a bank far more often than into open ground and a one-voxel
      // mud skin over stone reads as painted-on the moment it is broken.
      //
      // Solid (not powder) for the reason the note under the grass skin gives:
      // a powder shell on a slope avalanches out from under itself and the
      // chunk never sleeps.
      //
      // A stone face inside the band that is NOT the mud ring gets wet moss
      // instead — see below.
      mat = M_SHORE_MUD;
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
    // WET MOSS on the rock at the waterline. A SKIN SWAP on a surface cell that
    // already exists, not a plant placed above one, so it is free: no extra
    // voxel, no extra occupancy, nothing new for the CA to look at. Only the
    // topmost cell, and only where the mud ring has not already claimed the
    // column, so this is the OUTER half of the band — the stone that is damp
    // rather than the ground that is mud.
    //
    // Its own hash salt, like every other species here: slicing one column
    // hash for two rolls correlates them, which is documented at length in the
    // pond-life block below and is what once turned scattered planting into a
    // solid wall.
    if (mat == M_STONE && y == h && shore.onShore &&
        hash3(seed ^ 0x4D05u, bitcast<u32>(x), bitcast<u32>(z))
          % TUNE_SHORE_MOSS_CHANCE == 0u) {
      mat = M_WET_MOSS;
    }
  } else if (fluidTop >= 0 && y <= fluidTop) {
    mat = fluid;
  }

  // ---- pond life: kelp, reeds, lilypads ----
  // Placed into cells that would otherwise be pond WATER, so nothing here can
  // displace terrain or spill outside the bowl. Restricted to `pond >= 0`
  // (the disc ponds) rather than to any fluid: the authored lava and oil pools
  // are the same `fluid` machinery and should obviously not grow weeds, and
  // the disc pond is the only body whose floor and surface are both known here
  // as pure functions of the column.
  //
  // Everything is an INERT solid placed once at generation. Nothing grows,
  // spreads or reacts with the water it stands in — a plant that did would
  // keep every pond chunk awake forever and break the sleep budget (rule 2).
  //
  // All placement hashes are pure functions of (x, z, seed), like every other
  // worldgen feature, so a plant straddling a chunk border generates
  // identically from either chunk and regrows the same after an eviction.
  // SEPARATE HASHES PER SPECIES, not bit-slices of one. Slicing (fr, fr>>3,
  // fr>>17) looks independent and is not: the slices share entropy, so the
  // three rolls correlate and a column that grew one plant is far more likely
  // than chance to grow another. That is what turned a scattered planting into
  // a solid wall of stalks. Three distinct salts cost two extra hashes per
  // pond column and are actually independent.
  if (mat == M_WATER && pond >= 0) {
    let bed = min(h, pw.x);          // the carved bowl floor at this column
    let depth = pond - bed;          // water column height in voxels
    let above = pond - y;            // how far under the surface this cell is
    let hLily = hash3(seed ^ 0x71A9u, bitcast<u32>(x), bitcast<u32>(z));
    let hReed = hash3(seed ^ 0x2E3Du, bitcast<u32>(x), bitcast<u32>(z));
    let hKelp = hash3(seed ^ 0xC5B1u, bitcast<u32>(x), bitcast<u32>(z));

    // LILYPADS: a single cell floating ON the surface. Needs enough water
    // under it that a pad reads as floating rather than as lying on mud.
    if (y == pond && depth >= 10 && (hLily % TUNE_LILY_CHANCE) == 0u) {
      mat = M_LILYPAD;
    } else if (depth >= 4 && depth <= 14 && above >= 0 &&
               y - bed < TUNE_REED_HEIGHT &&
               (hReed % TUNE_REED_CHANCE) == 0u) {
      // REEDS: emergent, in the SHALLOW MARGIN only — a narrow depth band, so
      // they form a fringe around the shore rather than filling the bowl. They
      // grow from the bed and break the surface, which is what makes them read
      // as reeds rather than as underwater grass, so the height test is
      // against the BED, not against the waterline.
      mat = M_REED;
    } else if (depth > 16 && above > 4 && y - bed < TUNE_KELP_HEIGHT &&
               (hKelp % TUNE_KELP_CHANCE) == 0u) {
      // KELP: fully submerged, in the DEEP MIDDLE only (depth > 16 excludes
      // the whole shallow ring the reeds occupy, so the two never interleave).
      // `above > 4` keeps a clear margin below the surface so kelp never pokes
      // through — that margin is the difference between kelp and a reed. This
      // is the plant that gives the submerged view its vertical structure for
      // the light shafts to cut across.
      mat = M_KELP;
    }
  }
  // Above the waterline over a pond: the emergent half of the reeds, and the
  // lily blossoms that sit proud of their pads. Both are placed in AIR cells,
  // so they are the same features as the water-cell block above continued
  // upward — same hashes, same column tests, so a reed is one continuous stalk
  // through the surface rather than two unrelated halves.
  if (mat == MAT_AIR && pond >= 0 && y > pond) {
    let bed = min(h, pw.x);
    let depth = pond - bed;
    let hLily = hash3(seed ^ 0x71A9u, bitcast<u32>(x), bitcast<u32>(z));
    let hReed = hash3(seed ^ 0x2E3Du, bitcast<u32>(x), bitcast<u32>(z));
    if (depth >= 4 && depth <= 14 && y - bed < TUNE_REED_HEIGHT &&
        (hReed % TUNE_REED_CHANCE) == 0u) {
      mat = M_REED;
    } else if (y == pond + 1 && depth >= 10 &&
               (hLily % TUNE_LILY_CHANCE) == 0u &&
               ((hLily >> 9u) % TUNE_LILY_FLOWER_CHANCE) == 0u) {
      // Blossom on a minority of pads. Gated on the SAME pad roll, so a flower
      // can only ever appear on a cell that actually grew a pad under it.
      mat = M_LILYFLR;
    }
  }

  // ---- surface cover: trees, then ground flora ----
  // Only above ground and out of the water, and never inside the authored rims
  // (a tree rooted on a pool rim would drop leaves into the pool).
  if (mat == MAT_AIR && !inRim && y > h && h < TREELINE && pond < 0) {
    let tm = treeAt(x, y, z, seed);
    if (tm != MAT_AIR) { mat = tm; }
  }

  // ---- shore cover: the marsh fringe outside the pond ----
  // Cattails, marsh grass, horsetail and the water iris, on the band shoreAt()
  // found. Placed only into cells that are still AIR above the ground, so
  // nothing here can displace terrain, and — because the whole block is gated
  // on `shore.onShore`, which is only ever set outside a disc — nothing here
  // can spill into the bowl either. Running AFTER the tree block means a trunk
  // rooted on the bank keeps its cells; the marsh grows around it, which is
  // what a real bankside willow looks like.
  //
  // Everything is an INERT solid placed once at generation, exactly like the
  // pond life inside the disc. Nothing grows, spreads or reacts with the water
  // it stands beside: a shore plant that did would keep every pond chunk awake
  // forever and break the sleep budget (rule 2).
  //
  // FOUR DISTINCT HASH SALTS, one per species, never bit-slices of one hash.
  // The pond-life block above documents why at length — slices of a single
  // hash share entropy, so a column that grew one plant is far likelier than
  // chance to grow another, and the scattered planting collapses into a wall.
  // The cost is three extra hashes on shore columns only.
  //
  // Species by DISTANCE FROM THE WATER, so the band reads as a gradient rather
  // than as a mixed salad: cattails have their feet wet, horsetail stands just
  // behind them, marsh grass covers the lot, and the iris is the rare accent.
  if (mat == MAT_AIR && shore.onShore && y > h) {
    let up = y - h;                  // voxels above this column's ground
    let hCat  = hash3(seed ^ 0x9C41u, bitcast<u32>(x), bitcast<u32>(z));
    let hHors = hash3(seed ^ 0x3E77u, bitcast<u32>(x), bitcast<u32>(z));
    let hSedge= hash3(seed ^ 0x58BDu, bitcast<u32>(x), bitcast<u32>(z));
    let hIris = hash3(seed ^ 0xA219u, bitcast<u32>(x), bitcast<u32>(z));

    // CATTAILS: the tall silhouette at the waterline, and the only thing here
    // that is more than a couple of voxels tall. Height jitters per column —
    // a bed of stalks all cut to exactly one height reads as a fence.
    let catH = TUNE_SHORE_CATTAIL_HEIGHT + i32((hCat >> 5u) % 7u) - 3;
    if (shore.past <= TUNE_SHORE_CATTAIL_REACH && up < catH &&
        (hCat % TUNE_SHORE_CATTAIL_CHANCE) == 0u) {
      // The brown seed head caps the top two cells of the stalk. Not its own
      // roll: it is part of the same plant, so gating it on the SAME hash is
      // what keeps a head from ever floating over a column with no stalk.
      mat = select(M_CATTAIL, M_CATTAIL_HEAD, up >= catH - 2);
    } else if (up < TUNE_SHORE_HORSETAIL_HEIGHT + i32((hHors >> 5u) % 5u) - 2 &&
               (hHors % TUNE_SHORE_HORSETAIL_CHANCE) == 0u) {
      // HORSETAIL: mid-height jointed stalks filling between the cattails at
      // the water and the grass further up the bank. Grey-green, so the three
      // species do not merge into one block of the same colour.
      mat = M_HORSETAIL;
    } else if (up == 1 && (hIris % TUNE_SHORE_IRIS_CHANCE) == 0u) {
      // WATER IRIS: one cell, a micro model. Rare on purpose — this is the
      // thing you spot, not the thing you wade through.
      mat = M_WATER_IRIS;
    } else if (up == 1 && (hSedge % TUNE_SHORE_SEDGE_CHANCE) == 0u) {
      // MARSH GRASS: one cell, a micro model, and the densest of the four. It
      // is the ground cover of the whole band, which is what makes the fringe
      // read as marsh rather than as lawn running up to water — so it is rolled
      // LAST, filling whatever the taller species did not claim.
      mat = M_MARSH_GRASS;
    }
  }

  // ---- ground cover: undergrowth under the canopy, flowers in the gaps ----
  //
  // ONE block, TWO layers, split by canopy cover. The forest floor used to be a
  // single grass skin with confetti flowers on it, which is exactly backwards
  // for a closed canopy: under a crown almost no light reaches the ground, so
  // what grows there is the shade set (fern, mushroom, moss, bramble, litter,
  // and the seedlings waiting for a light gap), and grass and flowers are what
  // fill the GAPS between crowns. Inverting on cover is what turns a uniform
  // green skin into a layered forest.
  //
  // Everything here is INERT and lives in the ONE voxel above the surface, so a
  // settled world still costs nothing (rule 2). Nothing in this block is a
  // `stem`/`sprout`/`seed`; a generated forest of growing plants would keep
  // every chunk in the world awake, which is the trap the file header names.
  //
  // The fixture pads stay bare for the same reason they stay sandy: a single
  // grass tuft above the surface is a SOLID voxel the selftest's dropped bodies
  // come to rest on, which lifts them a voxel and re-geometries the burn. The
  // undergrowth materials are `passable` — the player walks through a fern
  // rather than into it — but passable is a COLLISION property only; the CA,
  // fire, the brush and the renderer all still see a solid, so the rule applies
  // to them exactly as it does to grass.
  // `!shore.onShore`: the shore band has its OWN cover set (the block above),
  // and a column that grew no marsh plant should stay bare rather than fall
  // through to the upland set. Meadow flowers and dry-woodland ferns scattered
  // through a reed bed are what would give away that the marsh is a decal on
  // ordinary ground instead of a different place — the same reason the shore
  // ground skin is mud rather than a tinted grass.
  if (mat == MAT_AIR && y == h + 1 && !inRim && pond < 0 && h < TREELINE &&
      biome != B_DESERT && !onFixturePad(x, z) && !shore.onShore) {
    let fr = hash3(seed ^ 0xF10Eu, bitcast<u32>(x), bitcast<u32>(z));
    // ONE 25-tile scan answers both "how shaded is this column" and "how far to
    // the nearest trunk". Calling treeCanopyAt as well would run the identical
    // scan a second time for a strictly weaker answer.
    let ug = undergrowthSite(x, z, seed);

    // SEPARATE HASH SALTS PER SPECIES, never bit-slices of one hash. Slicing
    // (fr, fr>>3, fr>>17) looks independent and is not — the slices share
    // entropy, so a column that grew one plant is far more likely than chance
    // to grow another, and a scattered planting collapses into clumps of
    // everything-at-once. That is the bug the pond-life block above documents;
    // it cost that feature a solid wall of stalks. These are the same cost as
    // the pond block pays: a handful of extra hashes on surface columns only.
    let hFern  = hash3(seed ^ 0xFE7Au, bitcast<u32>(x), bitcast<u32>(z));
    let hShroom= hash3(seed ^ 0x5A17u, bitcast<u32>(x), bitcast<u32>(z));
    let hMoss  = hash3(seed ^ 0x3C0Bu, bitcast<u32>(x), bitcast<u32>(z));
    let hSap   = hash3(seed ^ 0x9D42u, bitcast<u32>(x), bitcast<u32>(z));
    let hBram  = hash3(seed ^ 0x61E9u, bitcast<u32>(x), bitcast<u32>(z));
    let hLit   = hash3(seed ^ 0x0B8Fu, bitcast<u32>(x), bitcast<u32>(z));

    // Patch masks, so undergrowth grows in stands rather than as uniform
    // static — the same device the flower clump mask uses, and for the same
    // reason: uniform density at any rate reads as noise, never as a place.
    // Two independent fields at different scales so a fern bank and a moss
    // patch are not the same patch wearing different plants.
    let fernPatch = vnoise(x, z, 20 * HSCALE, seed ^ 0xFE70u);
    let mossPatch = vnoise(x, z, 14 * HSCALE, seed ^ 0x3C00u);

    // ---- layer 1: under the canopy ----
    // UG_COVER_MIN is where the crown's shadow is deep enough that the shade
    // plants win. It sits at the halfway point of the cover ramp so the
    // transition lands inside the crown rather than exactly on its rim — a
    // rim-aligned transition draws a visible circle of fern around every tree,
    // which is the artifact this threshold exists to avoid.
    if (ug.cover >= UG_COVER_MIN) {
      // MUSHROOMS AT THE TREE BASE. The single cheapest high-value detail
      // available here: trunk position is already known from the same scan, so
      // a ring of fungus around the bole costs one comparison. The ring is an
      // ANNULUS, not a disc — the trunk itself occupies the middle, and
      // mushrooms grow on the leaf mould around a bole rather than on the bark.
      // Radius scales with the tree so a great oak carries a wider ring.
      let ringOut = UG_SHROOM_RING + i32(ug.rnd >> 28u);
      let atBase = ug.trunkD2 > 9 && ug.trunkD2 < ringOut * ringOut &&
                   ug.species != 4u;
      if (atBase && (hShroom % UG_SHROOM_BASE_CHANCE) == 0u) {
        // Red fly-agaric is the rarer, showier one; the pale toadstool is the
        // common ring. Gated on the SAME roll that placed a mushroom at all, so
        // this only ever picks WHICH mushroom, never adds more of them.
        mat = select(M_TOADSTOOL, M_MUSHROOM, ((hShroom >> 13u) % 4u) == 0u);
      } else if ((hFern % UG_FERN_CHANCE) == 0u &&
                 fernPatch > UG_FERN_PATCH) {
        // FERNS: the signature closed-canopy plant, and the tallest thing in
        // this layer. Restricted to the patch mask so they form banks.
        mat = M_FERN;
      } else if ((hBram % UG_BRAMBLE_CHANCE) == 0u && ug.cover < UG_COVER_DEEP) {
        // BRAMBLES want the HALF-lit margin, not the deep shade — they are the
        // plant of a woodland edge and a light gap. Gating them below
        // UG_COVER_DEEP is what keeps them out of the darkest interior, where
        // the fern and moss belong.
        mat = M_BRAMBLE;
      } else if ((hMoss % UG_MOSS_CHANCE) == 0u && mossPatch > UG_MOSS_PATCH) {
        // MOSS: the damp carpet. Its own patch field, so a moss patch and a
        // fern bank are different places.
        mat = M_MOSS;
      } else if ((hSap % UG_SAPLING_CHANCE) == 0u) {
        // SAPLINGS: deliberately RARE. A seedling every few metres reads as a
        // nursery, not as a forest; and unlike everything else in this layer a
        // sapling is a recognisable tree, so the eye finds it. It is also the
        // one that must never become reactive — a growing sapling is exactly
        // the "reaction-driven growth" rule 2 forbids.
        mat = M_SAPLING;
      } else if ((hLit % UG_LITTER_CHANCE) == 0u) {
        // LEAF LITTER: the cheapest and commonest cover, one voxel of fallen
        // leaves and twigs. Last in the chain on purpose — it is the default
        // floor of a wood, so it fills whatever the plants above did not take.
        mat = M_LITTER;
      }
    } else {
      // ---- layer 2: the gaps ----
      // The ORIGINAL grass/flower block, now gated on LOW canopy cover. It was
      // always meant to be the light-loving layer; it just had nothing to be
      // the complement of. Its rates are untouched.
      //
      // clump mask, species field and per-species rolls all live in flowerAt()
      // now, because the upper cells of a tall flower have to re-derive exactly
      // the same answer. Everything the old inline block did is still done, in
      // the same order, with the same salts and the same rates — see flowerAt.
      let fl = flowerAt(x, z, seed, ug.cover);
      if (fl.mat != MAT_AIR) {
        // The base cell of the plant. Cells 1..height-1 are placed by the
        // separate stalk branch below, which re-derives this same answer.
        mat = fl.mat;
      } else if (ug.cover >= UG_COVER_EDGE &&
                 (hLit % UG_LITTER_EDGE_CHANCE) == 0u) {
        // The half-lit margin still gets litter, thinly. Without it the two
        // layers meet on a hard line — flowers on one side, fern on the other —
        // and the boundary reads as a seam. A thinning scatter of fallen leaves
        // reaching a little way out past the crown is what a real canopy edge
        // looks like, and it costs one more roll on columns that grew nothing.
        mat = M_LITTER;
      }
    }
  }

  // ---- meadow flowers, cells 2..height: the rest of the stalk ---------------
  // The block above places only the BASE cell (y == h + 1). A flower taller
  // than one cell continues here, exactly the way the reed block continues its
  // stalk above the waterline: same column, same hashes, same species answer,
  // so the plant is one continuous thing rather than two features that happen
  // to touch.
  //
  // COST. This branch is deliberately NOT part of the block above, because that
  // block runs undergrowthSite() — the 25-tile scan — and putting the stalk
  // inside it would multiply the most expensive thing on the surface by the
  // flower height. Here the scan is replaced by ONE cheap fact: the only
  // species that needs canopy cover is the wild rose, and cover is a property
  // of the COLUMN, not of Y. So the stalk asks flowerAt for the species with
  // cover forced to the edge threshold, and then keeps the answer only if the
  // base cell agrees — `mat` at the base is already the authority. Concretely:
  // a column whose base grew a rose regrows a rose here; a column whose base
  // grew something else regrows that. The one case the shortcut could differ on
  // (cover below the rose threshold) is the case where flowerAt returns the
  // non-rose species anyway, because the rose is the LAST override in the
  // chain — so forcing cover high can only ever ADD a rose to a column that
  // already rolled `hRose % 9 == 0`, and that column's base grew a rose too.
  //
  // Y range is bounded by the tallest flower (FLOWER_MAX_H), so a column pays
  // at most that many extra evaluations and a settled world still costs nothing
  // (rule 2 — nothing here is reactive).
  if (mat == MAT_AIR && y > h + 1 && y <= h + FLOWER_MAX_H &&
      !inRim && pond < 0 && h < TREELINE &&
      biome != B_DESERT && !onFixturePad(x, z) && !shore.onShore) {
    let fl = flowerAt(x, z, seed, UG_COVER_EDGE);
    // Grass and petal are the one-cell ground layer and never stack.
    if (fl.mat != MAT_AIR && fl.mat != M_GRASS && fl.mat != M_PETAL &&
        (y - h) <= fl.height) {
      mat = fl.mat;
      // Tall grass caps its stack with the head material — dried tips at the
      // per-plant height, the way cattail_head caps the cattail stalk. Only
      // the terminal cell: heights start at 4, so the base block below never
      // needs the same test.
      if (fl.mat == M_TALLGRASS && (y - h) == fl.height) {
        mat = M_TALLGRASS_HEAD;
      }
    }
  }

  // ---- DESERT: cacti, then the scrub-and-tussock floor ----------------------
  // The desert generated as bare sand with an occasional dead bush, and the
  // ground-flora block above excludes it outright (`biome != B_DESERT`). That
  // exclusion is correct — meadow flowers in a desert would be absurd — but it
  // left the biome with no ground layer at all, so the one place in the world
  // you deliberately walk TO was the one place with nothing to look at.
  //
  // Two layers, in the order they occlude each other:
  //   1. CACTI, a metre-scale implicit shape (cactusAt), placed into air ABOVE
  //      the surface exactly the way a tree is.
  //   2. GROUND COVER, one voxel above the surface, only where a cactus did
  //      not already claim the cell.
  //
  // Everything is INERT (rule 2): no reaction uses any of these as `self` with
  // an emit, so a generated desert settles and sleeps exactly as bare sand did.
  // Nothing here is a `stem`/`sprout`/`seed`.
  //
  // KEEP-OUTS are the same as every other feature's: the authored pool rims,
  // the ponds, the spawn clearing and the selftest fixture pads. cactusInfo()
  // enforces them at the SITE (so a column rooted outside cannot lean back in),
  // and the ground block re-tests them per column.
  if (mat == MAT_AIR && biome == B_DESERT && !inRim && y > h && pond < 0 &&
      h < TREELINE) {
    let cm = cactusAt(x, y, z, seed);
    if (cm != MAT_AIR) { mat = cm; }
  }

  // Desert ground cover. Same shape as the flora block above: one voxel above
  // the surface, gated by a patch mask so the biome keeps open sand between its
  // stands. The mask is what makes a desert read as arid — an even sprinkle of
  // tussock over the whole biome is a dry lawn, and the bare stretches between
  // stands are the thing that says "desert" rather than "dry field".
  //
  // TWO DISTINCT HASH SALTS, one per species, never bit-slices of one hash —
  // the pond-life block above documents why at length, and these two species
  // would visibly co-locate if they shared entropy.
  if (mat == MAT_AIR && y == h + 1 && biome == B_DESERT && !inRim && pond < 0 &&
      h < TREELINE && !onFixturePad(x, z)) {
    // Patch mask, offset off the other flora lattices so the two do not line up
    // at their cell corners (the same reason the wildflower species field is
    // sampled at an offset).
    let cover = vnoise(x - 617, z + 431, 34 * HSCALE, seed ^ 0xD5E7u);
    if (cover > TUNE_DESERT_PATCH) {
      let hTus = hash3(seed ^ 0x7055u, bitcast<u32>(x), bitcast<u32>(z));
      let hScr = hash3(seed ^ 0x5C2Bu, bitcast<u32>(x), bitcast<u32>(z));
      // Tussock first and commonest: it is the species that turns bare sand
      // from a texture into ground. Scrub is the sparser woody accent among it.
      if ((hTus % TUNE_TUSSOCK_CHANCE) == 0u) {
        mat = M_TUSSOCK;
      } else if ((hScr % TUNE_SCRUB_CHANCE) == 0u) {
        mat = M_SCRUB;
      }
    }
  }

  // ---- PINE HIGHLANDS: the conifer floor ------------------------------------
  // The pine biome grew trees and nothing under them. A conifer stand has a real
  // floor — huckleberry and juniper in the light gaps, needles everywhere else —
  // and without it the highlands read as trunks standing on bare stone.
  //
  // Gated on the pine BIOME rather than on canopy cover, because the undergrowth
  // block (where present) owns the cover-driven layer and this is the species
  // set that belongs to a biome. A column that grew undergrowth is already
  // non-air by the time we get here, so the two never fight over a cell: this
  // fills what the shade set left bare.
  if (mat == MAT_AIR && y == h + 1 && biome == B_PINE && !inRim && pond < 0 &&
      h < TREELINE && !onFixturePad(x, z)) {
    let cover = vnoise(x + 271, z - 859, 30 * HSCALE, seed ^ 0x4EA7u);
    if (cover > TUNE_HEATH_PATCH) {
      let hHth = hash3(seed ^ 0x483Bu, bitcast<u32>(x), bitcast<u32>(z));
      if ((hHth % TUNE_HEATH_CHANCE) == 0u) {
        mat = M_HEATH;
      }
    }
  }

  // ---- SNOWLINE: hardy alpine cushions above the treeline -------------------
  // Everything else in this file stops at TREELINE (`h < TREELINE` gates the
  // trees, the flowers, the undergrowth and both blocks above), which left the
  // high ridges as pure bare snow — correct, and completely dead.
  //
  // ONE species, DELIBERATELY SPARSE. The point of the alpine band is that it
  // reads as harsh, so what goes up there is a scatter of cushions clinging on,
  // not a planted ridge. TUNE_ALPINE_CHANCE defaults to the sparsest density in
  // worldgen for exactly that reason, and making it generous is the one change
  // that undoes the intent of the whole band.
  //
  // This is the ONE cover block gated on `h >= TREELINE` rather than
  // `h < TREELINE`, which is also why it cannot collide with any of them: no
  // column satisfies both.
  //
  // The same material doubles as the lichen crust on exposed rock. At this scale
  // a cushion plant and a lichen mat are the same object — a couple of
  // centimetres of growth pressed flat against the ground — so rather than spend
  // a material id on the distinction, the ground under it makes it: on snow the
  // cell reads as a cushion, on wind-scoured stone as lichen.
  if (mat == MAT_AIR && y == h + 1 && h >= TREELINE && !inRim && pond < 0 &&
      !onFixturePad(x, z)) {
    let hAlp = hash3(seed ^ 0xA1F1u, bitcast<u32>(x), bitcast<u32>(z));
    // A patch mask here too, but a WEAK one: alpine plants really do grow in
    // scattered colonies wherever the wind lets them, so the mask only thins the
    // most exposed ground rather than carving the band into stands.
    let cover = vnoise(x - 1103, z + 977, 26 * HSCALE, seed ^ 0xA1F2u);
    if (cover > 96 && (hAlp % TUNE_ALPINE_CHANCE) == 0u) {
      mat = M_CUSHION;
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

  // ---- combat test arena (authored POI) ----
  // A flat walled deck a short walk from the spawn point, for trying melee,
  // spells and mob fights on ground that isn't a noisy hillside. Terrain slope
  // is the confound this removes: on natural ground a miss is ambiguous between
  // bad reach and a foot half a voxel up a slope.
  //
  // Placed OFF the x==z diagonal on purpose. Every selftest fixture column sits
  // on it (60,80,90,100,108,120,140,150) and each one assumes TerrainHeight()
  // is the top of the world there, so a deck over any of them would turn a
  // passing gate into a mystery — the same trap inSpawnClearing() documents.
  // z stays <= 142 to clear the wood platform above (z >= 146).
  //
  // The deck is ONE flat plane and the space between it and the real terrain is
  // filled, so there is no lip to trip the step-up and no cave under the floor.
  // Everything is anchored to baseHeight rather than a literal Y, so the arena
  // rides the terrain wherever the seed puts it.
  let arenaCX = 180;
  let arenaCZ = 110;
  let arenaHalf = 32;                 // 64 voxels square, ~4 m
  // The deck sits ABOVE the highest ground in its own footprint, not at the
  // centre height. Terrain here spans 20 voxels across 64 (51..71 at the
  // default seed, centre 60), so levelling to the centre buried the uphill half
  // and dug the deck into a pit you could not see over the rim of — measured,
  // not guessed. +16 clears the +11 worst case with margin for other seeds, and
  // turns the arena into a low plinth that reads as built rather than excavated.
  let arenaY = baseHeight(arenaCX, arenaCZ, seed) + 16;
  let adx = x - arenaCX;
  let adz = z - arenaCZ;
  let inArena = abs(adx) <= arenaHalf && abs(adz) <= arenaHalf;
  if (inArena) {
    // Deck plus the plinth under it, filled all the way down past the lowest
    // ground so a downhill corner is supported instead of hanging over a void.
    if (y <= arenaY && y > arenaY - 64) { mat = M_STONE; }
    // Nothing survives above the deck: the plane is the floor everywhere.
    if (y > arenaY) { mat = MAT_AIR; }

    // Perimeter wall, 2 voxels thick and 24 tall (1.5 m) — high enough to keep
    // a spawned mob in, low enough to see over in third person.
    // Doorways are 32 voxels (2 m) tall so they clear the 1.7 m player, the
    // same reason the ruin's door is not halved with the rest of the world.
    let onWall = abs(adx) >= arenaHalf - 1 || abs(adz) >= arenaHalf - 1;
    let inDoor = (abs(adx) <= 10 && abs(adz) >= arenaHalf - 1) ||
                 (abs(adz) <= 10 && abs(adx) >= arenaHalf - 1);
    if (onWall && !inDoor && y > arenaY && y <= arenaY + 24) {
      mat = M_STONE;
    }
  }

  // ---- ivy on the arena wall ----
  // Same closed-form trick as the tree ivy, one level up: rather than sampling
  // the neighbouring column to ask "is there a wall next to me?", re-evaluate
  // the WALL PREDICATE ITSELF at the adjacent column. The predicate is a box
  // test on constants, so this is a few comparisons and no world access — the
  // reason a per-cell function can have neighbour-aware decoration at all.
  //
  // Ivy climbs from the wall foot up, thinning with height (a creeper that
  // reaches the coping everywhere reads as paint, not as a plant), and skips
  // the doorways so the entrances stay legible.
  if (mat == MAT_AIR && y > arenaY && y <= arenaY + 24) {
    let climb = y - arenaY;                       // 1..24 up the wall
    // the two faces this column could be leaning against
    let nearX = abs(abs(adx) - (arenaHalf - 2)) == 0 && abs(adz) < arenaHalf - 1;
    let nearZ = abs(abs(adz) - (arenaHalf - 2)) == 0 && abs(adx) < arenaHalf - 1;
    let outX = abs(abs(adx) - arenaHalf) == 1 && abs(adz) <= arenaHalf;
    let outZ = abs(abs(adz) - arenaHalf) == 1 && abs(adx) <= arenaHalf;
    let doorHere = (abs(adx) <= 12 && abs(abs(adz) - arenaHalf) <= 2) ||
                   (abs(adz) <= 12 && abs(abs(adx) - arenaHalf) <= 2);
    if ((nearX || nearZ || outX || outZ) && !doorHere) {
      let hw = hash3(seed ^ 0x19A7u, bitcast<u32>(x), bitcast<u32>(z));
      // Coverage falls off linearly with height: full odds at the foot, none
      // at the coping. Integer compare against a 0..24 ramp, no float.
      let want = i32(hw % 32u);
      if (want * 24 < (24 - climb) * i32(32u / TUNE_WALL_IVY_DENSITY) &&
          ((hw >> 13u) % 3u) != 0u) {
        mat = M_IVY;
      }
    }
  }

  // Approach ramp up to the -z doorway. The deck stands ~16 voxels (1 m) proud
  // of the ground, which is well over the step-up reach, so without this the
  // only way in is to jump the plinth wall. Runs 24 voxels out from the wall and
  // rises linearly, giving a ~34 degree slope the gait walks up without the
  // step-up ever firing.
  let rampLen = 24;
  let rampOut = (arenaCZ - arenaHalf) - z;      // 0 at the wall, grows outward
  // As wide as the doorway it feeds (+-10 -> 21 voxels), so walking straight at
  // the gap never drops you off the side of the approach.
  if (abs(adx) <= 10 && rampOut > 0 && rampOut <= rampLen) {
    let rampTop = arenaY - (arenaY - baseHeight(x, z, seed)) * rampOut / rampLen;
    if (y <= rampTop && y > rampTop - 64) { mat = M_STONE; }
    if (y > rampTop) { mat = MAT_AIR; }
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
      // ---- ivy over the ruin ----
      // A ruin is the one structure in the world that is meant to look OLD, so
      // it gets the heaviest coverage. Tested on a box one voxel WIDER than the
      // building so the outer faces are reachable: the shell test above only
      // runs inside the footprint, and the cell hugging the outside of a wall
      // is not in it. Same predicate-re-evaluation trick as the arena wall —
      // no neighbour sampling, just the same closed-form box at ±1.
      if (mat == MAT_AIR &&
          x >= rx - 1 && x < rx + rw + 1 && z >= rz - 1 && z < rz + rw + 1) {
        let ry = baseHeight(rx + rw / 2, rz + rw / 2, seed);
        let climb = y - ry;
        if (climb > 0 && climb < rht) {
          // faces: just outside the shell, or just inside it
          let fx = (x == rx - 1 || x == rx + rw) && z >= rz - 1 && z < rz + rw + 1;
          let fz = (z == rz - 1 || z == rz + rw) && x >= rx - 1 && x < rx + rw + 1;
          let ix = (x == rx + 4 || x == rx + rw - 5) &&
                   z >= rz + 4 && z < rz + rw - 4;
          let iz = (z == rz + 4 || z == rz + rw - 5) &&
                   x >= rx + 4 && x < rx + rw - 4;
          let atDoor = abs(z - (rz + rw / 2)) <= 14 && x <= rx + 5 && climb < 34;
          if ((fx || fz || ix || iz) && !atDoor) {
            let hr = hash3(seed ^ 0x2117u, bitcast<u32>(x), bitcast<u32>(z));
            // Ruins are overgrown from the ground up: full coverage low down,
            // thinning out near the roofline.
            let want = i32(hr % 32u);
            if (want * rht < (rht - climb) * i32(48u / TUNE_WALL_IVY_DENSITY)) {
              mat = M_IVY;
            }
          }
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
  // STAMP_NEVER, not a live code: a generated voxel has not acted, so it must
  // be free to move on the first tick it is simulated. (This was 0xFF when the
  // stamp was a byte; masked into the 3-bit field that would be 7, a REAL
  // stamp code, and every worldgen voxel would sit out one substep in 1 tick
  // out of 7.)
  return packVox(mat, state, STAMP_NEVER);
}

// The one-shot form: one isolated cell, column and all. This is what genCell
// has always been, and it stays the definition for the callers that sample a
// single scattered cell and have no column to amortize over — the far-field
// cascade sampler and the ruin skin lookup. genChunk does NOT use it; it walks
// columns and calls the two halves itself, which is the whole point of the
// split.
fn genCell(c : vec3<i32>, seed : u32) -> u32 {
  return genCellIn(genColumn(c.x, c.z, seed), c.x, c.y, c.z, seed);
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
  // The combat arena levels its whole footprint to one plane (mirrors genCell).
  // Without this the far field keeps painting the ORIGINAL hillside height
  // there, so the deck reads as the wrong material at distance and pops when
  // you walk into fine-detail range.
  if (abs(x - 180) <= 32 && abs(z - 110) <= 32) {
    h = baseHeight(180, 110, seed) + 16;
  }
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
// Cells in this chunk that can ACT (matCanAct, common.wgsl). Third counter
// rather than a reuse of wgCount because occupancy needs the total and the
// wake needs this one, and they are different questions about the same sweep.
var<workgroup> wgAct : atomic<u32>;

fn genChunk(slot : u32, li : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
    atomicStore(&wgAct, 0u);
  }
  workgroupBarrier();

  let sc = vec3<i32>(vec3<u32>(slot % NCHUNK, (slot / NCHUNK) % NCHUNK,
                               slot / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  var count = 0u;
  var block = 0u;
  var act = 0u;
  // COLUMN-MAJOR, and that is the whole point of the genColumn/genCellIn
  // split: the column half is evaluated ONCE per (x, z) and shared by the 16
  // cells stacked on it, instead of being recomputed by every one of them.
  // 256 columns over 64 threads is four columns each, 16 cells apiece — the
  // same 4,096 cells and the same per-thread count as the flat loop this
  // replaces, just grouped so the invariant work can be hoisted.
  //
  // The writes stay coalesced enough: within one height step threads 0..15
  // hold x = 0..15 of the same z and write 16 CONSECUTIVE words, so the wave
  // issues four 64-byte runs instead of one 256-byte one.
  for (var ci = li; ci < CHUNK * CHUNK; ci += 64u) {
    let lx = ci % CHUNK;
    let lz = ci / CHUNK;
    let col = genColumn(base.x + i32(lx), base.z + i32(lz), T.seed);
    for (var ly = 0u; ly < CHUNK; ly += 1u) {
      let i = lx + ly * CHUNK + lz * CHUNK * CHUNK;
      let w = genCellIn(col, base.x + i32(lx), base.y + i32(ly),
                        base.z + i32(lz), T.seed);
      // Chunk-linear: the slot's page resolved once, per §2.1's second entry
      // point. genChunk overwrites the WHOLE chunk, so the CPU materializes
      // every target slot before the dispatch (§3.5c) and this never faults.
      voxStore(voxWordInChunk(slot, i), w);
      let m = w & 0xFFFu;
      if (m != MAT_AIR) {
        count += 1u;
        let md = materials[m];
        if (isRayBlocker(md)) { block += 1u; }
        if (matCanAct(md)) { act += 1u; }
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  atomicAdd(&wgAct, act);
  workgroupBarrier();

  if (li == 0u) {
    let n = atomicLoad(&wgCount);
    // in-kernel so the renderer never sees a stale 0.
    //
    // packOcc, not packOccStain, and that is correct rather than an omission:
    // worldgen writes no stain bits at all, so a freshly generated chunk is
    // stainless by construction and bit 31 must read 0. The page-table free
    // path relies on exactly that — a generated sky chunk has to be demotable
    // on sight, without a readback.
    occupancy[slot] = packOcc(n, atomicLoad(&wgBlock));
    // ---- THE STREAMING WAKE (CLAUDE.md rule 2) -------------------------
    //
    // Wake once so loose material settles and then sleeps. The question is
    // WHICH chunks that has to mean, and `n > 0` — "it holds any matter at
    // all" — was far wider than the answer: buried stone and static plant
    // dressing cannot act, and a streamed-in vertical plane is mostly buried
    // stone. Every one of those was a workgroup in all 54 CA dispatches for a
    // tick, in the regime (~550 active chunks under surface flight) where
    // ROADMAP_scale.md §3.0's PER-CHUNK term is 96% of the CA cost.
    //
    // The predicate is `matCanAct` (common.wgsl), evaluated over the cells
    // this loop already visited with the material this loop already read, so
    // it costs one compare per non-air cell. It is the SAME function sim_step
    // returns on, which is what makes the change bit-identical rather than
    // merely plausible: a chunk where no cell can act is a chunk where every
    // thread of every colour pass would return before writing anything, so
    // dispatching it and not dispatching it produce the same voxels, the same
    // dirty set, and the same world hash. `7cfa2420` is the gate.
    //
    // NOT a claim that nothing can ever happen there. A cave-in, a brush edit,
    // an explosion or an acting neighbour all wake the chunk through the
    // ordinary paths (markDirty reaches every bordering chunk, and mutations
    // set both flags themselves). This only declines to wake it AT BIRTH.
    if (atomicLoad(&wgAct) > 0u) {
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

// ---- JITTER page materialization (world.h's JITTER block) ----------------
// Filling a page that replaces a JITTER sentinel cannot be a vkCmdFillBuffer:
// the sentinel's words vary per cell, and a fill takes ONE 32-bit pattern.
// EMPTY and UNIFORM keep the cheap one-command fill; only JITTER comes here.
//
// This lives in worldgen.wgsl rather than its own file because it needs
// exactly what genChunk needs — the slot->world mapping, T.origin, T.seed —
// and sharing the file is what keeps the two positional rules from drifting.
// It does NOT call genCell: a JITTER chunk's material is whatever the sentinel
// says (it may be the result of play, not of worldgen), and only the palette
// VARIANT follows worldgen's formula. Calling genCell here would silently
// revert a mined-out chunk to pristine terrain.
//
// Reuses genList/T.genCount: page fills and worldgen list-fills are always
// separate submits (page fills are drained at the head of a tick's command
// buffer, worldgen list-fills are their own mid-frame encoder), so the two
// never contend for the buffer.
//
// The list holds SLOT indices whose table entry is ALREADY the freshly
// allocated page — the CPU rewrote it before this dispatch — so the entry no
// longer says JITTER and cannot be read back here. The material and jitter
// flag therefore travel in the list itself: two u32 per entry, slot then the
// sentinel entry it is replacing.
@compute @workgroup_size(64)
fn pagefill(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  // NO `wg.x >= T.genCount` GUARD, deliberately — and this cost a debugging
  // cycle, so it is written down.
  //
  // T.genCount belongs to the TICK UBO, which only Stream::FillSlots writes
  // (stream.cpp, before EncodeGenList). EncodePageFill sets the recorder's
  // dispatch EXTENT from its own count but never touches the uniform, so
  // T.genCount here still holds the previous tick's value — 0 in every normal
  // tick. With the guard, every one of the dispatched workgroups returned
  // immediately, the pages materialized as ALL ZEROS, and 2,114 chunks of
  // stone silently became air (measured: the world hash moved and slot 0
  // digested 76EFDDC5 instead of 360F1DC5).
  //
  // The extent IS the bound: D_GENCOUNT dispatches exactly one workgroup per
  // (slot, entry) pair, so wg.x is in range by construction. `list` above needs
  // its guard because it shares the tick's UBO write; this entry point does not
  // share that write and must not read that field.
  let slot  = pageFillList[wg.x * 2u];
  let entry = pageFillList[wg.x * 2u + 1u];
  let sc = vec3<i32>(vec3<u32>(slot % NCHUNK, (slot / NCHUNK) % NCHUNK,
                               slot / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  for (var i = li; i < CHUNK_VOL; i += 64u) {
    let l = vec3<i32>(vec3<u32>(i % CHUNK, (i / CHUNK) % CHUNK,
                                i / (CHUNK * CHUNK)));
    // The SAME synthesis the sentinel read as, so the page is bit-identical to
    // what every reader saw one instruction earlier. That equality IS the hash
    // contract; the page-roundtrip gate asserts it.
    voxStore(voxWordInChunk(slot, i), synthWordAt(entry, base + l, T.seed));
  }
  // No occupancy or dirty writes here, deliberately: materialization does not
  // CHANGE the world, it only changes where the world is stored. The chunk's
  // occupancy already reflects these words (sim_occupancy's analytic sentinel
  // branch computed them), and waking it would make a storage decision into a
  // simulation event — exactly the feedback that dilates the dirty set.
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
// One workgroup per farList entry: (level-1) << FAR_SLOT_SHIFT | slot. Each thread
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
  let level = (packed >> FAR_SLOT_SHIFT) + 1u;   // 1-based
  let slot = packed & FAR_SLOT_MASK;
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
      let mat = voxWordAt(fine) & 0xFFFu;
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
