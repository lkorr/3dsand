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
// THE DEFERRED-WAKE SIDE CHANNEL (docs/RESEARCH_streaming_hitch.md R1).
// One u32 per genList POSITION: 1 if this entry's chunk holds a cell that can
// act, 0 otherwise. Written only when T.genDeferWake is set (a window shift);
// the CPU reads it back asynchronously and puts the act set into dirtyIn
// kWakeLatency ticks later, which is what lets the shift stop fencing.
// Binding 30 exists ONLY in simBGL_ — `far`/`fardown` run on the slim group
// and never reach genChunk, exactly like pageFillList at 19.
@group(0) @binding(30) var<storage, read_write> genAct : array<u32>;
// The BAKED TREE ATLAS (src/sim/treeatlas.h). Read-only asset data uploaded
// once at load, like `materials` — see the tree section below for the layout
// and for why worldgen samples a baked grid instead of evaluating tree shapes.
// Binding 26 in BOTH simBGL_ and simSlimBGL_: `far`/`fardown` run on the slim
// group and both reach the tree sampler.
@group(0) @binding(26) var<storage, read> treeAtlas : array<u32>;
// The authored world map (src/sim/worldmap.h): the biome record table now,
// the painted planes and the site table later. Read-only asset data on the
// same terms as treeAtlas, at binding 31 in both sim layouts.
@group(0) @binding(31) var<storage, read> worldMap : array<u32>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Every
// shader that declares `read_write> voxels` must define this: gPtKernel's
// initializer references it, so omitting it is a compile error rather than
// a fault that reports as "unknown".
const PT_KERNEL : u32 = PT_K_WORLDGEN;

const M_STONE : u32 = 1u;
const M_WOOD  : u32 = 2u;
const M_SAND  : u32 = 3u;
const M_GRAVEL : u32 = 4u;
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
// ---- climbers (materials.json ids 77..80) ----
// These numbers are ARRAY POSITIONS in materials.json (id == index + 1), and
// they are not the ids this block was authored against: it was reserved 70..76
// and landed at 77..80 because another agent's block committed in between.
// That is the append-only contract working as intended — recompute from
// positions, never from what was reserved:
//   python -c "import json;[print(i+1,m['id']) for i,m in
//              enumerate(json.load(open('assets/materials/materials.json'))['materials'])]"
// vine_hang (77), creeper_flower (78) and moss_hang (79) still EXIST as
// materials — the brush and the micro models are unchanged — but worldgen no
// longer places any of them: see the note where treeVineFrom used to be. Ivy is
// the one climber still generated, and only on arena and ruin WALLS.
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
// ---- the lawn tuft (materials.json id 39) and the big toadstool (122; iron is 121) ----
// grass_tuft is the one-to-two-cell analytic grass that replaced the solid
// grass/petal cubes flowerAt used to scatter as its "ground layer" (those read
// as green gravel). mushroom_large is a TILE plant — see plantColumnAt.
const M_GRASS_TUFT     : u32 = 39u;
const M_MUSHROOM_LARGE : u32 = 122u;
// ---- THE VOXEL-SIZE SCALE --------------------------------------------------
//
// VOXELS_PER_M is emitted from world.h's kVoxelsPerMetre (the integer
// reciprocal of kVoxelMeters); TUNE_REF_VOXELS_PER_METRE is the scale the
// worldgen TUNING ROWS are authored at, and LoadTuning has already rescaled
// those. What is left is the lengths hardcoded HERE — cave band depths, the
// magma table, tile pitches, the authored set pieces — and `vlen()` is how they
// follow. Multiply first, divide second, so the shipped case (num == den) is
// exact and this whole mechanism is bit-identical to the literals it replaced.
//
// A LENGTH gets vlen(). A count, a probability, a 0..255 noise threshold and a
// GRADIENT do not — a gradient is dimensionless, which is why the angle of
// repose is 1 voxel/column at every voxel size and why pondMaxSlope needs no
// scaling anywhere.
const VLEN_NUM : i32 = VOXELS_PER_M;
const VLEN_DEN : i32 = TUNE_REF_VOXELS_PER_METRE;
fn vlen(v : i32) -> i32 { return (v * VLEN_NUM) / VLEN_DEN; }

// Tallest a meadow flower can be, in CELLS — must be >= the largest value
// flowerHeight() can return (now tall grass, 4 + 4 = 8; foxglove reaches 5).
// It bounds the Y range the stalk branch scans, so an under-count silently
// beheads the tall species and an over-count just costs a few wasted
// evaluations per column.
const FLOWER_MAX_H : i32 = (8 * VLEN_NUM) / VLEN_DEN;
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

// ---- cave flora (materials.json id 116) ----
// The only light under the world that is not lava. Placed by caveFloraAt on the
// deep band's floor and ceiling; see the note in materials.json for why it is
// inert and why its emission is under lava's.
const M_CRYSTAL   : u32 = 116u;

// ---- CAVE / RUIN PLACEMENT CONSTANTS ----
// Plain WGSL consts, same reasoning as the UG_* block above: these are
// PLACEMENT CONTENT, not look/feel. The three DENSITIES are TUNE_* knobs
// (worldgen.caveMushroomChance / caveCrystalChance / mossFace) because those
// are the numbers an author reaches for; the patch masks and the margins are
// structure.
//
// Crystal grows in SEAMS, not as an even sprinkle: the same device the fern and
// moss patches use on the surface, at a cavern's scale. Without it a cavern
// reads as a texture rather than as a place with a find in it.
const CAVE_CRYSTAL_PATCH_CELL : i32 = 40 * HSCALE;
const CAVE_CRYSTAL_PATCH      : i32 = 168;   // vnoise 0..255; ~35% of the area
const CAVE_SHROOM_PATCH_CELL  : i32 = 22 * HSCALE;
const CAVE_SHROOM_PATCH       : i32 = 128;
// Keep-out above the magma table. A crystal seam growing into the lava it is
// lighting is the same class of mistake as a ruin sunk into a hillside.
const CAVE_LAVA_MARGIN : i32 = (6 * VLEN_NUM) / VLEN_DEN;

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
//
// HALVED ACROSS THE BOARD on 2026-09-04 (every 1-in-N below doubled, every
// percent halved, and the same in flowerAt, the shore/pond tuning rows and
// the biome cover rows): the ground layer was the largest single term in
// worldgen and the far refill while flying — a column inside a fern footprint
// pays a second landColumn and a 25-tile tree scan (plantSiteAt), and every
// placed cell is one the renderer treats as a micro model. Density is a look
// knob; halve it here, not by lowering the render LOD.
const UG_FERN_CHANCE    : u32 = 14u;
// TILE PLANTS (ferns, big toadstools): percent of tiles inside the patch mask
// that grow one. Tile size / footprint / height live in common.wgsl as the
// PLANT_* consts because the renderer rebuilds the plant from the same hash.
const PLANT_FERN_CHANCE   : u32 = 22u;
const PLANT_SHROOM_CHANCE : u32 = 3u;
const UG_FERN_PATCH     : i32 = 140;    // vnoise 0..255; ~40% of the area
const UG_MOSS_CHANCE    : u32 = 6u;
const UG_MOSS_PATCH     : i32 = 150;
const UG_BRAMBLE_CHANCE : u32 = 46u;
const UG_SAPLING_CHANCE : u32 = 900u;   // rare on purpose: it reads as a TREE
const UG_LITTER_CHANCE  : u32 = 8u;     // the default floor of a wood
const UG_LITTER_EDGE_CHANCE : u32 = 22u;  // thinner, past the crown rim

// Mushrooms ring the BOLE. Radius in voxels (a great oak's ring is wider, via
// the per-tree jitter added at the call site); the inner d2 > 9 keeps them off
// the trunk cells themselves.
const UG_SHROOM_RING : i32 = (11 * VLEN_NUM) / VLEN_DEN;
const UG_SHROOM_BASE_CHANCE : u32 = 6u;

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

// The LEGACY noise. 0..255 out, linear (not smooth) interpolation, five integer
// divides, and a hard ceiling at a 2901-voxel cell (255*cs^2 crosses 2^31).
//
// Still here, still the right tool, for the fourteen DECORATIVE fields that call
// it — flower clumps, undergrowth patches, desert/heath/alpine cover masks, the
// ruin scatter. Those all run at cs in {11..48}, nowhere near the ceiling, and
// an 8-bit answer is exactly the resolution a "1-in-N inside this patch mask"
// test wants. Rewriting them would re-roll every flower bed in the world for no
// gain. The TERRAIN fields (baseHeight, biomeAt, caveAt) moved to vnoise2d.
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

// MIRROR-BEGIN noise
// ===========================================================================
// Q14 VALUE NOISE — the terrain primitive. Mirrored line-for-line in
// src/sim/world.cpp between the markers of the same name; check_invariants.py
// compares the two token streams, so an edit to one side that is not made to
// the other fails at edit time rather than as a player falling through ground.
//
// THE ONE PLACE THE TWO LANGUAGES COULD HAVE DIVERGED, and the reason this
// block can be written twice at all: `>>` on a NEGATIVE signed integer is an
// ARITHMETIC shift in WGSL (spec: "shift right, sign-extending") and is
// FLOOR-division-by-a-power-of-two in C++20 (P0907, signed integers are two's
// complement and >> is arithmetic). They agree, exactly, at every negative
// coordinate. That is what lets `x >> csl` replace fdiv() and `x & mask`
// replace fmodp() — and it is why the noise cell is passed as a LOG2 SHIFT
// rather than a size. Five integer divides per sample become zero.
//
// WHY 14 BITS OF VALUE and not 8, and not 16:
//   * 8 is an output-RESOLUTION ceiling, not an overflow one. The old vnoise
//     returns 0..255, so at a continental amplitude of 1024 voxels one output
//     LSB is FOUR VOXELS — a perfectly non-overflowing terrain made of 4-voxel
//     terraces. At 14 bits the same amplitude resolves to 1 voxel.
//   * 15 is the overflow ceiling and it has no margin. Written in the
//     sum-of-products form c0*(32768-s) + c1*s, the cross term reaches
//     2*(2^b - 1)*32767; at b = 15 that is 2.147e9 against INT32_MAX's
//     2.1474836e9 — clear by 65k, i.e. by nothing. b = 14 gives 2x headroom,
//     and it is headroom the ladder in package C spends.
// The lerps below are written in the DIFFERENCE form c0 + (c1-c0)*s, which is
// cheaper and has more margin still; 14 is kept anyway so the bound holds no
// matter which form a later edit uses.
struct N2 {
  n  : i32,   // value, Q14: 0..16383
  dx : i32,   // d(n)/d(tx): change in n across one WHOLE CELL of +x, Q14
  dz : i32,   // likewise for +z. Feeds package C's derivative attenuation.
};

// Cubic 3t^2 - 2t^3 and its derivative 6t(1-t), Q15 in, Q15 out. The
// derivative peaks at 1.5 (49152), which is why it may not be packed tighter.
fn vsmooth(t : i32) -> i32 {
  let t2 = (t * t) >> 15;
  let t3 = (t2 * t) >> 15;
  return 3 * t2 - 2 * t3;
}
fn vsmoothd(t : i32) -> i32 {
  return (6 * t * (32768 - t)) >> 15;
}
// In-cell fraction as Q15. Split out (rather than inlined as f << (15 - csl))
// because a cell log2 above 15 is legal tuning and a negative shift is not.
fn q15frac(f : i32, csl : u32) -> i32 {
  if (csl <= 15u) { return f << (15u - csl); }
  return f >> (csl - 15u);
}
// Bilinear blend of four Q14 corners by two Q15 weights.
fn vbilerp(c00 : i32, c10 : i32, c01 : i32, c11 : i32, sx : i32, sz : i32) -> i32 {
  let a = c00 + (((c10 - c00) * sx) >> 15);
  let b = c01 + (((c11 - c01) * sx) >> 15);
  return a + (((b - a) * sz) >> 15);
}

fn vnoise2d(x : i32, z : i32, csl : u32, seed : u32) -> N2 {
  let gx = x >> csl;
  let gz = z >> csl;
  let mask = i32((1u << csl) - 1u);
  let tx = q15frac(x & mask, csl);
  let tz = q15frac(z & mask, csl);
  let c00 = i32(hash3(seed, bitcast<u32>(gx),     bitcast<u32>(gz))     & 0x3FFFu);
  let c10 = i32(hash3(seed, bitcast<u32>(gx + 1), bitcast<u32>(gz))     & 0x3FFFu);
  let c01 = i32(hash3(seed, bitcast<u32>(gx),     bitcast<u32>(gz + 1)) & 0x3FFFu);
  let c11 = i32(hash3(seed, bitcast<u32>(gx + 1), bitcast<u32>(gz + 1)) & 0x3FFFu);
  let sx = vsmooth(tx);
  let sz = vsmooth(tz);
  var o : N2;
  o.n = vbilerp(c00, c10, c01, c11, sx, sz);
  // Analytic gradient: the x-difference of the two z-edges, blended in z, times
  // the smoothstep's own slope. Exact, not a finite difference — a finite
  // difference would cost four more corner hashes and be wrong at cell seams.
  let ga = c10 - c00;
  let gb = c11 - c01;
  o.dx = ((ga + (((gb - ga) * sz) >> 15)) * vsmoothd(tx)) >> 15;
  let ha = c01 - c00;
  let hb = c11 - c10;
  o.dz = ((ha + (((hb - ha) * sx) >> 15)) * vsmoothd(tz)) >> 15;
  return o;
}

// 16-BIT-PHASE INTEGER SINE. Q15 out (-32768..32768), 65536 steps to the turn.
//
// The 8-bit phase of isin() below is the ARTIFACT, not the parabola: a 256-step
// circle driving a ridge flank puts 32 visible terraces across it, and no amount
// of amplitude fixes a quantized angle. The parabola's own error is 5.6% (its
// comment claims ~1.5% and is wrong), but it is a SMOOTH 5.6% and invisible once
// squared. Corrected anyway by the standard two-multiply refinement
// y*(0.775 + 0.225*y), which takes it under 0.2% for two multiplies — 25395 and
// 7373 are those constants in Q15, and they sum to exactly 32768 so the peak
// stays at 1.0.
fn isin16(a : i32) -> i32 {
  let p = a & 65535;
  let half = p & 32767;
  let y = (4 * half * (32768 - half)) >> 15;
  let r = (y * (25395 + ((7373 * y) >> 15))) >> 15;
  if (p >= 32768) { return -r; }
  return r;
}

// 3D Q14 value noise, for the cave fields of package F. Separate cell log2s for
// XZ and Y are free here and are what turns a tunnel network into a RAVINE
// network (stretch Y, get vertical slots instead of round tubes).
//
// THE Y LATTICE INDEX SALTS THE SEED rather than becoming a fourth hash
// argument, and that is a 2.3x saving written out explicitly rather than left
// to the compiler — world.cpp has to mirror it, and a mirror of "whatever CSE
// did" is not a mirror. hash3(a,b,c) is pcg(a ^ pcg(b ^ pcg(c))), so with the
// seed in slot `a` the ENTIRE inner half pcg(gx ^ pcg(gz)) is independent of y
// and shared by both Y layers: 2 pcg for the two gz, 4 for the four (gx,gz)
// pairs, then 8 outer pcg for 2 layers x 4 corners. 14 pcg, against 32 for a
// four-argument hash. Each layer is therefore EXACTLY vnoise2d's corner set at
// seed ^ (gy * golden), which is the property that keeps the two consistent.
fn vnoise3(x : i32, y : i32, z : i32, cxl : u32, cyl : u32, seed : u32) -> i32 {
  let gx = x >> cxl;
  let gz = z >> cxl;
  let gy = y >> cyl;
  let mxz = i32((1u << cxl) - 1u);
  let my = i32((1u << cyl) - 1u);
  let sx = vsmooth(q15frac(x & mxz, cxl));
  let sz = vsmooth(q15frac(z & mxz, cxl));
  let sy = vsmooth(q15frac(y & my, cyl));
  let pz0 = pcg(bitcast<u32>(gz));
  let pz1 = pcg(bitcast<u32>(gz + 1));
  let i00 = pcg(bitcast<u32>(gx) ^ pz0);
  let i10 = pcg(bitcast<u32>(gx + 1) ^ pz0);
  let i01 = pcg(bitcast<u32>(gx) ^ pz1);
  let i11 = pcg(bitcast<u32>(gx + 1) ^ pz1);
  let s0 = seed ^ (bitcast<u32>(gy) * 2654435769u);
  let s1 = seed ^ (bitcast<u32>(gy + 1) * 2654435769u);
  let v0 = vbilerp(i32(pcg(s0 ^ i00) & 0x3FFFu), i32(pcg(s0 ^ i10) & 0x3FFFu),
                   i32(pcg(s0 ^ i01) & 0x3FFFu), i32(pcg(s0 ^ i11) & 0x3FFFu),
                   sx, sz);
  let v1 = vbilerp(i32(pcg(s1 ^ i00) & 0x3FFFu), i32(pcg(s1 ^ i10) & 0x3FFFu),
                   i32(pcg(s1 ^ i01) & 0x3FFFu), i32(pcg(s1 ^ i11) & 0x3FFFu),
                   sx, sz);
  return v0 + (((v1 - v0) * sy) >> 15);
}
// MIRROR-END noise

// ---- world scale ----
// One voxel is VOXEL_METERS = 10 cm, so 10 voxels to the metre and a 17-voxel
// player capsule. The original desert worldgen assumed a voxel was about a
// metre, so every feature came out a tabletop model of itself — 4 m hills, a
// 2 m "lake", 0.9 m ruins, knee-high trees.
//
// HORIZONTAL features scale by HSCALE. X/Z stream infinitely, so widening them
// costs nothing but noise-cell size: a lake becomes a lake, a biome becomes a
// region you walk across rather than step over.
//
// VERTICAL relief does not scale with HSCALE — by choice, not by force. This
// comment used to say the window was 16 m tall and did not stream in Y. Both
// halves are false now: kWorldN is 512 (51.2 m), and Stream::ShiftAxis handles
// `axis == 1` with no Y clamp, so the window follows the player up and down.
//
// What 51.2 m still bounds is how much of a tall feature is SIM-LIVE at once.
// A 300 m cliff is legal — its far half renders from the cascades — but a sand
// slide only runs inside the resident band, and descending it is one window
// shift per 51.2 m (measure with --autofly-hard, not a standing player).
//
// So the y32..y86 band below is a leftover of the old rule. Widening it needs
// three re-checks the old budget made moot: page residency (a mountain's
// interior is JITTER(mat) and free, its SURFACE is not), the settle budget on
// the new slopes, and cascade fill cost once levels 6-8 have something to show.
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

// ---- the biome FIELD, split from the biome DECISION ------------------------
//
// One low-frequency noise picks the biome, a second breaks up the boundary so
// biomes interlock instead of meeting on a smooth contour. Desert is gated to
// the top of the range (~12% of the field) so it reads as a rare destination
// you walk to rather than the default world. Height still overrides at the top:
// snow caps above TREELINE regardless of biome (handled in genCellIn).
// The biome cell is a LOG2 EXPONENT (9 = 512 voxels = 51.2 m) — deliberately
// NOT halved with the rest of the third scale pass. Trees kept their size and
// their 9 m spacing, so a biome region has to stay many tree-tiles wide or a
// "meadow" holds one bush and the field reads as per-tree noise. The break-up
// octave is two exponents down (128 voxels) so edges stay proportionally
// ragged. The Q14 samples are shifted back down to the 0..255 band the four
// THRESHOLD knobs are authored in, so the primitive swap does not silently
// re-scale them.
//
// The BAND and the DECISION are separate functions because the height curve
// below needs the band's CONTINUOUS value — a thresholded biome id has no
// "how close to the edge am I", and a curve that switches on the id alone puts
// a cliff along every biome boundary. Everything above still calls biomeAt and
// sees exactly what it always did.
// Package G retires this noise entirely for the compass climate.
fn biomeBand(x : i32, z : i32, seed : u32) -> i32 {
  return (vnoise2d(x, z, TUNE_BIOME_LOG2, seed ^ 0x1Bu).n >> 6)
       + (((vnoise2d(x, z, TUNE_BIOME_LOG2 - 2u, seed ^ 0x1Cu).n >> 6) - 128) / 3);
}

fn biomeFromBand(b : i32) -> u32 {
  if (b > i32(TUNE_DESERT_THRESHOLD)) { return B_DESERT; }
  if (b > i32(TUNE_PINE_THRESHOLD)) { return B_PINE; }
  if (b < i32(TUNE_MEADOW_THRESHOLD))  { return B_MEADOW; }
  return B_FOREST;
}

// ---- PER-BIOME HEIGHT CURVES (Lin 13.3.3) ---------------------------------
//
// "This biome is flat plains, that one is jagged mountains", authored as nine
// numbers instead of a hand-tuned noise ladder. The curve reshapes the COARSE
// SUM only — the continental and range rungs, the two that decide where the
// mountains and the basins are — and leaves hill/detail/grain alone. So a
// biome changes the LANDFORM and never the texture on it, which is the same
// split `Land.slope` already makes for the sediment wedge.
//
// ---- NINE KNOTS, NOT EIGHT, AND WHY ---------------------------------------
// The plan asked for eight. Eight knots is SEVEN intervals, so the identity
// curve's knot values are -16384 + i*32768/7 — not integers. An identity curve
// could then not be AUTHORED at all, only approximated, and "the default curve
// moves nothing" would be unprovable rather than merely untested. Nine knots is
// eight intervals and the identity values are -16384 + i*4096 exactly, which is
// what all four biomes default to. Four extra rows buys the proof.
//
// ---- THE DOMAIN -----------------------------------------------------------
// The plan said the input spans +-(contAmp + rangeAmp). It does not: `octave`
// returns `((n - 8192) * amp) >> 14` and `n - 8192` is +-8192, so ONE rung
// spans +-amp/2 and the two together span +-(contAmp + rangeAmp)/2. Authoring
// against the doubled range would have left the outer two knots at each end
// unreachable at every seed.
//
// ---- WHY THE IDENTITY IS BIT-EXACT ----------------------------------------
// Three separate pieces of the arithmetic, and all three are load-bearing:
//
//  1. The Hermite basis is summed BEFORE the shift, not per term. h00+h01 is
//     exactly 4096 and h10+h11+h01 is exactly `t`, in integers, whatever the
//     rounding of t^2 and t^3 — so a straight line through the knots evaluates
//     to k0 + t with no residue. Shifting each of the four products separately
//     would floor four times and leak up to 3 units.
//  2. The result is applied as a DELTA against the identity, whose value at the
//     same parameter is exactly `p - 16384`. An identity curve therefore adds
//     exactly zero, so the round trip out of Q14 back into voxels — which is a
//     floor, and would bias every column down by one — never happens at all.
//  3. The gradient scale falls out the same way: dv/dp is exactly 4096 for the
//     identity, so the Q8 slope is exactly 256 and `(g * 256) >> 8 == g`.
//
// On top of that, `CURVE_IDENT_ALL` is a MODULE CONST — four biomes' worth of
// comparisons between two consts — so with the default knots Tint folds the
// whole feature, including its two extra vnoise2d samples, out of the shader.
// A default world pays nothing for a curve it does not use.
const CURVE_KNOTS : i32 = 9;
const CURVE_SEGS  : i32 = 8;
const CURVE_HI : i32 = (TUNE_CONT_AMPLITUDE + TUNE_RANGE_AMPLITUDE) / 2;

const CURVE_IDENT_ALL : bool =
    (TUNE_CURVE_FOREST0 == -16384 &&
     TUNE_CURVE_FOREST1 == -12288 &&
     TUNE_CURVE_FOREST2 == -8192 &&
     TUNE_CURVE_FOREST3 == -4096 &&
     TUNE_CURVE_FOREST4 == 0 &&
     TUNE_CURVE_FOREST5 == 4096 &&
     TUNE_CURVE_FOREST6 == 8192 &&
     TUNE_CURVE_FOREST7 == 12288 &&
     TUNE_CURVE_FOREST8 == 16384 &&
     TUNE_CURVE_PINE0 == -16384 &&
     TUNE_CURVE_PINE1 == -12288 &&
     TUNE_CURVE_PINE2 == -8192 &&
     TUNE_CURVE_PINE3 == -4096 &&
     TUNE_CURVE_PINE4 == 0 &&
     TUNE_CURVE_PINE5 == 4096 &&
     TUNE_CURVE_PINE6 == 8192 &&
     TUNE_CURVE_PINE7 == 12288 &&
     TUNE_CURVE_PINE8 == 16384 &&
     TUNE_CURVE_MEADOW0 == -16384 &&
     TUNE_CURVE_MEADOW1 == -12288 &&
     TUNE_CURVE_MEADOW2 == -8192 &&
     TUNE_CURVE_MEADOW3 == -4096 &&
     TUNE_CURVE_MEADOW4 == 0 &&
     TUNE_CURVE_MEADOW5 == 4096 &&
     TUNE_CURVE_MEADOW6 == 8192 &&
     TUNE_CURVE_MEADOW7 == 12288 &&
     TUNE_CURVE_MEADOW8 == 16384 &&
     TUNE_CURVE_DESERT0 == -16384 &&
     TUNE_CURVE_DESERT1 == -12288 &&
     TUNE_CURVE_DESERT2 == -8192 &&
     TUNE_CURVE_DESERT3 == -4096 &&
     TUNE_CURVE_DESERT4 == 0 &&
     TUNE_CURVE_DESERT5 == 4096 &&
     TUNE_CURVE_DESERT6 == 8192 &&
     TUNE_CURVE_DESERT7 == 12288 &&
     TUNE_CURVE_DESERT8 == 16384);

fn pick9(i : i32, a0 : i32, a1 : i32, a2 : i32, a3 : i32, a4 : i32,
         a5 : i32, a6 : i32, a7 : i32, a8 : i32) -> i32 {
  var v = a0;
  v = select(v, a1, i == 1);
  v = select(v, a2, i == 2);
  v = select(v, a3, i == 3);
  v = select(v, a4, i == 4);
  v = select(v, a5, i == 5);
  v = select(v, a6, i == 6);
  v = select(v, a7, i == 7);
  v = select(v, a8, i == 8);
  return v;
}

// A SELECT CHAIN, never a runtime-indexed array: CLAUDE.md's note about a
// dynamic index into a by-value uniform spilling the whole struct to scratch
// applies to any indexable aggregate, and this is read four times per column.
fn curveKnot(b : u32, i : i32) -> i32 {
  let j = clamp(i, 0, CURVE_KNOTS - 1);
  if (b == B_FOREST) {
    return pick9(j, TUNE_CURVE_FOREST0, TUNE_CURVE_FOREST1, TUNE_CURVE_FOREST2,
               TUNE_CURVE_FOREST3, TUNE_CURVE_FOREST4, TUNE_CURVE_FOREST5,
               TUNE_CURVE_FOREST6, TUNE_CURVE_FOREST7, TUNE_CURVE_FOREST8);
  }
  if (b == B_PINE) {
    return pick9(j, TUNE_CURVE_PINE0, TUNE_CURVE_PINE1, TUNE_CURVE_PINE2,
               TUNE_CURVE_PINE3, TUNE_CURVE_PINE4, TUNE_CURVE_PINE5,
               TUNE_CURVE_PINE6, TUNE_CURVE_PINE7, TUNE_CURVE_PINE8);
  }
  if (b == B_MEADOW) {
    return pick9(j, TUNE_CURVE_MEADOW0, TUNE_CURVE_MEADOW1, TUNE_CURVE_MEADOW2,
               TUNE_CURVE_MEADOW3, TUNE_CURVE_MEADOW4, TUNE_CURVE_MEADOW5,
               TUNE_CURVE_MEADOW6, TUNE_CURVE_MEADOW7, TUNE_CURVE_MEADOW8);
  }
  return pick9(j, TUNE_CURVE_DESERT0, TUNE_CURVE_DESERT1, TUNE_CURVE_DESERT2,
               TUNE_CURVE_DESERT3, TUNE_CURVE_DESERT4, TUNE_CURVE_DESERT5,
               TUNE_CURVE_DESERT6, TUNE_CURVE_DESERT7, TUNE_CURVE_DESERT8);
}

// Fritsch-Carlson, for uniformly spaced knots: the harmonic mean of the two
// secants, and zero at a local extremum. That is what makes an authored plateau
// FLAT and an authored ramp crease-free — a Catmull-Rom tangent would overshoot
// both, and an overshoot here is a hill the author did not put there.
//
// The multiply is safe in i32 because it only happens when the two secants
// share a sign: knot values are clamped to +-16384 by LoadTuning, so a secant
// of +32768 forces its neighbour negative and takes the early return.
fn curveTangent(dPrev : i32, dNext : i32) -> i32 {
  if (dPrev * dNext <= 0) { return 0; }
  return (2 * dPrev * dNext) / (dPrev + dNext);
}

// One biome's curve. Returns (value in voxels, d(value)/d(input) in Q8).
fn curveOne(b : u32, u : i32) -> vec2<i32> {
  let hi = max(CURVE_HI, 1);
  let uc = clamp(u, -hi, hi);
  // Parameter across the eight segments, Q12 within a segment. The identity's
  // value at this parameter is exactly `p - 16384`, which is what (2) above
  // subtracts.
  let p = clamp(((uc + hi) * (CURVE_SEGS << 12)) / (2 * hi), 0, CURVE_SEGS << 12);
  let seg = min(p >> 12, CURVE_SEGS - 1);
  let t = p - (seg << 12);
  let km = curveKnot(b, seg - 1);
  let k0 = curveKnot(b, seg);
  let k1 = curveKnot(b, seg + 1);
  let k2 = curveKnot(b, seg + 2);
  let d0 = k1 - k0;
  let m0 = select(curveTangent(k0 - km, d0), d0, seg == 0);
  let m1 = select(curveTangent(d0, k2 - k1), d0, seg == CURVE_SEGS - 1);

  let t2 = (t * t) >> 12;
  let t3 = (t2 * t) >> 12;
  let h00 = 2 * t3 - 3 * t2 + 4096;
  let h10 = t3 - 2 * t2 + t;
  let h01 = 3 * t2 - 2 * t3;
  let h11 = t3 - t2;
  // ONE shift over the whole sum -- see (1) above.
  let v = (k0 * h00 + m0 * h10 + k1 * h01 + m1 * h11) >> 12;

  // The Hermite basis differentiated, same trick, same reason.
  let g00 = 6 * t2 - 6 * t;
  let g10 = 3 * t2 - 4 * t + 4096;
  let g01 = 6 * t - 6 * t2;
  let g11 = 3 * t2 - 2 * t;
  let dv = (k0 * g00 + m0 * g10 + k1 * g01 + m1 * g11) >> 12;

  return vec2<i32>(uc + (((v - (p - 16384)) * hi) >> 14),
                   clamp(dv >> 4, 0, 4096));
}

// Which two biomes this column sits between, and how far across. Returns
// (loBiome, hiBiome, Q8 weight of hi). A hard switch on `biomeFromBand` would
// put a CLIFF along every biome edge -- the curve is applied to the coarse rungs
// whose amplitude is 100 m, so two different curves meeting on a contour is a
// step of tens of voxels, not a texture seam.
fn curveBiomePair(band : i32) -> vec3<i32> {
  let hard = i32(biomeFromBand(band));
  let bw = max(TUNE_BIOME_BLEND, 0);
  if (bw <= 0) { return vec3<i32>(hard, hard, 0); }
  // The three boundaries of the band, low side to high side. LoadTuning keeps
  // the thresholds more than 2*biomeBlend apart, so at most one can be in
  // range and the order of the tests does not matter.
  let tm = i32(TUNE_MEADOW_THRESHOLD);   // below it: meadow, above: forest
  let tp = i32(TUNE_PINE_THRESHOLD);     // below it: forest, above: pine
  let td = i32(TUNE_DESERT_THRESHOLD);   // below it: pine,   above: desert
  if (band > tm - bw && band <= tm + bw) {
    return vec3<i32>(i32(B_MEADOW), i32(B_FOREST),
                     ((band - (tm - bw)) * 256) / (2 * bw));
  }
  if (band > tp - bw && band <= tp + bw) {
    return vec3<i32>(i32(B_FOREST), i32(B_PINE),
                     ((band - (tp - bw)) * 256) / (2 * bw));
  }
  if (band > td - bw && band <= td + bw) {
    return vec3<i32>(i32(B_PINE), i32(B_DESERT),
                     ((band - (td - bw)) * 256) / (2 * bw));
  }
  return vec3<i32>(hard, hard, 0);
}

// The curve, blended across the biome edge. `u` is the coarse sum; the result
// is (reshaped sum, Q8 slope) and BOTH are used -- the slope multiplies the
// accumulated gradient so the iq attenuation of hill/detail/grain still sees
// the ground it is actually attenuating against.
fn biomeCurve(x : i32, z : i32, u : i32, seed : u32) -> vec2<i32> {
  if (CURVE_IDENT_ALL) { return vec2<i32>(u, 256); }
  let pr = curveBiomePair(biomeBand(x, z, seed));
  let lo = curveOne(u32(pr.x), u);
  if (pr.z <= 0) { return lo; }
  let hg = curveOne(u32(pr.y), u);
  return vec2<i32>(lo.x + (((hg.x - lo.x) * pr.z) >> 8),
                   lo.y + (((hg.y - lo.y) * pr.z) >> 8));
}

// MIRROR-BEGIN height
// The terrain, and how steep it is there. THE SLOPE IS NOT DECORATION: this
// CA's angle of repose is exactly 1 voxel per column (sim_step.wgsl slides a
// powder into any free down-diagonal), so every rule that lays loose material
// has to know the gradient or it lays powder on a wall and the chunk never
// sleeps — a rule-2 failure that surfaces two gates away as "the world does not
// settle". Package C's sediment wedge is the big consumer; the tarn placement
// below is the first.
//
// It is ANALYTIC, from vnoise2d's own derivative, not a finite difference —
// four extra corner hashes per octave is what a difference would cost, and it
// would be wrong at cell seams besides.
struct Land {
  h     : i32,
  slope : i32,   // |dh/dx| + |dh/dz| in Q8; 256 == 1 voxel/voxel == repose
  sed   : i32,   // loose wedge thickness ALREADY INCLUDED in h (see below)
};

// ---- ONE OCTAVE OF THE ATTENUATED LADDER ----------------------------------
//
// The contribution is CENTRED — `n - 8192` rather than `n` — so the ladder sums
// to a zero-mean deviation and `baseHeight` is the world's mean height rather
// than its floor. That is what leaves as much room BELOW the datum for sea
// basins as above it for mountains, and it is why the amplitudes here are read
// as full swings (the rung spans +-amp/2).
//
// `gx`/`gz` are the gradient accumulated from the octaves ABOVE this one, in
// Q8. iq's attenuation divides this rung by 1 + atten*|g|^2, which is what
// makes the ladder self-limiting instead of a sum of five 0.5 slopes: at |g| = 1
// every subsequent octave is halved, so the field saturates near slope 1.2 on
// ridges and goes genuinely flat in valleys. THAT IS THE RULE-2 MECHANISM. A
// plain fBm at this depth would put the entire world above the CA's angle of
// repose (exactly 1 voxel/column) and nothing loose could ever come to rest.
//
// Ranges, for the i32 headroom: `n - 8192` is +-8192, amp <= 2048 after the
// LoadTuning clamp, so the numerator peaks at 1.7e7 and `att` at 256 takes the
// product to 4.3e9/256 -- the shifts are ordered so the >> 14 lands FIRST and
// the intermediate never exceeds 2^25.
struct Oct {
  dev : i32,   // centred contribution, whole voxels
  gx  : i32,   // its own gradient, Q8
  gz  : i32,
};

fn octave(x : i32, z : i32, csl : u32, amp : i32,
          gx : i32, gz : i32, seed : u32) -> Oct {
  let n = vnoise2d(x, z, csl, seed);
  // att = 65536 / (256 + atten*|g|^2/256), i.e. Q8 with att == 256 meaning
  // "unattenuated". ONE divide per octave, which is still fewer than the five
  // the legacy vnoise did per SAMPLE.
  let g = abs(gx) + abs(gz);
  let att = 65536 / (256 + ((TUNE_FBM_ATTEN * ((g * g) >> 8)) >> 8));
  var o : Oct;
  o.dev = ((((n.n - 8192) * amp) >> 14) * att) >> 8;
  // dh/dx is (dn/dt * amp) / (2^14 * cell) and Q8 multiplies by 256, so the
  // whole conversion is ONE arithmetic shift: >> (14 - 8 + log2).
  o.gx = (((n.dx * amp) >> (6u + csl)) * att) >> 8;
  o.gz = (((n.dz * amp) >> (6u + csl)) * att) >> 8;
  return o;
}

// The continental rung, FROM THE MAP (P4): where the old o0 octave sampled
// noise at contLog2, this reads the painted landform plane. contAmplitude
// keeps its meaning -- the height span the full 0..255 landform range maps
// to, centred on 128 -- so the knob still says how tall the world is and the
// map says where. Its own function inside the mirror so the two sides spell
// the same three lines; the plane readers it calls live outside it.
fn landformOctave(x : i32, z : i32) -> Oct {
  var o : Oct;
  o.dev = ((mapLandformQ8(x, z) - 32768) * TUNE_CONT_AMPLITUDE) >> 16;
  o.gx = mapLandformGx(x, z);
  o.gz = mapLandformGz(x, z);
  return o;
}

fn landAt(x : i32, z : i32, seed : u32) -> Land {
  // ---- five octaves, lacunarity 4, persistence 1/4 ----
  // Every rung has the same amplitude/wavelength ratio of 0.5, which is the
  // whole design: detail without extra slope. Each one sees the gradient of
  // everything coarser than it, so the accumulation order below is load-bearing
  // and is written out rather than looped — world.cpp has to mirror it, and a
  // mirror of "whatever the compiler unrolled" is not a mirror.
  let o0 = landformOctave(x, z);
  let o1 = octave(x, z, TUNE_RANGE_LOG2,  TUNE_RANGE_AMPLITUDE,
                  o0.gx, o0.gz, seed ^ 2u);
  // ---- THE PER-BIOME HEIGHT CURVE (13.3.3) ----
  // HERE, and only here: the two coarse rungs are the landform, and the three
  // fine ones are the texture on it. Reshaping the sum of the coarse pair is
  // what lets a meadow be flat plains and a pine highland be jagged without
  // either one changing how the ground FEELS underfoot.
  //
  // `cv.y` is the curve's own slope in Q8 and it scales the accumulated
  // gradient, not the deviation: iq's attenuation divides each finer rung by
  // 1 + atten*|g|^2, and if `g` still described the pre-curve ladder then a
  // biome that flattened its landform would keep the hill octave attenuated as
  // though the mountains were still there. Identity gives exactly 256, so
  // (g * 256) >> 8 == g and nothing moves.
  let cv = biomeCurve(x, z, o0.dev + o1.dev, seed);
  let g1x = ((o0.gx + o1.gx) * cv.y) >> 8;
  let g1z = ((o0.gz + o1.gz) * cv.y) >> 8;
  let o2 = octave(x, z, TUNE_HILL_LOG2,   TUNE_HILL_AMPLITUDE,
                  g1x, g1z, seed ^ 3u);
  let g2x = g1x + o2.gx;
  let g2z = g1z + o2.gz;
  let o3 = octave(x, z, TUNE_DETAIL_LOG2, TUNE_DETAIL_AMPLITUDE,
                  g2x, g2z, seed ^ 4u);
  let g3x = g2x + o3.gx;
  let g3z = g2z + o3.gz;
  let o4 = octave(x, z, TUNE_GRAIN_LOG2,  TUNE_GRAIN_AMPLITUDE,
                  g3x, g3z, seed ^ 5u);

  // ---- THE CALM HOME AREA ----
  // Only the TWO COARSE octaves fade toward the origin. Fading the whole
  // deviation would pin spawn to a mathematically exact plane 64 m across —
  // which is not "calm", it is a dinner plate, and it would also make the
  // terrain gate's pass C a test of a constant. The three fine rungs stay at
  // full amplitude, so the home area is rolling country of about the shape the
  // pre-overhaul world had, sitting at spawnPlainY.
  //
  // CHEBYSHEV distance, so no isqrt; a square region has its steepest boundary
  // on the axes and its longest on the diagonal, which is why the gate's A4
  // transects walk both. The `d < fade` guard is not cosmetic: `d << 14` on a
  // world coordinate a few hundred thousand voxels out would overflow, and X/Z
  // are infinite here.
  let d = max(abs(x), abs(z)) - TUNE_SPAWN_PLAIN_R;
  var w = 16384;
  if (d < TUNE_SPAWN_PLAIN_FADE) {
    w = (max(d, 0) * 16384) / TUNE_SPAWN_PLAIN_FADE;
  }
  let ws = vsmooth(w << 1) >> 1;                    // Q14 smoothstep of the ramp
  let coarse = TUNE_BASE_HEIGHT + cv.x - TUNE_SPAWN_PLAIN_Y;
  let bed = TUNE_SPAWN_PLAIN_Y + o2.dev + o3.dev + o4.dev
          + ((coarse * ws) >> 14);

  // ---- THE SEDIMENT WEDGE ----
  // Low flat ground carries metres of loose dirt over gravel; ridges carry
  // none. This is what makes the relief mean something to the SIM rather than
  // only to the eye — digging a valley floor gives you material that flows.
  //
  // THE SLOPE GATE IS THE SAFETY PROPERTY, not a look knob. Dirt and gravel are
  // POWDERS and sim_step.wgsl slides a powder into any free down-diagonal. The
  // stack is capped by a SOLID skin at y == h, so the topmost grain sits at
  // h-1 and is exposed exactly when a neighbouring column's ground is 3 or more
  // voxels lower. Gating on slope, and ramping the wedge continuously to zero
  // as that slope is approached, is what keeps the generated world at rest.
  // The old constant-depth dirt shell (deleted; see the note under the grass
  // skin in genCellIn) had no such gate and crept down every hillside.
  //
  // `Land.slope` IS THE LANDFORM GRADIENT — `g2`, accumulated through the hill
  // octave and deliberately NOT through detail and grain. Both of its consumers
  // want that and neither wants the roughness:
  //
  //   * THE WEDGE. Its own spatial derivative is
  //     (sed / sedSlope) * d(slope)/dcolumn, and d(slope)/dcolumn for an octave
  //     is ~6*amp/cell^2. For the GRAIN octave (amp 4, cell 8) that is 96 Q8
  //     per column — the entire gate range in ONE step, which turns a 24-voxel
  //     wedge into a 24-voxel cliff wherever the fine noise happens to cross
  //     the threshold. Through the hill octave (amp 64, cell 128) it is 6 Q8
  //     per column, so the wedge thins over ~32 columns and contributes under
  //     one voxel to any adjacent step. Measured: gating on the full gradient
  //     left 108 chunks awake at tick 120; on the landform gradient, 8.
  //   * TARN PLACEMENT (pondInfo). "Is this ground flat enough to hold a bowl
  //     of water" is a question about the hillside, not about whether one
  //     sub-metre bump happens to sit under the centre column. Gated on the
  //     full gradient it is effectively a coin toss, and the tarns it accepts
  //     on real hillsides lay their SAND bed down the inside of a cut cliff —
  //     which is where every remaining page fault in this gate came from.
  //
  // Physically it is also the better model in both cases: sediment is what
  // FILLS surface roughness, so roughness must not switch it off.
  let slope = abs(g2x) + abs(g2z);
  let room = max(0, TUNE_SED_CEIL - bed);
  var sed = ((room * TUNE_SED_FRACTION) >> 8) - TUNE_SED_STRIP;
  sed = (max(sed, 0) * max(TUNE_SED_SLOPE - slope, 0)) /
        max(TUNE_SED_SLOPE, 1);
  sed = clamp(sed, 0, TUNE_SED_MAX);
  if (bed < seaLevelY()) { sed = 0; }

  var l : Land;
  l.h = bed + sed;
  l.slope = slope;
  l.sed = sed;
  return l;
}

fn baseHeight(x : i32, z : i32, seed : u32) -> i32 {
  return landAt(x, z, seed).h;
}
// MIRROR-END height

// ---- biome field ----
// The field and the thresholds moved up above landAt (see `biomeBand` /
// `biomeFromBand`), because the height curve needs the band's continuous value.
// Same two noise samples, same shifts, same salts, same thresholds, same order:
// this is one function split in two, not a new one.
// Since the world map's P2 the biome is READ FROM THE PAINTED MAP
// (mapBiomeAt, below the tree atlas block), not derived from the noise band.
// biomeBand/biomeFromBand and the threshold knobs survive only as inputs to
// the (identity, compile-time-folded) per-biome height curve until P4 retires
// that with the landform plane.
fn biomeAt(x : i32, z : i32, seed : u32) -> u32 {
  return mapBiomeAt(x, z, seed);
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
//
// ---- PERCHED TARNS: the waterline comes from the CENTRE, not the rim -------
//
// The rim-sampled formulation this replaces set the surface to
// `min(24 baseHeight samples around the rim) - 2` and then relied on that
// minimum being a good enough estimate of the true rim minimum. Two things were
// wrong with it, and only one of them was a cost:
//
//   * COST. 24 baseHeight = 192 hash3 for every pond column AND every shore
//     column, evaluated per column. On package C's five-octave ladder the same
//     24 samples become 480 hashes. It was the single most expensive thing in
//     worldgen and it bought an estimate.
//   * THE GUARANTEE WAS ALREADY STALE. The deleted comment justified 24
//     directions as "one sample every ~9 voxels of arc on the largest (r=36)
//     pond". Tuning had since taken the radius to 127, which is one sample every
//     33 voxels of arc against a -2 margin and a 16-voxel detail octave — a
//     below-water notch fits between two samples easily. Containment was a
//     SAMPLING DENSITY pretending to be an invariant.
//
// So invert it. Take the surface from the pond's own CENTRE column (one
// baseHeight, shared by pondAt and the shore band), carve the bowl below it, and
// FORCE THE ANNULUS UP to `surf + pondBerm` — exactly what the authored pool at
// (420,420) already does with `h = max(h, poolY + 26)`, ramped back to natural
// ground over `pondBermWidth` so it reads as a bank rather than a wall. Every
// column in the core of the annulus is then provably above the waterline at
// every seed and every radius, with ZERO rim samples: the guarantee is
// structural instead of statistical, and 192 hashes become 8.
//
// On flat ground the berm is invisible. On a slope only the downhill side has to
// rise, which reads as a dammed tarn — and a small perched tarn is what a
// mountainside should carry once package C gives it 200 m of relief, which is
// why the radii shrank with this change rather than growing.
const POND_TILE : i32 = TUNE_POND_TILE;   // 14 m between pond sites

// Per-tile pond descriptor, unpacked from one tile hash — the pond analogue of
// treeInfo. Split out of pondAt so the SHORE band (shoreAt, below) can ask
// "where is the nearest pond rim" for a column that is OUTSIDE every disc, and
// therefore gets `none` back from pondAt. Both callers must see exactly the
// same disc, so there is one place that decides it.
// MIRROR-BEGIN height
struct Pond {
  present : bool,
  cx      : i32,   // disc centre, world coords
  cz      : i32,
  r       : i32,   // disc radius
  surf    : i32,   // water surface Y: the ground the CENTRE column would have
};

fn pondInfo(pt : i32, pz : i32, seed : u32) -> Pond {
  var p : Pond;
  p.present = false; p.cx = 0; p.cz = 0; p.r = 0; p.surf = -1;

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
  // clearing + fixture pads, and the three authored pools (128 covers the widest
  // rim 80 + max radius 36 + slack).
  //
  // The streaming ball column (408,128) used to need its own keep-out because
  // its test assumed TerrainHeight() was the surface there and TerrainHeight()
  // did not know about pond bowls. It does now — World::TerrainHeight is
  // genColumn's `h` exactly (see the height contract in DESIGN.md) — so the
  // keep-out is gone and a pond may land there like anywhere else.
  // ---- THE AUTHORED ORIGIN REGION ----
  // A tarn may not land in the 512-voxel cube at the world origin, and this box
  // is that cube plus one full disc-and-berm of margin so nothing REACHES in
  // either. The region is authored content end to end: three set-piece pools,
  // the combat arena, the wood platform, the spawn clearing, the fixture pads,
  // and every column the selftest suite drops a body onto. It is also exactly
  // the residency window the harness runs in.
  //
  // The box used to be -44..264, which covered the fixtures and nothing else.
  // It is widened here for a second reason that is a DEFECT, not a design, and
  // is recorded rather than hidden: a generated tarn does not reach rest. Seven
  // chunks around one stay awake indefinitely — five of them from the pond
  // vegetation, two from the water itself — which `sleep` tolerates (its bound
  // is 32) and `ca-skip` and `wind-prim` do not, because both need a tick with
  // an EMPTY dirty set. Nothing in the height function causes it: the wedge,
  // the bowl, the berm, the shore fringe, the ruins, evaporation and the MPM
  // seam were each ruled out by measurement, and the residue is a liquid-CA
  // question. See docs/PLAN_terrain_overhaul.md.
  if (siteKeepOut(cx, cz)) { return p; }
  // ---- THE SLOPE GATE: a tarn is PERCHED, not QUARRIED --------------------
  //
  // Last, because it is the only test here that costs a noise sample, and the
  // hash rejects above throw away three tiles in four before it.
  //
  // The waterline comes from the centre column, so on a hillside it sits far
  // below the uphill rim -- and `h = min(h, bowl floor)` then cuts a cliff into
  // that hillside and lays a SAND bed down its inside face. That bed is powder
  // on a wall: the CA's angle of repose is 1 voxel per column and the bowl's
  // rim gradient is 2*(pondDepth - pondDepthRim)/r, so on steep ground the
  // tarn is an avalanche that never stops (CLAUDE.md rule 2 -- and it does not
  // announce itself here, it announces itself as `ca-skip` finding the world
  // never quiet). LoadTuning bounds the depth against the radius for the same
  // reason; this bounds the GROUND.
  //
  // Refusing the site is also what makes a tarn read as a tarn. A pond needs a
  // flat shelf to sit on; carved into a slope it reads as a quarry.
  //
  // TWO TESTS, and the second is radius-aware because the first is not enough.
  // A slope of s drops s*r voxels across the radius; where that exceeds the
  // bowl's own depth the ground UNDERCUTS the bowl, `pondAt` stops describing
  // the floor and the floor is raw hillside again — with a sand bed on it. So
  // the drop across the radius must stay inside the bowl, which at a fixed
  // slope makes big tarns need flatter ground than small ones. `slope` is Q8,
  // hence the 256.
  let c = landAt(cx, cz, seed);
  if (c.slope > TUNE_POND_MAX_SLOPE) { return p; }
  if (c.slope * r > (TUNE_POND_DEPTH - TUNE_POND_DEPTH_RIM) * 256) { return p; }
  p.present = true; p.cx = cx; p.cz = cz; p.r = r; p.surf = c.h;
  return p;
}

// The berm: what makes containment structural. `past` is whole voxels beyond
// the rim. The inner CORE is forced flat at `surf + pondBerm` — that is the wall
// the water cannot cross, and it is the only part the guarantee rests on. The
// rest ramps the lift linearly back to the natural ground so the bank blends;
// where the ground is already above the berm nothing moves at all.
fn bermLift(h : i32, surf : i32, past : i32) -> i32 {
  let bw = TUNE_POND_BERM_WIDTH;
  let core = max(bw / 4, 2);
  if (past < core) { return max(h, surf + TUNE_POND_BERM); }
  let span = max(bw - core, 1);
  let t = span - (past - core);
  if (t <= 0) { return h; }
  return max(h, h + ((surf + TUNE_POND_BERM - h) * t) / span);
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
  let surf = p.surf;
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
// MIRROR-END height

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
//   * the waterline is carried IN the Pond (one landAt at the centre, paid
//     inside pondInfo) rather than resampled per caller.
//
// Returns (distance PAST the rim in voxels, water surface Y), or (-1,-1) when
// this column is not near any disc. Distance 0 is the first column outside the
// disc; the inside of the disc returns none (that is pondAt's job).
//
// TWO CONSUMERS, ONE SCAN. genColumn uses this both for the BERM (which must be
// applied to every column near a disc, whatever the biome or the ground height)
// and for the marsh FRINGE (which is the berm band narrowed by biome, fixture
// and waterline tests). The scan band is therefore the wider of the two widths,
// and `onShore` here means only "a disc is near enough to matter" — genColumn
// is what decides whether that is a shore.
//
// Why the band cannot simply be read off `pondAt` returning none: the disc's
// clearance inside its own tile can be as little as 4 voxels for the largest
// radius, so a wider band derived from one tile alone would be sliced off flat
// along a tile edge — a straight-line haircut through a marsh, which is exactly
// the artifact the tile scan buys us out of.
// MIRROR-BEGIN height
struct Shore {
  onShore : bool,
  past    : i32,   // voxels beyond the rim (0 = first dry column)
  surf    : i32,   // the pond's water surface Y
};

fn pondNear(x : i32, z : i32, seed : u32) -> Shore {
  var s : Shore;
  s.onShore = false; s.past = 0; s.surf = -1;

  let band = max(TUNE_SHORE_BAND, TUNE_POND_BERM_WIDTH);
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
  bestP.present = false; bestP.cx = 0; bestP.cz = 0; bestP.r = 0; bestP.surf = -1;
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
  s.surf = bestP.surf;
  return s;
}
// MIRROR-END height

// ---- trees ----
//
// SCALE, for everything below that is still authored as a number rather than
// baked: dimensions are written as DECIMETRES and converted with
// `* VOX_PER_M / 10`, never as a bare voxel count — the first cut of the tree
// system used bare counts and produced 10-voxel "oaks" that were 60 cm tall.
// VOX_PER_M IS THE WORLD'S OWN VOXELS-PER-METRE, read from the prelude so the
// tables stay metre-true at any voxel size.
//
// It used to say here that "the trees themselves no longer need it: their sizes
// are metres in assets/trees/*.json and voxels in the baked atlas". Both halves
// were true and the conclusion was false — the metres never reached this
// shader, only the voxels did, and NOTHING rescaled between them, so the atlas
// silently inherited whatever scale the baker happened to run at. That premise
// is what hid half-height forests at 5 cm voxels for as long as it did.
//
// What actually keeps trees metre-true now is that the .svtree header records
// its bake scale and src/sim/treeatlas.cpp REFUSES an atlas whose scale is not
// this world's. The stamping below can then stay 1:1, which is the cheap path —
// but it is only correct because of that check, not because of a property of
// the data.
const VOX_PER_M : i32 = VOXELS_PER_M;

// ---- THE BAKED ATLAS, AND WHY THE IMPLICIT SHAPES ARE GONE ----------------
//
// Worldgen is a pure per-cell function: genChunk answers for one voxel with no
// memory of its neighbours and no way to walk a turtle. Every tree this engine
// ever grew was therefore an IMPLICIT SHAPE re-derived per cell — a hash-eroded
// ellipsoid for oak, a diamond cone for pine, a hand-unrolled five-limb
// skeleton for birch. That is the ceiling of the technique, and it is why the
// forest read as lollipops.
//
// So the trees are voxelized ONCE, offline, by assets/editor/treegen.js: a
// Weber-Penn branch skeleton stamped as round-cone SDFs, with smooth-min'd
// ellipsoid leaf clumps at the outer stems and a SHADING BAKE that resolves
// each leaf voxel's lit/mid/dark tier into a material choice. src/sim/
// treeatlas.h uploads the result. What is left here is a bounds check, one
// column lookup and a short run scan.
//
// THE EDITOR IS THE ONLY VOXELIZER, deliberately. A WGSL copy of the SDF and
// clump logic would be a second implementation that has to agree with the
// first, which is the drift the tuner's "one authoring surface" rule exists to
// prevent. The price is that editing a species and re-baking MOVES THE WORLD
// HASH — one `--selftest --rebaseline`, exactly as for tuning.json.
//
// WHAT THE ATLAS IS, as words:
//
//   header             16 words (TA_H_* below)
//   species directory  TA_SPECIES_WORDS per species (TA_S_*)
//   biome table        4 x (1 + speciesCount) cumulative weights
//   per species:       variant directory (TA_V_*), then per variant a
//                      column table of (runOffset, runCount) pairs indexed
//                      [lz * nx + lx], then the runs
//
//   run word: material (12 bits) | state (4) | y0 (9) | length (7)
//
// Every offset is a WORD INDEX into this buffer, absolute, fixed up by the
// C++ loader. Material ids are resolved from the file's NAME table at load
// (design guideline 4), so renumbering materials.json cannot silently recolour
// a forest. The baked state nibble is IGNORED here: worldgen applies its own
// positional palette jitter at the bottom of genCellIn, because a voxel word
// with an authored state cannot be represented by the page table's JITTER
// sentinel and a forest that defeats page compression is not worth three
// colours the engine already provides.
const TA_H_SPECIES_COUNT : u32 = 2u;
const TA_H_MAX_REACH     : u32 = 4u;
const TA_H_MAX_ABOVE     : u32 = 5u;
const TA_H_BIOME_TABLE   : u32 = 6u;
const TA_H_SPECIES_DIR   : u32 = 7u;

const TA_SPECIES_WORDS : u32 = 24u;
const TA_S_VARIANT_DIR : u32 = 0u;
const TA_S_VARIANT_CNT : u32 = 1u;
const TA_S_REACH       : u32 = 2u;
const TA_S_ABOVE       : u32 = 3u;
const TA_S_CROWN_Y     : u32 = 4u;
const TA_S_CROWN_R     : u32 = 5u;
const TA_S_MIN_Y       : u32 = 6u;
const TA_S_MAX_Y       : u32 = 7u;
const TA_S_MAX_SLOPE   : u32 = 8u;
const TA_S_SPARSITY    : u32 = 9u;
const TA_S_CANOPY_MAT  : u32 = 10u;
const TA_S_SHADE       : u32 = 11u;
const TA_S_AUTUMN      : u32 = 12u;
const TA_S_LEAF0       : u32 = 13u;
const TA_S_AUTUMN0     : u32 = 16u;

const TA_VARIANT_WORDS : u32 = 12u;
const TA_V_NX      : u32 = 0u;
const TA_V_NY      : u32 = 1u;
const TA_V_NZ      : u32 = 2u;
const TA_V_ANCHORX : u32 = 3u;
const TA_V_ANCHORZ : u32 = 4u;
const TA_V_COLUMNS : u32 = 5u;

fn taSpeciesCount() -> i32 { return i32(treeAtlas[TA_H_SPECIES_COUNT]); }
fn taSpecies(sp : i32, w : u32) -> u32 {
  return treeAtlas[treeAtlas[TA_H_SPECIES_DIR] + u32(sp) * TA_SPECIES_WORDS + w];
}

// ---- the world map's biome record table (src/sim/worldmap.h) --------------
// Mirrors worldmap.h's header words, record fields and cover-row fields;
// check_invariants.py holds the two together. One record per biome id, the
// cover rows appended after the records. Everything is already an integer the
// kernel can use: material IDs resolved at load, heights in voxels, chances as
// 1-in-N, slopes in Q8.
const WM_H_CELL_LOG2     : u32 = 2u;
const WM_H_WIDTH         : u32 = 3u;
const WM_H_HEIGHT        : u32 = 4u;
const WM_H_ORIGIN_X      : u32 = 5u;
const WM_H_ORIGIN_Z      : u32 = 6u;
const WM_H_SEA_LEVEL_Y   : u32 = 7u;
const WM_H_OCEAN_FADE    : u32 = 8u;
const WM_H_WARP_AMP      : u32 = 9u;
const WM_H_BIOME_COUNT   : u32 = 10u;
const WM_H_BIOME_RECORDS : u32 = 11u;
const WM_H_BIOME_PLANE   : u32 = 12u;
const WM_H_LANDFORM_PLANE: u32 = 13u;
const WM_H_MOISTURE_PLANE: u32 = 14u;
const WM_H_MAX_COVER_H   : u32 = 20u;
const WM_H_OCEAN_BIOME   : u32 = 21u;
const WM_H_SITE_INDEX    : u32 = 15u;
const WM_H_SITE_TABLE    : u32 = 16u;
const WM_H_SITE_COUNT    : u32 = 17u;
const WM_H_HARNESS_X0    : u32 = 22u;
const WM_H_HARNESS_Z0    : u32 = 23u;
const WM_H_HARNESS_X1    : u32 = 24u;
const WM_H_HARNESS_Z1    : u32 = 25u;
const WM_H_WATER_RECORDS : u32 = 26u;
const WM_H_WATER_COUNT   : u32 = 27u;
// the site table (worldmap.h kS_* / kStamp_*)
const WM_S_WORDS         : u32 = 16u;
const WM_S_KIND          : u32 = 0u;
const WM_S_X             : u32 = 1u;
const WM_S_Z             : u32 = 2u;
const WM_S_RADIUS        : u32 = 3u;
const WM_S_PAD_MARGIN    : u32 = 4u;
const WM_S_ROT           : u32 = 5u;
const WM_S_SALT          : u32 = 6u;
const WM_S_STAMP_OFF     : u32 = 7u;
const WM_STAMP_HDR_WORDS : u32 = 4u;
const WM_STAMP_NX        : u32 = 0u;
const WM_STAMP_NY        : u32 = 1u;
const WM_STAMP_NZ        : u32 = 2u;
const WM_STAMP_COLUMNS   : u32 = 3u;
const WM_B_WORDS         : u32 = 32u;
const WM_B_SKIN          : u32 = 0u;
const WM_B_SUBSOIL       : u32 = 1u;
const WM_B_SKIN_DEPTH    : u32 = 2u;
const WM_B_PATCH_THRESH  : u32 = 3u;
const WM_B_PATCH_LOG2    : u32 = 4u;
const WM_B_TREE_TILE     : u32 = 5u;
const WM_B_TREE_DENSITY  : u32 = 6u;
const WM_B_COVER_COUNT   : u32 = 7u;
const WM_B_COVER_OFF     : u32 = 8u;
const WM_B_CAVE_T1       : u32 = 9u;
const WM_B_CAVE_T2       : u32 = 10u;
const WM_B_SED_MAX       : u32 = 11u;
const WM_B_FLAGS         : u32 = 12u;
const WM_B_MAX_COVER_H   : u32 = 13u;
// P-E: the flora that used to be worldgen.* knobs (worldmap.h kB_* 16..20)
const WM_B_WATER_PRESET  : u32 = 16u;
const WM_B_CAVE_MUSHROOM_CHANCE : u32 = 17u;
const WM_B_CAVE_CRYSTAL_CHANCE  : u32 = 18u;
const WM_B_CACTUS_CHANCE : u32 = 19u;
const WM_B_SAGUARO_FRACTION : u32 = 20u;
const WM_C_WORDS         : u32 = 8u;
const WM_C_MAT           : u32 = 0u;
const WM_C_HEAD          : u32 = 1u;
const WM_C_CHANCE        : u32 = 2u;
const WM_C_HEIGHT        : u32 = 3u;
const WM_C_MIN_Y         : u32 = 4u;
const WM_C_MAX_Y         : u32 = 5u;
const WM_C_MAX_SLOPE     : u32 = 6u;
const WM_C_PATCH_THRESH  : u32 = 7u;
const WM_BF_GROUND_FLORA : u32 = 1u;
const WM_BF_CACTI        : u32 = 2u;
const WM_BF_SAND_CAP     : u32 = 4u;
// the water preset table (worldmap.h kW_* / kP_*): the FLORA half of
// assets/water/<name>.json. Depths are voxels of water over the bed, heights
// cells from the bed (aquatic) or the ground (shore); chances 1-in-N, 0 = off.
const WM_W_WORDS               : u32 = 32u;
const WM_W_FILL                : u32 = 0u;
const WM_W_SHORE_COUNT         : u32 = 1u;
const WM_W_SHORE_OFF           : u32 = 2u;
const WM_W_MOSS_CHANCE         : u32 = 3u;
const WM_W_MOSS_MAT            : u32 = 4u;
const WM_W_EMERGENT_MAT        : u32 = 5u;
const WM_W_EMERGENT_CHANCE     : u32 = 6u;
const WM_W_EMERGENT_MIN_DEPTH  : u32 = 7u;
const WM_W_EMERGENT_MAX_DEPTH  : u32 = 8u;
const WM_W_EMERGENT_HEIGHT     : u32 = 9u;
const WM_W_FLOATING_MAT        : u32 = 10u;
const WM_W_FLOATING_FLOWER     : u32 = 11u;
const WM_W_FLOATING_CHANCE     : u32 = 12u;
const WM_W_FLOATING_FLOWER_CHANCE : u32 = 13u;
const WM_W_FLOATING_MIN_DEPTH  : u32 = 14u;
const WM_W_FLOATING_MAX_DEPTH  : u32 = 15u;
const WM_W_SUBMERGED_MAT       : u32 = 16u;
const WM_W_SUBMERGED_CHANCE    : u32 = 17u;
const WM_W_SUBMERGED_MIN_DEPTH : u32 = 18u;
const WM_W_SUBMERGED_HEIGHT    : u32 = 19u;
const WM_W_SUBMERGED_CLEARANCE : u32 = 20u;
const WM_W_MAX_PLANT_H         : u32 = 21u;
const WM_P_WORDS               : u32 = 8u;
const WM_P_MAT                 : u32 = 0u;
const WM_P_HEAD                : u32 = 1u;
const WM_P_CHANCE              : u32 = 2u;
const WM_P_REACH               : u32 = 3u;
const WM_P_HEIGHT              : u32 = 4u;

fn wmBiomeCount() -> u32 { return worldMap[WM_H_BIOME_COUNT]; }
// A biome id past the table (a stale save, a buffer that has not been
// uploaded) reads record 0 rather than whatever lies past the end: the same
// robustness the tree atlas gets from its header, and never a wild read.
fn wmBiome(b : u32, w : u32) -> u32 {
  let n = wmBiomeCount();
  if (n == 0u) { return 0u; }
  let id = select(b, 0u, b >= n);
  return worldMap[worldMap[WM_H_BIOME_RECORDS] + id * WM_B_WORDS + w];
}
fn wmFlag(b : u32, f : u32) -> bool { return (wmBiome(b, WM_B_FLAGS) & f) != 0u; }
fn wmCover(b : u32, i : u32, w : u32) -> u32 {
  return worldMap[wmBiome(b, WM_B_COVER_OFF) + i * WM_C_WORDS + w];
}
// ---- the water preset a column's pond and shore wear (P-E) -----------------
// P-E INTERIM: a disc pond has no preset of its own until P-F drives the bowl
// from the water table, so every pond and shore in a biome wears the preset
// of the biome's FIRST water.features row (worldmap.cpp WaterPresetOf). The
// index is 1-based; 0 = the biome authors no water and every read below is 0,
// which turns every chance off -- no shore plants, no pond life, no moss.
fn wmWaterOf(b : u32) -> u32 { return wmBiome(b, WM_B_WATER_PRESET); }
fn wmWater(p : u32, w : u32) -> u32 {
  if (p == 0u || p > worldMap[WM_H_WATER_COUNT]) { return 0u; }
  return worldMap[worldMap[WM_H_WATER_RECORDS] + (p - 1u) * WM_W_WORDS + w];
}
fn wmShore(p : u32, i : u32, w : u32) -> u32 {
  return worldMap[wmWater(p, WM_W_SHORE_OFF) + i * WM_P_WORDS + w];
}
// `h % chance == 0` with chance 0 = never, in one place. Every flora chance
// in the tables is authored 1-in-N with 0 meaning off, and a modulo by zero
// is undefined on the GPU, so no reader below spells the test itself.
fn rollChance(h : u32, chance : u32) -> bool {
  return chance != 0u && (h % chance) == 0u;
}

// ---- the painted planes (P2) ------------------------------------------------
// Four cells per word, little-endian; i = cz * width + cx (worldmap.h).
fn wmPlaneAt(off : u32, cx : i32, cz : i32) -> u32 {
  let i = u32(cz) * worldMap[WM_H_WIDTH] + u32(cx);
  return (worldMap[off + (i >> 2u)] >> ((i & 3u) * 8u)) & 0xFFu;
}
// World column -> plane cell. `>>` on a negative i32 is an arithmetic shift
// in WGSL and in C++ (World::MapBiomeAt is the twin), so this floors.
fn wmCellOf(x : i32, z : i32) -> vec2<i32> {
  let l = worldMap[WM_H_CELL_LOG2];
  return vec2<i32>((x >> l) + i32(worldMap[WM_H_ORIGIN_X]),
                   (z >> l) + i32(worldMap[WM_H_ORIGIN_Z]));
}
fn wmInside(c : vec2<i32>) -> bool {
  return c.x >= 0 && c.y >= 0 && c.x < i32(worldMap[WM_H_WIDTH]) &&
         c.y < i32(worldMap[WM_H_HEIGHT]);
}

// ---- THE HARNESS SITE ------------------------------------------------------
// The one authored site the map ships until P5's site table: a box in world
// voxels (map.json sites[], kind "pad") that keeps the selftest fixtures'
// ground -- no tree trunks or crowns, no tarns, no cover, no caves' flora.
// It replaces the spawn clearing, the fixture pads and the pond keep-out box
// that used to be literals in this file and in world.cpp. Read from the
// header so the C++ twin (World::InHarness) reads the same numbers.
fn inHarness(x : i32, z : i32) -> bool {
  return x >= i32(worldMap[WM_H_HARNESS_X0]) && x <= i32(worldMap[WM_H_HARNESS_X1]) &&
         z >= i32(worldMap[WM_H_HARNESS_Z0]) && z <= i32(worldMap[WM_H_HARNESS_Z1]);
}
// Does a tree at (wx,wz) with horizontal reach `r` put ANY of itself over the
// harness? The trunk being outside is not enough -- see the note at the call
// site in treeInfoAt.
fn crownMeetsHarness(wx : i32, wz : i32, r : i32) -> bool {
  return wx + r >= i32(worldMap[WM_H_HARNESS_X0]) && wx - r <= i32(worldMap[WM_H_HARNESS_X1]) &&
         wz + r >= i32(worldMap[WM_H_HARNESS_Z0]) && wz - r <= i32(worldMap[WM_H_HARNESS_Z1]);
}

// ---- THE SITE TABLE (P5) ----------------------------------------------------
// `wmSiteAt` is the per-column cost: one plane read, 0 = no site. A site's
// record gives its centre, footprint radius, pad margin and stamp block. The
// pad (`sitePadAt`, inside the height mirror) levels the ground under the
// footprint to the height at the site's centre and ramps it back over the
// margin -- Lin's "shape the terrain toward the structure", the ruinPad this
// replaced generalised to authored sites. The stamp (`wmStampCell`) is a
// per-cell overlay of the template's runs, exactly the tree atlas's shape,
// so it is correct in `far` at any distance with nothing to patch. Keep-outs
// (`siteKeepOut`) suppress trees, tarns and cover on the site's cells, the
// harness box included.
fn wmSiteAt(x : i32, z : i32) -> u32 {
  let plane = worldMap[WM_H_SITE_INDEX];
  if (plane == 0u) { return 0u; }
  let c = wmCellOf(x, z);
  if (!wmInside(c)) { return 0u; }
  return wmPlaneAt(plane, c.x, c.y);
}
fn wmSiteI(sid : u32, w : u32) -> i32 {
  return bitcast<i32>(worldMap[worldMap[WM_H_SITE_TABLE] + (sid - 1u) * WM_S_WORDS + w]);
}
fn siteKeepOut(x : i32, z : i32) -> bool {
  return inHarness(x, z) || wmSiteAt(x, z) != 0u;
}
// The template voxel this world cell would carry, MAT_AIR if none: the
// stamp's footprint is centred on the site, its bottom row sits one above
// the pad height (`padY`, which the caller resolves the way sitePadAt does).
fn wmStampCell(sid : u32, x : i32, y : i32, z : i32, padY : i32) -> u32 {
  let blk = u32(wmSiteI(sid, WM_S_STAMP_OFF));
  if (blk == 0u) { return MAT_AIR; }
  let nx = i32(worldMap[blk + WM_STAMP_NX]);
  let ny = i32(worldMap[blk + WM_STAMP_NY]);
  let nz = i32(worldMap[blk + WM_STAMP_NZ]);
  let lx = x - (wmSiteI(sid, WM_S_X) - nx / 2);
  let lz = z - (wmSiteI(sid, WM_S_Z) - nz / 2);
  let ly = y - padY - 1;
  if (lx < 0 || lz < 0 || ly < 0 || lx >= nx || lz >= nz || ly >= ny) { return MAT_AIR; }
  let ci = worldMap[blk + WM_STAMP_COLUMNS] + u32(lz * nx + lx) * 2u;
  let runOff = worldMap[ci];
  let cnt = worldMap[ci + 1u];
  for (var k = 0u; k < cnt; k++) {
    let r = worldMap[runOff + k];
    let y0 = i32((r >> 16u) & TREE_RUN_Y0_MASK);
    if (ly < y0) { return MAT_AIR; }
    if (ly < y0 + i32((r >> TREE_RUN_LEN_SHIFT) & TREE_RUN_LEN_MASK)) { return r & 0xFFFu; }
  }
  return MAT_AIR;
}
// The top of whatever a site puts above the ground at this column, for the
// sky early-out and the far blocker band: pad height + the stamp's height.
// -1e6 where there is no site, so max() ignores it.
fn wmSiteTopAt(x : i32, z : i32, seed : u32) -> i32 {
  let sid = wmSiteAt(x, z);
  if (sid == 0u) { return -1048576; }
  let blk = u32(wmSiteI(sid, WM_S_STAMP_OFF));
  if (blk == 0u) { return -1048576; }
  let padY = landColumnBare(wmSiteI(sid, WM_S_X), wmSiteI(sid, WM_S_Z), seed).h;
  return padY + 1 + i32(worldMap[blk + WM_STAMP_NY]);
}

// ---- THE LANDFORM PLANE (P4): the map owns the continental rung -----------
// These are the ONLY readers of the landform plane and the sea level, and
// they sit OUTSIDE the height mirror on purpose: the mirrored landAt calls
// landformOctave()/seaLevelY() by name and the C++ twins (world.cpp, same
// names, same arithmetic over the same bytes) are what World::TerrainHeight
// runs. The `terrain` gate's C1 pass proves the two agree per voxel; the
// token compare only has to see the call.
//
// Tier A, no seed: the plane is where the mountains ARE. Q8 bilinear over
// the four cells around the column; outside the painted planes the value
// fades to 0 (the ocean floor) over WM_H_OCEAN_FADE cells, so past the
// coast the ground keeps falling and the sea fill below takes over. Clamped
// reads: a cell index past the edge reads the edge cell, never the next
// row, and the same clamp is spelled in the C++ twin.
fn seaLevelY() -> i32 { return i32(worldMap[WM_H_SEA_LEVEL_Y]); }
fn wmLandformCellQ8(cx : i32, cz : i32) -> i32 {
  let w = i32(worldMap[WM_H_WIDTH]);
  let h = i32(worldMap[WM_H_HEIGHT]);
  let c = vec2<i32>(clamp(cx, 0, w - 1), clamp(cz, 0, h - 1));
  return i32(wmPlaneAt(worldMap[WM_H_LANDFORM_PLANE], c.x, c.y)) << 8;
}
// Q8 landform at a column: 0..65280. Bilinear over the cell's four corners
// (the cell value is its CENTRE), then the ocean fade past the plane's edge.
fn mapLandformQ8(x : i32, z : i32) -> i32 {
  if (worldMap[WM_H_LANDFORM_PLANE] == 0u) { return 32768; }   // no planes: flat
  let l = worldMap[WM_H_CELL_LOG2];
  let half = 1 << (l - 1u);
  let mx = x - half + (i32(worldMap[WM_H_ORIGIN_X]) << l);
  let mz = z - half + (i32(worldMap[WM_H_ORIGIN_Z]) << l);
  let cx = mx >> l;
  let cz = mz >> l;
  let mask = (1 << l) - 1;
  let fx = mx & mask;
  let fz = mz & mask;
  let c00 = wmLandformCellQ8(cx, cz);
  let c10 = wmLandformCellQ8(cx + 1, cz);
  let c01 = wmLandformCellQ8(cx, cz + 1);
  let c11 = wmLandformCellQ8(cx + 1, cz + 1);
  let a = c00 + (((c10 - c00) * fx) >> l);
  let b = c01 + (((c11 - c01) * fx) >> l);
  let v = a + (((b - a) * fz) >> l);
  // outside the plane: how many cells past the edge, then the fade
  let w = i32(worldMap[WM_H_WIDTH]);
  let hh = i32(worldMap[WM_H_HEIGHT]);
  let dOut = max(max(-cx, cx + 1 - w), max(-cz, cz + 1 - hh));
  let fade = max(i32(worldMap[WM_H_OCEAN_FADE]), 1);
  if (dOut <= 0) { return v; }
  return (v * max(fade - dOut, 0)) / fade;
}
// Gradient of the landform contribution, in the octave's Q8-per-voxel units:
// the cell-to-cell difference of the Q8 plane, scaled by the amplitude, over
// one cell of voxels. Piecewise constant per cell; it only feeds the domain
// warp of the finer rungs and the sediment slope gate.
fn mapLandformGx(x : i32, z : i32) -> i32 {
  if (worldMap[WM_H_LANDFORM_PLANE] == 0u) { return 0; }
  let l = worldMap[WM_H_CELL_LOG2];
  let half = 1 << (l - 1u);
  let cx = (x - half + (i32(worldMap[WM_H_ORIGIN_X]) << l)) >> l;
  let cz = (z - half + (i32(worldMap[WM_H_ORIGIN_Z]) << l)) >> l;
  let d = wmLandformCellQ8(cx + 1, cz) - wmLandformCellQ8(cx, cz);
  return (d * TUNE_CONT_AMPLITUDE) >> (8u + l);
}
fn mapLandformGz(x : i32, z : i32) -> i32 {
  if (worldMap[WM_H_LANDFORM_PLANE] == 0u) { return 0; }
  let l = worldMap[WM_H_CELL_LOG2];
  let half = 1 << (l - 1u);
  let cx = (x - half + (i32(worldMap[WM_H_ORIGIN_X]) << l)) >> l;
  let cz = (z - half + (i32(worldMap[WM_H_ORIGIN_Z]) << l)) >> l;
  let d = wmLandformCellQ8(cx, cz + 1) - wmLandformCellQ8(cx, cz);
  return (d * TUNE_CONT_AMPLITUDE) >> (8u + l);
}
// THE BIOME, from the map: Tier A (the plane) is seed-independent; the
// boundary warp is Tier B and takes the seed, so a region's EDGE wanders per
// seed by up to warpAmp (<= cell/4, enforced by the loader) while its centre
// stays put -- "the mountains are always north, but a little different every
// seed". Outside the painted planes the world is ocean. Two vnoise2d calls
// with distinct salts: the same eight hashes the old noise band cost.
fn mapBiomeAt(x : i32, z : i32, seed : u32) -> u32 {
  let plane = worldMap[WM_H_BIOME_PLANE];
  if (plane == 0u) { return 0u; }   // no planes uploaded (a tool with records only)
  let amp = i32(worldMap[WM_H_WARP_AMP]);
  let wx = x + (((vnoise2d(x, z, 9u, seed ^ 0x3A9Fu).n - 8192) * amp) >> 14);
  let wz = z + (((vnoise2d(x, z, 9u, seed ^ 0x3AA0u).n - 8192) * amp) >> 14);
  let c = wmCellOf(wx, wz);
  if (!wmInside(c)) { return worldMap[WM_H_OCEAN_BIOME]; }
  return wmPlaneAt(plane, c.x, c.y);
}

// Placement is per TREE_TILE XZ tile: hash the tile, and it either holds one
// tree or none. The tile has to be at least as wide as a canopy or trees
// overlap into mush. Some overlap is good — that is what closes a canopy — but
// it has to be overlap, not merger.
const TREE_TILE : i32 = TUNE_TREE_TILE;         // ~14 m between trunk sites
// How many tiles out to search. See the NINE derivation at TreeCands.
const TREE_SCAN : i32 = 2;
//
// Everything is a pure function of (tile coords, seed) — no state, no
// scattering pass — so a tree straddling a chunk border generates identically
// from either chunk, and a chunk evicted and re-entered regrows the same tree.

// ---- BOUNDS, now MEASURED rather than derived ------------------------------
//
// The old file carried a hand-maintained table of per-species maxima with a
// standing warning that a bound too TIGHT shears canopies and moves the world
// hash. Both numbers are now measured off the baked grids by the bake
// (`meta.reachXZ` / `meta.above` in treegen.js, asserted per species by
// scripts/test_treegen.mjs) and reduced to a max by the C++ loader. There is
// nothing left to keep in sync: the bound is a property of the voxels.
fn treeMaxAbove() -> i32 { return i32(treeAtlas[TA_H_MAX_ABOVE]); }
fn treeMaxReach() -> i32 { return i32(treeAtlas[TA_H_MAX_REACH]); }
// A trunk only exists below the treeline, so that is the highest ground any
// trunk can stand on; above it plus the tallest species there is no tree
// anywhere in the world, at any seed — one compare replaces the whole scan.
// The HOISTED path does not use this at all: `cands.top` is strictly tighter.
fn treeMaxTop() -> i32 { return TUNE_TREELINE - 1 + treeMaxAbove(); }

// Per-tile tree descriptor. The FULL form, used by the scans that need a
// species' metadata (undergrowth cover, the far-field canopy proxy); the
// per-cell path uses the compact TreeCand below.
struct Tree {
  present : bool,
  sp      : i32,   // species index into the atlas, -1 when absent
  varOff  : u32,   // word offset of this tree's variant directory entry
  wx      : i32,   // trunk world x/z
  wz      : i32,
  base    : i32,   // ground height at the trunk
  rot     : u32,   // 0..3 quarter turns applied to the variant
  mir     : bool,  // mirrored in x as well
  reach   : i32,   // horizontal reach of the species, voxels
  above   : i32,   // vertical reach above `base`
  crownR  : i32,   // crown proxy radius (far field, undergrowth cover)
  shade   : i32,   // 0..255 canopy cover cast on the forest floor
  autumn  : bool,  // this TREE (not this species) wears the autumn ramp
  rnd     : u32,   // spare bits for per-tree jitter
};

// The HASH-ONLY half: WHERE the trunk stands, and nothing that costs a noise
// lookup. Split out so the scan can reject a tile on distance — and then on
// ground height alone — before paying for the site's biome and pond queries.
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

// The rest of treeInfo, given a site and the LAND at that site.
//
// `land` is passed in rather than sampled here because the caller has already
// had to know the ground height for its vertical reject, and landAt is eight
// hashes — sampling it twice would be the most expensive thing this function
// does. It carries the SLOPE as well, which costs nothing extra and is what the
// per-species steepness gate needs: `Land.slope` is the coarse landform
// gradient (the hill octaves, not the grain), which is the only gradient a
// slope gate may read — the grain octave crosses a whole gate in one column.
fn treeInfoAt(s : TreeSite, land : Land, seed : u32) -> Tree {
  var t : Tree;
  t.present = false;
  t.sp = -1; t.varOff = 0u;
  t.wx = s.wx; t.wz = s.wz; t.base = land.h;
  t.rot = 0u; t.mir = false;
  t.reach = 0; t.above = 0; t.crownR = 0; t.shade = 0; t.autumn = false;
  t.rnd = s.hsh;

  let ns = taSpeciesCount();
  if (ns <= 0) { return t; }            // no atlas: a legal, treeless world

  let hsh = s.hsh;
  let h = land.h;

  // No trees on snowfields, in ponds, or over the selftest fixture sites.
  if (h >= TREELINE) { return t; }
  if (pondAt(t.wx, t.wz, seed).y >= 0) { return t; }
  // (The spawn clearing is checked AFTER the species draw, where the crown's
  // real width is known — see the note at that test.)

  // Density by biome, from the biome's record (assets/biomes/<name>.json
  // `trees.density`, percent of tiles): forest is nearly every tile, meadow a
  // sparse clearing, desert the occasional dead bush. The TILE is global
  // (TREE_TILE): a per-biome tile would need a per-biome lattice, and the 5x5
  // candidate scan assumes one lattice for every column it looks at.
  let biome = biomeAt(t.wx, t.wz, seed);
  let roll = (hsh >> 17u) % 100u;
  let chance = wmBiome(biome, WM_B_TREE_DENSITY);
  if (roll >= chance) { return t; }

  // ---- WHICH SPECIES: a weighted draw from the biome's own table -----------
  // The table is cumulative weights, built by the C++ loader from each
  // species' authored `placement.biomes` and divided by its `sparsity`. So
  // adding a species file DILUTES the others rather than needing every table
  // rewritten, and "where does an oak grow" lives in oak.json and nowhere else.
  //
  // ITS OWN HASH, not a bit-slice of `hsh`. Slices of one hash correlate, and
  // the density roll above is already spending this one — the pond-life block
  // in this file documents at length what that correlation does to a scatter.
  let h2 = hash3(seed ^ 0x7BEE6u, bitcast<u32>(s.wx), bitcast<u32>(s.wz));
  let bt = treeAtlas[TA_H_BIOME_TABLE] + u32(biome) * (1u + u32(ns));
  let total = treeAtlas[bt];
  if (total == 0u) { return t; }        // no species wants this biome
  let pickRoll = h2 % total;
  var sp = -1;
  for (var i = 0; i < ns; i++) {
    if (pickRoll < treeAtlas[bt + 1u + u32(i)]) { sp = i; break; }
  }
  if (sp < 0) { return t; }

  // ---- PLACEMENT GATES ------------------------------------------------------
  // Altitude band and steepness, per species, from the species file. A gated-out
  // pick grows NOTHING rather than re-rolling, and that is the point: it is what
  // thins a forest as the ground steepens and what gives each species its own
  // treeline, instead of substituting a different tree and keeping the density
  // flat everywhere.
  let minY = i32(taSpecies(sp, TA_S_MIN_Y));
  let maxY = i32(taSpecies(sp, TA_S_MAX_Y));
  if (minY >= 0 && h < minY) { return t; }
  if (maxY >= 0 && h > maxY) { return t; }
  let maxSlope = i32(taSpecies(sp, TA_S_MAX_SLOPE));
  if (maxSlope > 0 && land.slope > maxSlope) { return t; }

  // ---- WHICH VARIANT, AND HOW IT SITS --------------------------------------
  // variants x 4 rotations x mirror is 24 appearances from three baked trees,
  // and the rotation costs two integer swaps at sample time. A third hash for
  // the same reason as the second.
  let h3 = hash3(seed ^ 0x7BEE7u, bitcast<u32>(s.wx), bitcast<u32>(s.wz));
  let vc = max(i32(taSpecies(sp, TA_S_VARIANT_CNT)), 1);
  t.varOff = taSpecies(sp, TA_S_VARIANT_DIR) + (h3 % u32(vc)) * TA_VARIANT_WORDS;
  t.rot = (h3 >> 9u) & 3u;
  t.mir = ((h3 >> 11u) & 1u) != 0u;

  t.reach = i32(taSpecies(sp, TA_S_REACH));

  // ---- THE SPAWN CLEARING IS A CLEARING, NOT A TRUNK BAN -------------------
  // Checked HERE rather than up with the other rejects, because it needs the
  // species' CROWN WIDTH and that is not known until the draw above.
  //
  // The old test refused trunks INSIDE the box and nothing else, which was
  // survivable while the widest crown was ~5 m: an oak just outside the fence
  // overhung it by a few metres of leaves nobody stood under. The baked atlas
  // put a great oak's reach at 11.5 m, and the player spawns ten voxels above
  // the ground at the middle of that box — so they materialised INSIDE a canopy
  // that had grown over the clearing from outside it, landed on the leaves and
  // walked off the edge. Expanding the keep-out by the tree's own reach is what
  // makes the clearing mean what its name says at any crown width.
  // Box OVERLAP, not a corner test: a crown wider than the clearing would pass
  // every corner check while covering the whole thing.
  if (crownMeetsHarness(t.wx, t.wz, t.reach)) { return t; }
  // No trunk on an authored site's cells (P5): the pad is a floor, the stamp
  // a building. A crown reaching in from outside is allowed and wanted.
  if (wmSiteAt(t.wx, t.wz) != 0u) { return t; }

  t.sp = sp;
  t.above = i32(taSpecies(sp, TA_S_ABOVE));
  t.crownR = i32(taSpecies(sp, TA_S_CROWN_R));
  t.shade = i32(taSpecies(sp, TA_S_SHADE));
  // Autumn is per TREE, never per voxel: a stand turns together or not at all.
  //
  // The species file authors WHETHER and HOW OFTEN this species turns (an oak
  // does, a spruce never will): `autumnChance` in assets/trees/<name>.json,
  // 1-in-N trees, carried into the atlas header as TA_S_AUTUMN. There is no
  // global scale on it any more (P-E deleted worldgen.autumnFraction, whose
  // default was the no-op): one authoring surface per fact, and the number on
  // the tree page is the number the world rolls.
  let ac = taSpecies(sp, TA_S_AUTUMN);
  t.autumn = ac != 0u && ((h3 >> 14u) % max(ac, 1u)) == 0u;
  t.present = true;
  return t;
}

// The one-shot form, for callers with no reject of their own to do first
// (undergrowthSite, treeCanopyAt).
fn treeInfo(tx : i32, tz : i32, seed : u32) -> Tree {
  let s = treeSite(tx, tz, seed);
  return treeInfoAt(s, landAt(s.wx, s.wz, seed), seed);
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

// ---- THE TREE COLUMN, hoisted out of the per-cell path --------------------
//
// WHICH TREES CAN PUT A VOXEL OVER THIS COLUMN IS A FUNCTION OF (x, z) ALONE.
// The tile scan, the trunk sites, the ground under each of them, the species
// draw, the variant and its rotation are all y-independent — and so, now, is
// the COLUMN LOOKUP itself: rotating (dx,dz) into variant space and fetching
// that column's run list happens once per column, not once per cell. genCellIn
// asks per AIR cell above the ground, which in a streamed-in vertical slab is
// most of the chunk, so this is the difference between one lookup and sixteen.
//
// It also SHRINKS THE CANDIDATE SET to trees that actually cover this column.
// The old code kept every tile whose crown radius reached, then re-tested a
// shape per cell; a column-empty tree is now dropped outright at hoist time,
// so the per-cell loop typically runs zero or one iterations under open sky and
// one or two under a closed canopy.
//
// NINE IS ENOUGH, and it is checked rather than assumed. A site sits in the
// middle half of its tile, so tile `t` puts its trunk in
// [TILE*t + TILE/4, TILE*t + 3*TILE/4); a tile can reach column x only if that
// range meets [x - reach, x + reach], which spans 2*reach + TILE/2 - 1 voxels
// of tile origin and therefore covers at most (2*reach + TILE/2 - 1)/TILE + 1
// tiles per axis. At TILE 144 that is three per axis — nine — for any species
// narrower than 180 voxels. `reach` is now ASSET DATA, so LoadTreeAtlas
// (src/sim/treeatlas.h MaxReachForNineCandidates) REFUSES an atlas that would
// break the derivation: past the bound the shader silently drops a candidate,
// and the symptom is a canopy missing from some columns and present on others.
const TREE_CAND_MAX : i32 = 9;

// The compact per-column form. Deliberately NINE WORDS: this array lives on the
// function stack of genChunk and a wide struct here is scratch traffic on every
// worldgen thread. Everything a per-cell test needs and nothing it does not —
// the species metadata stays in the atlas, where the two scans that want it
// (undergrowth, far canopy) read it directly.
struct TreeCand {
  wx     : i32,
  wz     : i32,
  base   : i32,   // ground at the trunk; local y 0 sits at base + 1
  ny     : i32,   // variant height, for the vertical bound
  vtop   : i32,   // highest world y this tree can occupy
  colOff : u32,   // this column's run list in the atlas
  colCnt : u32,
  leafSw : u32,   // autumn substitution: 0 = none, else the species' word offset
};

struct TreeCands {
  n   : i32,
  top : i32,             // highest vtop in the set; far below any y when empty
  t   : array<TreeCand, 9>,  // TREE_CAND_MAX; WGSL wants a literal here
};

// Rotate a trunk-relative offset into the variant's own grid. Four quarter
// turns plus an optional mirror give 8 orientations from one baked tree, for
// two integer swaps and a negate.
fn treeLocalXZ(t : Tree, dx : i32, dz : i32) -> vec2<i32> {
  var rx = dx;
  var rz = dz;
  switch (t.rot) {
    case 1u: { rx = -dz; rz = dx; }
    case 2u: { rx = -dx; rz = -dz; }
    case 3u: { rx = dz;  rz = -dx; }
    default: {}
  }
  if (t.mir) { rx = -rx; }
  let ax = i32(treeAtlas[t.varOff + TA_V_ANCHORX]);
  let az = i32(treeAtlas[t.varOff + TA_V_ANCHORZ]);
  return vec2<i32>(ax + rx, az + rz);
}

fn treeCandsInto(c : ptr<function, TreeCands>, x : i32, z : i32, seed : u32) {
  (*c).n = 0;
  (*c).top = -1048576;
  if (taSpeciesCount() <= 0) { return; }
  let maxReach = treeMaxReach();
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN; oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN; ox <= TREE_SCAN; ox++) {
      // Horizontal reject on the trunk site alone (one hash), against the
      // WIDEST species in the atlas. A +-2 tile's trunk is at least
      // TILE*2 - TILE/4 away, so the outer ring is rejected outright and only
      // a few of the 25 tiles reach the noise queries below.
      let s = treeSite(tx + ox, tz + oz, seed);
      if (abs(x - s.wx) > maxReach || abs(z - s.wz) > maxReach) { continue; }
      let t = treeInfoAt(s, landAt(s.wx, s.wz, seed), seed);
      if (!t.present) { continue; }
      // Now the species' OWN reach, which is what actually decides.
      if (abs(x - t.wx) > t.reach || abs(z - t.wz) > t.reach) { continue; }

      // The column lookup, hoisted. A tree whose baked grid has nothing in this
      // column is not a candidate at all: the atlas is the whole tree, so an
      // empty column can contribute nothing.
      let l = treeLocalXZ(t, x - t.wx, z - t.wz);
      let nx = i32(treeAtlas[t.varOff + TA_V_NX]);
      let nz = i32(treeAtlas[t.varOff + TA_V_NZ]);
      if (l.x < 0 || l.y < 0 || l.x >= nx || l.y >= nz) { continue; }
      let ci = treeAtlas[t.varOff + TA_V_COLUMNS] +
               u32(l.y * nx + l.x) * 2u;
      let cnt = treeAtlas[ci + 1u];
      if (cnt == 0u) { continue; }

      if ((*c).n >= TREE_CAND_MAX) { continue; }   // see the NINE derivation
      var e : TreeCand;
      e.wx = t.wx; e.wz = t.wz; e.base = t.base;
      e.ny = i32(treeAtlas[t.varOff + TA_V_NY]);
      // Local y 0 sits one voxel ABOVE the ground: genCellIn only asks about
      // cells with y > h, so a row at y == base could never be reached and
      // baking one would waste a layer of every variant.
      e.vtop = t.base + 1 + e.ny;
      e.colOff = treeAtlas[ci];
      e.colCnt = cnt;
      e.leafSw = select(0u,
          treeAtlas[TA_H_SPECIES_DIR] + u32(t.sp) * TA_SPECIES_WORDS,
          t.autumn);
      (*c).t[(*c).n] = e;
      (*c).n = (*c).n + 1;
      (*c).top = max((*c).top, e.vtop);
    }
  }
}

// The .svtree run word: material(12) | state(4) | y0(11) | len(5).
//
// MIRRORS src/sim/treeatlas.h (kRunY0Bits / kRunLenBits) and
// assets/editor/treegen.js (packRun). Three implementations of one layout;
// scripts/check_invariants.py asserts they agree, because a silent disagreement
// here decodes every tree in the world into noise.
//
// Y0 used to be 9 bits, capping a variant at 512 voxels — fine at 10 cm, but a
// 22 m redwood needs ~520 at 5 cm. Two bits moved from LEN to Y0, so runs are
// split at 31 instead of 127 and the height ceiling is 2047.
const TREE_RUN_Y0_MASK : u32 = 2047u;
const TREE_RUN_LEN_SHIFT : u32 = 27u;   // 16 + Y0 bits
const TREE_RUN_LEN_MASK : u32 = 31u;

// Material this tree contributes at world height `y`, or MAT_AIR.
//
// The whole per-cell cost of a tree, and it is a linear scan of ONE column's
// runs — typically one to three of them. Runs are sorted ascending and do not
// overlap, so the first run that starts above `ly` ends the search.
fn treeCellFrom(c : TreeCand, y : i32) -> u32 {
  let ly = y - c.base - 1;
  if (ly < 0 || ly >= c.ny) { return MAT_AIR; }
  for (var k = 0u; k < c.colCnt; k++) {
    let r = treeAtlas[c.colOff + k];
    let y0 = i32((r >> 16u) & TREE_RUN_Y0_MASK);
    if (ly < y0) { return MAT_AIR; }
    if (ly < y0 + i32((r >> TREE_RUN_LEN_SHIFT) & TREE_RUN_LEN_MASK)) {
      var m = r & 0xFFFu;
      // AUTUMN: a per-TREE substitution across the species' three-step leaf
      // ramp. Done as a ramp rather than one flat colour because replacing a
      // shaded green with a flat orange would throw away the shading bake on
      // exactly the trees the eye goes to. Three compares, on leaf voxels of
      // autumn trees only.
      if (c.leafSw != 0u) {
        for (var i = 0u; i < 3u; i++) {
          if (m == treeAtlas[c.leafSw + TA_S_LEAF0 + i]) {
            m = treeAtlas[c.leafSw + TA_S_AUTUMN0 + i];
            break;
          }
        }
      }
      return m;
    }
  }
  return MAT_AIR;
}

// NO IMPLICIT DECORATION HANGS OFF A TREE. There used to be a per-cell
// `treeVineFrom` here that draped vine curtains and Spanish-moss beards from
// the canopy underside and spiralled ivy ropes up the bole, all as closed-form
// predicates on top of the baked runs. It is gone on purpose: the .svtree atlas
// is now the WHOLE tree, so what the tuner's Trees tab renders is exactly what
// the world grows, with nothing added behind the author's back. A decoration
// that belongs on a tree belongs in treegen.js, where it can be seen while it
// is being authored. (`ivy` the material survives — the arena and ruin walls
// place it, and that is a wall, not a tree.)
//
// The y-dependent half. Order is by tile index, a fixed priority, never
// dispatch order (rule 1) — candidates were appended in that order, so first
// non-air still wins the same tile it always did.
fn treeFromCands(c : ptr<function, TreeCands>, y : i32) -> u32 {
  if (y > (*c).top) { return MAT_AIR; }
  for (var i = 0; i < (*c).n; i++) {
    let e = (*c).t[i];
    if (y < e.base || y > e.vtop) { continue; }
    let m = treeCellFrom(e, y);
    if (m != MAT_AIR) { return m; }
  }
  return MAT_AIR;
}

// The one-shot form, for callers with no column to amortize over (genCell, and
// with it the far cascades). ONE implementation of the rule, split the way
// genColumn/genCellIn are split — not a second copy that has to agree.
//
// The world-wide vertical pre-reject stays HERE and only here: it is what stops
// an isolated sky cell paying for a candidate scan it will not use. The hoisted
// path does not need it, because `cands.top` is strictly tighter.
fn treeAt(x : i32, y : i32, z : i32, seed : u32) -> u32 {
  if (y > treeMaxTop()) { return MAT_AIR; }
  var c : TreeCands;
  treeCandsInto(&c, x, z, seed);
  return treeFromCands(&c, y);
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
const CACTUS_TILE : i32 = (40 * VLEN_NUM) / VLEN_DEN;   // 2.5 m between sites
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

  // Only where the biome says so (cover.cacti), and never on the keep-out
  // ground every other feature avoids: the spawn clearing, the selftest
  // fixture pads, or a pond.
  let cb = biomeAt(c.wx, c.wz, seed);
  if (!wmFlag(cb, WM_BF_CACTI)) { return c; }
  let h = baseHeight(c.wx, c.wz, seed);
  c.base = h;
  if (h >= TREELINE) { return c; }
  if (siteKeepOut(c.wx, c.wz)) { return c; }
  if (pondAt(c.wx, c.wz, seed).y >= 0) { return c; }

  // Density and mix are the biome's (cover.cactusChance / saguaroFraction,
  // both percents, packed by worldmap.cpp): the flag says whether, the
  // record says how many.
  let roll = (hsh >> 17u) % 100u;
  if (roll >= wmBiome(cb, WM_B_CACTUS_CHANCE)) { return c; }

  // Barrels outnumber saguaros heavily. A desert with a saguaro every 2.5 m is
  // a plantation; the columns have to be occasional or they stop being
  // landmarks, which is the entire job they do here.
  let sroll = (hsh >> 24u) % 100u;
  c.species = select(1u, 0u, sroll < wmBiome(cb, WM_B_SAGUARO_FRACTION));

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
    default:          { return 5 + i32(h % 3u); }           // foxglove 50-70 cm
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
      let dens = min(u32(tg - 176) >> 3u, 8u);   // 0..8 in twentieths of columns
      // % 20, not % 10: half the blades of the first cut (see UG_FERN_CHANCE).
      if ((hTall % 20u) < dens + 1u) {
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

  // Per-mille per column; half the first cut (see UG_FERN_CHANCE).
  var thresh = 0u;
  if (biome == B_MEADOW) { thresh = select(3u, 30u, clump > 165); }
  else                   { thresh = select(1u, 8u, clump > 190); }
  if ((fr % 1000u) >= thresh) { return f; }

  let sp = vnoise(x + 911, z - 733, 40 * HSCALE, seed ^ 0xF1A5u);
  let spj = sp + (vnoise(x, z, 11 * HSCALE, seed ^ 0xF1A6u) - 128) / 4;
  let hBell = hash3(seed ^ 0xB1E7u, bitcast<u32>(x), bitcast<u32>(z));
  let hFoxg = hash3(seed ^ 0xF0C9u, bitcast<u32>(x), bitcast<u32>(z));
  let hButt = hash3(seed ^ 0x8B77u, bitcast<u32>(x), bitcast<u32>(z));
  let hClov = hash3(seed ^ 0xC10Fu, bitcast<u32>(x), bitcast<u32>(z));
  let hRose = hash3(seed ^ 0x8053u, bitcast<u32>(x), bitcast<u32>(z));

  // A lawn tuft is the default: a meadow is grass WITH flowers in it. It used
  // to be a solid grass or petal cube on the surface, which read as green
  // gravel; the tuft is one or two cells of short analytic blades.
  var m = M_GRASS_TUFT;
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
  // The tuft is one or two cells; the flowers stack by species.
  if (m == M_GRASS_TUFT) { f.height = 1 + i32((fr >> 13u) & 1u); }
  else { f.height = flowerHeight(m, hFoxg >> 7u); }
  return f;
}

struct Undergrowth {
  cover   : i32,   // 0 = open sky, 255 = deep under a crown
  trunkD2 : i32,   // squared XZ distance to the nearest trunk, or a large value
  shade   : i32,   // how much canopy that nearest tree casts, 0 for a shrub
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
  u.shade = 0;
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
        u.shade = t.shade;
        u.rnd = t.rnd;
      }

      // Canopy cover, from the species' OWN authored shade and its MEASURED
      // crown radius. A shrub is authored at shade 0 and contributes none —
      // counting one made every meadow read as closed forest, because shrubs
      // are the commonest meadow tile. An airy birch and a dark spruce differ
      // by their number here rather than by a branch on a species id, which is
      // what lets a new species file arrive without touching this function.
      let r = t.crownR;
      let peak = t.shade;
      if (peak <= 0 || r <= 0) { continue; }
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
const LAVA_LEVEL : i32 = (-80 * VLEN_NUM) / VLEN_DEN;

// What a carved cell is filled with. The one place the magma table is applied.
fn caveFill(y : i32) -> i32 {
  if (y <= LAVA_LEVEL) { return 2; }   // flooded, and flat, and therefore still
  return 1;                            // open cave
}

// ---- THE CAVE COLUMN, hoisted out of the per-cell path --------------------
//
// EVERY ONE of the six noise samples below is a pure function of (x, z, h,
// seed). `y` appears nowhere except in the range compares. genCellIn calls this
// per STONE cell, so a buried column recomputed the identical band geometry
// sixteen times — about 234 hash3 against genColumn's own ~18, i.e. ~93% of the
// worldgen cost of a buried chunk, and by a distance the largest unclaimed win
// in this file.
//
// So it splits exactly the way genColumn/genCellIn split: caveBands() is the
// (x,z)-only half and caveIn() is the y-only half. caveAt() below composes them
// and is what the one-shot callers (genCell, and with it the far cascades) still
// use, so there is ONE implementation of the rule and not two that must agree.
struct CaveBands {
  on1 : bool,   // near-surface band present at this column
  f1  : i32,    // its floor / ceiling
  c1  : i32,
  on2 : bool,   // deep band present
  f2  : i32,
  c2  : i32,
  top2 : i32,   // deep band additionally capped at h - 40
};

fn caveBands(x : i32, z : i32, h : i32, biome : u32, seed : u32) -> CaveBands {
  var b : CaveBands;
  // band 1: near-surface caverns following the terrain
  // Cell sizes are log2 exponents (5 = 32 voxels, 4 = 16); the masks are shifted
  // back to the 0..255 band the two THRESHOLD values are authored in. The
  // thresholds come from the biome's record (assets/biomes/<name>.json
  // caves.features, near_surface / deep); a biome that authors none carries
  // the global knobs, packed in by worldmap.cpp.
  b.on1 = (vnoise2d(x, z, 5u, seed ^ 5u).n >> 6) > i32(wmBiome(biome, WM_B_CAVE_T1));
  b.f1 = h - vlen(40) - ((vnoise2d(x, z, 5u, seed ^ 6u).n * vlen(60)) >> 14);
  b.c1 = min(b.f1 + vlen(10) + ((vnoise2d(x, z, 4u, seed ^ 7u).n * vlen(20)) >> 14),
             h - vlen(40));
  // band 2: deep caverns at absolute depth (streamed depth is real terrain)
  b.on2 = (vnoise2d(x + 7717, z - 4177, 6u, seed ^ 8u).n >> 6) >
          i32(wmBiome(biome, WM_B_CAVE_T2));
  b.f2 = -vlen(40) - ((vnoise2d(x, z, 5u, seed ^ 9u).n * vlen(70)) >> 14);
  b.c2 = b.f2 + vlen(12) + ((vnoise2d(x, z, 4u, seed ^ 10u).n * vlen(26)) >> 14);
  // No `m2 > 190` lava test here any more: gating the fill on the cavern
  // MASK put a vertical lava wall against open air wherever m2 crossed 190,
  // which is a flow the moment the world ticks. Depth is the only thing the
  // fill may depend on.
  b.top2 = h - vlen(40);
  return b;
}

// Band 1 wins ties, exactly as the sequential form did.
fn caveIn(b : CaveBands, y : i32) -> i32 {
  if (b.on1 && y >= b.f1 && y <= b.c1) { return caveFill(y); }
  if (b.on2 && y >= b.f2 && y <= b.c2 && y <= b.top2) { return caveFill(y); }
  return 0;
}

fn caveAt(x : i32, y : i32, z : i32, h : i32, biome : u32, seed : u32) -> i32 {
  return caveIn(caveBands(x, z, h, biome, seed), y);
}

// ---- CAVE FLORA: openness placement where openness is a closed form --------
//
// Lin 13.3.4 asks for plants placed by how open the sky is. Above ground that
// is already what `undergrowthSite`'s canopy cover does, and there is no other
// overhang in the world. Below ground there is no sky at all, so the question
// becomes "what grows in the dark", and the cave bands answer the two structural
// halves of it — where the FLOOR is and where the CEILING is — per column, in
// closed form, with no march and no neighbour lookup.
//
// ONE THING THE PLAN GOT WRONG, and it is worth writing down because it is the
// same class of bug as the floating ruin wall: `caveIn` carves from f1 UPWARD
// (`y >= b.f1`), so **f1 is the lowest AIR cell** and the stone it stands on is
// f1-1. A mushroom at f1+1 — as planned — would float one voxel above its own
// floor. It goes AT f1.
//
// Every test below also asks whether the neighbouring cell is carved, because
// the two bands overlap: band 2 can undercut band 1's floor, and band 1 can eat
// band 2's ceiling. `caveIn` is the authority for both and costs comparisons.
//
// The chances are the biome's (assets/biomes/<name>.json caves.features:
// mushroomChance on the near_surface row, crystalChance on the deep row,
// 1-in-N, 0 = never), read from its record -- there is no global knob.
//
// Returns MAT_AIR for "leave the cave open".
fn caveFloraAt(b : CaveBands, biome : u32, x : i32, y : i32, z : i32, seed : u32) -> u32 {
  // Never in the flooded band, and never within reach of it.
  if (y <= LAVA_LEVEL + CAVE_LAVA_MARGIN) { return MAT_AIR; }

  // ---- SHALLOW BAND FLOOR: mushrooms ----
  // The near-surface caverns are the ones a player walks into from a hillside,
  // so they get the soft, findable thing. Only where the cell below is really
  // solid: a mushroom over a hole is the floating decoration this whole package
  // is about.
  if (b.on1 && y == b.f1 && caveIn(b, y - 1) == 0) {
    if (vnoise(x, z, CAVE_SHROOM_PATCH_CELL, seed ^ 0x5CA9u) >
        CAVE_SHROOM_PATCH) {
      let hm = hash3(seed ^ 0x5A18u, bitcast<u32>(x), bitcast<u32>(z));
      if (rollChance(hm, wmBiome(biome, WM_B_CAVE_MUSHROOM_CHANCE))) {
        // Same red/pale split the forest floor uses, and gated on the SAME roll
        // so this only picks WHICH mushroom, never adds more of them.
        return select(M_TOADSTOOL, M_MUSHROOM, ((hm >> 13u) % 4u) == 0u);
      }
    }
  }

  // ---- DEEP BAND: crystal on the ceiling and the floor ----
  // The deep caverns are the ones you only reach by digging, so they get the
  // light. `top2` is the deep band's extra cap, so its topmost carved cell is
  // min(c2, top2) — the ceiling — and f2 is its floor.
  if (b.on2) {
    let ceil2 = min(b.c2, b.top2);
    let atCeiling = y == ceil2 && caveIn(b, y + 1) == 0;
    let atFloor = y == b.f2 && caveIn(b, y - 1) == 0;
    if ((atCeiling || atFloor) &&
        vnoise(x, z, CAVE_CRYSTAL_PATCH_CELL, seed ^ 0xC275u) >
        CAVE_CRYSTAL_PATCH) {
      // Its own salt, never a bit-slice of the mushroom hash — see the long
      // note in the pond-life block about what correlated slices do to a
      // scatter. A seam and a mushroom bank must be different places.
      let hc = hash3(seed ^ 0xC17Au, bitcast<u32>(x), bitcast<u32>(z));
      if (rollChance(hc, wmBiome(biome, WM_B_CAVE_CRYSTAL_CHANCE))) { return M_CRYSTAL; }
    }
  }
  return MAT_AIR;
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
// ---- AUTHORED POI ANCHORS: the two heights that depend on the SEED ALONE ----
//

struct Col {
  h           : i32,         // ground height, after pool and pond carving
  sed         : i32,         // loose wedge thickness at this column, 0 if none
  biome       : u32,
  pond        : i32,         // disc-pond water surface Y, or -1
  pw          : vec2<i32>,   // pondAt's (bowl floor, surface)
  fluid       : u32,         // standing fluid material at this column
  fluidTop    : i32,         // its surface Y, or -1
  inPoolFloor : bool,
  inRim       : bool,
  shore       : Shore,
  plant       : PlantCol,    // the tile plant whose footprint covers this column
};

// ---- tile plants: ferns and big toadstools ---------------------------------
// A column plant (grass, a flower) is one column and flowerAt() answers it per
// column. A TILE plant is wider than a cell — a fern is a 30 cm rosette, a big
// fly agaric a 30 cm cap — so its footprint is foot x foot columns painted
// with ONE material, base+1 .. base+h cells each, and the renderer rebuilds
// the whole plant from the tile hash in every one of those cells
// (plantTileAt in common.wgsl is the shared question, tracePlant in
// raymarch.wgsl the answer). The base is the CENTRE column's ground, so the
// plant stays one rigid thing across a slope instead of stepping per column;
// outer cells that land inside higher ground simply are not placed (the
// mat == MAT_AIR gate) and the renderer clips the plant to the cells that
// exist.
//
// Cost: one plantTileAt per column (a hash), and for columns INSIDE a present
// footprint one landColumn + one undergrowthSite scan at the centre. Ferns
// cover ~10% of forest columns, so that is one extra scan per ten columns.
struct PlantCol {
  mat  : u32,   // MAT_AIR when no tile plant covers this column
  base : i32,   // the centre column's ground; cells base+1 .. top are plant
  top  : i32,
};

// Is (cx, cz) a place a tile plant may stand? The same exclusions the
// ground-flora block applies to its own column, evaluated at the CENTRE, plus
// "not in or against a trunk" and, for ferns, canopy cover.
struct PlantSite { ok : bool, h : i32 };
fn plantSiteAt(cx : i32, cz : i32, seed : u32, needCover : bool) -> PlantSite {
  var ps : PlantSite;
  ps.ok = false;
  ps.h = 0;
  if (siteKeepOut(cx, cz)) { return ps; }
  if (!wmFlag(biomeAt(cx, cz, seed), WM_BF_GROUND_FLORA)) { return ps; }
  let L = landColumn(cx, cz, seed);
  ps.h = L.h;
  if (L.h >= TREELINE || L.pond >= 0 || L.inRim || L.inPoolFloor) { return ps; }
  if (L.near.onShore && L.near.past < TUNE_SHORE_BAND) { return ps; }
  let ug = undergrowthSite(cx, cz, seed);
  if (ug.trunkD2 <= 4) { return ps; }
  if (needCover && ug.cover < UG_COVER_MIN) { return ps; }
  ps.ok = true;
  return ps;
}

fn plantColumnAt(x : i32, z : i32, seed : u32, biome : u32) -> PlantCol {
  var pc : PlantCol;
  pc.mat = MAT_AIR;
  pc.base = 0;
  pc.top = -1;
  // The tile plants belong to biomes with the ground-flora layer (the
  // world map's flag), not to two hard-coded ids.
  if (!wmFlag(biome, WM_BF_GROUND_FLORA)) { return pc; }
  // FERNS: the signature closed-canopy plant, in banks (the same patch mask
  // the one-cell fern used), under real cover.
  {
    let pt = plantTileAt(x, z, seed, PLANT_FERN_SALT, PLANT_FERN_TILE,
                         PLANT_FERN_FOOT, PLANT_FERN_MINH, PLANT_FERN_MAXH,
                         PLANT_FERN_CHANCE);
    let half = PLANT_FERN_FOOT / 2;
    if (pt.present && abs(x - pt.cx) <= half && abs(z - pt.cz) <= half &&
        vnoise(pt.cx, pt.cz, 20 * HSCALE, seed ^ 0xFE70u) > UG_FERN_PATCH) {
      let site = plantSiteAt(pt.cx, pt.cz, seed, true);
      if (site.ok) {
        pc.mat = M_FERN;
        pc.base = site.h;
        pc.top = site.h + pt.h;
        return pc;
      }
    }
  }
  // BIG TOADSTOOLS: rare, anywhere in the wood — a fern bank is the wrong
  // place for one, so a column already in a fern footprint never gets here.
  {
    let pt = plantTileAt(x, z, seed, PLANT_SHROOM_SALT, PLANT_SHROOM_TILE,
                         PLANT_SHROOM_FOOT, PLANT_SHROOM_MINH, PLANT_SHROOM_MAXH,
                         PLANT_SHROOM_CHANCE);
    let half = PLANT_SHROOM_FOOT / 2;
    if (pt.present && abs(x - pt.cx) <= half && abs(z - pt.cz) <= half) {
      let site = plantSiteAt(pt.cx, pt.cz, seed, false);
      if (site.ok) {
        pc.mat = M_MUSHROOM_LARGE;
        pc.base = site.h;
        pc.top = site.h + pt.h;
      }
    }
  }
  return pc;
}

// ---- THE HEIGHT CONTRACT ---------------------------------------------------
//
//     World::TerrainHeight(x, z, seed)  ==  genColumn(x, z, seed).h,  exactly,
//     for all inputs.
//
// (DESIGN.md carries the same sentence.) `landColumn` is the height half of the
// column and the ONLY definition of ground level: the terrain octaves, the
// authored pool floors and rims, the pond bowl carve, and the tarn berm. The C++
// mirror in world.cpp reproduces it; the `terrain` gate's pass C1 is what proves
// they still agree, per voxel, on pristine procgen.
//
// WHAT IT IS NOT is "the topmost solid voxel". That would include canopy, ruin
// walls, a grass tuft and the arena deck, it cannot be mirrored cheaply (a tile
// scan in a tick path), and it is not what any of TerrainHeight's ~30 callers
// want — every one of them is asking where the GROUND is so it can stand
// something on it. The arena in particular stays OUT: it is a material override
// in genCellIn, and folding it into `h` would double-apply it and move cave
// depth and tree bases under its footprint.
//
// COST DISCIPLINE. This is now ~25 hash3 (two octaves, one pond tile, one pond
// centre, up to four neighbour tiles) and it is called from CPU paths that run
// at O(1) per frame — spawn placement, fixture anchoring, a mob probe. It must
// never be called in a per-voxel loop on either side.
// ---- RUIN SITES: decided in the COLUMN half, so the building gets a pad ----
//
// A ruin used to be stamped in the CELL half at `baseHeight(centre)` — the raw
// five-octave ladder, with the pond bowl, the authored pool floors and the tarn
// berm all missing from it — and with no gate at all on how steep the ground
// under it was. On a hillside that left one wall floating a metre in the air
// and buried the opposite one; beside a tarn it put the floor under the water
// table. Both are the same bug: the site was decided against a height that is
// not the height the world is actually built at.
//
// So the decision moves here, next to the ponds, and the site FLATTENS the
// ground it stands on the way a foundation does — the terrain yields to the
// building rather than the other way round. Three pieces:
//
//   * `ruinTileAt` is the CHEAP half: one tile hash and the same jittered
//     footprint the cell half always used. It costs one hash3 and knows nothing
//     about height, which is what lets the tree and ground-cover rules ask "is
//     this column a ruin floor?" per candidate without paying for a pad.
//   * `ruinPad` is the EXPENSIVE half: four column heights at the footprint
//     corners, AFTER pond and pool composition. Median for the pad height,
//     spread for the refusal. Only columns within `worldgen.ruinPadMargin` of
//     the footprint ever evaluate it, which is ~1.5% of the world.
//   * `landColumn` blends the pad out into the terrain over that margin.
//
// The footprint is always strictly inside its own tile (margin 32, width 56,
// jitter <= 136, so rx - tx*256 is in [32, 167] and rx + 56 <= 223 < 256), so a
// column only ever has to look at ITS OWN tile — which stays true as long as
// the pad margin is under 32, and LoadTuning clamps it there.


struct LandCol {
  h           : i32,         // GROUND. The contract above.
  sed         : i32,         // loose wedge thickness INSIDE h, 0 where overridden
  pond        : i32,         // disc-pond water surface Y, or -1
  pw          : vec2<i32>,   // pondAt's (bowl floor, surface)
  fluid       : u32,         // standing fluid material at this column
  fluidTop    : i32,         // its surface Y, or -1
  inPoolFloor : bool,
  inRim       : bool,
  near        : Shore,       // nearest disc OUTSIDE this column, or none
};

// MIRROR-BEGIN landheight
// THE GROUND BEFORE THE RUINS. Split out of landColumn because `ruinPad` has to
// sample four of these at the footprint corners, and a landColumn that calls
// itself is not a function. Everything the height contract is made of lives
// here EXCEPT the pad, which is the one override that needs to know about
// columns other than its own.
fn landColumnBare(x : i32, z : i32, seed : u32) -> LandCol {
  var L : LandCol;
  L.pond = -1;
  L.pw = vec2<i32>(-1, -1);
  L.fluid = MAT_AIR;
  L.fluidTop = -1;
  L.near.onShore = false; L.near.past = 0; L.near.surf = -1;
  // The fluid lab's flat slab — the same guard genColumn takes below, taken
  // here as well so World::TerrainHeight sees the slab through the contract
  // rather than through a second copy of the constant.
  if (T.labMode != 0u) {
    L.h = LAB_SLAB_Y;
    L.sed = 0;
    L.inPoolFloor = true;
    L.inRim = true;
    return L;
  }
  // THE SEDIMENT WEDGE IS DECIDED BEFORE THE HEIGHT IS COMPOSED, which is why
  // the disc tests below run before anything is added to `bed` rather than
  // interleaved with it as they used to be.
  //
  // `land.h` is `bed + land.sed` and every authored override here either
  // REPLACES the height (a pool floor, a bowl carve) or LIFTS it (a rim, a
  // berm). A lift is a deliberate STEP against the neighbouring column, and a
  // step is exactly where a powder wedge avalanches — so the wedge has to be
  // gone from those columns, and gone from `h` too, not merely relabelled.
  //
  // AND IT HAS TO RAMP OUT, not switch off. Zeroing 24 voxels of sediment at
  // the edge of a pond band builds a 24-voxel cliff there, which is worse than
  // the thing it was avoiding. Measured: this block as a hard switch left 108
  // chunks awake at tick 120 against 7 with the wedge disabled entirely, all of
  // them tarn banks and the rock under them.
  let land = landAt(x, z, seed);
  let bed = land.h - land.sed;

  // ---- authored origin-area set pieces (absolute world coords) ----
  // Halved in the third scale pass (radii 136/64/48 -> 68/32/24): a swimmable
  // ~8.5 m lake, a 4 m oil pond, a 3 m lava pool. Depths kept — halving depth
  // too would leave water too shallow to submerge in. Floor/surface heights
  // anchor to POOL_Y, and they sit outside the spawn clearing so they don't
  // disturb the fixtures.
  // SIZES scale with the voxel, CENTRES do not, and the split is deliberate.
  // A radius and a depth are lengths: unscaled, the lake would be 3.4 m across
  // at 20 voxels/m, and poolY would sit under terrain that HAD scaled — the
  // pools would be buried, which is the loudest possible way for a voxel-size
  // experiment to go wrong. The centres are POSITIONS in an origin-area content
  // region whose other inhabitants — the selftest fixture columns at (60,60),
  // (100,100), (140,140) — are absolute literals in a dozen files. Scaling one
  // and not the other would pull the set pieces off the fixtures. So the whole
  // origin region keeps its coordinates and simply occupies less ground at a
  // finer voxel; everything in it stays in the same place relative to
  // everything else.
  // THE POOL FLOOR IS RELATIVE TO THE HOME PLAIN, not an absolute Y, and that
  // is the line the datum move would otherwise have broken worst. It was a bare
  // `vlen(44)` back when the terrain band was y32..y86; with the datum at y200
  // the same literal put a 15 m crater with vertical walls at (420,420),
  // reported by the `terrain` gate as a 143-voxel adjacent step and by the page
  // table as 58 lost voxels' worth of matter avalanching down the inside of it.
  //
  // 15 below the plain, with a rim forced 26 above the floor, reproduces the
  // relationship the old numbers had against the old band (floor 15 under the
  // mean, rim 11 over it) at any datum.
  let poolY = TUNE_SPAWN_PLAIN_Y - vlen(15);
  // Water lake at (420,420), ~8.5 m across
  let pdx = x - 420; let pdz = z - 420;
  let pd2 = pdx * pdx + pdz * pdz;
  let pR = vlen(68); let pRim = vlen(80);
  L.inPoolFloor = pd2 < pR * pR;
  L.inRim = pd2 < pRim * pRim;

  // ---- disc ponds, queried before the height is composed ----
  // pondInfo's keep-out list excludes the pool areas, so a disc never overlaps
  // a rim; the fluidTop<0 check below is belt-and-braces. `pondNear` is the one
  // scan that serves BOTH the berm and the marsh fringe (genColumn narrows the
  // same answer), and it is skipped inside a disc or an authored rim, where
  // there is nothing outside to be near.
  L.pw = pondAt(x, z, seed);
  if (L.pw.y < 0 && !L.inRim) { L.near = pondNear(x, z, seed); }

  // ---- the wedge, after everything that has to suppress it ----
  // The pond band ramps rather than switches, over the same width `pondNear`
  // scans, so the wedge thins to nothing as it reaches the water instead of
  // ending in a wall of loose gravel above a bowl full of sand.
  var sed = land.sed;
  if (L.inRim || L.pw.y >= 0) {
    sed = 0;
  } else if (L.near.onShore) {
    let band = max(max(TUNE_SHORE_BAND, TUNE_POND_BERM_WIDTH), 1);
    sed = (sed * min(L.near.past, band)) / band;
  }
  var h = bed + sed;

  if (pd2 < pR * pR) {
    h = poolY;
    L.fluid = M_WATER; L.fluidTop = poolY + vlen(24);
  } else if (pd2 < pRim * pRim) {
    h = max(h, poolY + vlen(26));    // containment rim
  }

  // ---- carve the bowl inside a disc, raise the berm outside ----
  if (L.pw.y >= 0) {
    L.pond = L.pw.y;
    // THE BOWL REPLACES THE TERRAIN, it does not merely cut into it, and that
    // is the difference between a bounded bed and an avalanche. As `min(h,
    // floor)` the bowl described the floor only where the natural ground was
    // higher; wherever the hillside or the grain octave dipped below it, the
    // bed was raw terrain again — at whatever slope the noise happened to have
    // — and genCellIn lays SAND on the top three voxels of it. Powder on
    // arbitrary ground is the rule-2 failure that reports itself as `ca-skip`
    // finding the world never quiet, and as page faults when the grains slide
    // into a chunk that is still a sentinel.
    //
    // Assigned, the floor is exactly pondAt's parabola, whose steepest point is
    // its rim at 2*(pondDepth - pondDepthRim)/r — a number LoadTuning already
    // bounds under the angle of repose. So the bed is safe by construction at
    // every seed and every radius, the way the authored pool floors have always
    // been (`h = poolY`).
    //
    // On sloping ground this fills the downhill half as well as cutting the
    // uphill one, which is what a dammed tarn IS; pondInfo's radius-aware gate
    // above is what keeps the fill from becoming a wall.
    h = L.pw.x;
    if (L.fluidTop < 0) { L.fluid = M_WATER; L.fluidTop = L.pw.y; }
  } else if (!L.inRim && L.near.onShore &&
             L.near.past < TUNE_POND_BERM_WIDTH) {
    h = bermLift(h, L.near.surf, L.near.past);
  }
  // THE SEA (P4): ground under the map's sea level is under water. One
  // global plane (RESEARCH_worldgen 6.5's option (a)), which is what lets
  // the ocean ring past the painted map be water without a tile scheme.
  if (h < seaLevelY() && L.fluidTop < 0) { L.fluid = M_WATER; L.fluidTop = seaLevelY(); }
  L.h = h;
  L.sed = sed;
  return L;
}


// The height contract's public face: the bare column with the ruin pad blended
// into it. Everything else in this file and in World::TerrainHeight goes
// through here.
// The pad under an authored site (P5): inside the footprint the ground IS
// the height at the site's centre (exact, so a stamped floor is flat), and
// over `margin` columns past it the terrain ramps back. Spelled identically
// in world.cpp; the site readers it calls live outside the mirror.
fn sitePadAt(x : i32, z : i32, h : i32, seed : u32) -> i32 {
  let sid = wmSiteAt(x, z);
  if (sid == 0u) { return h; }
  let sx = wmSiteI(sid, WM_S_X);
  let sz = wmSiteI(sid, WM_S_Z);
  let r = wmSiteI(sid, WM_S_RADIUS);
  let margin = max(wmSiteI(sid, WM_S_PAD_MARGIN), 1);
  let d = max(max(abs(x - sx), abs(z - sz)) - r, 0);
  if (d >= margin) { return h; }
  let padY = landColumnBare(sx, sz, seed).h;
  let w = ((margin - d) * 256) / margin;
  return h + (((padY - h) * w) >> 8);
}

fn landColumn(x : i32, z : i32, seed : u32) -> LandCol {
  var L = landColumnBare(x, z, seed);
  let hp = sitePadAt(x, z, L.h, seed);
  if (hp != L.h) {
    // Cut-and-fill under a building: the loose wedge goes with it, for the
    // reason the pond-bank block gives (powder under a stone floor creeps).
    L.h = hp;
    L.sed = 0;
  }
  return L;
}
// MIRROR-END landheight

// The contract, as a function. genColumn calls landColumn directly (it needs the
// rest of the struct); this is the entry point for anything that only wants the
// ground — and it is what World::TerrainHeight mirrors.
fn colHeightAt(x : i32, z : i32, seed : u32) -> i32 {
  return landColumn(x, z, seed).h;
}

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
    lab.sed = 0;
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
    lab.plant.mat = MAT_AIR;
    lab.plant.base = 0;
    lab.plant.top = -1;
    return lab;
  }
  // THE GROUND, and everything derived from it, in one call. This is the same
  // `h` World::TerrainHeight returns — that equality is the whole point of the
  // split (see the height contract above landColumn).
  let L = landColumn(x, z, seed);
  let h = L.h;
  let biome = biomeAt(x, z, seed);

  // ---- the marsh fringe: landColumn's berm scan, narrowed ----
  // The scan itself (`L.near`) has already happened, once, for the berm — so
  // this costs comparisons and no hashes at all, where it used to cost a
  // 24-sample pondSurface for every shore column.
  //
  // Suppressed wherever the pond block itself is suppressed: never inside the
  // authored pool rims (the lava and oil pools must not grow weeds — the same
  // reason the pond-life block gates on `pond >= 0`), never on a fixture pad or
  // in the desert, and never above the treeline, so a shore is always a shore
  // and never a marsh growing out of a snowfield.
  var shore : Shore;
  shore.onShore = false; shore.past = 0; shore.surf = -1;
  if (L.near.onShore && L.near.past < TUNE_SHORE_BAND &&
      wmFlag(biome, WM_BF_GROUND_FLORA) && h < TREELINE) {
    shore = L.near;
    // A column whose ground stands well above the waterline is a BLUFF, not a
    // shore. This is the single most load-bearing test in the feature, and it
    // is a HEIGHT test rather than a second radius on purpose: a band defined
    // by radius alone paints marsh up whatever hillside happens to abut the
    // disc, which is exactly the artifact that gives away that the fringe is a
    // radius and not a wetness.
    //
    // Cutting on height instead makes the marsh follow the LOW ground around
    // the disc, so a pond in rolling terrain gets reed beds in its shallow bays
    // and dry bank on its steep sides — which is what a real pond does, and it
    // costs one comparison. `shoreLift` is the knob and it moves coverage a
    // LOT, so it is worth having as its own parameter rather than derived from
    // the band width. NOTE the interaction with the berm: `h` here is the
    // BERMED ground, so shoreLift must stay comfortably above `pondBerm` or the
    // berm suppresses the very fringe it is supposed to stand behind.
    if (h > shore.surf + TUNE_SHORE_LIFT) {
      shore.onShore = false;
    }
  }

  var col : Col;
  col.h = h;
  col.sed = L.sed;
  col.biome = biome;
  col.pond = L.pond;
  col.pw = L.pw;
  col.fluid = L.fluid;
  col.fluidTop = L.fluidTop;
  col.inPoolFloor = L.inPoolFloor;
  col.inRim = L.inRim;
  col.shore = shore;
  col.plant = plantColumnAt(x, z, seed, biome);
  return col;
}

// ---- THE CELL HALF: everything that actually depends on y -----------------
//
// Unpacks the column into exactly the local names the body below has always
// used, so the body is unchanged line for line. Every one of these is read-only
// from here down — the only thing this half writes is `mat`.
//
// THE TWO BIG HOISTS travel as separate POINTER parameters rather than inside `Col`,
// and both facts are deliberate. Cave bands and tree candidates are pure
// functions of (x, z, seed) like everything in Col, but unlike anything in Col
// they are BIG — a nine-tree candidate set is ~300 bytes — so folding them in
// would make the per-cell `col` copy 7x heavier, and the far cascade path
// (which evaluates one isolated cell and has no column to amortize over) would
// pay for candidates it never reuses. By pointer, because of the trap in
// CLAUDE.md's wind notes: a dynamic index into a BY-VALUE struct spills the
// whole struct to scratch, and `trees.t[i]` is exactly that shape.
//
// The `*Valid` flags are not an optimization switch — they select between two
// spellings of the SAME function. With them false this calls caveAt / treeAt,
// which are literally caveBands+caveIn and treeCandsInto+treeFromCands
// composed. Any divergence would be a bug in that composition, and the world
// hash is what proves there is none.
//
// `poi` is the THIRD hoist and needs no such flag: it is a pure function of the
// seed with no fallback spelling at all, so every caller passes the same two
// numbers and the only question is how often it bothered to compute them. See
// the Poi struct.
fn genCellIn(col : Col,
             cave : ptr<function, CaveBands>, caveValid : bool,
             trees : ptr<function, TreeCands>, treeValid : bool,
             stalk : Flower,
             x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let h = col.h;
  let sed = col.sed;
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
    // SNOW IS A POWDER, and that is why the rim keep-out is `!inRim` and not
    // merely `!inPoolFloor`. Every other feature already stops at the authored
    // rims — trees, shore life, caves — and snow was the one that did not,
    // which was invisible for as long as no ridge happened to stand next to a
    // pool. Put one there and the chain is: snow generates on the rim ring,
    // slides down the ring's inner face onto the lava surface two voxels below
    // it, and `snow + tag:hot -> water` lights a front that
    // `water + tag:hot -> steam` and `steam -> water` then sustain forever.
    // That is a rule-2 failure (CLAUDE.md), and it does not report itself here:
    // it reports itself as `ca-skip` finding the world never reaches a quiet
    // tick, three gates away, with no clue as to why.
    if (!inRim && h >= TREELINE && y > h - 2) {
      mat = M_SNOW;                        // snow caps on the high hills
    } else if (inPoolFloor) {
      mat = M_STONE;
    } else if (submerged && y > h - 3) {
      mat = M_SAND;                        // sandy pond bed
    } else if (wmFlag(biome, WM_BF_SAND_CAP) && y > h - 4) {
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
    } else if (y > h - i32(wmBiome(biome, WM_B_SKIN_DEPTH))) {
      // The biome's ground skin (assets/biomes/<name>.json cover.skin /
      // skinDepth): grass on the forest floor, snow on the tundra, mud in the
      // marsh. A SOLID, for the reason the sediment note below gives -- a
      // powder skin on a slope avalanches out from under itself.
      mat = wmBiome(biome, WM_B_SKIN);
    } else if (y > h - sed) {
      // NOT ON A FIXTURE PAD. The pad keeps its authored loose SAND cap on
      // purpose ("avalanches into repose piles"), but the four voxels under it
      // have to stay solid: `settle-back` drops a body four voxels above the pad
      // and waits for it to sleep, and a body resting on a stack that can creep
      // all the way down never does. This is a MATERIAL-only exclusion — `h` is
      // untouched, so World::TerrainHeight needs no matching branch and the
      // height contract is unaffected.
      // ---- THE SEDIMENT WEDGE: topsoil over gravel over bedrock ----
      //
      // This is the block whose earlier form was a bug, and the difference is
      // one gate. The old version laid a CONSTANT 3-4 voxel dirt shell under
      // the grass everywhere, on ground of any steepness; `dirt` is a POWDER,
      // so on every slope it avalanched out from under its own skin, the whole
      // surface crept, and chunks never slept. It was deleted, and the comment
      // that replaced it said "no loose layer under the grass", full stop.
      //
      // What makes it safe now is that `sed` is SLOPE-GATED in landAt and ramps
      // continuously to zero as the ground approaches the angle of repose. With
      // a solid skin at y == h the topmost grain is at h-1, so it has a free
      // down-diagonal only where a neighbouring column's ground is 3+ voxels
      // lower — which is precisely the ground the gate has already taken the
      // wedge to zero on. `worldgen.sedSlope` is the knob and 0 turns the
      // feature off; `--gate sleep` is what proves the setting.
      //
      // Topsoil first because that is the order a soil profile has, and because
      // gravel is what you want to hit when you dig a valley floor for
      // something that flows.
      if (y > h - 1 - TUNE_SED_TOPSOIL) { mat = wmBiome(biome, WM_B_SUBSOIL); }
      else { mat = M_GRAVEL; }
    } else {
      mat = M_STONE;
    }
    // depth is real now (no bedrock): caves carve the stone body, with lava
    // pooling on deep cavern floors. No caves under the authored pools/rims —
    // a cave breaching a rim column drains the pool through the tunnel system
    // and the world never settles.
    if (mat == M_STONE && !inRim) {
      // The bands, not just the answer: caveFloraAt needs f1/f2/c2 to ask where
      // the floor and the ceiling of THIS cavern are. `caveAt` was exactly
      // `caveIn(caveBands(...))`, so the one-shot arm below is the same
      // arithmetic in the same order and produces the same words.
      var cb : CaveBands;
      if (caveValid) { cb = *cave; } else { cb = caveBands(x, z, h, biome, seed); }
      let cv = caveIn(cb, y);
      if (cv == 1) {
        mat = MAT_AIR;
        // Cave flora fills the carved cell it stands in — no extra voxel, no
        // extra occupancy, nothing new for the CA to look at, and everything it
        // places is inert (rule 2).
        mat = caveFloraAt(cb, biome, x, y, z, seed);
      }
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
    // solid wall. Chance and material are the water preset's (shore.mossChance
    // / mossMaterial); a biome with no water rows reads 0 and grows none.
    if (mat == M_STONE && y == h && shore.onShore) {
      let wp = wmWaterOf(biome);
      let mossMat = wmWater(wp, WM_W_MOSS_MAT);
      if (mossMat != 0u &&
          rollChance(hash3(seed ^ 0x4D05u, bitcast<u32>(x), bitcast<u32>(z)),
                     wmWater(wp, WM_W_MOSS_CHANCE))) {
        mat = mossMat;
      }
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
  //
  // WHICH plants, at WHAT depth, HOW tall: the water preset's aquatic bands
  // (assets/water/<name>.json aquatic.emergent / floating / submerged), read
  // from the worldMap table -- the pond's biome names the preset (P-E interim,
  // see wmWaterOf). Depths are voxels of water over the bed, heights cells
  // above the bed. A band with chance 0, or a biome with no water rows, rolls
  // nothing (rollChance).
  if (mat == M_WATER && pond >= 0) {
    let wp = wmWaterOf(biome);
    let bed = min(h, pw.x);          // the carved bowl floor at this column
    let depth = pond - bed;          // water column height in voxels
    let above = pond - y;            // how far under the surface this cell is
    let hLily = hash3(seed ^ 0x71A9u, bitcast<u32>(x), bitcast<u32>(z));
    let hReed = hash3(seed ^ 0x2E3Du, bitcast<u32>(x), bitcast<u32>(z));
    let hKelp = hash3(seed ^ 0xC5B1u, bitcast<u32>(x), bitcast<u32>(z));

    // FLOATING (lilypads): a single cell ON the surface. Needs enough water
    // under it that a pad reads as floating rather than as lying on mud --
    // the band's minDepth -- and stops past maxDepth.
    if (y == pond && depth >= i32(wmWater(wp, WM_W_FLOATING_MIN_DEPTH)) &&
        depth <= i32(wmWater(wp, WM_W_FLOATING_MAX_DEPTH)) &&
        rollChance(hLily, wmWater(wp, WM_W_FLOATING_CHANCE))) {
      mat = wmWater(wp, WM_W_FLOATING_MAT);
    } else if (depth >= i32(wmWater(wp, WM_W_EMERGENT_MIN_DEPTH)) &&
               depth <= i32(wmWater(wp, WM_W_EMERGENT_MAX_DEPTH)) && above >= 0 &&
               y - bed < i32(wmWater(wp, WM_W_EMERGENT_HEIGHT)) &&
               rollChance(hReed, wmWater(wp, WM_W_EMERGENT_CHANCE))) {
      // EMERGENT (reeds): in the SHALLOW MARGIN only — a narrow depth band, so
      // they form a fringe around the shore rather than filling the bowl. They
      // grow from the bed and break the surface, which is what makes them read
      // as reeds rather than as underwater grass, so the height test is
      // against the BED, not against the waterline.
      mat = wmWater(wp, WM_W_EMERGENT_MAT);
    } else if (depth > i32(wmWater(wp, WM_W_SUBMERGED_MIN_DEPTH)) &&
               above > i32(wmWater(wp, WM_W_SUBMERGED_CLEARANCE)) &&
               y - bed < i32(wmWater(wp, WM_W_SUBMERGED_HEIGHT)) &&
               rollChance(hKelp, wmWater(wp, WM_W_SUBMERGED_CHANCE))) {
      // SUBMERGED (kelp): fully under, in the DEEP MIDDLE only (a minDepth
      // past the emergent band's maxDepth keeps the two from interleaving).
      // `above > clearance` keeps a clear margin below the surface so kelp
      // never pokes through — that margin is the difference between kelp and
      // a reed. This is the plant that gives the submerged view its vertical
      // structure for the light shafts to cut across.
      mat = wmWater(wp, WM_W_SUBMERGED_MAT);
    }
  }
  // Above the waterline over a pond: the emergent half of the reeds, and the
  // lily blossoms that sit proud of their pads. Both are placed in AIR cells,
  // so they are the same features as the water-cell block above continued
  // upward — same hashes, same column tests, so a reed is one continuous stalk
  // through the surface rather than two unrelated halves.
  if (mat == MAT_AIR && pond >= 0 && y > pond) {
    let wp = wmWaterOf(biome);
    let bed = min(h, pw.x);
    let depth = pond - bed;
    let hLily = hash3(seed ^ 0x71A9u, bitcast<u32>(x), bitcast<u32>(z));
    let hReed = hash3(seed ^ 0x2E3Du, bitcast<u32>(x), bitcast<u32>(z));
    if (depth >= i32(wmWater(wp, WM_W_EMERGENT_MIN_DEPTH)) &&
        depth <= i32(wmWater(wp, WM_W_EMERGENT_MAX_DEPTH)) &&
        y - bed < i32(wmWater(wp, WM_W_EMERGENT_HEIGHT)) &&
        rollChance(hReed, wmWater(wp, WM_W_EMERGENT_CHANCE))) {
      mat = wmWater(wp, WM_W_EMERGENT_MAT);
    } else if (y == pond + 1 &&
               depth >= i32(wmWater(wp, WM_W_FLOATING_MIN_DEPTH)) &&
               depth <= i32(wmWater(wp, WM_W_FLOATING_MAX_DEPTH)) &&
               rollChance(hLily, wmWater(wp, WM_W_FLOATING_CHANCE)) &&
               rollChance(hLily >> 9u, wmWater(wp, WM_W_FLOATING_FLOWER_CHANCE))) {
      // Blossom on a minority of pads. Gated on the SAME pad roll, so a flower
      // can only ever appear on a cell that actually grew a pad under it. The
      // packer zeroes the flower chance when the preset names no flower.
      mat = wmWater(wp, WM_W_FLOATING_FLOWER);
    }
  }

  // ---- surface cover: trees, then ground flora ----
  // Only above ground and out of the water, and never inside the authored rims
  // (a tree rooted on a pool rim would drop leaves into the pool).
  if (mat == MAT_AIR && !inRim && y > h && h < TREELINE && pond < 0) {
    var tm = MAT_AIR;
    if (treeValid) { tm = treeFromCands(trees, y); }
    else { tm = treeAt(x, y, z, seed); }
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
  // THE SPECIES ARE THE WATER PRESET'S (assets/water/<name>.json
  // shore.plants[], packed as WM_P_* rows; the biome names the preset, see
  // wmWaterOf). Rows are rolled IN AUTHORED ORDER and the first hit wins,
  // exactly like the biome cover stack, so the author puts the water-hugging
  // species (cattail: small reach, tall) first and the ground layer that
  // covers the whole band (marsh grass: full reach, dense) LAST, filling
  // whatever the taller rows did not claim -- that ordering is what makes the
  // band read as a gradient from the water rather than as a mixed salad.
  //
  // ONE DISTINCT HASH SALT PER ROW, never bit-slices of one hash. The
  // pond-life block above documents why at length — slices of a single hash
  // share entropy, so a column that grew one plant is far likelier than chance
  // to grow another, and the scattered planting collapses into a wall. The
  // cost is one hash per authored row until a hit, on shore columns only.
  //
  // Per row: `reach` is how far past the waterline (shore.past, voxels) it
  // still grows; `height` is the stalk, jittered per column by +-(H/6, at
  // least 1) for stalks of 3+ so a bed of stalks does not read as a fence --
  // the cover stack's rule, widened for a 2 m cattail; `head`, when named,
  // caps the top max(1, H/8) cells (two on a cattail, one on anything short).
  // The head is not its own roll: it is part of the same plant, so gating it
  // on the SAME hash is what keeps a head from floating over no stalk.
  // worldmap.cpp's MaxPlantH includes the jitter, so the sky-skip and far
  // blocker ceilings cover the tallest column a row can produce.
  if (mat == MAT_AIR && shore.onShore && y > h) {
    let up = y - h;                  // voxels above this column's ground
    let wp = wmWaterOf(biome);
    let nRows = wmWater(wp, WM_W_SHORE_COUNT);
    for (var i = 0u; i < nRows; i++) {
      if (shore.past > i32(wmShore(wp, i, WM_P_REACH))) { continue; }
      let hRow = hash3(seed ^ (0x9C41u + i * 0x9E37u), bitcast<u32>(x), bitcast<u32>(z));
      if (!rollChance(hRow, wmShore(wp, i, WM_P_CHANCE))) { continue; }
      let base = i32(wmShore(wp, i, WM_P_HEIGHT));
      let amp = select(0, max(1, base / 6), base >= 3);
      let hgt = max(1, base + i32((hRow >> 5u) % u32(2 * amp + 1)) - amp);
      // This row claimed the column: a cell above its stalk is air, never a
      // later row's -- the same `break` the cover stack takes, so two rows
      // never stack into one plant.
      if (up > hgt) { break; }
      let head = wmShore(wp, i, WM_P_HEAD);
      let headCells = max(1, hgt / 8);
      mat = select(wmShore(wp, i, WM_P_MAT), head, head != 0u && up > hgt - headCells);
      break;
    }
  }

  if (mat == MAT_AIR && col.plant.mat != MAT_AIR && y > col.plant.base &&
      y <= col.plant.top) {
    mat = col.plant.mat;
  }


  if (mat == MAT_AIR && y == h + 1 &&       !inRim && pond < 0 && h < TREELINE &&
      wmFlag(biome, WM_BF_GROUND_FLORA) && !shore.onShore && !siteKeepOut(x, z)) {
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
      // Not around a shrub: a mushroom ring wants a bole and leaf mould, and
      // `shade == 0` is exactly the species that have neither.
      let atBase = ug.trunkD2 > 9 && ug.trunkD2 < ringOut * ringOut &&
                   ug.shade > 0;
      if (atBase && (hShroom % UG_SHROOM_BASE_CHANCE) == 0u) {
        // Red fly-agaric is the rarer, showier one; the pale toadstool is the
        // common ring. Gated on the SAME roll that placed a mushroom at all, so
        // this only ever picks WHICH mushroom, never adds more of them.
        mat = select(M_TOADSTOOL, M_MUSHROOM, ((hShroom >> 13u) % 4u) == 0u);
      } else if ((hBram % UG_BRAMBLE_CHANCE) == 0u && ug.cover < UG_COVER_DEEP) {
        // (Ferns used to be next in this chain, one cell each. They are TILE
        // plants now — plantColumnAt — and have already claimed their cells
        // above, on the same patch mask.)
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
  // `!ruinFloor` for the same reason the base block has it, and it has to be
  // repeated here rather than inferred: this branch RE-DERIVES the species from
  // flowerAt instead of reading the base cell, so a guard the base block took
  // and this one did not would grow a headless stalk out of a stone floor.
  if (mat == MAT_AIR && y > h + 1 && y <= h + FLOWER_MAX_H && !inRim && pond < 0 &&
      h < TREELINE && wmFlag(biome, WM_BF_GROUND_FLORA) && !shore.onShore &&
      !siteKeepOut(x, z)) {
    let fl = flowerAt(x, z, seed, UG_COVER_EDGE);
    if (fl.mat != MAT_AIR && (y - h) <= fl.height) {
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
  if (mat == MAT_AIR && wmFlag(biome, WM_BF_CACTI) && !inRim && y > h && pond < 0 &&
      h < TREELINE) {
    let cm = cactusAt(x, y, z, seed);
    if (cm != MAT_AIR) { mat = cm; }
  }

  // ---- THE BIOME'S OWN COVER STACK (assets/biomes/<name>.json cover.plants) --
  // This replaces the hand-written desert (tussock/scrub) and pine-highland
  // (heath) floors, and is what gives every biome the tuner shows a floor of
  // its own without a shader edit. Same shape as those blocks: cells above
  // the surface, only where the canopy-inverted layer above left air, a patch
  // mask so the plants grow in stands rather than as uniform static (an even
  // sprinkle at any rate reads as noise, never as a place).
  //
  // Rows are rolled IN ORDER and the first hit wins, so an author puts the
  // common ground layer last, the way the shore set rolls marsh grass last.
  // ONE HASH SALT PER ROW, never bit-slices of one hash -- the pond-life
  // block documents why at length; two rows sharing entropy would co-locate.
  // The patch field is sampled at a per-row offset so the rows' lattices do
  // not line up at cell corners (the reason the wildflower species field is
  // sampled off-lattice), through vnoise2d, never the legacy vnoise.
  //
  // Cost: on surface columns only, one vnoise2d + one hash per AUTHORED row
  // until a hit; a biome with no rows pays one header read. Everything placed
  // is inert (rule 2): the loader resolves names against materials.json and
  // nothing here is a stem/sprout/seed.
  if (mat == MAT_AIR && y > h && !inRim && pond < 0 && h < TREELINE &&
      !siteKeepOut(x, z)) {
    let up = y - h;
    let nRows = wmBiome(biome, WM_B_COVER_COUNT);
    let bThresh = i32(wmBiome(biome, WM_B_PATCH_THRESH));
    let pLog2 = wmBiome(biome, WM_B_PATCH_LOG2);
    for (var i = 0u; i < nRows; i++) {
      let chance = wmCover(biome, i, WM_C_CHANCE);
      if (chance == 0u) { continue; }
      let hRow = hash3(seed ^ (0xC0E0u + i * 0x9E37u), bitcast<u32>(x), bitcast<u32>(z));
      if ((hRow % chance) != 0u) { continue; }
      // Conditions: altitude band and steepness, per row, like a species'.
      let minY = bitcast<i32>(wmCover(biome, i, WM_C_MIN_Y));
      let maxY = bitcast<i32>(wmCover(biome, i, WM_C_MAX_Y));
      if (minY >= 0 && h < minY) { continue; }
      if (maxY >= 0 && h > maxY) { continue; }
      // `maxSlope` (WM_C_MAX_SLOPE) is packed but NOT enforced yet: Col has
      // no slope (LandCol is inside the height mirror and grows a field in
      // P4, when landform lands). Every authored row is unbounded today.
      // The patch mask: the biome's threshold, raised further by the row's.
      let thresh = max(bThresh, i32(wmCover(biome, i, WM_C_PATCH_THRESH)));
      if (thresh > 0) {
        let off = i32(i) * 613;
        let pm = vnoise2d(x - 617 + off, z + 431 - off, pLog2, seed ^ (0xD5E7u + i)).n >> 6;
        if (pm <= thresh) { continue; }
      }
      // Height: the row's stalk, jittered per column so a bed of stalks all
      // cut to one height does not read as a fence; the head caps the top.
      let base = i32(wmCover(biome, i, WM_C_HEIGHT));
      let hgt = select(base, max(1, base + i32((hRow >> 5u) % 3u) - 1), base >= 3);
      if (up > hgt) { break; }
      let head = wmCover(biome, i, WM_C_HEAD);
      mat = select(wmCover(biome, i, WM_C_MAT), head, head != 0u && up == hgt);
      break;
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
      !siteKeepOut(x, z)) {
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


  // Procedural ruin POIs: one hollow stone building per ~5th tile, placed by
  // tile hash. Building halved with the world: ~3.5 m square, 3 m tall — a
  // hut, not a hall. The 2 m doorway is NOT halved: it has to clear the 1.7 m
  // player, which is exactly the mouse-hole mistake the first cut made.
  //
  // THE SITE IS THE COLUMN'S, NOT A SECOND DERIVATION. This block used to redo
  // the tile hash here and take its floor height from `baseHeight(centre)` — a
  // height nothing else in the world uses, because it predates the pond bowl,
  // the pool floors, the berm and the sediment wedge. `col.ruin` is the site
  // `landColumn` already accepted and already flattened the ground to, so the
  // shell now stands ON the pad by construction rather than by coincidence.
  // See the RUIN SITES block above landColumn.

  // ---- authored sites (P5): a stamp overlays everything above its pad -----
  // One plane read for the common case (no site); on a site's cells, the
  // template's column of runs. Non-air template voxels replace whatever the
  // terrain and cover put here; template air leaves the world alone, so a
  // stamp is a building on the ground, not a box cut out of it.
  {
    let sid = wmSiteAt(x, z);
    if (sid != 0u && y > h - 2) {
      let padY = landColumnBare(wmSiteI(sid, WM_S_X), wmSiteI(sid, WM_S_Z), seed).h;
      let sm = wmStampCell(sid, x, y, z, padY);
      if (sm != MAT_AIR) { mat = sm; }
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
  var cave : CaveBands;
  var trees : TreeCands;
  var noStalk : Flower;
  noStalk.mat = MAT_AIR;
  noStalk.height = -1;   // "not memoized"; see the stalk block in genCellIn
  return genCellIn(genColumn(c.x, c.z, seed), &cave, false, &trees, false,
                   noStalk, c.x, c.y, c.z, seed);
}

// The same, for a caller that ALREADY has the column. The far cascade sampler
// asks for the surface skin at (x, col.h, z) immediately after asking for the
// shape at (x, y, z), and rebuilding the column between the two was the second
// genColumn per cell.
// `poi` comes in from the caller for the same reason `col` does: the far sweep
// is column-major over a level chunk and the anchors are constant over the
// whole DISPATCH, so the kernel builds them once. A caller with nothing to
// amortize over passes `poiAnchors(seed)` and is exactly where it was.
fn genCellCol(col : Col, c : vec3<i32>, seed : u32) -> u32 {
  var cave : CaveBands;
  var trees : TreeCands;
  var noStalk : Flower;
  noStalk.mat = MAT_AIR;
  noStalk.height = -1;   // "not memoized"; see the stalk block in genCellIn
  return genCellIn(col, &cave, false, &trees, false, noStalk,
                   c.x, c.y, c.z, seed);
}

// ---- surfHeightAt IS GONE, and that is the point ---------------------------
//
// It was a fourth height function: a hand-copied re-derivation of `baseHeight +
// the authored pool overrides + the pond carve`, kept in sync with genColumn by
// a comment. It had already drifted — it never took the fluid-lab branch, so the
// far field painted the ORIGINAL hillside under the lab's flat slab. Its one
// consumer, farSurfaceMat below, now takes the COLUMN and reads `col.h`, which
// is the height contract's own definition of ground. Four height functions
// become three (baseHeight -> landColumn.h -> World::TerrainHeight, all one
// chain), and the drift has nowhere left to happen.
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
      if (!t.present) { continue; }
      // The species' own far-field proxy material, out of the atlas: the mid
      // step of its leaf ramp, or ZERO for a species with no foliage worth
      // painting at kilometre range (the bush, the dead tree). Authored, not
      // guessed from a species id.
      let cm = taSpecies(t.sp, TA_S_CANOPY_MAT);
      if (cm == MAT_AIR) { continue; }
      let dx = x - t.wx; let dz = z - t.wz;
      // The crown proxy is the MEASURED crown radius of the baked variants, so
      // the footprint matches the tree that actually grows here.
      if (dx * dx + dz * dz > t.crownR * t.crownR) { continue; }
      // An airy species (birch, eucalypt) spreads its foliage in tufts with sky
      // between them, so a solid disc at range would read as a denser wood than
      // the near field shows. Punch it out in proportion to how much shade the
      // species actually casts -- the same number the forest floor reads.
      if (t.shade < 160) {
        let punch = u32(clamp((160 - t.shade) / 24, 0, 5));
        if (hash3(seed ^ 0x2B17u, bitcast<u32>(x), bitcast<u32>(z)) % 8u < punch) {
          continue;
        }
      }
      if (t.autumn) { return taSpecies(t.sp, TA_S_AUTUMN0 + 1u); }
      return cm;
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
// Both entry points call this same pure function of (col, mat, coords, level,
// seed), so downsampled and pristine regions still agree exactly at their
// boundaries (the invariant the `far downsample` selftest gate protects). `col`
// must be genColumn at (fine.x, fine.z) — that shared requirement is what keeps
// the sieve and the downsample on one code path now that surfHeightAt is gone.
fn farSurfaceMat(col : Col, mat : u32, fine : vec3<i32>, shift : u32,
                 seed : u32) -> u32 {
  let k = materials[mat].klass;
  if (k != CLASS_SOLID && k != CLASS_POWDER) { return mat; }  // fluids keep their ID
  var h = col.h;
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
  // MEADOW COVER IS NOT FLATTENED INTO THE SKIN, and this was tried (LOD-seam
  // pass, 2026-09-04) before being taken out again: painting the surface cell
  // under a tall-grass column with the strand material turned every far meadow
  // the strand palette's dark green while the near meadow, seen at 25 m, reads
  // as the lime skin with thin blades over it -- a harder colour step than the
  // one it was meant to remove. A blade thinner than a cascade cell contributes
  // nothing to the far field (farCellIsSolid drops the plant cell itself); what
  // the near field shows between its blades is the skin, and the skin is what
  // the far field paints. A genuine blend would need a far-only proxy material
  // whose palette is the skin/blade average -- not a cell rule.
  let skin = genCellCol(col, vec3<i32>(fine.x, h, fine.z), seed) & 0xFFFu;
  // hollow ruin interiors can return air at y == h; keep the body mat then
  if (skin == MAT_AIR || materials[skin].klass == CLASS_GAS) { return mat; }
  return skin;
}

// ---- WHAT A CENTRE SAMPLE MAY TURN INTO A CASCADE CELL --------------------
// Shared by the sieve, the downsample and the edit patch, so the three
// producers keep their byte-for-byte agreement (the `far-downsample` gate).
// Gases were always dropped (no media in the far field). MICRO materials are
// now dropped too: a grass tuft or a flower is a mostly-air cell that the
// renderer fills with blades or a sub-voxel model, and the cascade has no such
// path — its centre sample became a SOLID CUBE of the plant's palette, 20 cm at
// level 1 and 25 m at level 8. Measured on screenshot_ground: the meadow past
// the window edge was littered with straw-coloured boulders that were
// tall_grass cells. A blade thinner than any cascade cell contributes nothing
// to the far GEOMETRY; its COLOUR reaches the far field through
// farSurfaceMat's cover flattening instead.
fn farCellIsSolid(mat : u32) -> bool {
  if (mat == MAT_AIR) { return false; }
  let m = materials[mat];
  return m.klass != CLASS_GAS && (m.flags & MATF_MICRO) == 0u;
}

// ---- the conservative "any blocker" bit (13.2.2) --------------------------
//
// `farSurfaceMat` above decides a far cell's COLOUR; this decides whether the
// cell counts as OCCUPIED even when its single centre sample missed. See the
// FAR_BLOCKER_BIT block in common.wgsl for what the flag means and why it has
// to be a pure function of (coords, seed).
//
// THE TOP OF A COLUMN, as the far field sees it: the ground contract, plus the
// two things that stand on top of it and are not in `h`. Kept beside
// `farSurfaceMat` because it makes the SAME three exceptions that function
// makes — standing fluid keeps its id and renders opaque at distance, a ruin
// is a stone box the height contract only pads the ground for, and the arena
// deck is a material override in genCellIn that `col.h` deliberately does not
// know about.
//
// The ruin term is a BOUNDING BOX, not `ruinShellAt`: the shell is hollow and
// a conservative bit is allowed to fill an interior a 3.5 m building could
// never show at cascade range. That box is also the one feature in here that
// the goal line names — "lets thin walls cast shadows in the distance".
//
// TREES ARE DELIBERATELY ABSENT. `treeCanopyAt` is an XZ mask with no height,
// and the only vertical bound available for it is `treeMaxAbove()` (~17.5 m),
// which is WIDER than a level-5 cell — flagging that band would turn every
// forested column into the column of solid cubes 13.2.2 warns about. The
// canopy is already carried at distance by `farSurfaceMat`'s flattening, which
// paints crown colour onto the surface cell at shift >= 5.
fn farColTopFrom(h : i32, fluidTop : i32, x : i32, z : i32, seed : u32) -> i32 {
  var top = max(h, fluidTop);
  // The biome cover stack stands ON the ground and, since the world map's P1,
  // is authored data that can be taller than a far cell: a stalk that pokes
  // into the cell above the flagged ground is a blocker the flag would
  // otherwise miss (the far-fog gate's "flag sits below the material top").
  // Global max, not per-biome: the corner-column callers hold no biome, and
  // the bit is conservative by design -- over-flagging costs nothing.
  top = max(top, h + i32(worldMap[WM_H_MAX_COVER_H]));
  top = max(top, wmSiteTopAt(x, z, seed));   // an authored stamp (P5)
  return top;
}
// The same for a column the caller does not already hold. `landColumn`, not
// `genColumn`: the top needs the height contract and the ruin pad and nothing
// else, and the biome/shore/undergrowth half of a Col is pure cost here.
fn farColTop(x : i32, z : i32, seed : u32) -> i32 {
  let L = landColumn(x, z, seed);
  return farColTopFrom(L.h, L.fluidTop, x, z, seed);
}

// The flag for one level cell. `topC` is `farColTopFrom` at the cell's CENTRE
// column — free at both call sites, because both already hold that column for
// the colour lookup.
//
// THREE BANDS, and the middle one is the only one that costs anything:
//
//   y0 <= topC              the cell's floor is at or below the centre
//                           column's top: it is at or under the surface. Set,
//                           no further samples. (Caves included on purpose.)
//   y0 - topC >= step       more than a whole cell of air under the cell's
//                           floor: clear, no further samples.
//   otherwise               THE SURFACE BAND — exactly ONE cell per column,
//                           the one whose centre sampled air but whose span
//                           straddles the ground. Here, and only here, the
//                           four corner columns are evaluated.
//
// That band is where the missing half of every surface cell lives: the centre
// sample calls a cell solid when `centre <= h`, and `centre` is `y0 + step/2`,
// so a cell holding the ground in its lower half reads as air today. It is
// also where a ridge crest that passes between two cell centres goes.
//
// WHAT IT IS NOT is a supremum over the footprint. Five samples bound the
// footprint exactly at level 1 (4 fine voxels, corners 3 apart) and only
// approximately at level 8 (512 fine voxels): a spire strictly between the
// corners, or more than one cell taller than the centre column, is still lost.
// A tighter bound needs either marching or a stored min/max pyramid, and this
// is an experiment with a kill criterion, not a mipmap.
fn farBlockerBitAt(topC : i32, cc : vec3<i32>, shift : u32, seed : u32) -> u32 {
  let step = 1 << shift;
  let y0 = cc.y << shift;               // the cell's bottom fine voxel
  if (y0 <= topC) { return FAR_BLOCKER_BIT; }
  if (y0 - topC >= step) { return 0u; }
  let x0 = cc.x << shift; let x1 = x0 + step - 1;
  let z0 = cc.z << shift; let z1 = z0 + step - 1;
  var top = topC;
  top = max(top, farColTop(x0, z0, seed));
  top = max(top, farColTop(x1, z0, seed));
  top = max(top, farColTop(x0, z1, seed));
  top = max(top, farColTop(x1, z1, seed));
  return select(0u, FAR_BLOCKER_BIT, y0 <= top);
}

var<workgroup> wgCount : atomic<u32>;
var<workgroup> wgBlock : atomic<u32>;
// Cells in this chunk that can ACT (matCanAct, common.wgsl). Third counter
// rather than a reuse of wgCount because occupancy needs the total and the
// wake needs this one, and they are different questions about the same sweep.
var<workgroup> wgAct : atomic<u32>;
// Sub-chunk occupancy bitmask (common.wgsl SUB-CHUNK OCCUPANCY block):
// [0..1] TOTAL class, [2..3] BLOCKER class. genChunk is a full producer of it
// and not merely of the counts — a conservative all-ones fill here would be
// safe but would give up the whole point, because a generated canopy or meadow
// chunk is inert (matCanAct false), never woken, and so never revisited by
// sim_occupancy's dirty pass. Streamed-in terrain would keep its pessimistic
// mask for as long as the player stayed near it.
var<workgroup> wgSub : array<atomic<u32>, 4>;

fn storeSubOcc(slot : u32, t0 : u32, t1 : u32, b0 : u32, b1 : u32) {
  occupancy[subOccIndex(slot, 0u, 0u)] = t0;
  occupancy[subOccIndex(slot, 0u, 1u)] = t1;
  occupancy[subOccIndex(slot, 1u, 0u)] = b0;
  occupancy[subOccIndex(slot, 1u, 1u)] = b1;
}

// `actIdx` is the caller's position in genList — the index genAct is keyed on.
// It is only read when T.genDeferWake is set, which only the streaming `list`
// entry point ever sees; the dense `main` path passes its own workgroup id and
// never defers.
fn genChunk(slot : u32, li : u32, actIdx : u32) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
    atomicStore(&wgAct, 0u);
    atomicStore(&wgSub[0], 0u);
    atomicStore(&wgSub[1], 0u);
    atomicStore(&wgSub[2], 0u);
    atomicStore(&wgSub[3], 0u);
  }
  workgroupBarrier();

  let sc = vec3<i32>(vec3<u32>(slot % NCHUNK, (slot / NCHUNK) % NCHUNK,
                               slot / (NCHUNK * NCHUNK)));
  let base = slotToWorldChunk(sc, T.origin) * i32(CHUNK);
  var count = 0u;
  var block = 0u;
  var act = 0u;
  var sm0 = 0u; var sm1 = 0u;   // sub-chunk TOTAL class
  var sb0 = 0u; var sb1 = 0u;   // sub-chunk BLOCKER class
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
  var cave : CaveBands;
  var trees : TreeCands;
  // ---- THE THIRD HOIST, and the widest one -----------------------------
  // The spawn deck's and the arena's anchor heights depend on the SEED alone,
  // so unlike the two above they do not even belong to a column: one pair per
  // thread covers all 64 cells it will generate. They used to be two `landAt`
  // ladders inside genCellIn's body, evaluated per CELL. See Poi.
  for (var ci = li; ci < CHUNK * CHUNK; ci += 64u) {
    let lx = ci % CHUNK;
    let lz = ci / CHUNK;
    let wx = base.x + i32(lx);
    let wz = base.z + i32(lz);
    let col = genColumn(wx, wz, T.seed);
    // ---- THE COLUMN PROLOGUE (the other 15/16ths of the saving) ----------
    //
    // Cave bands: only worth having where a cell of THIS chunk can be stone
    // under the surface. A chunk whose base is above the ground has no such
    // cell, and inside an authored rim genCellIn refuses to carve at all — in
    // both cases the six noise samples would be thrown away. That guard is why
    // the hoist lives in genChunk and not in genColumn: genColumn is also the
    // far path's per-cell entry, and computing cave bands there would put this
    // cost on every air column of every cascade fill.
    let caveValid = base.y <= col.h && !col.inRim;
    if (caveValid) { cave = caveBands(wx, wz, col.h, col.biome, T.seed); }
    // Tree candidates: because the answer is what makes the vertical reject a
    // LOCAL ceiling (`trees.top`) instead of a world-wide constant.
    //
    // ---- EXCEPT ABOVE THE WORLD'S TALLEST POSSIBLE TREE ------------------
    // `treeAt` opens with `if (y > treeMaxTop()) { return MAT_AIR; }` — the
    // one-compare world-wide reject — and the hoisted path deliberately did
    // NOT, because `cands.top` is strictly tighter once you have the scan.
    // That reasoning is right per CELL and wrong per COLUMN: a chunk whose
    // LOWEST cell already sits above the treeline plus the tallest species is
    // a chunk where all sixteen cells would take treeAt's branch, and the scan
    // that produces the tighter bound is 25 tile hashes plus a `landAt` and a
    // `pondAt` for every tile that survives the horizontal reject. On a
    // streamed-in vertical plane most chunks are that chunk.
    //
    // Skipping it is EXACT, not conservative: `treeCandsInto` would have
    // admitted only candidates with `vtop <= treeMaxTop()` (that bound is the
    // invariant treeAt already relies on to agree with treeFromCands, and the
    // world hash is what proves it), so every one of them fails
    // `treeFromCands`'s `y > top` test anyway. An empty set produces the same
    // MAT_AIR the full set would.
    if (base.y <= treeMaxTop()) {
      treeCandsInto(&trees, wx, wz, T.seed);
    } else {
      trees.n = 0;
      trees.top = -1048576;   // the same "far below any y" treeCandsInto uses
    }
    // ---- THE SKY EARLY-OUT: a column stack entirely above everything -----
    //
    // On a streamed-in vertical plane most chunks are sky, and a sky cell still
    // walked the whole of genCellIn to conclude nothing: sixteen guards, a
    // tree lookup, the two authored-POI boxes and the ruin box, out of a 708
    // KiB kernel whose body does not fit in the instruction cache. This
    // computes the column's CEILING once and, when the chunk's lowest cell is
    // already above it, stores sixteen zeros — which is exactly the word
    // genCellIn returns for MAT_AIR (`if (mat == MAT_AIR) { return 0u; }`),
    // contributes nothing to `count`/`block`/`act` or to either sub-occupancy
    // mask, and is therefore the same chunk by every observable.
    //
    // THE CEILING IS AN ENUMERATION, NOT AN ESTIMATE, and it is the only part
    // of this that can be wrong. Every block in genCellIn that can leave `mat`
    // non-air above the ground, with the highest y it can reach:
    //
    //   terrain body / grass skin / wet moss   h
    //   standing fluid, and the pond life          fluidTop
    //     placed INTO water cells (kelp, reed,
    //     lilypad — all gated on mat == M_WATER)
    //   pond life above the waterline          max(pond + 1, h + reedHeight)
    //     (`bed` is min(h, pw.x) <= h, so the
    //      reed test y - bed < H bounds by h)
    //   trees                                  trees.top   (exact, per column)
    //   shore fringe                           h + max(cattail + 3,
    //                                                  horsetail + 2)
    //   every y == h + 1 cover block           h + 1
    //   the flower stalk                       h + FLOWER_MAX_H
    //   spawn deck (pillars + slab)            deckY + 3, inside its own box
    //   arena deck / wall / ivy / ramp         arenaY + 24, inside its box
    //     (rampTop interpolates between
    //      baseHeight and arenaY, so arenaY
    //      dominates it)
    //   ruin shell / wall moss / ruin ivy      ruin.y + RUIN_HT, and only
    //                                          when THIS column's site is
    //                                          present (the footprint plus its
    //                                          one-voxel ivy skirt is strictly
    //                                          inside its own tile)
    //
    // CACTI ARE THE ONE THING WITH NO COLUMN-LOCAL BOUND, because a saguaro
    // stands at a NEIGHBOURING tile's ground height and `cactusAt` bounds it
    // against that site's own `base`, not against this column's `h`. Rather
    // than invent a bound for it, desert columns simply never take the
    // early-out — the block is gated on this column's `biome == B_DESERT`, so
    // excluding that biome is sufficient, and the desert is rare.
    //
    // All the h-relative plant reaches collapse into one margin, which is
    // strictly conservative: over-estimating the ceiling only declines a skip.
    // The biome's own cover stack AND its water preset's shore / emergent
    // plants arrive as WM_B_MAX_COVER_H (worldmap.cpp packs the tallest
    // authored row of either, jitter and head included). Since the world
    // map's P1 the cover rows are DATA and can be taller than the fixed term
    // here (a 2 m cattail is 23 voxels with its jitter); a margin that ignored
    // them would skip a chunk whose plants it never wrote.
    let skyMargin = max(FLOWER_MAX_H, i32(wmBiome(col.biome, WM_B_MAX_COVER_H)));
    var colTop = col.h + skyMargin;
    colTop = max(colTop, col.fluidTop);
    colTop = max(colTop, col.pond + 1);
    colTop = max(colTop, trees.top);
    colTop = max(colTop, wmSiteTopAt(wx, wz, T.seed));
    if (!wmFlag(col.biome, WM_BF_CACTI) && base.y > colTop) {
      for (var ly = 0u; ly < CHUNK; ly += 1u) {
        voxStore(voxWordInChunk(slot, lx + ly * CHUNK + lz * CHUNK * CHUNK), 0u);
      }
      continue;
    }

    // The upper-stalk flower answer, once per column instead of once per cell
    // of the FLOWER_MAX_H band. Every term of the guard below is a property of
    // the COLUMN — it is genCellIn's own guard for that block with the two
    // y tests replaced by "does this chunk's 16-cell stack reach the band at
    // all", which is the only part of it that is not column-invariant.
    let stalkValid = !col.inRim && col.pond < 0 &&
                     col.h < TREELINE && wmFlag(col.biome, WM_BF_GROUND_FLORA) &&
                     !col.shore.onShore && !siteKeepOut(wx, wz) &&
                     base.y + i32(CHUNK) > col.h + 1 &&
                     base.y <= col.h + FLOWER_MAX_H;
    var stalk : Flower;
    stalk.mat = MAT_AIR;
    stalk.height = -1;     // the "nobody memoized this" sentinel
    if (stalkValid) { stalk = flowerAt(wx, wz, T.seed, UG_COVER_EDGE); }
    for (var ly = 0u; ly < CHUNK; ly += 1u) {
      let i = lx + ly * CHUNK + lz * CHUNK * CHUNK;
      let w = genCellIn(col, &cave, caveValid, &trees, true, stalk,
                        wx, base.y + i32(ly), wz, T.seed);
      // Chunk-linear: the slot's page resolved once, per §2.1's second entry
      // point. genChunk overwrites the WHOLE chunk, so the CPU materializes
      // every target slot before the dispatch (§3.5c) and this never faults.
      voxStore(voxWordInChunk(slot, i), w);
      let m = w & 0xFFFu;
      if (m != MAT_AIR) {
        count += 1u;
        let md = materials[m];
        // The sub-chunk bit for this cell keys on the SAME chunk-linear index
        // the store above used, so a change to either layout moves both.
        let sbit = subOccBitOfLocalIdx(i);
        let sm = 1u << (sbit & 31u);
        if (sbit < 32u) { sm0 |= sm; } else { sm1 |= sm; }
        if (isRayBlocker(md)) {
          block += 1u;
          if (sbit < 32u) { sb0 |= sm; } else { sb1 |= sm; }
        }
        if (matCanAct(md)) { act += 1u; }
      }
    }
  }
  atomicAdd(&wgCount, count);
  atomicAdd(&wgBlock, block);
  atomicAdd(&wgAct, act);
  atomicOr(&wgSub[0], sm0);
  atomicOr(&wgSub[1], sm1);
  atomicOr(&wgSub[2], sb0);
  atomicOr(&wgSub[3], sb1);
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
    storeSubOcc(slot, atomicLoad(&wgSub[0]), atomicLoad(&wgSub[1]),
                atomicLoad(&wgSub[2]), atomicLoad(&wgSub[3]));
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
    let canAct = atomicLoad(&wgAct) > 0u;
    // ---- DEFERRED WAKE (docs/RESEARCH_streaming_hitch.md R1) -----------
    //
    // Waking here is only safe when the CPU page-table mirror learns the same
    // set in the SAME tick, because PageTable::Materialize has to give the
    // woken chunks' 26-neighbourhoods pages before the CA runs on them — and
    // learning it in the same tick is precisely the 33 ms fence in
    // Stream::FillSlots (a readback fence on one queue waits behind the
    // previous frame's render and the previous ticks' CA).
    //
    // So a window shift asks for the verdict to be PUBLISHED rather than
    // acted on: genAct carries it to a readback the CPU polls, and the CPU
    // sets dirtyIn/dirtyOut itself kWakeLatency ticks later. The clear is NOT
    // optional in that mode — the slot may carry the previous occupant's
    // dirty flag, and leaving it set would dispatch the CA over a plane whose
    // neighbourhood the mirror has not materialized yet.
    if (T.genDeferWake != 0u) {
      genAct[actIdx] = select(0u, 1u, canAct);
      atomicStore(&dirtyIn[slot], 0u);
      atomicStore(&dirtyOut[slot], 0u);
    } else if (canAct) {
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
  genChunk(wg.x, li, wg.x);
}

// Streamed-in chunks: T.genCount slot indices from genList.
@compute @workgroup_size(64)
fn list(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  // Not this module's default: a lost STREAMED plane and a lost WHOLE-WORLD
  // gen are different bugs with different owners (Stream::FillSlots vs
  // SubmitTick's batched worldgen), and the fault record has to say which.
  gPtKernel = PT_K_GENLIST;
  if (wg.x >= T.genCount) { return; }
  genChunk(genList[wg.x], li, wg.x);
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
  gPtKernel = PT_K_PAGEFILL;
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
// Cascade EDIT PATCHES (world.h kFarPatch*, src/sim/faredits.h). Header pairs
// (payload offset, count) indexed by DISPATCH entry, then the payload:
// (mat << 12) | cellIndexInLevelChunk. See the patch block in `far`.
@group(1) @binding(5) var<storage, read> farPatch : array<u32>;

var<workgroup> wgFarCount : atomic<u32>;
// Non-air cells contributed by the patch pass. Kept apart from wgFarCount
// because the two are answers to different questions and only their SUM is
// safe to publish (see the farOcc note at the bottom of `far`).
var<workgroup> wgFarPatchNZ : atomic<u32>;
// One plus the chunk-local row of the highest non-empty cell, sweep and patch
// together (common.wgsl FAR_OCC_TOP_SHIFT: the far readers skip the air above
// it). 0 when the chunk is empty.
var<workgroup> wgFarTop : atomic<u32>;

@compute @workgroup_size(64)
fn far(@builtin(workgroup_id) wg : vec3<u32>,
       @builtin(local_invocation_index) li : u32) {
  if (wg.x >= T.farCount) { return; }
  if (li == 0u) {
    atomicStore(&wgFarCount, 0u);
    atomicStore(&wgFarPatchNZ, 0u);
    atomicStore(&wgFarTop, 0u);
  }
  workgroupBarrier();

  let packed = farList[wg.x];
  let level = (packed >> FAR_SLOT_SHIFT) + 1u;   // 1-based
  let slot = packed & FAR_SLOT_MASK;
  let sc = vec3<i32>(vec3<u32>(slot % FAR_NCHUNK, (slot / FAR_NCHUNK) % FAR_NCHUNK,
                               slot / (FAR_NCHUNK * FAR_NCHUNK)));
  // base LEVEL-cell coord of this level chunk (origins are level-chunk units)
  let base = farSlotToChunk(sc, F.origins[level - 1u].xyz) * i32(CHUNK);
  let shift = farCellShift(level);   // fine voxels per cell, as a shift
  // The authored-POI anchors are constant over the whole dispatch (see Poi), so
  // they are built here rather than inside genCellIn's per-cell body.

  // ---- THE SWEEP, COLUMN-MAJOR (the far half of the genColumn/genCellIn split)
  //
  // A level chunk is 16^3 = 4096 cells but only 256 DISTINCT (x, z) columns:
  // the sample point is `(cc << shift) + half`, so fine.x and fine.z depend on
  // cc.x and cc.z alone. The flat form below rebuilt the whole column — two
  // terrain octaves, a biome field, a pond tile, a pond-neighbour scan — for
  // every one of the 4096, and then farSurfaceMat rebuilt it a SECOND time for
  // the skin lookup. Sixteen columns' worth of work per column, twice.
  //
  // WHY NOT WORKGROUP MEMORY (the obvious answer): 256 Cols is 12 KiB, which
  // fits the 16 KiB floor but costs occupancy on a kernel whose whole job is
  // bandwidth. The assignment below gets the same 16x with none of it. Thread
  // `li` owns ONE z row and FOUR consecutive x — exactly the four cells that
  // pack into one farVox word — so it holds four Cols, reuses each for the 16 y
  // above it, and still writes whole 32-bit words with no byte-granular
  // read-modify-write and no cross-thread sharing at all.
  //
  // Same 64 cells per thread, same total, same values. Only the ORDER of the
  // stores changes: 16 strided words instead of 16 consecutive ones.
  var count = 0u;
  let zi = li / 4u;              // this thread's z within the level chunk
  let x0 = (li % 4u) * 4u;       // and the first of its four x
  var cols : array<Col, 4>;
  // Column tops for the blocker bit, hoisted out of the y loop for the same
  // reason the columns themselves are: they depend on (x, z) alone.
  var tops : array<i32, 4>;
  for (var b = 0u; b < 4u; b++) {
    let cc = base + vec3<i32>(i32(x0 + b), 0, i32(zi));
    let fine = (cc << vec3<u32>(shift)) + vec3<i32>(1 << (shift - 1u));
    cols[b] = genColumn(fine.x, fine.z, T.seed);
    tops[b] = farColTopFrom(cols[b].h, cols[b].fluidTop, fine.x, fine.z, T.seed);
  }
  let planeBase = ((level - 1u) * FAR_VOX + slot * CHUNK_VOL) / 4u;
  var top = 0u;   // one plus the highest row with a non-empty cell, this thread
  for (var yi = 0u; yi < CHUNK; yi++) {
    var word = 0u;
    for (var b = 0u; b < 4u; b++) {
      let cc = base + vec3<i32>(i32(x0 + b), i32(yi), i32(zi));
      // the sieve: fine-voxel center of the 2^shift-wide region this cell covers
      let fine = (cc << vec3<u32>(shift)) + vec3<i32>(1 << (shift - 1u));
      let col = cols[b];
      let mat = genCellCol(col, fine, T.seed) & 0xFFFu;
      // The conservative flag first: it is what a cell keeps when the centre
      // sample found nothing (common.wgsl FAR_BLOCKER_BIT).
      var byteV = farBlockerBitAt(tops[b], cc, shift, T.seed);
      if (farCellIsSolid(mat)) {
        // shape from the center sample, color from the surface skin (phase 4)
        byteV |= min(farSurfaceMat(col, mat, fine, shift, T.seed), FAR_MAT_MASK);
      }
      // farOcc counts NON-EMPTY cells, which now includes blocker-only ones —
      // it gates empty-space skipping for every far reader, and a reader that
      // hits on the flag must not have its chunk skipped out from under it.
      if (byteV != 0u) { count += 1u; top = yi + 1u; }
      word |= byteV << (b * 8u);
    }
    // The cell index in this level chunk is x + y*CHUNK + z*CHUNK*CHUNK (see the
    // patch loop's unpack below), so in words of four x-consecutive cells that
    // is x/4 + y*(CHUNK/4) + z*(CHUNK*CHUNK/4). x0 is a multiple of 4, so byte
    // `b` of the word is cell x0+b and the packing above is the same one the
    // flat form used.
    atomicStore(&farVox[planeBase + zi * (CHUNK * CHUNK / 4u) +
                        yi * (CHUNK / 4u) + li % 4u],
                word);
  }
  atomicAdd(&wgFarCount, count);
  atomicMax(&wgFarTop, top);
  // ---- THE EDIT PATCH (far-field edit persistence) ---------------------
  //
  // The sweep above is PRISTINE PROCGEN, and that is the whole problem this
  // block exists to fix: `fardown` writes the player's edits into these same
  // cells from the live grid, and every refill of this level chunk — an
  // incoming plane after the player walked out and back, a teleport, a world
  // load — used to erase them. FarField::PrepareTick now hands each fill entry
  // the cells its CPU-side index (src/sim/faredits.h) knows were edited, taken
  // from the persisted chunk store, and they are re-applied here.
  //
  // SAME RULE, SAME FUNCTION. The patch carries only the RAW MATERIAL at the
  // cell's sample voxel; the surface-skin recolor is `farSurfaceMat`, exactly
  // as in the sweep above and in `fardown`. That is what makes a patched cell
  // byte-identical to what the live downsample would have written, so a region
  // that flips between "resident and downsampled" and "refilled and patched"
  // does not change appearance, and the sieve/downsample boundary agreement
  // the `far-downsample` gate protects still holds.
  //
  // The barrier is UNCONDITIONAL and sits outside the loop: the sweep's
  // whole-word atomicStores must land before the byte-granular read-modify-
  // writes below, and a control barrier may not sit in non-uniform control
  // flow (the counts come from a storage buffer, so a guarded barrier would
  // fail WGSL's uniformity analysis).
  //
  // No cross-workgroup race, unlike `fardown`: a farVox word packs 4 cells
  // that are consecutive in x WITHIN this level chunk, so every byte of every
  // word this workgroup touches belongs to this workgroup alone.
  storageBarrier();
  let pOff = farPatch[wg.x * 2u];
  let pCnt = farPatch[wg.x * 2u + 1u];
  var pnz = 0u;
  for (var pi = li; pi < pCnt; pi += 64u) {
    let e = farPatch[FAR_PATCH_BASE + pOff + pi];
    let ci = e & 0xFFFu;                       // cell index in this level chunk
    let pmat = (e >> 12u) & 0xFFFu;            // raw material at the sample voxel
    let pl = vec3<i32>(vec3<u32>(ci % CHUNK, (ci / CHUNK) % CHUNK,
                                 ci / (CHUNK * CHUNK)));
    let pcc = base + pl;
    let pfine = (pcc << vec3<u32>(shift)) + vec3<i32>(1 << (shift - 1u));
    // Its own genColumn: a patch cell is an arbitrary cell of this level
    // chunk, so it shares no column with the thread's four. Patches are rare
    // (only cells the player edited), so this is the one place in the kernel
    // that still pays a column per cell — and now it pays it unconditionally,
    // because the blocker flag is a property of the TERRAIN under the patch
    // and has to survive a patch that clears the cell's material.
    let pcol = genColumn(pfine.x, pfine.z, T.seed);
    var byteV = farBlockerBitAt(
        farColTopFrom(pcol.h, pcol.fluidTop, pfine.x, pfine.z, T.seed),
        pcc, shift, T.seed);
    if (farCellIsSolid(pmat)) {
      byteV |= min(farSurfaceMat(pcol, pmat, pfine, shift, T.seed), FAR_MAT_MASK);
    }
    if (byteV != 0u) { pnz += 1u; atomicMax(&wgFarTop, u32(pl.y) + 1u); }
    let bi = (level - 1u) * FAR_VOX + slot * CHUNK_VOL + ci;
    let bsh = (bi & 3u) * 8u;
    atomicAnd(&farVox[bi >> 2u], ~(0xFFu << bsh));
    atomicOr(&farVox[bi >> 2u], byteV << bsh);
  }
  atomicAdd(&wgFarPatchNZ, pnz);
  workgroupBarrier();
  if (li == 0u) {
    // farOcc only ever gates EMPTY-SPACE SKIPPING, so it must never be too
    // small and may be too large: an over-count costs one marched level chunk,
    // an under-count hides real terrain (the same conservative direction
    // `fardown`'s atomicMax takes). The sum double-counts a patched cell that
    // was already non-air in the sweep and ignores a patch that cleared one —
    // both land on the safe side.
    // The top row rides the same word (common.wgsl FAR_OCC_TOP_SHIFT); it
    // is the max over the sweep and the patch, so it is conservative-high in
    // exactly the way the count is.
    atomicStore(&farOcc[(level - 1u) * FAR_NUM_CHUNKS + slot],
                farOccPack(min(atomicLoad(&wgFarCount) + atomicLoad(&wgFarPatchNZ),
                               CHUNK_VOL),
                           atomicLoad(&wgFarTop)));
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
  // Constant over the whole dispatch, like in `far` above (see Poi).

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

      // THE BLOCKER FLAG IS PRISTINE ON BOTH SIDES, and that is the whole
      // reason it is computed from the procgen COLUMN here rather than from
      // the live grid this entry otherwise reads. `far` has no live grid to consult —
      // it fills from procgen — so a flag derived from real voxels here would
      // differ from the flag `far` writes for the same cell, and the seam
      // between a refilled plane and a downsampled chunk would show it. Same
      // function, same arguments, same answer: the argument `farSurfaceMat`
      // already makes for the colour. The cost is that an EDIT never sets or
      // clears the flag — only the material byte below records it, which is
      // the behaviour every reader had before the flag existed.
      //
      // The column is now HOISTED out of the material branch and shared by the
      // two, so a cell pays ONE genColumn instead of one for the flag and
      // another for the skin. That makes an air cell dearer than it was (it
      // used to pay none) and a solid cell exactly as dear as it was.
      let pcol = genColumn(fine.x, fine.z, T.seed);
      var byteV = farBlockerBitAt(
          farColTopFrom(pcol.h, pcol.fluidTop, fine.x, fine.z, T.seed),
          cc, shift, T.seed);
      // live grid (the sample point is inside this chunk, hence resident)
      let mat = voxWordAt(fine) & 0xFFFu;
      if (farCellIsSolid(mat)) {
        // Same skin rule as the sieve — the skin is looked up from PRISTINE
        // procgen (genCell), so a pristine chunk downsamples bit-identically
        // to the sieve's fill. An edited surface keeps its pristine skin color
        // while the cell's center voxel survives; the moment the center voxel
        // is dug away the cell empties for real. A slightly stale rim color is
        // invisible at cascade distances; a seam between refilled planes and
        // downsampled chunks is not.
        // SAME CALL, SAME ARGUMENTS as the sieve — that identity is what keeps
        // a downsampled chunk byte-identical to a refilled one at their shared
        // boundary, and it is why farSurfaceMat takes the column rather than
        // deriving a height of its own (surfHeightAt used to, and drifted).
        byteV |= min(farSurfaceMat(pcol, mat, fine, shift, T.seed), FAR_MAT_MASK);
      }
      let bi = farVoxByteIndex(level, cc);
      let bsh = (bi & 3u) * 8u;
      atomicAnd(&farVox[bi >> 2u], ~(0xFFu << bsh));
      atomicOr(&farVox[bi >> 2u], byteV << bsh);
      if (byteV != 0u) {
        // Count 1 (all a reader asks of the count is non-zero) under this
        // cell's row: max() compares the row first, so a live edit that stacks
        // something above the sieve's top row raises it, and nothing ever
        // lowers it (common.wgsl, the farOcc word).
        atomicMax(&farOcc[farOccIndex(level, cc)],
                  farOccPack(1u, u32(cc.y & (i32(CHUNK) - 1)) + 1u));
      }
    }
  }
}
