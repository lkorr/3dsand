// worldgen.wgsl — the procedural world, integer-only and seed-deterministic.
//
// Every voxel is a PURE FUNCTION of (world coords, seed, the uploaded asset
// tables): a chunk that is generated, evicted unmodified and re-entered
// regenerates bit-identically, and a feature straddling a chunk border comes
// out the same from either side. No state, no scattering pass, no neighbour
// walk -- anything larger than one cell (a tree, a pond, a cactus) is placed
// per lattice tile by tile hash and re-derived by every cell that asks.
//
// ENTRY POINTS (one 64-thread workgroup per chunk unless noted):
//   cols      the column-cache pre-pass: one workgroup per CHUNK-COLUMN of
//             the list main / list are about to generate (genCols), each
//             (x, z) evaluated once
//   main      the whole slot space (NUM_SLOTS workgroups): startup / regen
//   list      T.genCount slots from genList: streamed-in chunks; also
//             publishes the generation verdict (genAct) the CPU classifies
//             pages from
//   pagefill  materializes JITTER page-table sentinels (not procgen at all)
//   far       the far-field cascade sieve: one level chunk per farList entry
//   farpatch  re-applies persisted edits over a `far` refill
//   fardown   downsamples live, dirty chunks into the cascades
//
// THE PIPELINE: genColumn(x, z) evaluates everything that is a function of the
// column alone -- landColumn (the ground height `h`: the landAt octave ladder,
// the pond bowl / berm, the sea, the site pad; mirrored in world.cpp as
// World::TerrainHeight), the biome, the shore band and the tile plant -- and
// colPrologue adds the column's cave bands, tree candidates and canopy cover
// for the range of heights about to be asked. `cols` stores that per (x, z)
// into the column cache with the column's sky ceiling; genChunk (main / list)
// reads it back and walks each column's sixteen cells; `far` does the same
// per sample column without a cache. genCellIn(col, y) then decides the one
// material at each height: terrain body and skin, caves, standing fluid and
// pond life, trees, shore plants, cacti, the biome cover stack, site stamps.
//
// WHERE THE MAP ENTERS: everything authored arrives through two read-only
// asset buffers -- `worldMap` (the map's terrain numbers, painted biome /
// landform / site planes, biome records with their cover / water rows, water
// presets; src/sim/worldmap.h) and `treeAtlas` (the baked trees;
// src/sim/treeatlas.h) -- plus a few load-time prelude constants (the tree
// and pond lattices, the map's reference voxel scale).
//
// Everything placed is INERT material (wood, leaves, grass, plants -- never
// stem/sprout/vine/seed): a generated world has to settle and sleep (CLAUDE.md
// rule 2), and loose matter is laid only where it is already at rest.

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
// THE GENERATION VERDICT (docs/RESEARCH_streaming_hitch.md R1; world.h's
// kGenVerdict* block is the CPU half of this layout).
// One u32 per genList POSITION, written by the `list` entry point for every
// position below arrayLength(genAct):
//   bit 0      the chunk holds a cell that can act — the deferred wake's act
//              set (the whole word was this bit before the classification
//              joined it). The CPU reads it back asynchronously when
//              T.genDeferWake is set and puts the act set into dirtyIn
//              kWakeLatency ticks later, which is what lets the shift stop
//              fencing.
//   bit 1      GEN_V_VALID: the class below was published
//   bits 2..3  the chunk's page-table class, PageTable::Classify's answer to
//              the words just written: 0 needs a page, 1 EMPTY, 2 UNIFORM,
//              3 JITTER — so neither the batched worldgen nor a shift's full
//              chunks have to read 16 KiB of words back to demote
//   bits 4..15 the material, for UNIFORM / JITTER
// Binding 30 exists ONLY in simBGL_ — `far`/`fardown` run on the slim group
// and never reach genChunk, exactly like pageFillList at 19.
@group(0) @binding(30) var<storage, read_write> genAct : array<u32>;
const GEN_V_ACT : u32 = 1u;
const GEN_V_VALID : u32 = 2u;
const GEN_V_CLASS_SHIFT : u32 = 2u;
const GEN_V_EMPTY : u32 = 1u;
const GEN_V_UNIFORM : u32 = 2u;
const GEN_V_JITTER : u32 = 3u;
const GEN_V_MAT_SHIFT : u32 = 4u;
// ---- THE COLUMN CACHE (the `cols` pre-pass; Simulation::WriteGenList) ------
// A listed chunk's columns are functions of (x, z) alone, and a streamed
// X/Z plane is 32 chunks tall: genChunk used to evaluate each column ~32
// times. `cols` evaluates each (x, z) of the list ONCE, per chunk-COLUMN (the
// 16x16 columns of one world chunk x/z), and genChunk reads the answer.
//
// genCols, written by the CPU with the list:
//   [0, NUM_SLOTS)                  per list POSITION: its chunk-column index k
//   [GC_REC_BASE + GC_REC_WORDS k]  chunk-column k: world chunk x, z, and the
//                                   lowest / highest chunk y the list asks for
//                                   on it (i32 as u32)
// colCache, written by `cols` and read by `main` / `list`: one block of
// CC_BLOCK words per chunk-column -- a CC_HDR-word header (CCH_*: the
// block's highest column sky ceiling, highest tree-candidate top and lowest
// ground), then CHUNK*CHUNK columns of CC_WORDS each (x fastest, the order
// genChunk's ci walks; CCW_* below). The two layout numbers are
// src/sim/simulation.cpp's kColCacheWords / kColCacheHdr (check_invariants).
// Bindings 38/39 exist ONLY in simBGL_, like genAct.
@group(0) @binding(38) var<storage, read> genCols : array<u32>;
@group(0) @binding(39) var<storage, read_write> colCache : array<u32>;
const GC_REC_BASE : u32 = NUM_SLOTS;
const GC_REC_WORDS : u32 = 4u;
const CC_WORDS : u32 = 24u;
const CC_HDR : u32 = 16u;
const CC_BLOCK : u32 = CC_HDR + CHUNK * CHUNK * CC_WORDS;
const CCH_TOP : u32 = 0u;        // max over the block's columns of CCW_TOP
const CCH_TREE_TOP : u32 = 1u;   // max of CCW_TREE_TOP
const CCH_MIN_H : u32 = 2u;      // min of the columns' ground h
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

// ---- material ids worldgen places by constant -----------------------------
// ARRAY POSITIONS in materials.json (id == index + 1). Everything a biome,
// water preset, tree or stamp authors is resolved by NAME at load and arrives
// through the tables; these are the few the kernel still names itself (the
// terrain body, fluids, cacti, tile plants, cave flora). Recompute after any
// materials.json edit that is not a pure append:
//   python -c "import json;[print(i+1,m['id']) for i,m in
//              enumerate(json.load(open('assets/materials/materials.json'))['materials'])]"
const M_STONE : u32 = 1u;
const M_SAND  : u32 = 3u;
const M_GRAVEL : u32 = 4u;
const M_WATER : u32 = 5u;
const M_OIL   : u32 = 6u;
const M_LAVA  : u32 = 12u;
const M_SNOW  : u32 = 15u;
const M_CACTUS       : u32 = 70u;   // saguaro/barrel flesh: SOLID, blocking
const M_CACTUS_RIB   : u32 = 71u;   // ribbed skin + spines: SOLID, blocking
const M_CACTUS_BLOOM : u32 = 72u;   // crown flower: passable
const M_SHORE_MUD    : u32 = 81u;   // a shore band's mud ring when the preset names none
const M_FERN      : u32 = 88u;      // tile plant (plantColumnAt)
const M_MUSHROOM  : u32 = 89u;      // cave floor (caveFloraAt)
const M_TOADSTOOL : u32 = 90u;
const M_MUSHROOM_LARGE : u32 = 123u; // tile plant (plantColumnAt)
const M_CRYSTAL   : u32 = 116u;     // cave flora: the deep band's light

// ---- THE VOXEL-SIZE SCALE --------------------------------------------------
// VOXELS_PER_M is world.h's kVoxelsPerMetre; REF_VOXELS_PER_METRE is the scale
// the map's terrain (map.json `terrain`) is authored at, a worldgen-only
// prelude constant from the loaded map (mirrored by scripts/map_terrain.py
// for check_shaders.sh). LoadWorldMap has already rescaled the map's own
// numbers; `vlen()` rescales the lengths hardcoded HERE -- cave band depths,
// the magma table, tile pitches. Multiply first, divide second, so the
// shipped case (num == den) is exact.
//
// A LENGTH gets vlen(). A count, a probability, a 0..255 noise threshold and a
// GRADIENT do not -- a gradient is dimensionless, which is why the angle of
// repose is 1 voxel/column at every voxel size.
const VLEN_NUM : i32 = VOXELS_PER_M;
const VLEN_DEN : i32 = REF_VOXELS_PER_METRE;
fn vlen(v : i32) -> i32 { return (v * VLEN_NUM) / VLEN_DEN; }

// The floor of genChunk's sky-skip margin above the ground, in cells: covers
// the tallest TILE plant (a fern at PLANT_FERN_MAXH 5, a big toadstool at 3).
// The biome's cover rows and its water presets' plants arrive as
// WM_B_MAX_COVER_H, the other arm of the max().
const SKY_MARGIN_MIN : i32 = (8 * VLEN_NUM) / VLEN_DEN;

// ---- CAVE FLORA PLACEMENT ----
// The densities are the biome's (WM_B_CAVE_*_CHANCE); the patch masks and the
// margin are structure. Crystal grows in SEAMS, not as an even sprinkle: the
// same patch device the fern banks use on the surface, at a cavern's scale.
const CAVE_CRYSTAL_PATCH_CELL : i32 = 40;
const CAVE_CRYSTAL_PATCH      : i32 = 168;   // vnoise 0..255; ~35% of the area
const CAVE_SHROOM_PATCH_CELL  : i32 = 22;
const CAVE_SHROOM_PATCH       : i32 = 128;
// Keep-out above the magma table: no crystal seam grows into the lava.
const CAVE_LAVA_MARGIN : i32 = (6 * VLEN_NUM) / VLEN_DEN;

// ---- TILE PLANTS (plantColumnAt) ----
// undergrowthSite() returns a column's CANOPY COVER, 0 (open sky) .. 255 (deep
// under overlapping crowns). The cover rows read it through their canopyMin /
// canopyMax conditions; the one threshold left here gates the tile ferns: a
// fern bank wants real shade, and 96 sits near the middle of a single crown's
// ramp rather than at its rim, where a threshold draws a visible circle of
// fern around every tree.
const UG_COVER_MIN  : i32 = 96;
// Percent of tiles inside the patch mask that grow one. Tile size / footprint
// / height live in common.wgsl as the PLANT_* consts because the renderer
// rebuilds the plant from the same hash.
const PLANT_FERN_CHANCE   : u32 = 22u;
const PLANT_SHROOM_CHANCE : u32 = 3u;
const UG_FERN_PATCH_CELL : i32 = 20;
const UG_FERN_PATCH     : i32 = 140;    // vnoise 0..255; ~40% of the area

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

// The 8-bit decorative noise: 0..255 out, linear interpolation, integer
// divides, and a hard ceiling at a 2901-voxel cell (255*cs^2 crosses 2^31).
// Only the patch masks of the cave flora and the fern banks call it (cs 20..40,
// nowhere near the ceiling); an 8-bit answer is exactly the resolution a
// "1-in-N inside this patch mask" test wants. Terrain uses vnoise2d below.
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
//     and it is headroom the octave ladder spends.
// The lerps below are written in the DIFFERENCE form c0 + (c1-c0)*s, which is
// cheaper and has more margin still; 14 is kept anyway so the bound holds no
// matter which form a later edit uses.
struct N2 {
  n  : i32,   // value, Q14: 0..16383
  dx : i32,   // d(n)/d(tx): change in n across one WHOLE CELL of +x, Q14
  dz : i32,   // likewise for +z. Feeds the ladder's derivative attenuation.
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

// MIRROR-END noise

// Snow/treeline: the ground Y at and above which the skin is snow and no tree
// stands. map.json `terrain.treeline`, read from the worldMap header so
// the C++ side (worldmap::CurrentTerrain().treeline) sees the same number.
fn treeline() -> i32 { return wmTerrain(WM_H_TERRAIN_TREELINE); }
// debug.vegetation: the kill-switch for every plant this file places,
// TREES AND CACTI INCLUDED. Gated at the SOURCE of each feature (the *Info /
// *At functions and the table-driven blocks in genCellIn), not by a material
// test at the end, so the tree candidate scan, the undergrowth canopy scan and
// the far cascade's crown proxy all see the same treeless, coverless world --
// and none of them pays for plants that will not be placed. A frame-rate A/B
// lever; terrain, water, caves and site stamps are untouched.
//
// THE PAIR. This is the EXTREME end of the axis, not the everyday one: it also
// takes the wood away, and a world with no trees in it is a different world
// rather than the same world with less clutter. `groundCover` below is the
// half that removes only what a body walks through. Anything that reads as a
// LANDMARK -- a tree, a saguaro -- answers to this flag alone.
const VEGETATION : bool = TUNE_VEGETATION != 0u;
// debug.groundCover: the SMALL half of the switch above. Off leaves the
// trees and the cacti standing and removes everything a body walks THROUGH --
// the biome cover rows (tall grass, wildflowers, undergrowth, scrub, alpine
// cushion), the tile plants (ferns, big toadstools), the shore rows, the pond
// life, the wet moss skin and the cave flora. ANDed with VEGETATION, so the
// master switch is still the extreme end of the same axis and a reader never
// has to check two flags to know whether anything grows.
//
// Gated at each feature's SOURCE for the same reason the master is: the
// undergrowth canopy scan and the tile-plant site scan are the expensive part,
// and a material test at the end would still pay for them.
const GROUND_COVER : bool = VEGETATION && TUNE_GROUND_COVER != 0u;

// ---- PER-BIOME RELIEF: the curve and the multipliers, FROM THE MAP ----
//
// "This biome is flat plains, that one is jagged mountains", authored as nine
// numbers in assets/biomes/<name>.json `terrain.curve` plus three multipliers
// on the map's fine octaves (`terrain.hill / detail / grain`, Q8, 256 = the
// map's own amplitude). They live in the biome RECORD (WM_B_CURVE_KNOT0..8,
// WM_B_HILL_MUL ..) and a column reads them from the FOUR MAP CELLS around it,
// bilinearly, on the landform plane's own lattice (cell values at cell
// centres, mapLandformQ8's arithmetic exactly) -- so two biomes' curves
// crossfade over a whole cell (102 m) and never meet on a cliff. Tier A: no
// seed anywhere in here; the cells are the map's. The old noise band, its
// three thresholds and the +-18-unit crossfade went with the knobs.
//
// The curve reshapes the COARSE SUM only -- the landform plane and the range
// rung, the two that decide where the mountains and the basins are -- and
// leaves hill/detail/grain to the multipliers. So a biome changes the LANDFORM
// with the curve and the TEXTURE with the multipliers, which is the same split
// `Land.slope` makes for the sediment wedge.
//
// ---- NINE KNOTS, NOT EIGHT, AND WHY ---------------------------------------
// Eight knots is SEVEN intervals, so the identity curve's knot values are
// -16384 + i*32768/7 — not integers. An identity curve could then not be
// AUTHORED at all, only approximated, and "the default curve moves nothing"
// would be unprovable rather than merely untested. Nine knots is eight
// intervals and the identity values are -16384 + i*4096 exactly, which is what
// a biome that says nothing about its terrain carries.
//
// ---- THE DOMAIN -----------------------------------------------------------
// One rung spans +-amp/2 (`octave` returns `((n - 8192) * amp) >> 14` and
// `n - 8192` is +-8192; the landform plane's contribution is the same shape
// over its range), so the coarse pair spans +-(landformRange + rangeAmp)/2
// and that is the curve's input grid.
//
// ---- WHY THE IDENTITY IS BIT-EXACT ----------------------------------------
// Four separate pieces of the arithmetic, and all four are load-bearing:
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
//  4. The bilinear mix of four EQUAL values is that value: `a + ((b - a) * f)
//     >> l` with b == a adds exactly zero. So four identity biomes mix to the
//     identity, and four 256s to 256.
//
// Nothing folds at compile time any more -- the knots are table reads -- which
// is the price of an authorable curve, paid once per column. (With the knots as
// compile-time constants the driver's compile of this kernel ran past fifteen
// minutes the moment a non-identity set was authored; a table read is what
// the memory of that says to do.)
const CURVE_KNOTS : i32 = 9;
const CURVE_SEGS  : i32 = 8;

// The four map cells around a column and where the column sits between them:
// the landform plane's lattice (cell values at cell CENTRES), spelled exactly
// as mapLandformQ8 spells it, so the curve blends on the same grid the relief
// it reshapes is drawn on.
struct BiomeMix {
  b00 : u32,
  b10 : u32,
  b01 : u32,
  b11 : u32,
  fx : i32,
  fz : i32,
  l : u32,
};
fn wmBiomeCellId(cx : i32, cz : i32) -> u32 {
  let w = i32(worldMap[WM_H_WIDTH]);
  let h = i32(worldMap[WM_H_HEIGHT]);
  let c = vec2<i32>(clamp(cx, 0, w - 1), clamp(cz, 0, h - 1));
  return wmPlaneAt(worldMap[WM_H_BIOME_PLANE], c.x, c.y);
}
fn biomeMixAt(x : i32, z : i32) -> BiomeMix {
  var m : BiomeMix;
  let l = worldMap[WM_H_CELL_LOG2];
  let half = 1 << (l - 1u);
  let mx = x - half + (i32(worldMap[WM_H_ORIGIN_X]) << l);
  let mz = z - half + (i32(worldMap[WM_H_ORIGIN_Z]) << l);
  let cx = mx >> l;
  let cz = mz >> l;
  let mask = (1 << l) - 1;
  m.fx = mx & mask;
  m.fz = mz & mask;
  m.l = l;
  m.b00 = wmBiomeCellId(cx, cz);
  m.b10 = wmBiomeCellId(cx + 1, cz);
  m.b01 = wmBiomeCellId(cx, cz + 1);
  m.b11 = wmBiomeCellId(cx + 1, cz + 1);
  return m;
}
fn mixI(m : BiomeMix, v00 : i32, v10 : i32, v01 : i32, v11 : i32) -> i32 {
  let a = v00 + (((v10 - v00) * m.fx) >> m.l);
  let b = v01 + (((v11 - v01) * m.fx) >> m.l);
  return a + (((b - a) * m.fz) >> m.l);
}
// A biome record word read as a signed value (the knots are Q14, signed).
fn wmBiomeI(b : u32, w : u32) -> i32 { return bitcast<i32>(wmBiome(b, w)); }
fn curveKnotAt(m : BiomeMix, i : i32) -> i32 {
  let w = WM_B_CURVE_KNOT0 + u32(clamp(i, 0, CURVE_KNOTS - 1));
  return mixI(m, wmBiomeI(m.b00, w), wmBiomeI(m.b10, w), wmBiomeI(m.b01, w), wmBiomeI(m.b11, w));
}
fn curveHi() -> i32 {
  return max((wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE) + wmTerrain(WM_H_TERRAIN_RANGE_AMPLITUDE)) / 2, 1);
}

// Fritsch-Carlson, for uniformly spaced knots: the harmonic mean of the two
// secants, and zero at a local extremum. That is what makes an authored plateau
// FLAT and an authored ramp crease-free — a Catmull-Rom tangent would overshoot
// both, and an overshoot here is a hill the author did not put there.
//
// The multiply is safe in i32 because it only happens when the two secants
// share a sign: knot values are clamped to +-16384 by the loader, so a secant
// of +32768 forces its neighbour negative and takes the early return.
fn curveTangent(dPrev : i32, dNext : i32) -> i32 {
  if (dPrev * dNext <= 0) { return 0; }
  return (2 * dPrev * dNext) / (dPrev + dNext);
}

// The blended curve at a column. Returns (value in voxels, d(value)/d(input)
// in Q8); BOTH are used -- the slope multiplies the accumulated gradient so
// the iq attenuation of hill/detail/grain still sees the ground it is actually
// attenuating against.
fn curveOne(m : BiomeMix, u : i32) -> vec2<i32> {
  let hi = curveHi();
  let uc = clamp(u, -hi, hi);
  // Parameter across the eight segments, Q12 within a segment. The identity's
  // value at this parameter is exactly `p - 16384`, which is what (2) above
  // subtracts.
  let p = clamp(((uc + hi) * (CURVE_SEGS << 12)) / (2 * hi), 0, CURVE_SEGS << 12);
  let seg = min(p >> 12, CURVE_SEGS - 1);
  let t = p - (seg << 12);
  let km = curveKnotAt(m, seg - 1);
  let k0 = curveKnotAt(m, seg);
  let k1 = curveKnotAt(m, seg + 1);
  let k2 = curveKnotAt(m, seg + 2);
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
fn biomeCurve(m : BiomeMix, u : i32) -> vec2<i32> {
  if (worldMap[WM_H_BIOME_PLANE] == 0u) { return vec2<i32>(u, 256); }   // no planes: the identity
  return curveOne(m, u);
}
// The three multipliers, blended the same way. 256 = the map's own amplitude.
struct BiomeRelief {
  hill : i32,
  detail : i32,
  grain : i32,
};
fn biomeReliefAt(m : BiomeMix) -> BiomeRelief {
  var r : BiomeRelief;
  r.hill = 256;
  r.detail = 256;
  r.grain = 256;
  if (worldMap[WM_H_BIOME_PLANE] == 0u) { return r; }
  r.hill = mixI(m, wmBiomeI(m.b00, WM_B_HILL_MUL), wmBiomeI(m.b10, WM_B_HILL_MUL),
                wmBiomeI(m.b01, WM_B_HILL_MUL), wmBiomeI(m.b11, WM_B_HILL_MUL));
  r.detail = mixI(m, wmBiomeI(m.b00, WM_B_DETAIL_MUL), wmBiomeI(m.b10, WM_B_DETAIL_MUL),
                  wmBiomeI(m.b01, WM_B_DETAIL_MUL), wmBiomeI(m.b11, WM_B_DETAIL_MUL));
  r.grain = mixI(m, wmBiomeI(m.b00, WM_B_GRAIN_MUL), wmBiomeI(m.b10, WM_B_GRAIN_MUL),
                 wmBiomeI(m.b01, WM_B_GRAIN_MUL), wmBiomeI(m.b11, WM_B_GRAIN_MUL));
  return r;
}

// MIRROR-BEGIN height
// The terrain, and how steep it is there. THE SLOPE IS NOT DECORATION: this
// CA's angle of repose is exactly 1 voxel per column (sim_step.wgsl slides a
// powder into any free down-diagonal), so every rule that lays loose material
// has to know the gradient or it lays powder on a wall and the chunk never
// sleeps — a rule-2 failure that surfaces two gates away as "the world does not
// settle". The sediment wedge and the pond slope gate are the consumers.
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
  let att = 65536 / (256 + ((wmTerrain(WM_H_TERRAIN_FBM_ATTEN) * ((g * g) >> 8)) >> 8));
  var o : Oct;
  o.dev = ((((n.n - 8192) * amp) >> 14) * att) >> 8;
  // dh/dx is (dn/dt * amp) / (2^14 * cell) and Q8 multiplies by 256, so the
  // whole conversion is ONE arithmetic shift: >> (14 - 8 + log2).
  o.gx = (((n.dx * amp) >> (6u + csl)) * att) >> 8;
  o.gz = (((n.dz * amp) >> (6u + csl)) * att) >> 8;
  return o;
}

// The continental rung, FROM THE MAP (P4): where the old o0 octave sampled
// noise, this reads the painted landform plane. map.json terrain
// .landformRangeVox (WM_H_TERRAIN_LANDFORM_RANGE) is the height span the full
// 0..255 landform range maps to, centred on 128 -- so the map says how tall
// the world is AND where. Its own function inside the mirror so the two
// sides spell the same three lines; the plane readers it calls live outside.
fn landformOctave(x : i32, z : i32) -> Oct {
  var o : Oct;
  o.dev = ((mapLandformQ8(x, z) - 32768) * wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE)) >> 16;
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
  //
  // EVERY NUMBER IS THE MAP'S: map.json `terrain` through the worldMap
  // header (wmTerrain), and the biome's own relief record through the four
  // map cells around the column (biomeMixAt, read ONCE here and handed to the
  // curve and the multipliers). Nothing in this function is a tuning knob.
  let bm = biomeMixAt(x, z);
  let rl = biomeReliefAt(bm);
  let o0 = landformOctave(x, z);
  let o1 = octave(x, z, wmTerrainU(WM_H_TERRAIN_RANGE_LOG2), wmTerrain(WM_H_TERRAIN_RANGE_AMPLITUDE),
                  o0.gx, o0.gz, seed ^ 2u);
  // ---- THE PER-BIOME HEIGHT CURVE ----
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
  let cv = biomeCurve(bm, o0.dev + o1.dev);
  let g1x = ((o0.gx + o1.gx) * cv.y) >> 8;
  let g1z = ((o0.gz + o1.gz) * cv.y) >> 8;
  // The three fine rungs, each scaled by the biome's Q8 multiplier (256 = the
  // map's amplitude; a flatter meadow authors 128 on the hills).
  let o2 = octave(x, z, wmTerrainU(WM_H_TERRAIN_HILL_LOG2), (wmTerrain(WM_H_TERRAIN_HILL_AMPLITUDE) * rl.hill) >> 8,
                  g1x, g1z, seed ^ 3u);
  let g2x = g1x + o2.gx;
  let g2z = g1z + o2.gz;
  let o3 = octave(x, z, wmTerrainU(WM_H_TERRAIN_DETAIL_LOG2), (wmTerrain(WM_H_TERRAIN_DETAIL_AMPLITUDE) * rl.detail) >> 8,
                  g2x, g2z, seed ^ 4u);
  let g3x = g2x + o3.gx;
  let g3z = g2z + o3.gz;
  let o4 = octave(x, z, wmTerrainU(WM_H_TERRAIN_GRAIN_LOG2), (wmTerrain(WM_H_TERRAIN_GRAIN_AMPLITUDE) * rl.grain) >> 8,
                  g3x, g3z, seed ^ 5u);

  // ---- THE CALM HOME AREA ----
  // Only the TWO COARSE octaves fade toward the SPAWN SITE (map.json sites[],
  // kind "spawn"; spawnCentre() reads it from the header). Fading the whole
  // deviation would pin spawn to a mathematically exact plane 64 m across —
  // which is not "calm", it is a dinner plate, and it would also make the
  // terrain gate's pass C a test of a constant. The three fine rungs stay at
  // full amplitude, so the home area is rolling country of about the shape the
  // pre-overhaul world had, sitting at terrain.homeArea.y.
  //
  // CHEBYSHEV distance, so no isqrt; a square region has its steepest boundary
  // on the axes and its longest on the diagonal, which is why the gate's A4
  // transects walk both. The `d < fade` guard is not cosmetic: `d << 14` on a
  // world coordinate a few hundred thousand voxels out would overflow, and X/Z
  // are infinite here.
  //
  // TWO CENTRES. The harness box gets the same fade, measured from its EDGE
  // (harnessOutside is 0 anywhere inside it), and the calmer of the two wins.
  // The pad's flatness only ever came from this fade -- kind "pad" is not in
  // the site table, so sitePadAt never levels it -- and the fixtures were
  // written against that ground (terrain C2, floaters, corpse-burn). Moving
  // the spawn out of the pad must not move the pad's ground with it.
  let sc = spawnCentre();
  let fade = wmTerrain(WM_H_TERRAIN_HOME_FADE);
  let d = max(abs(x - sc.x), abs(z - sc.y)) - wmTerrain(WM_H_TERRAIN_HOME_R);
  var w = 16384;
  if (d < fade) {
    w = (max(d, 0) * 16384) / fade;
  }
  let dh = harnessOutside(x, z);
  var wh = 16384;
  if (dh < fade) {
    wh = (dh * 16384) / fade;
  }
  let ws = vsmooth(min(w, wh) << 1) >> 1;           // Q14 smoothstep of the ramp
  let homeY = wmTerrain(WM_H_TERRAIN_HOME_Y);
  let coarse = wmTerrain(WM_H_TERRAIN_BASE_HEIGHT) + cv.x - homeY;
  let bed = homeY + o2.dev + o3.dev + o4.dev
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
  //   * POND PLACEMENT (pondGate). "Is this ground flat enough to hold a bowl
  //     of water" is a question about the hillside, not about whether one
  //     sub-metre bump happens to sit under the centre column. Gated on the
  //     full gradient it is effectively a coin toss, and the ponds it accepts
  //     on real hillsides lay a powder bed down the inside of a cut cliff.
  //
  // Physically it is also the better model in both cases: sediment is what
  // FILLS surface roughness, so roughness must not switch it off.
  let slope = abs(g2x) + abs(g2z);
  let room = max(0, wmTerrain(WM_H_TERRAIN_SED_CEIL) - bed);
  var sed = ((room * wmTerrain(WM_H_TERRAIN_SED_FRACTION)) >> 8) - wmTerrain(WM_H_TERRAIN_SED_STRIP);
  let sedSlope = wmTerrain(WM_H_TERRAIN_SED_SLOPE);
  sed = (max(sed, 0) * max(sedSlope - slope, 0)) /
        max(sedSlope, 1);
  sed = clamp(sed, 0, wmTerrain(WM_H_TERRAIN_SED_MAX));
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
// The biome is READ FROM THE PAINTED MAP with a seeded edge warp (mapBiomeAt,
// with the other map readers below). The height curve blends on the map's own
// cells instead (biomeMixAt).
fn biomeAt(x : i32, z : i32, seed : u32) -> u32 {
  return mapBiomeAt(x, z, seed);
}


// ---- ponds ----
// Bounded DISC ponds: an object with a knowable boundary, not a field contour
// (a basin-noise contour filled to a noise water table spills wherever the
// mask edge crosses lower ground, and a spill is a chunk that never sleeps).
//
// Ponds come from TWO sources through ONE record: AUTHORED water sites on the
// map (Tier A, `waterSiteNear`: centre, radius and preset are the map's, the
// same on every seed) and ROLLED ponds (Tier B, `pondRoll`: at most one per
// tile of the one pond lattice, `pondTile()`, thinned per biome by the
// biome's water rows). Both become a `Pond` wearing a water PRESET (`wp`,
// 1-based into the worldMap buffer's kW_* records), and from there the bowl,
// the berm, the shore band and the flora are the preset's.
//
// CONTAINMENT IS STRUCTURAL. The waterline is the ground at the pond's own
// CENTRE column (pondGate: one landAt); the bowl is carved below it
// (bowlDepth), and the annulus outside the disc is forced UP to
// `surf + berm height` (bermLift), ramped back to natural ground over the
// berm width so it reads as a bank. Every column in the berm's core is then
// above the waterline at every seed and radius, with no rim sampling. On
// flat ground the berm is invisible; on a slope only the downhill side
// rises, which reads as a dammed tarn. Independent of the cave system: caves
// stop well under the surface, bowls reach a few metres.
// MIRROR-BEGIN height
struct Pond {
  present  : bool,
  authored : bool, // an authored lake: no row gates, no undercut gate
  cx       : i32,  // disc centre, world coords
  cz       : i32,
  r        : i32,  // disc radius
  surf     : i32,  // water surface Y: the ground the CENTRE column would have (-1 until gated)
  wp       : u32,  // the water preset this pond wears; 0 = none
  minY     : i32,  // the row's conditions, carried to the gate (-1 = unbounded)
  maxY     : i32,
  maxSlope : i32,  // Q8; 1024 = unbounded
};

fn pondNone() -> Pond {
  var p : Pond;
  p.present = false; p.authored = false; p.cx = 0; p.cz = 0; p.r = 0; p.surf = -1; p.wp = 0u;
  p.minY = -1; p.maxY = -1; p.maxSlope = 1024;
  return p;
}

// THE POND SET of a column: every pond CANDIDATE that could touch it -- the
// authored lake of its cell (slot 0 when present) and the rolled candidates
// of its own tile and of the neighbouring tiles whose edge is within the
// scan band. Built ONCE per column by pondScan and handed by value to every
// reader (the bowl, the shore, the tree and cactus scans, the near-water
// conditions), because pondRoll is thirty table reads and the tree scans
// used to inline it five hundred times per column path -- which is what
// took the driver's compile of this kernel from minutes to never. Five named
// slots rather than an array: the height mirror's token compare has no
// array syntax in common between the two languages, and nothing indexes it
// dynamically (a dynamic index into a by-value struct spills it).
struct PondSet {
  n  : i32,
  d0 : Pond,
  d1 : Pond,
  d2 : Pond,
  d3 : Pond,
  d4 : Pond,
};

fn pondSetNone() -> PondSet {
  var s : PondSet;
  s.n = 0;
  s.d0 = pondNone(); s.d1 = pondNone(); s.d2 = pondNone(); s.d3 = pondNone(); s.d4 = pondNone();
  return s;
}

fn setPush(s : PondSet, p : Pond) -> PondSet {
  var q = s;
  if (q.n == 0) { q.d0 = p; } else if (q.n == 1) { q.d1 = p; } else if (q.n == 2) { q.d2 = p; }
  else if (q.n == 3) { q.d3 = p; } else { q.d4 = p; }
  q.n = q.n + 1;
  return q;
}

// The bowl's depth below the waterline at squared distance d2 from the centre
// of a disc of radius r wearing preset wp: the preset's sampled profile,
// linear in d2 between knot floor(16 d2 / r2) and the next (worldmap.h says
// why the knots sit at sqrt(k/16)). Integer throughout and monotone in d2,
// which is what lets the basin registry invert it by bisection.
fn bowlDepth(wp : u32, r : i32, d2 : i32) -> i32 {
  let r2 = max(r * r, 1);
  let s = min(d2, r2) * 16;
  let u = min(s / r2, 15);
  let frac = s - u * r2;
  let k0 = waterKnot(wp, u32(u));
  let k1 = waterKnot(wp, u32(u) + 1u);
  let f = k0 - ((k0 - k1) * frac) / r2;
  let rd = wmWaterI(wp, WM_W_RIM_DEPTH);
  return rd + ((wmWaterI(wp, WM_W_DEPTH) - rd) * f) / 256;
}

// The largest s <= hi0 with s * s <= v, by bisection: an integer sqrt for the
// bed-steepness test below (12 steps cover a 2048-voxel radius). Rule 1.
fn isqrtLe(v : i32, hi0 : i32) -> i32 {
  var lo = 0;
  var hi = hi0;
  for (var i = 0; i < 12; i++) {
    if (lo >= hi) { break; }
    let mid = (lo + hi + 1) / 2;
    if (mid * mid <= v) { lo = mid; } else { hi = mid - 1; }
  }
  return lo;
}

// Is the bowl face at this column steeper than the CA's angle of repose (one
// voxel per column)? Compared against the column one voxel further out along
// the radius. A face this steep gets the preset's SUBSTRATE instead of its
// powder bed (genCellIn), so a steep authored tarn stays settled (rule 2)
// instead of avalanching its sand forever -- which is why the preset's depth
// no longer has to be bounded by its radius.
//
// KNOWN GAP (2026-09-09, measured): the ring comparison under-reads the step
// where the profile crosses 2 voxels between integer radius rings -- 168 bed
// grains on one marsh bowl face sat submerged at a >=2 step this test called
// 1, and slid on tick 0. The exact fix (test the four axis neighbours' own
// bowlDepth) exists and works, but it changes the harness pool's bed enough
// to need the waterbody-gate ledger reconciled with it, so it ships with
// that reconciliation, not here.
fn bowlSteep(p : Pond, x : i32, z : i32) -> bool {
  let dx = x - p.cx;
  let dz = z - p.cz;
  let d2 = dx * dx + dz * dz;
  let d = isqrtLe(d2, p.r) + 1;
  let here = bowlDepth(p.wp, p.r, d2);
  let out = bowlDepth(p.wp, p.r, min(d * d, p.r * p.r));
  return here - out > 1;
}

// THE AUTHORED LAKE (Tier A) whose index cell this column sits in, or none.
// No seed touches its centre, radius or preset; only its waterline is the
// terrain's, filled by pondGate like a rolled pond's, so it sits IN the
// ground rather than at an authored Y.
fn waterSiteNear(x : i32, z : i32) -> Pond {
  var p = pondNone();
  let sid = wmSiteAt(x, z);
  if (sid == 0u) { return p; }
  if (u32(wmSiteI(sid, WM_S_KIND)) != WM_SITE_WATER) { return p; }
  p.cx = wmSiteI(sid, WM_S_X);
  p.cz = wmSiteI(sid, WM_S_Z);
  p.r = wmSiteI(sid, WM_S_RADIUS);
  p.wp = u32(wmSiteI(sid, WM_S_PRESET));
  p.present = true;
  p.authored = true;
  return p;
}

// ONE HASH SALT PER ROW, never bit-slices of one hash (the pond-life block in
// genCellIn documents why): two rows sharing entropy would co-locate.
fn waterRowHit(biome : u32, i : u32, pt : i32, pz : i32, seed : u32) -> bool {
  let hRow = hash3(seed ^ (0xB0A7u + i * 0x9E37u), bitcast<u32>(pt), bitcast<u32>(pz));
  return i32(hRow & 0xFFFFu) < wmWaterRow(biome, i, WM_R_CHANCE_Q16);
}

// THE ROLLED POND of one lattice tile (Tier B): the CANDIDATE. The pond
// analogue of treeInfo, on the pond analogue of the one tree lattice: the
// tile hash puts the site in the middle half of the tile, the painted cell
// there says whose water rows roll (biomeCellAt: the cell, no warp), the
// rows roll in authored order by chance alone -- UNROLLED over the packer's
// cap of four (worldmap.h kWaterRowsMax), last to first so the first hit
// wins; a row's chance is already thinned for the lattice by worldmap.cpp
// PackWaterRows -- and the row that hits names the preset and OWNS the tile.
// The radius is the preset's (a multiply-and-shift, never a modulo by a
// table word), the centre is pulled inward so the disc never leaves its tile
// (a column consults ONE tile for its cover, so a disc that overhung its
// edge would be half a bowl), the keep-out runs. NO landAt here: the row's
// conditions and the slope gates are pondGate's, paid once per column on the
// candidate that matters.
fn pondRoll(pt : i32, pz : i32, seed : u32) -> Pond {
  var p = pondNone();
  let tile = pondTile();
  if (tile <= 0) { return p; }
  let rh = hash3(seed ^ 0xB0A7u, bitcast<u32>(pt), bitcast<u32>(pz));
  let span = u32(max(tile / 2, 1));
  var cx = pt * tile + tile / 4 + i32((rh >> 9u) % span);
  var cz = pz * tile + tile / 4 + i32((rh >> 17u) % span);
  let biome = biomeCellAt(cx, cz);
  let n = wmWaterRowCount(biome);
  if (n == 0u) { return p; }
  var row = 4u;
  if (n > 3u && waterRowHit(biome, 3u, pt, pz, seed)) { row = 3u; }
  if (n > 2u && waterRowHit(biome, 2u, pt, pz, seed)) { row = 2u; }
  if (n > 1u && waterRowHit(biome, 1u, pt, pz, seed)) { row = 1u; }
  if (n > 0u && waterRowHit(biome, 0u, pt, pz, seed)) { row = 0u; }
  if (row == 4u) { return p; }
  let wp = u32(wmWaterRow(biome, row, WM_R_PRESET));
  if (wp == 0u) { return p; }
  let r = wmWaterI(wp, WM_W_RADIUS_MIN) + i32((((rh >> 4u) & 0xFFFFu) * u32(wmWaterI(wp, WM_W_RADIUS_SPAN))) >> 16u);
  let inset = r + 4;
  if (tile - 2 * inset < 1) { return p; }
  cx = clamp(cx, pt * tile + inset, pt * tile + tile - 1 - inset);
  cz = clamp(cz, pz * tile + inset, pz * tile + tile - 1 - inset);
  // Keep-outs, by CENTRE (as stamps always were): the harness box, every
  // stamp's cells, every authored lake's disc + band. A site wins its ground.
  if (siteKeepOut(cx, cz)) { return p; }
  p.present = true; p.cx = cx; p.cz = cz; p.r = r; p.wp = wp;
  p.minY = wmWaterRow(biome, row, WM_R_MIN_Y);
  p.maxY = wmWaterRow(biome, row, WM_R_MAX_Y);
  p.maxSlope = wmWaterRow(biome, row, WM_R_MAX_SLOPE);
  return p;
}

// The column's pond set: the authored lake of its cell, then the rolled
// candidates of its own tile and of the neighbouring tiles whose edge is
// within the widest band any preset asks for (pondBand(): at most one
// neighbour per axis, so at most 2x2 tiles -- ValidateBiomeSet keeps every
// band under half the lattice). At most five pondRoll per column, on the
// column path only.
fn pondScan(x : i32, z : i32, seed : u32) -> PondSet {
  var s = pondSetNone();
  let a = waterSiteNear(x, z);
  if (a.present) { s = setPush(s, a); }
  let tile = pondTile();
  if (tile <= 0) { return s; }
  let band = pondBand();
  let pt = fdiv(x, tile);
  let pz = fdiv(z, tile);
  let lx = fmodp(x, tile);
  let lz = fmodp(z, tile);
  let sx = select(select(0, 1, lx >= tile - band), -1, lx < band);
  let sz = select(select(0, 1, lz >= tile - band), -1, lz < band);
  let p0 = pondRoll(pt, pz, seed);
  if (p0.present) { s = setPush(s, p0); }
  if (sx != 0) {
    let p1 = pondRoll(pt + sx, pz, seed);
    if (p1.present) { s = setPush(s, p1); }
  }
  if (sz != 0) {
    let p2 = pondRoll(pt, pz + sz, seed);
    if (p2.present) { s = setPush(s, p2); }
  }
  if (sx != 0 && sz != 0) {
    let p3 = pondRoll(pt + sx, pz + sz, seed);
    if (p3.present) { s = setPush(s, p3); }
  }
  return s;
}

// THE GATE: one landAt at the candidate's centre answers its waterline, the
// row's conditions and the slope gates. An authored lake takes only the
// waterline -- the author placed it.
//
// ---- THE SLOPE GATE: a tarn is PERCHED, not QUARRIED ----------------------
// The row's maxSlope is one test; the radius-aware half is the other. A slope
// of s drops s*r voxels across the radius; where that exceeds the bowl's own
// depth the ground UNDERCUTS the bowl and the floor is raw hillside again
// with a bed on it. `slope` is Q8, hence the 256.
fn pondGate(q : Pond, seed : u32) -> Pond {
  var p = q;
  if (!p.present) { return p; }
  let c = landAt(p.cx, p.cz, seed);
  p.surf = c.h;
  if (p.authored) { return p; }
  if (p.minY >= 0 && c.h < p.minY) { return pondNone(); }
  if (p.maxY >= 0 && c.h > p.maxY) { return pondNone(); }
  if (c.slope > p.maxSlope) { return pondNone(); }
  if (c.slope * p.r > (wmWaterI(p.wp, WM_W_DEPTH) - wmWaterI(p.wp, WM_W_RIM_DEPTH)) * 256) { return pondNone(); }
  return p;
}

// The berm: what makes containment structural. `past` is whole voxels beyond
// the rim. The inner CORE is forced flat at `surf + berm height` -- that is the
// wall the water cannot cross, and it is the only part the guarantee rests
// on. The rest ramps the lift linearly back to the natural ground so the bank
// blends; where the ground is already above the berm nothing moves at all.
// Height and width are the preset's.
fn bermLift(wp : u32, h : i32, surf : i32, past : i32) -> i32 {
  let bw = wmWaterI(wp, WM_W_BERM_W);
  let bh = wmWaterI(wp, WM_W_BERM_H);
  let core = max(bw / 4, 2);
  if (past < core) { return max(h, surf + bh); }
  let span = max(bw - core, 1);
  let t = span - (past - core);
  if (t <= 0) { return h; }
  return max(h, h + ((surf + bh - h) * t) / span);
}

// Does disc p cover column (x, z)?
fn inDisc(p : Pond, x : i32, z : i32) -> bool {
  let dx = x - p.cx;
  let dz = z - p.cz;
  return p.present && dx * dx + dz * dz <= p.r * p.r;
}

// "Is this column under a pond CANDIDATE?" -- pure arithmetic on the set,
// for the tree and cactus scans. Ungated: a tile that rolled a tarn its
// slope gate then refused still refuses a trunk in its disc, which costs a
// tree on a hillside now and then and no table read.
fn pondCovers(s : PondSet, x : i32, z : i32) -> bool {
  return inDisc(s.d0, x, z) || inDisc(s.d1, x, z) || inDisc(s.d2, x, z) ||
         inDisc(s.d3, x, z) || inDisc(s.d4, x, z);
}

// The pond whose DISC covers this column, gated, or none. Slot 0 (the
// authored lake, when present) wins its ground. ONE pondGate, so one landAt.
fn pondCover(s : PondSet, x : i32, z : i32, seed : u32) -> Pond {
  var cand = pondNone();
  if (inDisc(s.d4, x, z)) { cand = s.d4; }
  if (inDisc(s.d3, x, z)) { cand = s.d3; }
  if (inDisc(s.d2, x, z)) { cand = s.d2; }
  if (inDisc(s.d1, x, z)) { cand = s.d1; }
  if (inDisc(s.d0, x, z)) { cand = s.d0; }
  return pondGate(cand, seed);
}

// (bowl floor, water surface) at this column for a covering pond, or (-1,-1).
// landColumnBare carves the ground to the floor and fills (floor, surface]
// with the preset's fill.
fn bowlAt(p : Pond, x : i32, z : i32) -> vec2<i32> {
  if (!p.present) { return vec2<i32>(-1, -1); }
  let dx = x - p.cx;
  let dz = z - p.cz;
  let depth = bowlDepth(p.wp, p.r, dx * dx + dz * dz);
  return vec2<i32>(p.surf - depth, p.surf);
}

// A candidate's squared distance to the column if the column is within
// `band` voxels of the candidate's rim; "far" (0x7FFFFFFF) otherwise, and for
// an absent candidate. waterDistAt passes an authored nearWater band.
fn candD2(p : Pond, x : i32, z : i32, band : i32) -> i32 {
  if (!p.present) { return 0x7FFFFFFF; }
  let dx = x - p.cx;
  let dz = z - p.cz;
  let d2 = dx * dx + dz * dz;
  let outer = p.r + band;
  if (d2 > outer * outer) { return 0x7FFFFFFF; }
  return d2;
}
// The same against the candidate's OWN preset's shore/berm band (pondNear).
// An absent candidate's preset is 0, whose every word reads 0.
fn shoreD2(p : Pond, x : i32, z : i32) -> i32 {
  return candD2(p, x, z, wmWaterI(p.wp, WM_W_BAND));
}
// MIRROR-END height

// ---- the shore band ----
// The wet fringe OUTSIDE the disc: the berm, the mud ring, the marsh plants
// and the wet moss all key on the column's distance past the nearest rim.
//
// COST. Pure arithmetic on the column's pond set (pondScan already
// paid the table reads), then ONE pondGate (one landAt) on the nearest
// candidate. A nearest candidate that fails its gate leaves the column with
// no shore, even if a second candidate within its band would have passed --
// two ponds within one band of one column is rare, and the alternative is
// four landAt per column. Each candidate is tested against ITS OWN preset's
// band (shoreD2), so a marsh's 4 m fringe and a tarn's 2.4 m one coexist on
// one lattice.
//
// Returns (distance PAST the rim in voxels, water surface Y, the preset), or
// none when this column is not near any disc. Distance 0 is the first column
// outside the disc; the inside of the disc returns none (that is the bowl's
// job).
//
// TWO CONSUMERS, ONE SCAN. landColumnBare uses this for the BERM (applied to
// every column near a disc, whatever the biome or the ground height) and
// genColumn for the marsh FRINGE (the band narrowed by the treeline and the
// bluff test). `onShore` here means only "a disc is near enough to matter" --
// genColumn is what decides whether that is a shore.
// MIRROR-BEGIN height
struct Shore {
  onShore : bool,
  past    : i32,   // voxels beyond the rim (0 = first dry column)
  surf    : i32,   // the pond's water surface Y
  wp      : u32,   // the pond's preset; 0 = none
};

fn pondNear(s : PondSet, x : i32, z : i32, seed : u32) -> Shore {
  var sh : Shore;
  sh.onShore = false; sh.past = 0; sh.surf = -1; sh.wp = 0u;
  // Inside any candidate's disc is the pond, not the shore.
  if (pondCovers(s, x, z)) { return sh; }
  var best = 0x7FFFFFFF;
  var bestP = pondNone();
  let e0 = shoreD2(s.d0, x, z);
  if (e0 < best) { best = e0; bestP = s.d0; }
  let e1 = shoreD2(s.d1, x, z);
  if (e1 < best) { best = e1; bestP = s.d1; }
  let e2 = shoreD2(s.d2, x, z);
  if (e2 < best) { best = e2; bestP = s.d2; }
  let e3 = shoreD2(s.d3, x, z);
  if (e3 < best) { best = e3; bestP = s.d3; }
  let e4 = shoreD2(s.d4, x, z);
  if (e4 < best) { best = e4; bestP = s.d4; }
  let g = pondGate(bestP, seed);
  if (!g.present) { return sh; }

  // Integer distance past the rim, by bisection on the squared radius -- 8
  // steps over the winner's band, no sqrt and no f32 (rule 1). `past` is the
  // smallest k with d2 <= (r+k)^2, minus one, i.e. the number of whole voxels
  // of dry ground between this column and the waterline.
  var lo = 0;
  var hi = wmWaterI(g.wp, WM_W_BAND);
  for (var i = 0; i < 8; i++) {
    if (lo >= hi) { break; }
    let mid = (lo + hi) / 2;
    let rr = g.r + mid;
    if (best <= rr * rr) { hi = mid; } else { lo = mid + 1; }
  }
  sh.onShore = true;
  sh.past = max(lo - 1, 0);
  sh.surf = g.surf;
  sh.wp = g.wp;
  return sh;
}
// MIRROR-END height

// ---- trees ----
//
// SCALE, for everything below that is still authored as a number rather than
// baked (the cactus table): dimensions are written as DECIMETRES and converted
// with `* VOX_PER_M / 10`, never as bare voxel counts, so they stay metre-true
// at any voxel size. The baked trees are voxels already, and stamp 1:1 --
// correct only because the .svtree header records its bake scale and
// src/sim/treeatlas.cpp REFUSES an atlas whose scale is not this world's.
const VOX_PER_M : i32 = VOXELS_PER_M;

// ---- THE BAKED ATLAS ---------------------------------------------------------
//
// Worldgen answers for one voxel at a time with no way to walk a turtle, so a
// tree cannot be GROWN here. It is voxelized ONCE, offline, by
// assets/editor/treegen.js (a Weber-Penn branch skeleton stamped as round-cone
// SDFs, smooth-min'd leaf clumps, and a shading bake that picks each leaf
// voxel's lit/mid/dark material), and src/sim/treeatlas.h uploads the result.
// What is left here is a bounds check, one column lookup and a short run scan.
//
// THE EDITOR IS THE ONLY VOXELIZER, deliberately: a WGSL copy of the SDF and
// clump logic would be a second implementation that has to agree with the
// first. The price is that re-baking a species MOVES THE WORLD HASH -- one
// `--selftest --rebaseline`.
//
// WHAT THE ATLAS IS, as words:
//
//   header             16 words (TA_H_* below)
//   species directory  TA_SPECIES_WORDS per species (TA_S_*)
//   biome table        biomeCount x (1 + speciesCount) cumulative weights
//   condition table    biomeCount x speciesCount x TA_COND_WORDS (TA_C_*)
//   per species:       variant directory (TA_V_*), then per variant a
//                      column table of (runOffset, runCount) pairs indexed
//                      [lz * nx + lx], then the runs
//
//   run word: material (12 bits) | state (4) | y0 (11) | length (5)
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
const TA_H_COND_TABLE    : u32 = 9u;   // biome x species placement conditions
// One row per (biome, species): the biome file's tree-row `conditions`
// (treeatlas.h kCondWords). Voxels and Q8, -1 = unbounded where it says so.
const TA_COND_WORDS      : u32 = 8u;
const TA_C_MIN_Y          : u32 = 0u;
const TA_C_MAX_Y          : u32 = 1u;
const TA_C_MAX_SLOPE      : u32 = 2u;   // 0 or >= 1024 = unbounded
const TA_C_NEAR_WATER_MAX : u32 = 3u;   // -1 = unbounded
const TA_C_NEAR_WATER_MIN : u32 = 4u;   // 0 = off
const TA_C_PATCH_THRESH   : u32 = 5u;   // 0 = off

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
const WM_H_SPAWN_X       : u32 = 28u;
const WM_H_SPAWN_Z       : u32 = 29u;
const WM_H_POND_TILE     : u32 = 30u;   // the one pond lattice, voxels; 0 = no water rows
const WM_H_POND_BAND     : u32 = 31u;   // the widest shore/berm band any preset asks for
// ---- THE TERRAIN: map.json `terrain`, per map (worldmap.h kHTerrain*) ----
// Read inside the height mirror by name (wmTerrain / wmTerrainU), spelled the
// same in world.cpp. Voxels at the live voxel size, log2 shifts, Q8 counts.
const WM_H_TERRAIN_BASE_HEIGHT      : u32 = 32u;
const WM_H_TERRAIN_LANDFORM_RANGE   : u32 = 33u;
const WM_H_TERRAIN_RANGE_AMPLITUDE  : u32 = 34u;
const WM_H_TERRAIN_RANGE_LOG2       : u32 = 35u;
const WM_H_TERRAIN_HILL_AMPLITUDE   : u32 = 36u;
const WM_H_TERRAIN_HILL_LOG2        : u32 = 37u;
const WM_H_TERRAIN_DETAIL_AMPLITUDE : u32 = 38u;
const WM_H_TERRAIN_DETAIL_LOG2      : u32 = 39u;
const WM_H_TERRAIN_GRAIN_AMPLITUDE  : u32 = 40u;
const WM_H_TERRAIN_GRAIN_LOG2       : u32 = 41u;
const WM_H_TERRAIN_FBM_ATTEN        : u32 = 42u;
const WM_H_TERRAIN_HOME_Y           : u32 = 43u;
const WM_H_TERRAIN_HOME_R           : u32 = 44u;
const WM_H_TERRAIN_HOME_FADE        : u32 = 45u;
const WM_H_TERRAIN_SED_CEIL         : u32 = 46u;
const WM_H_TERRAIN_SED_FRACTION     : u32 = 47u;
const WM_H_TERRAIN_SED_STRIP        : u32 = 48u;
const WM_H_TERRAIN_SED_SLOPE        : u32 = 49u;
const WM_H_TERRAIN_SED_MAX          : u32 = 50u;
const WM_H_TERRAIN_SED_TOPSOIL      : u32 = 51u;
const WM_H_TERRAIN_TREELINE         : u32 = 52u;
const WM_H_TERRAIN_REF_VPM          : u32 = 53u;
fn wmTerrain(w : u32) -> i32 { return bitcast<i32>(worldMap[w]); }
fn wmTerrainU(w : u32) -> u32 { return worldMap[w]; }
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
const WM_S_PRESET        : u32 = 8u;    // water site: 1 + preset index
const WM_S_PAD_Y         : u32 = 9u;    // pad / stamp site: bare ground at the centre (baked)
const WM_SITE_STAMP      : u32 = 1u;
const WM_SITE_WATER      : u32 = 2u;    // an authored lake (worldmap.h kSiteWater)
const WM_STAMP_HDR_WORDS : u32 = 4u;
const WM_STAMP_NX        : u32 = 0u;
const WM_STAMP_NY        : u32 = 1u;
const WM_STAMP_NZ        : u32 = 2u;
const WM_STAMP_COLUMNS   : u32 = 3u;
const WM_B_WORDS         : u32 = 48u;
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
const WM_B_FLAGS         : u32 = 12u;
const WM_B_MAX_COVER_H   : u32 = 13u;
const WM_B_TREE_CHANCE_Q16 : u32 = 14u;   // thinning on the ONE tree lattice, Q16
// the biome's water rows, cave flora and cactus numbers (worldmap.h kB_* 16..21)
const WM_B_WATER_COUNT   : u32 = 16u;   // the biome's water rows (WM_R_*)
const WM_B_CAVE_MUSHROOM_CHANCE : u32 = 17u;
const WM_B_CAVE_CRYSTAL_CHANCE  : u32 = 18u;
const WM_B_CACTUS_CHANCE : u32 = 19u;
const WM_B_SAGUARO_FRACTION : u32 = 20u;
const WM_B_WATER_OFF     : u32 = 21u;
// the biome's terrain record -- nine Q14 relief-curve knots and three Q8
// multipliers on the map's hill / detail / grain octaves (biomeCurve /
// biomeReliefAt, blended over the four map cells around a column).
const WM_B_CURVE_KNOT0   : u32 = 22u;   // ..30u
const WM_B_HILL_MUL      : u32 = 31u;
const WM_B_DETAIL_MUL    : u32 = 32u;
const WM_B_GRAIN_MUL     : u32 = 33u;
// The SOLID this biome's loose cover becomes where the ground is too steep to
// hold a powder (worldmap.h kB_FirmCover, biomes/<name>.json cover.firmSkin).
// 0 = unauthored; coverFirmMat below falls back to the subsoil or to stone.
const WM_B_FIRM_COVER    : u32 = 34u;
const WM_C_WORDS         : u32 = 12u;
const WM_C_MAT           : u32 = 0u;
const WM_C_HEAD          : u32 = 1u;
const WM_C_CHANCE        : u32 = 2u;
const WM_C_HEIGHT        : u32 = 3u;
const WM_C_MIN_Y         : u32 = 4u;
const WM_C_MAX_Y         : u32 = 5u;
const WM_C_MAX_SLOPE     : u32 = 6u;
const WM_C_PATCH_THRESH  : u32 = 7u;
const WM_C_NEAR_WATER_MAX : u32 = 8u;   // voxels from a pond rim, -1 = unbounded
const WM_C_NEAR_WATER_MIN : u32 = 9u;   // at least this far from a rim, 0 = off
const WM_C_CANOPY_MIN    : u32 = 10u;   // undergrowthSite cover band, 0..255; 0 / 255 = unbounded
const WM_C_CANOPY_MAX    : u32 = 11u;
// the biome's water rows (worldmap.h kR_*): preset, thinning chance on
// the one pond lattice (Q16), and the row's conditions at the pond centre.
const WM_R_WORDS         : u32 = 8u;
const WM_R_PRESET        : u32 = 0u;
const WM_R_CHANCE_Q16    : u32 = 1u;
const WM_R_MIN_Y         : u32 = 2u;
const WM_R_MAX_Y         : u32 = 3u;
const WM_R_MAX_SLOPE     : u32 = 4u;
const WM_BF_GROUND_FLORA : u32 = 1u;
const WM_BF_CACTI        : u32 = 2u;
const WM_BF_SAND_CAP     : u32 = 4u;
const WM_BF_CANOPY_ROWS  : u32 = 8u;    // a cover row bounds the canopy cover; scan the trees once per column
// the water preset table (worldmap.h kW_* / kP_*): the FLORA half of
// assets/water/<name>.json and, from word 22, the GEOMETRY half.
// Depths are voxels of water over the bed, heights cells from the bed
// (aquatic) or the ground (shore); chances 1-in-N, 0 = off.
const WM_W_WORDS               : u32 = 64u;
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
// the geometry half, voxels: see worldmap.h for the d^2-parametrised
// profile knots and why there are seventeen of them
const WM_W_RADIUS_MIN          : u32 = 22u;
const WM_W_RADIUS_SPAN         : u32 = 23u;
const WM_W_DEPTH               : u32 = 24u;
const WM_W_RIM_DEPTH           : u32 = 25u;
const WM_W_BERM_H              : u32 = 26u;
const WM_W_BERM_W              : u32 = 27u;
const WM_W_SHORE_BAND          : u32 = 28u;
const WM_W_SHORE_LIFT          : u32 = 29u;
const WM_W_MUD_WIDTH           : u32 = 30u;
const WM_W_MUD_MAT             : u32 = 31u;
const WM_W_BED_SHALLOW         : u32 = 32u;
const WM_W_BED_DEEP            : u32 = 33u;
const WM_W_BED_SHALLOW_DEPTH   : u32 = 34u;
const WM_W_BED_THICKNESS       : u32 = 35u;
const WM_W_BED_SUBSTRATE       : u32 = 36u;
const WM_W_BAND                : u32 = 40u;
const WM_W_KNOTS               : u32 = 41u;
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
// ---- the water preset table (flora, then geometry) ---------------------------
// A pond wears the preset of the row that ROLLED it or the authored site that
// PLACED it: `Pond.wp` / `Shore.wp` / `Col.wp` carry the 1-based index
// down to every reader. 0 = no pond and every read below is 0, which turns
// every chance off -- no shore plants, no pond life, no moss, no bowl.
fn wmWater(p : u32, w : u32) -> u32 {
  if (p == 0u || p > worldMap[WM_H_WATER_COUNT]) { return 0u; }
  return worldMap[worldMap[WM_H_WATER_RECORDS] + (p - 1u) * WM_W_WORDS + w];
}
fn wmWaterI(p : u32, w : u32) -> i32 { return bitcast<i32>(wmWater(p, w)); }
fn wmShore(p : u32, i : u32, w : u32) -> u32 {
  return worldMap[wmWater(p, WM_W_SHORE_OFF) + i * WM_P_WORDS + w];
}
// A biome's water rows (WM_R_*), and the one pond lattice + scan band from
// the header. These, `wmWaterI` and `waterKnot` are what the height mirror
// reads the table through: they sit OUTSIDE the mirror on both sides and
// world.cpp spells the same names over WorldMapData::water, so the mirrored
// pondRoll / bowlDepth / pondNear are token-identical and the `terrain`
// gate's C1 is the per-voxel proof they read the same integers.
fn wmWaterRowCount(b : u32) -> u32 { return wmBiome(b, WM_B_WATER_COUNT); }
fn wmWaterRow(b : u32, i : u32, w : u32) -> i32 {
  return bitcast<i32>(worldMap[wmBiome(b, WM_B_WATER_OFF) + i * WM_R_WORDS + w]);
}
// The pond lattice is a PRELUDE CONSTANT (POND_TILE, gpu/resources.cpp
// ShaderConstantPrelude from the same WorldMapData word the buffer carries at
// WM_H_POND_TILE): worldgen divides by it in ~1000 inlined places, and a
// division by a runtime word there took the driver's compile from minutes to
// never. Behind a function so the mirrored code spells `pondTile()` on both
// sides; the driver folds it.
fn pondTile() -> i32 { return POND_TILE; }
fn pondBand() -> i32 { return i32(worldMap[WM_H_POND_BAND]); }
// ---- THE UNROLL FENCE ------------------------------------------------------
// An OPAQUE ZERO: always 0 at runtime (no authored pond tile or shore band
// reaches 1<<20 voxels = 52 km), but unprovable at compile time. Added to the
// start of a constant-trip-count loop it changes nothing the GPU executes and
// everything the driver's optimizer does: a loop whose trip count it cannot
// prove is a loop it cannot UNROLL, so the loop body — for the loops below,
// a full landColumn or treeInfoAt inline, ~half of worldgen each — exists in
// the kernel ONCE instead of 4/16/25 times. The pond table grew landColumn
// past the driver's superlinear compile cliff, and the `far` entry point
// (4-corner blocker scan x 16x4 cell loop over that code) went from part of a
// slow minute to tens of minutes at ~10 GB — a load screen that never ends.
// Same trick as moving the curve knots off the prelude (2026-09-02): starve
// the const-folder, keep the values.
fn unrollFence() -> i32 {
  return i32((worldMap[WM_H_POND_TILE] | worldMap[WM_H_POND_BAND]) >> 20u);
}
fn unrollFenceU() -> u32 {
  return (worldMap[WM_H_POND_TILE] | worldMap[WM_H_POND_BAND]) >> 20u;
}
// Knot k (0..16) of the preset's depth profile, Q8; two per word, low first.
fn waterKnot(p : u32, k : u32) -> i32 {
  return i32((wmWater(p, WM_W_KNOTS + (k >> 1u)) >> ((k & 1u) * 16u)) & 0xFFFFu);
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
// Chebyshev distance from a column to the harness box, 0 inside it: the
// mirrored landAt keeps the box's ground calm (its own coarse-octave fade,
// beside the spawn's) so the fixtures stand on the ground their gates were
// written against wherever the spawn site goes. A map with no pad (x1 < x0)
// reports "far", which switches that fade off. Outside the height mirror on
// both sides; world.cpp spells the same name.
fn harnessOutside(x : i32, z : i32) -> i32 {
  let x0 = i32(worldMap[WM_H_HARNESS_X0]);
  let x1 = i32(worldMap[WM_H_HARNESS_X1]);
  if (x1 < x0) { return 1073741824; }
  let z0 = i32(worldMap[WM_H_HARNESS_Z0]);
  let z1 = i32(worldMap[WM_H_HARNESS_Z1]);
  return max(max(max(x0 - x, x - x1), max(z0 - z, z - z1)), 0);
}

// ---- THE SPAWN SITE -----------------------------------------------------------
// Where the player starts (map.json sites[], kind "spawn") and the centre of
// the calm home area. Read from the header so the C++ twin (world.cpp
// spawnCentre, reading worldmap::CurrentWorldMap()) sees the same column;
// the mirrored landAt calls it by name. Not a Tier-A/B question: it is an
// authored point, no seed.
fn spawnCentre() -> vec2<i32> {
  return vec2<i32>(i32(worldMap[WM_H_SPAWN_X]), i32(worldMap[WM_H_SPAWN_Z]));
}

// ---- THE SITE TABLE ---------------------------------------------------------
// `wmSiteAt` is the per-column cost: one plane read, 0 = no site. A site's
// record gives its centre, footprint radius, pad margin and stamp block. The
// pad (`sitePadAt`, inside the height mirror) levels the ground under the
// footprint to the height at the site's centre and ramps it back over the
// margin -- Lin's "shape the terrain toward the structure". The stamp (`wmStampCell`) is a
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
// The bare ground under a pad / stamp site's CENTRE: landColumnBare's h there,
// which the pad levels its footprint to and the stamp stands on. Seed-dependent
// but position-fixed, so LoadWorldMap bakes it once for the load seed
// (worldmap.h kS_PadY) instead of every column of a pad margin and every voxel
// of a stamp re-running the octave ladder at the centre -- and, the bigger
// half, instead of one more inlined landColumnBare in every caller the driver
// compiles. The lab slab is the one world whose centre column is not that
// terrain (landColumnBare returns the slab there), so it answers the slab, as
// the per-call form did. world.cpp spells the same helper.
fn sitePadY(sid : u32) -> i32 {
  if (T.labMode != 0u) { return LAB_SLAB_Y; }
  return wmSiteI(sid, WM_S_PAD_Y);
}
// A water site keeps out by its DISC plus its shore/berm band, not by
// its cells: a lake's cells are four 102 m squares and a stamp's rule would
// bald the forest around every tarn on the map. Every other kind keeps its
// cells, as before.
fn siteKeepOut(x : i32, z : i32) -> bool {
  if (inHarness(x, z)) { return true; }
  let sid = wmSiteAt(x, z);
  if (sid == 0u) { return false; }
  if (u32(wmSiteI(sid, WM_S_KIND)) != WM_SITE_WATER) { return true; }
  let dx = x - wmSiteI(sid, WM_S_X);
  let dz = z - wmSiteI(sid, WM_S_Z);
  let reach = wmSiteI(sid, WM_S_RADIUS) + wmSiteI(sid, WM_S_PAD_MARGIN);
  return dx * dx + dz * dz <= reach * reach;
}
// The template voxel this world cell would carry, MAT_AIR if none: the
// stamp's footprint is centred on the site, its bottom row sits one above
// the pad height (`padY`, sitePadY: the same baked height sitePadAt levels to).
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
fn wmSiteTopAt(x : i32, z : i32) -> i32 {
  let sid = wmSiteAt(x, z);
  if (sid == 0u) { return -1048576; }
  let blk = u32(wmSiteI(sid, WM_S_STAMP_OFF));
  if (blk == 0u) { return -1048576; }
  return sitePadY(sid) + 1 + i32(worldMap[blk + WM_STAMP_NY]);
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
  return (d * wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE)) >> (8u + l);
}
fn mapLandformGz(x : i32, z : i32) -> i32 {
  if (worldMap[WM_H_LANDFORM_PLANE] == 0u) { return 0; }
  let l = worldMap[WM_H_CELL_LOG2];
  let half = 1 << (l - 1u);
  let cx = (x - half + (i32(worldMap[WM_H_ORIGIN_X]) << l)) >> l;
  let cz = (z - half + (i32(worldMap[WM_H_ORIGIN_Z]) << l)) >> l;
  let d = wmLandformCellQ8(cx, cz + 1) - wmLandformCellQ8(cx, cz);
  return (d * wmTerrain(WM_H_TERRAIN_LANDFORM_RANGE)) >> (8u + l);
}
// The painted cell's biome with NO warp and no seed: what decides whose water
// rows roll at a pond tile (pondRoll). The warped read below costs two noise
// samples, and pondRoll is inlined into every pondScan copy -- the warp there
// was the difference between a 200 s worldgen compile and one that never
// finished. Tier A: the cell, not the wobbled edge; a pond half a cell from a
// biome border may roll the neighbour's rows. world.cpp spells the same.
fn biomeCellAt(x : i32, z : i32) -> u32 {
  let plane = worldMap[WM_H_BIOME_PLANE];
  if (plane == 0u) { return 0u; }
  let c = wmCellOf(x, z);
  if (!wmInside(c)) { return worldMap[WM_H_OCEAN_BIOME]; }
  return wmPlaneAt(plane, c.x, c.y);
}
// THE BIOME, from the map: Tier A (the plane) is seed-independent; the
// boundary warp is Tier B and takes the seed, so a region's EDGE wanders per
// seed by up to warpAmp (<= cell/4, enforced by the loader) while its centre
// stays put. Outside the painted planes the world is ocean.
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
// tree or none. Some overlap is good — that is what closes a canopy — but it
// has to be overlap, not merger, and each biome decides how much it gets.
//
// ONE LATTICE, THINNED PER BIOME. TREE_TILE, TREE_SCAN and TREE_CAND_MAX are
// worldgen PRELUDE CONSTANTS derived at load (sim/treeatlas.h TreeLattice): the tile is the FINEST `trees.tile` any
// tree-growing biome authors, the scan and the candidate cap follow from it
// and the atlas's widest reach. A biome authored coarser than the lattice is
// thinned on it to its own density — WM_B_TREE_CHANCE_Q16, rolled in
// treeInfoAt — so trees per hectare are what its page predicts and only the
// jitter pattern differs. A per-biome lattice was rejected on purpose: the
// candidate scan below looks at tiles whose biome it has not yet paid to
// know, so every column has to agree on where the tiles are.
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
// anywhere in the world, at any seed — one compare replaces the whole scan
// (treeAt per cell; genChunk / `far` per column, where the scan's own
// `cands.top` is the tighter bound once it has run).
fn treeMaxTop() -> i32 { return treeline() - 1 + treeMaxAbove(); }

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
  // The (biome, species) row's near-water bounds (-1 / 0 = unbounded), for
  // treePondOk: the one gate that depends on the ASKING column's pond set.
  nwMax   : i32,
  nwMin   : i32,
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

// ---- distance to water, for the authored `nearWater` conditions -----------
// How many whole voxels of dry ground lie between column (x,z) and the nearest
// pond CANDIDATE's rim within `band` voxels: 0 = the first column outside the
// rim, -1 = no rim within the band, or the column is inside a disc (that is
// water, not near it). The same nearest-candidate walk as pondNear, with the
// band supplied by the caller instead of the preset's: a tree row's "within
// 6 m of water" is wider than any shore band. Pure arithmetic on the column's
// pond set, and only reached by a row that authors a water condition.
// The pond set BY POINTER (the scans' form, like `trees`): a by-value PondSet
// in a function inlined 25 times per column is 51 words copied 25 times, and
// the driver's compile of `far` died of it. Nothing here indexes it
// dynamically.
fn pondCoversP(ponds : ptr<function, PondSet>, x : i32, z : i32) -> bool {
  return inDisc((*ponds).d0, x, z) || inDisc((*ponds).d1, x, z) || inDisc((*ponds).d2, x, z) ||
         inDisc((*ponds).d3, x, z) || inDisc((*ponds).d4, x, z);
}
fn waterDistAt(ponds : ptr<function, PondSet>, x : i32, z : i32, band : i32) -> i32 {
  if (band <= 0) { return -1; }
  // Pure arithmetic on the column's pond set: inside any candidate's
  // disc is water; otherwise the nearest candidate whose rim is within `band`.
  // UNGATED, like pondCoversP: a placement condition measures distance to the
  // nearest pond CANDIDATE and never pays a table read inside the tree scan.
  if (pondCoversP(ponds, x, z)) { return -1; }
  var best = 0x7FFFFFFF;
  var bestR = 0;
  let c0 = candD2((*ponds).d0, x, z, band);
  if (c0 < best) { best = c0; bestR = (*ponds).d0.r; }
  let c1 = candD2((*ponds).d1, x, z, band);
  if (c1 < best) { best = c1; bestR = (*ponds).d1.r; }
  let c2 = candD2((*ponds).d2, x, z, band);
  if (c2 < best) { best = c2; bestR = (*ponds).d2.r; }
  let c3 = candD2((*ponds).d3, x, z, band);
  if (c3 < best) { best = c3; bestR = (*ponds).d3.r; }
  let c4 = candD2((*ponds).d4, x, z, band);
  if (c4 < best) { best = c4; bestR = (*ponds).d4.r; }
  if (best == 0x7FFFFFFF) { return -1; }
  // Bisection on the squared radius, as pondNear does it: the smallest k with
  // d2 <= (r+k)^2, minus one. Integer throughout (rule 1).
  var lo = 0;
  var hi = band;
  for (var i = 0; i < 10; i++) {
    if (lo >= hi) { break; }
    let mid = (lo + hi) / 2;
    let rr = bestR + mid;
    if (best <= rr * rr) { hi = mid; } else { lo = mid + 1; }
  }
  return max(lo - 1, 0);
}
// One compare per authored bound. `d` is waterDistAt's answer; a row with
// neither bound never calls it.
fn nearWaterOk(d : i32, nearMax : i32, nearMin : i32) -> bool {
  if (nearMax >= 0 && (d < 0 || d >= nearMax)) { return false; }
  if (nearMin > 0 && d >= 0 && d < nearMin) { return false; }
  return true;
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
//
// SPLIT IN TWO AT THE POND SET. Every gate but two is a function of the TILE
// alone; those two -- "no trunk inside a pond disc" and a row's near-water
// bound -- read the ASKING column's pond candidates, which differ from column
// to column. treeInfoBare is the tile half, treePondOk the column half, and a
// tree is present exactly when both say so: every gate is a `return` of a
// non-present tree, so their order cannot change the answer, and no reader
// looks at a non-present tree's other fields. The split is what lets genChunk
// and `cols` evaluate each tile ONCE per chunk (the tree tile cache, below)
// instead of once per column that scans it.
fn treeInfoAt(s : TreeSite, land : Land, seed : u32, ponds : ptr<function, PondSet>) -> Tree {
  var t = treeInfoBare(s, land, seed);
  if (t.present && !treePondOk(&t, ponds)) { t.present = false; }
  return t;
}
// The column half: the trunk is not in a pond candidate's disc, and a row
// that authors a water distance has it.
fn treePondOk(t : ptr<function, Tree>, ponds : ptr<function, PondSet>) -> bool {
  if (pondCoversP(ponds, (*t).wx, (*t).wz)) { return false; }
  let nwMax = (*t).nwMax;
  let nwMin = (*t).nwMin;
  if (nwMax >= 0 || nwMin > 0) {
    let d = waterDistAt(ponds, (*t).wx, (*t).wz, max(nwMax, nwMin));
    if (!nearWaterOk(d, nwMax, nwMin)) { return false; }
  }
  return true;
}
// The tile half: everything else.
fn treeInfoBare(s : TreeSite, land : Land, seed : u32) -> Tree {
  var t : Tree;
  t.present = false;
  t.sp = -1; t.varOff = 0u;
  t.wx = s.wx; t.wz = s.wz; t.base = land.h;
  t.rot = 0u; t.mir = false;
  t.reach = 0; t.above = 0; t.crownR = 0; t.shade = 0; t.autumn = false;
  t.rnd = s.hsh;
  t.nwMax = -1; t.nwMin = 0;

  let ns = taSpeciesCount();
  if (ns <= 0) { return t; }            // no atlas: a legal, treeless world
  // The master switch only: a tree is a landmark, so `groundCover` leaves it.
  if (!VEGETATION) { return t; }        // debug.vegetation = 0: no trees

  let hsh = s.hsh;
  let h = land.h;

  // No trees on snowfields, in ponds (treePondOk), or over the selftest
  // fixture sites.
  if (h >= treeline()) { return t; }
  // (The spawn clearing is checked AFTER the species draw, where the crown's
  // real width is known — see the note at that test.)

  // Density by biome, from the biome's record: its authored `trees.density`
  // and `trees.tile` folded into ONE Q16 chance on the world's shared lattice
  // (worldmap.h kB_TreeChanceQ16; the derivation is at the TREE_TILE note
  // above). Forest at 5.6 m / 48 % rolls 0.48 per tile; a meadow at 14.4 m /
  // 22 % rolls 0.033 per fine tile, which is the same 22 % of its own coarse
  // tiles by area. The top 16 bits of the site hash: bits 3.. and 9.. are
  // the trunk jitter, and for any tile up to 256 voxels the three slices are
  // disjoint.
  let biome = biomeAt(t.wx, t.wz, seed);
  let roll = hsh >> 16u;
  let chance = wmBiome(biome, WM_B_TREE_CHANCE_Q16);
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
  // ---- THE BIOME'S OWN ROW CONDITIONS (assets/biomes/<name>.json trees.
  // species[].conditions), per (biome, species), from the atlas's condition
  // table. The same shape as the species gates and the same rule: a gated-out
  // pick grows nothing. One compare each on numbers already in hand; the
  // water distance is the one that costs a lookup, and only a row that
  // authors it pays.
  {
    let cb = treeAtlas[TA_H_COND_TABLE] +
             (biome * u32(ns) + u32(sp)) * TA_COND_WORDS;
    let rMinY = bitcast<i32>(treeAtlas[cb + TA_C_MIN_Y]);
    let rMaxY = bitcast<i32>(treeAtlas[cb + TA_C_MAX_Y]);
    if (rMinY >= 0 && h < rMinY) { return t; }
    if (rMaxY >= 0 && h > rMaxY) { return t; }
    let rSlope = i32(treeAtlas[cb + TA_C_MAX_SLOPE]);
    if (rSlope > 0 && rSlope < 1024 && land.slope > rSlope) { return t; }
    // The water distance is the column's to ask (treePondOk); the bounds ride
    // on the tree.
    t.nwMax = bitcast<i32>(treeAtlas[cb + TA_C_NEAR_WATER_MAX]);
    t.nwMin = i32(treeAtlas[cb + TA_C_NEAR_WATER_MIN]);
    // The biome's patch field, sampled at the trunk with a tree-only offset
    // so a stand of this species does not line up with a cover row's lattice.
    let rPatch = i32(treeAtlas[cb + TA_C_PATCH_THRESH]);
    if (rPatch > 0) {
      let pLog2 = wmBiome(biome, WM_B_PATCH_LOG2);
      let pm = vnoise2d(t.wx + 307, t.wz - 911, pLog2, seed ^ 0xD5F7u).n >> 6;
      if (pm <= rPatch) { return t; }
    }
  }

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
  // 1-in-N trees, carried into the atlas as TA_S_AUTUMN: the number on the
  // tree page is the number the world rolls.
  let ac = taSpecies(sp, TA_S_AUTUMN);
  t.autumn = ac != 0u && ((h3 >> 14u) % max(ac, 1u)) == 0u;
  t.present = true;
  return t;
}

// The one-shot form, for callers with no reject of their own to do first
// (undergrowthSite, treeCanopyAt).
fn treeInfo(tx : i32, tz : i32, seed : u32, ponds : ptr<function, PondSet>) -> Tree {
  let s = treeSite(tx, tz, seed);
  return treeInfoAt(s, landAt(s.wx, s.wz, seed), seed, ponds);
}

// ---- integer geometry for the cactus arms (cactusArm / cactusCell) ----------
// Integer sine on a 256-step circle, returning -256..256: a parabolic
// approximation, exact in integers -- NO f32, because this feeds voxel
// placement and rule 1 rests on avoiding vendor-divergent float math. Its
// error (a few percent) is a fraction of a voxel over an arm and identical
// on every machine.
fn isin(a : i32) -> i32 {
  let p = a & 255;                     // 0..255 == 0..2pi
  let half = p & 127;                  // 0..127 == 0..pi
  // parabola 4h(128-h)/128^2 peaks at 1 for h=64; scale to 256
  let v = (4 * half * (128 - half) * 256) / (128 * 128);
  return select(v, -v, p >= 128);
}

// Squared distance from point p to the segment a->b, all in voxels. The
// projection parameter is quantized to 1/1024 so every product stays inside
// i32 (no i64 in WGSL) for segments up to ~200 voxels; rounding the axis by a
// fraction of a voxel is invisible and identical on every machine.
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
// TREE_CAND_MAX IS ENOUGH, and it is derived rather than assumed. A site sits
// in the middle half of its tile, so tile `t` puts its trunk in
// [TILE*t + TILE/4, TILE*t + 3*TILE/4); a tile can reach column x only if that
// range meets [x - reach, x + reach], which spans 2*reach + TILE/2 - 1 voxels
// of tile origin and therefore covers at most (2*reach + TILE/2 - 1)/TILE + 1
// tiles per axis; the cap is that count squared, and TREE_SCAN is the number
// of tiles per side whose trunk range can meet the window at all. At a tile
// of 144 and the great oak's 115 that was three per axis — nine; at the
// forest's 56 it is five — twenty-five. Both `reach` and the tile are ASSET
// DATA, so LoadTreeAtlas (src/sim/treeatlas.h TreeLatticeFor) derives all
// three numbers at load, hands them to every shader as prelude constants,
// and REFUSES an atlas past kTreeCandPerAxisCap: past the cap the shader
// would silently drop a candidate, and the symptom is a canopy missing from
// some columns and present on others.

// The compact per-column form. Deliberately EIGHT WORDS per candidate: this
// array lives on the function stack of genChunk and a wide struct here is
// scratch traffic on every worldgen thread. Everything a per-cell test needs
// and nothing it does not — the species metadata stays in the atlas, where
// the two scans that want it (undergrowth, far canopy) read it directly.
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
  t   : array<TreeCand, TREE_CAND_MAX>,   // a prelude const, so it sizes at load
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

// One tile's tree offered to column (x, z)'s candidate set: `t` is the tile's
// tree with the column's pond gates applied. The scan's per-tile half, shared
// by the direct scan and the tile-cache scan so the two cannot disagree.
fn treeCandAdd(c : ptr<function, TreeCands>, t : ptr<function, Tree>, x : i32, z : i32) {
  if (!(*t).present) { return; }
  // Now the species' OWN reach, which is what actually decides.
  if (abs(x - (*t).wx) > (*t).reach || abs(z - (*t).wz) > (*t).reach) { return; }

  // The column lookup, hoisted. A tree whose baked grid has nothing in this
  // column is not a candidate at all: the atlas is the whole tree, so an
  // empty column can contribute nothing.
  let l = treeLocalXZ(*t, x - (*t).wx, z - (*t).wz);
  let vo = (*t).varOff;
  let nx = i32(treeAtlas[vo + TA_V_NX]);
  let nz = i32(treeAtlas[vo + TA_V_NZ]);
  if (l.x < 0 || l.y < 0 || l.x >= nx || l.y >= nz) { return; }
  let ci = treeAtlas[vo + TA_V_COLUMNS] + u32(l.y * nx + l.x) * 2u;
  let cnt = treeAtlas[ci + 1u];
  if (cnt == 0u) { return; }

  if ((*c).n >= TREE_CAND_MAX) { return; }   // see the derivation at TreeCands
  var e : TreeCand;
  e.wx = (*t).wx; e.wz = (*t).wz; e.base = (*t).base;
  e.ny = i32(treeAtlas[vo + TA_V_NY]);
  // Local y 0 sits one voxel ABOVE the ground: genCellIn only asks about
  // cells with y > h, so a row at y == base could never be reached and
  // baking one would waste a layer of every variant.
  e.vtop = (*t).base + 1 + e.ny;
  e.colOff = treeAtlas[ci];
  e.colCnt = cnt;
  e.leafSw = select(0u,
      treeAtlas[TA_H_SPECIES_DIR] + u32((*t).sp) * TA_SPECIES_WORDS,
      (*t).autumn);
  (*c).t[(*c).n] = e;
  (*c).n = (*c).n + 1;
  (*c).top = max((*c).top, e.vtop);
}

fn treeCandsInto(c : ptr<function, TreeCands>, x : i32, z : i32, seed : u32, ponds : ptr<function, PondSet>) {
  (*c).n = 0;
  (*c).top = -1048576;
  if (taSpeciesCount() <= 0) { return; }
  let maxReach = treeMaxReach();
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN + unrollFence(); oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN + unrollFence(); ox <= TREE_SCAN; ox++) {
      // Horizontal reject on the trunk site alone (one hash), against the
      // WIDEST species in the atlas. Only the tiles whose trunk can reach
      // this column go on to the noise queries below; on a coarse lattice
      // that is the inner ring, on a fine one most of the scan.
      let s = treeSite(tx + ox, tz + oz, seed);
      if (abs(x - s.wx) > maxReach || abs(z - s.wz) > maxReach) { continue; }
      var t = treeInfoAt(s, landAt(s.wx, s.wz, seed), seed, ponds);
      treeCandAdd(c, &t, x, z);
    }
  }
}

// ---- THE TREE TILE CACHE: a chunk's tiles, evaluated once per workgroup ------
//
// Every column of a 16x16 chunk scans the same few tiles: its own tile +-
// TREE_SCAN, and at a 56-voxel lattice all 256 columns share at most 6x6 of
// them. The tile's tree -- its site, the landAt under it, the biome draw, the
// species and variant, every gate but the column's pond gates -- is a function
// of the TILE alone (treeInfoBare), so genChunk and `cols` evaluate each
// tile once into workgroup memory and every column's scan reads it, instead of
// each column re-deriving ~25 trees (a landAt and a biome draw apiece). The
// pond gates stay per column (treePondOk): they read the ASKING column's pond
// set, and that is the half that genuinely differs between columns.
//
// The tile window of a chunk at (bx, bz): for a column x in [bx, bx + CHUNK),
// fdiv(x, TREE_TILE) spans at most (CHUNK - 1) / TREE_TILE + 1 tiles, and the
// scan adds TREE_SCAN on each side.
const WG_TREE_AXIS : i32 = (i32(CHUNK) - 1) / TREE_TILE + 2 + 2 * TREE_SCAN;
const WG_TREE_TILES : i32 = WG_TREE_AXIS * WG_TREE_AXIS;
// The workgroup memory it costs, bounded well inside the 16 KiB every Vulkan
// device guarantees: a Tree is 16 words, so 128 tiles is 8 KiB. The tree
// atlas loader bounds TREE_SCAN (kTreeCandPerAxisCap); only a lattice finer
// than ~6 voxels with tiny trees could reach this, and it fails the compile
// loudly rather than overflowing.
const_assert WG_TREE_TILES <= 128;
var<workgroup> wgTree : array<Tree, WG_TREE_TILES>;

fn wgTreeOrigin(b : i32) -> i32 { return fdiv(b, TREE_TILE) - TREE_SCAN; }

// Fill the cache for the chunk whose lowest corner column is (bx, bz). Called
// by the whole workgroup in uniform control flow; the caller barriers after.
fn treeTilesFill(li : u32, bx : i32, bz : i32, seed : u32) {
  let tx0 = wgTreeOrigin(bx);
  let tz0 = wgTreeOrigin(bz);
  for (var i = i32(li); i < WG_TREE_TILES; i += 64) {
    let s = treeSite(tx0 + i % WG_TREE_AXIS, tz0 + i / WG_TREE_AXIS, seed);
    wgTree[i] = treeInfoBare(s, landAt(s.wx, s.wz, seed), seed);
  }
}

// The cached tile (tx, tz), pond-gated for the asking column.
fn wgTreeFor(tx : i32, tz : i32, bx : i32, bz : i32, ponds : ptr<function, PondSet>) -> Tree {
  var t = wgTree[(tz - wgTreeOrigin(bz)) * WG_TREE_AXIS + (tx - wgTreeOrigin(bx))];
  if (t.present && !treePondOk(&t, ponds)) { t.present = false; }
  return t;
}

// treeCandsInto, reading the cache of the chunk at (bx, bz). Same tiles, same
// order, same rejects: the maxReach reject is on the site, which the cached
// tree carries (t.wx / t.wz).
fn treeCandsFromTiles(c : ptr<function, TreeCands>, x : i32, z : i32, bx : i32, bz : i32,
                      ponds : ptr<function, PondSet>) {
  (*c).n = 0;
  (*c).top = -1048576;
  if (taSpeciesCount() <= 0) { return; }
  let maxReach = treeMaxReach();
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN + unrollFence(); oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN + unrollFence(); ox <= TREE_SCAN; ox++) {
      let ti = (tz + oz - wgTreeOrigin(bz)) * WG_TREE_AXIS + (tx + ox - wgTreeOrigin(bx));
      let sx = wgTree[ti].wx;
      let sz = wgTree[ti].wz;
      if (abs(x - sx) > maxReach || abs(z - sz) > maxReach) { continue; }
      var t = wgTreeFor(tx + ox, tz + oz, bx, bz, ponds);
      treeCandAdd(c, &t, x, z);
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

// NO IMPLICIT DECORATION HANGS OFF A TREE: the .svtree atlas is the WHOLE
// tree, so what the tuner's Trees tab renders is exactly what the world grows.
// A decoration that belongs on a tree belongs in treegen.js.
//
// The y-dependent half. Order is by tile index, a fixed priority, never
// dispatch order (rule 1) — candidates were appended in that order, so first
// non-air wins.
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

// The one-shot form, for a caller with no candidate set (genCellIn's
// `treeValid = false` spelling: the far entries' surface-skin lookup). ONE
// implementation of the rule, split the way genColumn/genCellIn are split —
// not a second copy that has to agree.
//
// The world-wide vertical pre-reject stays HERE and only here: it is what stops
// an isolated sky cell paying for a candidate scan it will not use. The hoisted
// path does not need it, because `cands.top` is strictly tighter.
fn treeAt(x : i32, y : i32, z : i32, seed : u32, ponds : ptr<function, PondSet>) -> u32 {
  if (y > treeMaxTop()) { return MAT_AIR; }
  var c : TreeCands;
  treeCandsInto(&c, x, z, seed, ponds);
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
// SCALE. Dimensions are decimetres * VOX_PER_M / 10 (see VOX_PER_M above),
// never bare voxel counts.
//
// Two species, because they read completely differently and the contrast is
// what sells the biome:
//   0 SAGUARO — a tall ribbed column, 3.2-5.2 m, with 0-2 upcurved arms. The
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
// The tallest bole cactusInfo builds, in decimetres: a saguaro at j == 4
// (32 + 4 * 5). Barrels top out at 9.
const CACTUS_MAX_DM : i32 = 52;

// The highest y ANY cactus can occupy, world-wide -- the cactus analogue of
// treeMaxTop(), for the sky early-outs. A cactus roots only on ground below
// the treeline (cactusInfo refuses `h >= treeline()`), is at most
// CACTUS_MAX_DM tall, and cactusAt clips every cell to base + height + 2 (the
// crown bloom sits one above the tip).
fn cactusMaxTop() -> i32 {
  return treeline() - 1 + CACTUS_MAX_DM * VOX_PER_M / 10 + 2;
}

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

fn cactusInfo(tx : i32, tz : i32, seed : u32, ponds : ptr<function, PondSet>) -> Cactus {
  var c : Cactus;
  c.present = false;
  c.species = 0u; c.wx = 0; c.wz = 0; c.base = 0;
  c.height = 0; c.radius = 0; c.arms = 0; c.rnd = 0u;

  // DISTINCT SALT. Not a bit-slice of the tree hash and not the tree salt with
  // a different shift: slices of one hash correlate (the pond-life block in
  // genCellIn says why), and correlated placements read as a planted grid.
  let hsh = hash3(seed ^ 0xCAC71u, bitcast<u32>(tx), bitcast<u32>(tz));
  c.rnd = hsh;
  let inset = CACTUS_TILE / 4;
  let span = u32(CACTUS_TILE / 2);
  c.wx = tx * CACTUS_TILE + inset + i32((hsh >> 3u) % span);
  c.wz = tz * CACTUS_TILE + inset + i32((hsh >> 9u) % span);

  // Only where the biome says so (cover.cacti), below the treeline, and never
  // on the keep-out ground every other feature avoids (siteKeepOut: the
  // harness box and authored sites) or in a pond candidate's disc.
  //
  // A saguaro is metre-scale and reads as a landmark the way a tree does, so
  // it answers to the MASTER switch only -- `groundCover` off leaves the cacti
  // standing and takes the scrub-and-tussock floor out from under them (that
  // floor is a biome cover row, gated with the rest of the stack below).
  if (!VEGETATION) { return c; }
  let cb = biomeAt(c.wx, c.wz, seed);
  if (!wmFlag(cb, WM_BF_CACTI)) { return c; }
  let h = baseHeight(c.wx, c.wz, seed);
  c.base = h;
  if (h >= treeline()) { return c; }
  if (siteKeepOut(c.wx, c.wz)) { return c; }
  if (pondCoversP(ponds,c.wx, c.wz)) { return c; }

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

  // Dimensions in TENTHS OF A METRE, converted below. CACTUS_MAX_DM (and with
  // it cactusMaxTop, the sky early-outs' bound) is the top of this table.
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
fn cactusAt(x : i32, y : i32, z : i32, seed : u32, ponds : ptr<function, PondSet>) -> u32 {
  let tx = fdiv(x, CACTUS_TILE);
  let tz = fdiv(z, CACTUS_TILE);
  for (var oz = -CACTUS_SCAN + unrollFence(); oz <= CACTUS_SCAN; oz++) {
    for (var ox = -CACTUS_SCAN + unrollFence(); ox <= CACTUS_SCAN; ox++) {
      let c = cactusInfo(tx + ox, tz + oz, seed, ponds);
      if (!c.present) { continue; }
      // HORIZONTAL reject. Must cover the widest thing the species can produce
      // or the outer arm gets sliced off at an invisible cylinder. A saguaro
      // arm reaches radius*3 + jitter from the axis, plus its own thickness.
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

// ---- canopy cover: what the forest floor is placed by ------------------------
// Under a closed crown the ground gets the shade plants; in the gaps it gets
// grass and flowers. So the floor is placed by CANOPY COVER, and this is the
// one measurement of it: the (2*TREE_SCAN+1)^2 tile scan, returning the cover
// and the nearest trunk. The cover rows read the cover through their
// canopyMin / canopyMax conditions (assets/biomes/<name>.json, asserted by the
// env-truth gate); the tile ferns read it through UG_COVER_MIN and refuse a
// column against a trunk. genChunk computes it once per column (the canopy
// memo), never per cell.

struct Undergrowth {
  cover   : i32,   // 0 = open sky, 255 = deep under a crown
  trunkD2 : i32,   // squared XZ distance to the nearest trunk, or a large value
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
fn undergrowthSite(x : i32, z : i32, seed : u32, ponds : ptr<function, PondSet>) -> Undergrowth {
  var u : Undergrowth;
  u.cover = 0;
  u.trunkD2 = 1 << 24;      // "no trunk anywhere near", larger than any reach

  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN + unrollFence(); oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN + unrollFence(); ox <= TREE_SCAN; ox++) {
      var t = treeInfo(tx + ox, tz + oz, seed, ponds);
      undergrowthAdd(&u, &t, x, z);
    }
  }
  return u;
}

// undergrowthSite's canopy cover, reading the tree tile cache of the chunk at
// (bx, bz) (treeTilesFill): the same 25 tiles in the same order.
fn undergrowthCoverFromTiles(x : i32, z : i32, bx : i32, bz : i32,
                             ponds : ptr<function, PondSet>) -> i32 {
  var u : Undergrowth;
  u.cover = 0;
  u.trunkD2 = 1 << 24;
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN + unrollFence(); oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN + unrollFence(); ox <= TREE_SCAN; ox++) {
      var t = wgTreeFor(tx + ox, tz + oz, bx, bz, ponds);
      undergrowthAdd(&u, &t, x, z);
    }
  }
  return u.cover;
}

// One tile's tree into column (x, z)'s undergrowth: the per-tile half of the
// scan, shared by the direct form and the tile-cache form.
fn undergrowthAdd(u : ptr<function, Undergrowth>, t : ptr<function, Tree>, x : i32, z : i32) {
  if (!(*t).present) { return; }
  let dx = x - (*t).wx;
  let dz = z - (*t).wz;
  let d2 = dx * dx + dz * dz;

  // Nearest trunk (plantSiteAt keeps a fern out of a bole).
  (*u).trunkD2 = min((*u).trunkD2, d2);

  // Canopy cover, from the species' OWN authored shade and its MEASURED
  // crown radius. A shrub is authored at shade 0 and contributes none —
  // counting one made every meadow read as closed forest, because shrubs
  // are the commonest meadow tile. An airy species and a dark one differ
  // by their number here rather than by a branch on a species id.
  let r = (*t).crownR;
  let peak = (*t).shade;
  if (peak <= 0 || r <= 0) { return; }
  if (d2 > r * r) { return; }
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
  // Done in u32 deliberately: d2 << 16 is past i32 for a crown radius over
  // 181 voxels, and every operand here is non-negative by construction.
  let frac = i32(isqrt((u32(d2) << 16u) / u32(rr * rr)));   // 0..256
  (*u).cover = min(255, (*u).cover + (peak * (256 - min(frac, 256))) / 256);
}

// ---- caves -------------------------------------------------------------------
// COLUMN BANDS carved by 2D noise: for each (x,z) inside a cavern mask, one
// contiguous vertical span is removed. Unlike 3D-threshold carving this cannot
// create free-floating stone blobs (stone above/below a band is horizontally
// connected to full columns at the mask boundary), which matters because the
// island detector would convert generated floaters into debris the moment
// anything moved nearby. Two bands: a near-surface one following the terrain
// (kept vlen(40) under the ground so it never breaches the soil) and a deep
// one at absolute depth.
//
// ---- THE MAGMA TABLE: generated matter has to be generated AT REST ---------
//
// A per-cell function cannot find the rim of a basin, and "at rest" for a
// liquid means "flat at the level its basin sets". A flat cut does not need
// the basin, because the cave's own complement already is one: fill every
// carved cell at or below LAVA_LEVEL with lava and every carved cell above it
// with air, and at each y <= LAVA_LEVEL a cell is lava exactly when it is
// carved and stone otherwise. So:
//
//   * laterally, every lava cell's neighbours are lava or STONE, at every
//     level — containment is a property of the carve, not of the fill;
//   * vertically, everything under a lava cell is lava or stone;
//   * the only lava/air interface in the world is the single plane
//     y == LAVA_LEVEL.
//
// Full cells over full cells with no lateral fullness difference: stepLiquid
// (sim_step.wgsl) falls through to its settled tail and the chunk sleeps
// after one tick like the stone around it. (Lava laid on the raw cavern floor
// instead was a sheet on a hillside that took ~90 s of sim to level, and every
// window shift regenerated a fresh one.)
//
// Two properties that must not be broken:
//
//   * The cut applies to BOTH BANDS, which is why it lives in caveFill and not
//     in band 2. Band 1's floor dips below LAVA_LEVEL wherever a pond bowl has
//     carved h down, and a band-1 AIR cell beside a band-2 LAVA cell at the
//     same y would be a hole in the container.
//   * The level must be a CONSTANT, and depend on nothing but depth (not the
//     cavern mask, not the column). Any per-column or per-noise-cell level
//     reintroduces a step in the surface, and a step in a liquid is a flow.
const LAVA_LEVEL : i32 = (-80 * VLEN_NUM) / VLEN_DEN;

// What a carved cell is filled with. The one place the magma table is applied.
fn caveFill(y : i32) -> i32 {
  if (y <= LAVA_LEVEL) { return 2; }   // flooded, and flat, and therefore still
  return 1;                            // open cave
}

// ---- THE CAVE COLUMN, hoisted out of the per-cell path --------------------
//
// EVERY ONE of the six noise samples below is a pure function of (x, z, h,
// seed); `y` appears only in the range compares. So the rule splits the way
// genColumn/genCellIn split: caveBands() is the (x,z)-only half (hoisted per
// column by genChunk and `far`, ~93% of a buried chunk's cost when it was per
// cell) and caveIn() the y-only half. genCellIn's `caveValid = false` spelling
// composes the two for a caller with nothing hoisted.
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
  // caves.features, near_surface / deep); a biome that authors none gets
  // worldmap.cpp's defaults (150 / 148).
  b.on1 = (vnoise2d(x, z, 5u, seed ^ 5u).n >> 6) > i32(wmBiome(biome, WM_B_CAVE_T1));
  b.f1 = h - vlen(40) - ((vnoise2d(x, z, 5u, seed ^ 6u).n * vlen(60)) >> 14);
  b.c1 = min(b.f1 + vlen(10) + ((vnoise2d(x, z, 4u, seed ^ 7u).n * vlen(20)) >> 14),
             h - vlen(40));
  // band 2: deep caverns at absolute depth (streamed depth is real terrain)
  b.on2 = (vnoise2d(x + 7717, z - 4177, 6u, seed ^ 8u).n >> 6) >
          i32(wmBiome(biome, WM_B_CAVE_T2));
  b.f2 = -vlen(40) - ((vnoise2d(x, z, 5u, seed ^ 9u).n * vlen(70)) >> 14);
  b.c2 = b.f2 + vlen(12) + ((vnoise2d(x, z, 4u, seed ^ 10u).n * vlen(26)) >> 14);
  // Depth is the only thing the fill may depend on (caveFill): a lava test
  // on the cavern MASK would stand a lava wall against open air.
  b.top2 = h - vlen(40);
  return b;
}

// 0 = solid, 1 = carve to air, 2 = carve to lava (caveFill). Band 1 wins ties.
fn caveIn(b : CaveBands, y : i32) -> i32 {
  if (b.on1 && y >= b.f1 && y <= b.c1) { return caveFill(y); }
  if (b.on2 && y >= b.f2 && y <= b.c2 && y <= b.top2) { return caveFill(y); }
  return 0;
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
// `caveIn` carves from f1 UPWARD (`y >= b.f1`), so **f1 is the lowest AIR
// cell** and the stone it stands on is f1-1: a floor plant goes AT f1, not
// at f1+1 (which would float one voxel above its floor).
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
  if (!GROUND_COVER) { return MAT_AIR; }
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
// Everything genColumn computes is a pure function of (x, z) — no `y` appears
// in it — and none of it is cheap (landColumn alone is the octave ladder plus
// the pond scan and gate). genChunk evaluates it ONCE per column and hands the
// result to the 16 cells that share it; genCellIn is the y-dependent half.

struct Col {
  h           : i32,         // ground height: landColumn's `h` (the height contract)
  slope       : i32,         // coarse landform gradient, Q8 (LandCol.slope)
  sed         : i32,         // loose wedge thickness at this column, 0 if none
  biome       : u32,
  pond        : i32,         // disc-pond water surface Y, or -1
  pw          : vec2<i32>,   // bowlAt's (bowl floor, surface), or (-1, -1)
  fluid       : u32,         // standing fluid material at this column
  fluidTop    : i32,         // its surface Y, or -1
  inPoolFloor : bool,
  inRim       : bool,
  shore       : Shore,
  // The shore band WITHOUT the bluff cut `shore` applies. `shore.onShore` asks
  // "does a wet fringe belong here", and answers NO on a column standing well
  // above the waterline -- a bluff is dry bank, not marsh, and that test is the
  // most load-bearing one in the shore feature. But "is the ground here shaped
  // by a water body rather than by the noise" is a DIFFERENT question with the
  // opposite answer on exactly that column: a bluff over a lake is a cut wall
  // 17 voxels tall that the analytic gradient reads as level plateau. So the
  // cover gate (looseCoverDepth) reads this flag and the marsh reads the other.
  nearWater   : bool,
  wp          : u32,         // the water preset this column's pond or shore wears; 0 = none
  bedSolid    : bool,        // inside a disc: the bowl face here is steeper than a powder bed can hold
  plant       : PlantCol,    // the tile plant whose footprint covers this column
  // The highest y a LOOSE cover cell may occupy and still be at rest on
  // tick 1: min over the four axis neighbours' ground + 1 (see
  // looseRestTop). genColumn sets it to `h` (no restriction); only
  // genChunk, which owns the pristine words the save compares against,
  // pays the four neighbour heights to tighten it.
  looseTop    : i32,
};

// ---- tile plants: ferns and big toadstools ---------------------------------
// A cover-row plant (grass, a flower) is one column. A TILE plant is wider
// than a cell — a fern is a 30 cm rosette, a big
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
  var L : LandCol;
  landColumn(cx, cz, seed, &L);
  ps.h = L.h;
  if (L.h >= treeline() || L.pond >= 0 || L.inRim || L.inPoolFloor) { return ps; }
  if (L.near.onShore && L.near.past < wmWaterI(L.near.wp, WM_W_SHORE_BAND)) { return ps; }
  let ug = undergrowthSite(cx, cz, seed, &L.ponds);
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
  if (!GROUND_COVER) { return pc; }
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
        vnoise(pt.cx, pt.cz, UG_FERN_PATCH_CELL, seed ^ 0xFE70u) > UG_FERN_PATCH) {
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
// authored pool floor and rim, the pond bowl carve, the berm and the site
// pad. The C++ mirror in world.cpp reproduces it; the `terrain` gate's pass C1
// is what proves they still agree, per voxel, on pristine procgen.
//
// WHAT IT IS NOT is "the topmost solid voxel". That would include canopy, a
// grass tuft and a site's stamped building, it cannot be mirrored cheaply (a
// tile scan in a tick path), and it is not what any of TerrainHeight's callers
// want — every one of them is asking where the GROUND is so it can stand
// something on it. A stamp is a material overlay in genCellIn; only its pad
// is ground.
//
// COST DISCIPLINE. This is ~25 hash3 plus the pond scan's table reads, and it
// is called from CPU paths that run at O(1) per frame — spawn placement,
// fixture anchoring, a mob probe. It must never be called in a per-voxel loop
// on either side.

struct LandCol {
  h           : i32,         // GROUND. The contract above.
  slope       : i32,         // Land.slope at this column: the coarse landform
                             // gradient, Q8, for the per-row maxSlope gates
  sed         : i32,         // loose wedge thickness INSIDE h, 0 where overridden
  pond        : i32,         // disc-pond water surface Y, or -1
  pw          : vec2<i32>,   // bowlAt's (bowl floor, surface), or (-1, -1)
  fluid       : u32,         // standing fluid material at this column
  fluidTop    : i32,         // its surface Y, or -1
  inPoolFloor : bool,
  inRim       : bool,
  near        : Shore,       // nearest disc OUTSIDE this column, or none
  wp          : u32,         // the covering pond's preset, else the near shore's; 0 = none
  bedSolid    : bool,        // inside a disc: face steeper than a powder bed can hold
  ponds       : PondSet,     // the column's pond candidates (pondScan), for the scans
};

// MIRROR-BEGIN landheight
// THE GROUND BEFORE THE SITE PAD. Everything the height contract is made of
// lives here EXCEPT the pad, the one override that needs another column's
// ground -- the site centre's, which LoadWorldMap bakes on the CPU from THIS
// function's twin (sitePadY), so the pad costs no second evaluation here.
fn landColumnBare(x : i32, z : i32, seed : u32,
                  L : ptr<function, LandCol>) {
  // OUT-PARAMETER, not a return value: LandCol is ~65 words (the column
  // fields plus the embedded PondSet) and this function is inlined several
  // times per far entry point (genColumn, farColTop's corners, plantSiteAt);
  // a by-value return leaves one aggregate copy per inline site for the
  // driver's front end, the documented NVIDIA compile cost.
  (*L).pond = -1;
  (*L).pw = vec2<i32>(-1, -1);
  (*L).fluid = MAT_AIR;
  (*L).fluidTop = -1;
  (*L).near.onShore = false; (*L).near.past = 0; (*L).near.surf = -1; (*L).near.wp = 0u;
  (*L).wp = 0u;
  (*L).bedSolid = false;
  (*L).ponds = pondSetNone();
  // The fluid lab's flat slab — the same guard genColumn takes below, taken
  // here as well so World::TerrainHeight sees the slab through the contract
  // rather than through a second copy of the constant.
  if (T.labMode != 0u) {
    (*L).h = LAB_SLAB_Y;
    (*L).slope = 0;
    (*L).sed = 0;
    (*L).inPoolFloor = true;
    (*L).inRim = true;
    return;
  }
  // THE SEDIMENT WEDGE IS DECIDED BEFORE THE HEIGHT IS COMPOSED, which is why
  // the disc tests below run before anything is added to `bed`.
  //
  // `land.h` is `bed + land.sed` and every authored override here either
  // REPLACES the height (a pool floor, a bowl carve) or LIFTS it (a rim, a
  // berm). A lift is a deliberate STEP against the neighbouring column, and a
  // step is exactly where a powder wedge avalanches — so the wedge has to be
  // gone from those columns, and gone from `h` too, not merely relabelled.
  //
  // AND IT HAS TO RAMP OUT, not switch off. Zeroing 24 voxels of sediment at
  // the edge of a pond band builds a 24-voxel cliff there, which is worse than
  // the thing it was avoiding (as a hard switch it left 108 chunks awake at
  // tick 120, against 7 with no wedge at all).
  let land = landAt(x, z, seed);
  (*L).slope = land.slope;
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
  let poolY = wmTerrain(WM_H_TERRAIN_HOME_Y) - vlen(15);
  // Water lake at (420,420), ~8.5 m across
  let pdx = x - 420; let pdz = z - 420;
  let pd2 = pdx * pdx + pdz * pdz;
  let pR = vlen(68); let pRim = vlen(80);
  (*L).inPoolFloor = pd2 < pR * pR;
  (*L).inRim = pd2 < pRim * pRim;

  // ---- disc ponds, queried before the height is composed ----
  // pondRoll's keep-out (siteKeepOut) excludes the harness box the pool sits
  // in, so a rolled disc never overlaps its rim; the fluidTop<0 check below is
  // belt-and-braces. `pondNear` is the one
  // scan that serves BOTH the berm and the marsh fringe (genColumn narrows the
  // same answer), and it is skipped inside a disc or an authored rim, where
  // there is nothing outside to be near.
  // The column's pond candidates, scanned ONCE (the only table reads on this
  // path) and handed to the bowl, the shore and, through LandCol, every scan.
  (*L).ponds = pondScan(x, z, seed);
  let pc = pondCover((*L).ponds, x, z, seed);
  (*L).pw = bowlAt(pc, x, z);
  if ((*L).pw.y < 0 && !(*L).inRim) { (*L).near = pondNear((*L).ponds, x, z, seed); }
  (*L).wp = select((*L).near.wp, pc.wp, pc.present);

  // ---- the wedge, after everything that has to suppress it ----
  // The pond band ramps rather than switches, over the same width `pondNear`
  // scans for THIS preset, so the wedge thins to nothing as it reaches the
  // water instead of ending in a wall of loose gravel above a bowl of sand.
  var sed = land.sed;
  if ((*L).inRim || (*L).pw.y >= 0) {
    sed = 0;
  } else if ((*L).near.onShore) {
    let band = max(wmWaterI((*L).near.wp, WM_W_BAND), 1);
    sed = (sed * min((*L).near.past, band)) / band;
  }
  var h = bed + sed;

  if (pd2 < pR * pR) {
    h = poolY;
    (*L).fluid = M_WATER; (*L).fluidTop = poolY + vlen(24);
  } else if (pd2 < pRim * pRim) {
    h = max(h, poolY + vlen(26));    // containment rim
  }

  // ---- carve the bowl inside a disc, raise the berm outside ----
  if ((*L).pw.y >= 0) {
    (*L).pond = (*L).pw.y;
    // THE BOWL REPLACES THE TERRAIN, it does not merely cut into it, and that
    // is the difference between a bounded bed and an avalanche. As `min(h,
    // floor)` the bowl would describe the floor only where the natural ground
    // was higher; wherever the ground dipped below it the bed would be raw
    // terrain at whatever slope the noise had, and genCellIn lays a powder
    // bed on it. Assigned, the floor is exactly the preset's profile
    // (bowlDepth), and where that face is steeper than a powder can hold the
    // bed is the preset's solid substrate instead (bowlSteep).
    //
    // On sloping ground this fills the downhill half as well as cutting the
    // uphill one, which is what a dammed tarn IS; pondGate's radius-aware
    // slope gate is what keeps the fill from becoming a wall.
    h = (*L).pw.x;
    // The fill is the preset's (water, lava, or none for a dry playa), and
    // the bed is a powder only where the face can hold it (bowlSteep).
    let fill = wmWater(pc.wp, WM_W_FILL);
    if ((*L).fluidTop < 0 && fill != 0u) { (*L).fluid = fill; (*L).fluidTop = (*L).pw.y; }
    (*L).bedSolid = bowlSteep(pc, x, z);
  } else if (!(*L).inRim && (*L).near.onShore &&
             (*L).near.past < wmWaterI((*L).near.wp, WM_W_BERM_W)) {
    h = bermLift((*L).near.wp, h, (*L).near.surf, (*L).near.past);
  }
  // THE SEA: ground under the map's sea level is under water. One global
  // plane, which is what lets the ocean ring past the painted map be water
  // without a tile scheme.
  if (h < seaLevelY() && (*L).fluidTop < 0) { (*L).fluid = M_WATER; (*L).fluidTop = seaLevelY(); }
  (*L).h = h;
  (*L).sed = sed;
  return;
}


// The pad under an authored site: inside the footprint the ground IS
// the height at the site's centre (exact, so a stamped floor is flat), and
// over `margin` columns past it the terrain ramps back. The centre's height is
// sitePadY's baked one. Spelled identically in world.cpp; the site readers it
// calls live outside the mirror.
fn sitePadAt(x : i32, z : i32, h : i32) -> i32 {
  let sid = wmSiteAt(x, z);
  if (sid == 0u) { return h; }
  // A water site is a bowl, not a building: the pond block above already
  // shaped the ground under it, and levelling it here would fill the lake.
  if (u32(wmSiteI(sid, WM_S_KIND)) == WM_SITE_WATER) { return h; }
  let sx = wmSiteI(sid, WM_S_X);
  let sz = wmSiteI(sid, WM_S_Z);
  let r = wmSiteI(sid, WM_S_RADIUS);
  let margin = max(wmSiteI(sid, WM_S_PAD_MARGIN), 1);
  let d = max(max(abs(x - sx), abs(z - sz)) - r, 0);
  if (d >= margin) { return h; }
  let padY = sitePadY(sid);
  let w = ((margin - d) * 256) / margin;
  return h + (((padY - h) * w) >> 8);
}

// The height contract's public face: the bare column with the site pad
// blended into it. Everything else in this file and World::TerrainHeight go
// through here.
fn landColumn(x : i32, z : i32, seed : u32,
              L : ptr<function, LandCol>) {
  landColumnBare(x, z, seed, L);
  let hp = sitePadAt(x, z, (*L).h);
  if (hp != (*L).h) {
    // Cut-and-fill under a building: the loose wedge goes with it, for the
    // reason the pond-bank block gives (powder under a stone floor creeps).
    (*L).h = hp;
    (*L).sed = 0;
  }
  return;
}
// MIRROR-END landheight

// The contract, as a function. genColumn calls landColumn directly (it needs the
// rest of the struct); this is the entry point for anything that only wants the
// ground — and it is what World::TerrainHeight mirrors.
fn colHeightAt(x : i32, z : i32, seed : u32) -> i32 {
  var L : LandCol;
  landColumn(x, z, seed, &L);
  return L.h;
}

// `ponds` receives the column's pond set (landColumn's pondScan), so the
// scans genChunk and the far entries run over this column -- trees, cacti,
// canopy cover, near-water conditions -- reuse it instead of scanning again.
fn genColumn(x : i32, z : i32, seed : u32, ponds : ptr<function, PondSet>) -> Col {
  // ---- the fluid lab's flat slab (world.h kLabSlabY; PLAN_fluid_overhaul §4)
  // One guard, HERE, covers every worldgen consumer — genChunk (main + list)
  // and the far entries — because they all come through this column
  // function. The Col it returns is chosen so genCellIn's existing
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
    lab.slope = 0;
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
    lab.shore.wp = 0u;
    lab.nearWater = false;
    lab.wp = 0u;
    lab.bedSolid = false;
    lab.plant.mat = MAT_AIR;
    lab.plant.base = 0;
    lab.plant.top = -1;
    lab.looseTop = LAB_SLAB_Y;
    *ponds = pondSetNone();
    return lab;
  }
  // THE GROUND, and everything derived from it, in one call. This is the same
  // `h` World::TerrainHeight returns — that equality is the whole point of the
  // split (see the height contract above landColumn).
  var L : LandCol;
  landColumn(x, z, seed, &L);
  let h = L.h;
  let biome = biomeAt(x, z, seed);

  // ---- the marsh fringe: landColumn's berm scan, narrowed ----
  // The scan itself (`L.near`) has already happened, once, for the berm — so
  // this costs comparisons and no hashes at all.
  //
  // Never inside an authored pool rim (landColumnBare does not scan there),
  // and never above the treeline, so a shore is always a shore and never a
  // marsh growing out of a snowfield.
  var shore : Shore;
  shore.onShore = false; shore.past = 0; shore.surf = -1; shore.wp = 0u;
  // The band's width is the pond's preset's, and the band's EXISTENCE
  // follows the pond, not the biome's ground-flora flag: an oasis's shore
  // rows show in the desert because the oasis preset authored them.
  if (L.near.onShore && L.near.past < wmWaterI(L.near.wp, WM_W_SHORE_BAND) &&
      h < treeline()) {
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
    if (h > shore.surf + wmWaterI(shore.wp, WM_W_SHORE_LIFT)) {
      shore.onShore = false;
    }
  }
  // Deliberately NOT gated on the bluff cut above, nor on the treeline: this is
  // the geometric question ("is this column's ground the wall of a bowl someone
  // dug"), not the ecological one. See Col.nearWater.
  let nearWater = L.near.onShore &&
                  L.near.past < wmWaterI(L.near.wp, WM_W_SHORE_BAND);

  var col : Col;
  col.h = h;
  col.slope = L.slope;
  col.sed = L.sed;
  col.biome = biome;
  col.pond = L.pond;
  col.pw = L.pw;
  col.fluid = L.fluid;
  col.fluidTop = L.fluidTop;
  col.inPoolFloor = L.inPoolFloor;
  col.inRim = L.inRim;
  col.shore = shore;
  col.nearWater = nearWater;
  col.wp = L.wp;
  col.bedSolid = L.bedSolid;
  col.plant = plantColumnAt(x, z, seed, biome);
  col.looseTop = h;
  *ponds = L.ponds;
  return col;
}

// ---- A LOOSE COVER HAS TO BE BORN AT REST ---------------------------------
//
// Worldgen's contract (the magma-table note above) is that generated matter is
// generated AT REST: a cell the CA would move on tick 0 is a chunk that never
// sleeps (CLAUDE.md rule 2). The sediment wedge is slope-gated in landAt, and
// the mud ring and most skins are SOLID; the sand cap and a POWDER ground skin
// are split in two instead. `looseCoverDepth` ramps the powder part to zero
// between the ground the wedge calls flat (terrain.sedSlope) and the CA's
// angle of repose (CAP_REPOSE_Q8), and whatever the ramp takes away becomes
// the biome's firm cover. On flat ground the ramp returns the full authored
// depth and nothing changes.
//
// `Col.nearWater` is the second half and it is not an approximation. A water
// body's bank is CUT, not eroded: bermLift forces h up inside the band and the
// bowl is excavated out from under its inner edge. Neither wall is in the
// noise field, so `slope` reads a bank cliff as the level plateau it was cut
// from; the shore band can see it, so inside the band loose depth is 0. It
// reads `nearWater` and NOT `shore.onShore`, because the latter carries the
// marsh fringe's BLUFF CUT -- off on a column standing well above the
// waterline, which is exactly the tallest cut wall there is.
// The CA's angle of repose in landform-gradient units: sim_step slides a
// powder into any free down-diagonal, so the steepest pile it holds is one
// voxel per column = 256 in the Q8 slope Land carries. Declared here and not
// in common.wgsl: only this kernel reads it (the common.wgsl compile cliff).
const CAP_REPOSE_Q8 : i32 = 256;

fn looseCoverDepth(col : ptr<function, Col>, depth : i32) -> i32 {
  // Every AUTHORED discontinuity, none of which is in the noise field:
  //   nearWater -- a water body's bermed / excavated bank (see above)
  //   inRim     -- the annulus of an authored pool, a forced cylinder wall
  //                (what the snow cap, the caves and the sediment wedge
  //                also read as "this ground was placed, not grown").
  if ((*col).nearWater || (*col).inRim) { return 0; }
  // A CLAMPED taper, not a straight line from slope 0: full authored depth up
  // to terrain.sedSlope (the ground the sediment wedge already calls flat),
  // then tapering to zero at the CA's OWN angle of repose. A ramp ending at
  // sedSlope strips dunes that hold sand fine; one starting at 0 keeps a thin
  // loose voxel on rough near-repose faces. The start is the wedge's number
  // (one definition of "flat"); the end is not a knob, because repose is the
  // CA's constant, not an aesthetic.
  let flat = wmTerrain(WM_H_TERRAIN_SED_SLOPE);
  let span = max(CAP_REPOSE_Q8 - flat, 1);
  return clamp((depth * (CAP_REPOSE_Q8 - (*col).slope)) / span, 0, depth);
}

// ---- THE LOCAL STEP TEST: no loose grain above a free down-diagonal ------
//
// looseCoverDepth is a GRADIENT test on the landform gradient by design
// (gated on the full gradient the taper becomes a cliff), so it cannot see the
// +-2-voxel steps the detail and grain octaves put on an otherwise gentle dune
// face, and on every such step one loose voxel would slide on tick 1.
//
// This test is exact. sim_step moves a powder that authors no `repose` (sand
// is 1:1) straight down or into one of the FOUR AXIS down-diagonals, nothing
// else. A loose cell at y therefore rests iff every axis neighbour's cell at
// y-1 is ground, i.e. iff  y <= min(axis neighbour ground) + 1.  Cells above
// that line go to the firm cover, exactly what the taper hands its steep
// ground; the loose depth below the line is untouched. Corners do not matter
// (the CA never takes a corner diagonal for a powder), and neither does what
// stands above a lower neighbour's ground: the line is drawn at the
// neighbour's GROUND -- conservative, and it only fires on a real 2+ step.
//
// COST, and why it lives in genChunk and not genColumn. Four colHeightAt per
// column, paid only by columns of a biome whose cover splits (coverSplits),
// only in the chunk(s) that reach the loose band, and only where the taper
// left loose depth. genColumn is also the far entries' per-column call, where
// sand vs sandstone is one far palette slot and four more ground evaluations
// per sample would be pure cost.
fn looseRestTop(col : ptr<function, Col>, x : i32, z : i32, seed : u32) -> i32 {
  var m = colHeightAt(x + 1, z, seed);
  m = min(m, colHeightAt(x - 1, z, seed));
  m = min(m, colHeightAt(x, z + 1, seed));
  m = min(m, colHeightAt(x, z - 1, seed));
  return min((*col).h, m + 1);
}

// Does this column's cover get the loose/firm split at all? The same two opt-ins
// genCellIn's cap and skin branches read: the sand cap flag, or a POWDER skin
// in a biome that authored a firm skin (tundra's snow authored none and keeps
// its settle transient, as before).
fn coverSplits(biome : u32) -> bool {
  if (wmFlag(biome, WM_BF_SAND_CAP)) { return true; }
  let skin = wmBiome(biome, WM_B_SKIN);
  return skin != MAT_AIR && materials[skin].klass == CLASS_POWDER &&
         wmBiome(biome, WM_B_FIRM_COVER) != MAT_AIR;
}

// The material the ramp above hands the rest of the cap to: the biome's
// authored cover.firmSkin (WM_B_FIRM_COVER; desert and ocean say `sandstone`).
// A biome that authored none keeps its subsoil where that is already a solid,
// and otherwise gets stone -- either way a SOLID, which is the whole point.
fn coverFirmMat(biome : u32) -> u32 {
  let firm = wmBiome(biome, WM_B_FIRM_COVER);
  if (firm != MAT_AIR) { return firm; }
  let sub = wmBiome(biome, WM_B_SUBSOIL);
  if (sub != MAT_AIR && materials[sub].klass == CLASS_SOLID) { return sub; }
  return M_STONE;
}

// Can any cell of this column be a TREE cell? The column half of genCellIn's
// tree-block gate (the other half is `y > h`); genChunk and `far` skip the
// tree candidate scan where it is false.
fn colGrowsTrees(col : ptr<function, Col>) -> bool {
  return VEGETATION && !(*col).inRim && (*col).h < treeline() && (*col).pond < 0;
}

// The h-relative plant reach above a column: the biome's tallest cover row
// and its water presets' shore / emergent plants (worldmap.cpp packs the
// tallest authored row of either, jitter and head included, as
// WM_B_MAX_COVER_H), never less than SKY_MARGIN_MIN.
fn colSkyMargin(biome : u32) -> i32 {
  return max(SKY_MARGIN_MIN, i32(wmBiome(biome, WM_B_MAX_COVER_H)));
}

// ---- THE COLUMN PROLOGUE: what a stack of cells over one column needs ------
//
// ONE statement of the exact skips, for the two callers that hold a column and
// are about to ask genCellIn about cells whose y lies in [yLo, yHi]: the
// column-cache pre-pass (`cols`, over the y-range of every listed chunk on
// the column) and the far sweep (`far`, over its sixteen row samples). It
// fills the cave bands and says which of the two tile scans the range needs;
// the caller runs them (`cols` from the tree tile cache, `far` directly --
// its sample columns are 2^shift apart and share no tiles). A declined
// computation is one no cell in [yLo, yHi] can read:
//
//   cave bands   genCellIn reads them only for `y <= h && !inRim`: none when
//                the column is an authored rim or yLo is above the ground
//                (then `*cave` stays as the caller declared it, zeroed).
//   trees        read only in the tree block, gated `colGrowsTrees && y > h`;
//                and treeCandsInto admits only candidates below treeMaxTop(),
//                so a range entirely above that gets nothing from the scan.
//                Declined: n = 0, top = far below any y (colNoTrees).
//   canopy       read only by a biome's cover rows (WM_BF_CANOPY_ROWS), only
//                above the ground, never in a rim, a pond or a site keep-out,
//                and every row breaks to air past its height (<= colSkyMargin).
//                Declined: -1; callers hand genCellIn max(canopy, 0), and a
//                non-negative memo is what keeps genCellIn's own per-cell scan
//                from running (and lets the compiler drop it).
//
// genChunk used to carry a copy of this block with the chunk's own sixteen
// cells as the range; `far` carried another with its sample rows.
struct ColNeeds { trees : bool, canopy : bool };
fn colPrologue(col : ptr<function, Col>, x : i32, z : i32, yLo : i32, yHi : i32,
               cave : ptr<function, CaveBands>) -> ColNeeds {
  let h = (*col).h;
  if (!(*col).inRim && yLo <= h) {
    *cave = caveBands(x, z, h, (*col).biome, T.seed);
  }
  var need : ColNeeds;
  need.trees = colGrowsTrees(col) && yHi > h && yLo <= treeMaxTop();
  need.canopy = wmFlag((*col).biome, WM_BF_CANOPY_ROWS) && !(*col).inRim && (*col).pond < 0 &&
                !siteKeepOut(x, z) && yHi > h && yLo <= h + colSkyMargin((*col).biome);
  return need;
}
fn colNoTrees(trees : ptr<function, TreeCands>) {
  (*trees).n = 0;
  (*trees).top = -1048576;   // the same "far below any y" treeCandsInto uses
}

// ---- one cached column: CC_WORDS words (the column cache, top of file) ----
// Everything genChunk reads about a column that is a pure function of
// (x, z): the Col genColumn returns, and the column prologue's answers --
// the cave bands, the canopy memo, the loose step line, the sky ceiling
// and the tree candidates' top. The candidate SET is not cached (TREE_CAND_MAX
// x 8 words); genChunk rebuilds it in the few chunks that can hold a tree
// cell. Neither is the pond set (51 words): a flag says whether it is empty,
// and genChunk re-runs pondScan where it is not and a cell can read it.
const CCW_H          : u32 = 0u;
const CCW_SLOPE      : u32 = 1u;
const CCW_SED        : u32 = 2u;
const CCW_POND       : u32 = 3u;
const CCW_PW_X       : u32 = 4u;
const CCW_PW_Y       : u32 = 5u;
const CCW_FLUID_TOP  : u32 = 6u;
const CCW_SHORE_PAST : u32 = 7u;
const CCW_SHORE_SURF : u32 = 8u;
const CCW_PLANT_BASE : u32 = 9u;
const CCW_PLANT_TOP  : u32 = 10u;
const CCW_BIOME      : u32 = 11u;
const CCW_WP         : u32 = 12u;
const CCW_SHORE_WP   : u32 = 13u;
const CCW_PACKED     : u32 = 14u;   // fluid | plant.mat << 12 | CCF_* flags << 24
const CCW_LOOSE_TOP  : u32 = 15u;
const CCW_CANOPY     : u32 = 16u;   // colPrologue's memo, -1 = not computed
const CCW_TOP        : u32 = 17u;   // the column's sky ceiling (see `cols`)
const CCW_TREE_TOP   : u32 = 18u;   // the tree candidates' top, or far below any y
const CCW_CAVE       : u32 = 19u;   // f1, c1, f2, c2, top2
// CCW_PACKED's flag byte. Material ids are 12 bits (the voxel word), so the
// fluid and the tile plant fill the low 24.
const CCF_POOL_FLOOR : u32 = 1u;
const CCF_RIM        : u32 = 2u;
const CCF_ON_SHORE   : u32 = 4u;
const CCF_NEAR_WATER : u32 = 8u;
const CCF_BED_SOLID  : u32 = 16u;
const CCF_CAVE_ON1   : u32 = 32u;
const CCF_CAVE_ON2   : u32 = 64u;
const CCF_PONDS      : u32 = 128u;  // the column's pond set is not empty

fn ccStore(w : u32, col : ptr<function, Col>, cave : ptr<function, CaveBands>,
           pondsN : i32, canopy : i32, colTop : i32, treeTop : i32) {
  var f = 0u;
  f |= select(0u, CCF_POOL_FLOOR, (*col).inPoolFloor);
  f |= select(0u, CCF_RIM, (*col).inRim);
  f |= select(0u, CCF_ON_SHORE, (*col).shore.onShore);
  f |= select(0u, CCF_NEAR_WATER, (*col).nearWater);
  f |= select(0u, CCF_BED_SOLID, (*col).bedSolid);
  f |= select(0u, CCF_CAVE_ON1, (*cave).on1);
  f |= select(0u, CCF_CAVE_ON2, (*cave).on2);
  f |= select(0u, CCF_PONDS, pondsN > 0);
  colCache[w + CCW_H] = bitcast<u32>((*col).h);
  colCache[w + CCW_SLOPE] = bitcast<u32>((*col).slope);
  colCache[w + CCW_SED] = bitcast<u32>((*col).sed);
  colCache[w + CCW_POND] = bitcast<u32>((*col).pond);
  colCache[w + CCW_PW_X] = bitcast<u32>((*col).pw.x);
  colCache[w + CCW_PW_Y] = bitcast<u32>((*col).pw.y);
  colCache[w + CCW_FLUID_TOP] = bitcast<u32>((*col).fluidTop);
  colCache[w + CCW_SHORE_PAST] = bitcast<u32>((*col).shore.past);
  colCache[w + CCW_SHORE_SURF] = bitcast<u32>((*col).shore.surf);
  colCache[w + CCW_PLANT_BASE] = bitcast<u32>((*col).plant.base);
  colCache[w + CCW_PLANT_TOP] = bitcast<u32>((*col).plant.top);
  colCache[w + CCW_BIOME] = (*col).biome;
  colCache[w + CCW_WP] = (*col).wp;
  colCache[w + CCW_SHORE_WP] = (*col).shore.wp;
  colCache[w + CCW_PACKED] = ((*col).fluid & 0xFFFu) | (((*col).plant.mat & 0xFFFu) << 12u) |
                             (f << 24u);
  colCache[w + CCW_LOOSE_TOP] = bitcast<u32>((*col).looseTop);
  colCache[w + CCW_CANOPY] = bitcast<u32>(canopy);
  colCache[w + CCW_TOP] = bitcast<u32>(colTop);
  colCache[w + CCW_TREE_TOP] = bitcast<u32>(treeTop);
  colCache[w + CCW_CAVE] = bitcast<u32>((*cave).f1);
  colCache[w + CCW_CAVE + 1u] = bitcast<u32>((*cave).c1);
  colCache[w + CCW_CAVE + 2u] = bitcast<u32>((*cave).f2);
  colCache[w + CCW_CAVE + 3u] = bitcast<u32>((*cave).c2);
  colCache[w + CCW_CAVE + 4u] = bitcast<u32>((*cave).top2);
}

// The inverse of ccStore for the Col and the cave bands. Out-parameters, not
// return values, for landColumnBare's reason (an aggregate by value is a copy
// per inline site for the driver's front end).
fn ccLoad(w : u32, col : ptr<function, Col>, cave : ptr<function, CaveBands>) {
  let f = colCache[w + CCW_PACKED] >> 24u;
  (*col).h = bitcast<i32>(colCache[w + CCW_H]);
  (*col).slope = bitcast<i32>(colCache[w + CCW_SLOPE]);
  (*col).sed = bitcast<i32>(colCache[w + CCW_SED]);
  (*col).biome = colCache[w + CCW_BIOME];
  (*col).pond = bitcast<i32>(colCache[w + CCW_POND]);
  (*col).pw = vec2<i32>(bitcast<i32>(colCache[w + CCW_PW_X]),
                        bitcast<i32>(colCache[w + CCW_PW_Y]));
  (*col).fluid = colCache[w + CCW_PACKED] & 0xFFFu;
  (*col).fluidTop = bitcast<i32>(colCache[w + CCW_FLUID_TOP]);
  (*col).inPoolFloor = (f & CCF_POOL_FLOOR) != 0u;
  (*col).inRim = (f & CCF_RIM) != 0u;
  (*col).shore.onShore = (f & CCF_ON_SHORE) != 0u;
  (*col).shore.past = bitcast<i32>(colCache[w + CCW_SHORE_PAST]);
  (*col).shore.surf = bitcast<i32>(colCache[w + CCW_SHORE_SURF]);
  (*col).shore.wp = colCache[w + CCW_SHORE_WP];
  (*col).nearWater = (f & CCF_NEAR_WATER) != 0u;
  (*col).wp = colCache[w + CCW_WP];
  (*col).bedSolid = (f & CCF_BED_SOLID) != 0u;
  (*col).plant.mat = (colCache[w + CCW_PACKED] >> 12u) & 0xFFFu;
  (*col).plant.base = bitcast<i32>(colCache[w + CCW_PLANT_BASE]);
  (*col).plant.top = bitcast<i32>(colCache[w + CCW_PLANT_TOP]);
  (*col).looseTop = bitcast<i32>(colCache[w + CCW_LOOSE_TOP]);
  (*cave).on1 = (f & CCF_CAVE_ON1) != 0u;
  (*cave).f1 = bitcast<i32>(colCache[w + CCW_CAVE]);
  (*cave).c1 = bitcast<i32>(colCache[w + CCW_CAVE + 1u]);
  (*cave).on2 = (f & CCF_CAVE_ON2) != 0u;
  (*cave).f2 = bitcast<i32>(colCache[w + CCW_CAVE + 2u]);
  (*cave).c2 = bitcast<i32>(colCache[w + CCW_CAVE + 3u]);
  (*cave).top2 = bitcast<i32>(colCache[w + CCW_CAVE + 4u]);
}

// ---- THE CELL HALF: everything that actually depends on y -----------------
//
// Unpacks the column into local names; every one of them is read-only from
// here down — the only thing this half writes is `mat`.
//
// Everything column-sized travels BY POINTER: `col`, the cave bands, the tree
// candidates and the pond set. They are pure functions of (x, z, seed), and
// this function is inlined into every entry point; a by-value aggregate
// parameter leaves a per-call copy for NVIDIA's front end to promote, and a
// dynamic index into a by-value struct (`trees.t[i]`) spills it to scratch.
// The cave bands and tree candidates stay OUT of `Col` because they are big
// (a candidate set is hundreds of bytes) and only genChunk and `far` hoist
// them.
//
// The `*Valid` flags are not an optimization switch — they select between two
// spellings of the SAME function. With them false this computes caveBands /
// calls treeAt on demand, which are caveBands+caveIn and
// treeCandsInto+treeFromCands composed; any divergence would be a bug in
// that composition, and the world hash (genChunk hoists, the far skin lookup
// does not) is what proves there is none.
//
// `canopyMemo` is the column's canopy cover when the caller computed it,
// -1 to have the cover block scan on demand.
fn genCellIn(col : ptr<function, Col>,
             cave : ptr<function, CaveBands>, caveValid : bool,
             trees : ptr<function, TreeCands>, treeValid : bool,
             ponds : ptr<function, PondSet>,
             canopyMemo : i32,
             x : i32, y : i32, z : i32, seed : u32) -> u32 {
  let h = (*col).h;
  let sed = (*col).sed;
  let biome = (*col).biome;
  let pond = (*col).pond;
  let pw = (*col).pw;
  let fluid = (*col).fluid;
  let fluidTop = (*col).fluidTop;
  let inPoolFloor = (*col).inPoolFloor;
  let inRim = (*col).inRim;
  let shore = (*col).shore;
  let wp = (*col).wp;            // the preset this column's pond or shore wears
  let bedSolid = (*col).bedSolid;
  var mat = MAT_AIR;

  if (y <= h) {
    let submerged = pond >= 0;
    // SNOW IS A POWDER, which is why the rim keep-out is `!inRim` and not
    // merely `!inPoolFloor`: snow on a rim ring slides down its inner face
    // into whatever the pool holds, and against a hot fluid `snow -> water ->
    // steam -> water` sustains itself forever (a rule-2 failure that reports
    // itself as `ca-skip` never finding a quiet tick).
    if (!inRim && h >= treeline() && y > h - 2) {
      mat = M_SNOW;                        // snow caps on the high hills
    } else if (inPoolFloor) {
      mat = M_STONE;
    } else if (submerged && y > h - wmWaterI(wp, WM_W_BED_THICKNESS)) {
      // THE BED: the preset's, by water depth -- bed.shallow (sand)
      // where the water over this column is shallower than bed.shallowDepth,
      // bed.deep (mud) below that -- and its SUBSTRATE wherever the bowl
      // face is steeper than a powder can hold (landColumnBare's bowlSteep),
      // so a steep tarn wall is stone with a sand bed only where it flattens.
      // A preset naming no material falls back to sand.
      var bed = select(wmWater(wp, WM_W_BED_DEEP), wmWater(wp, WM_W_BED_SHALLOW),
                       pond - h < wmWaterI(wp, WM_W_BED_SHALLOW_DEPTH));
      if (bedSolid) { bed = wmWater(wp, WM_W_BED_SUBSTRATE); }
      mat = select(bed, select(M_SAND, M_STONE, bedSolid), bed == 0u);
    } else if (wmFlag(biome, WM_BF_SAND_CAP) && y > h - 4) {
      // The loose cap, but only as deep as this column's ground can HOLD loose
      // matter (looseCoverDepth above). Flat desert: loose == 4 and every cell
      // here is sand. At or past the angle of repose, or anywhere on a pond's
      // berm wall: loose == 0 and the whole cap is the firm material, so a cut
      // bank reads as sandstone rock instead of as sand that has not fallen
      // yet. In between the two split at the ramp.
      // And never above the local step line (looseRestTop): a cell with a
      // free down-diagonal is firm whatever the gradient said.
      let loose = looseCoverDepth(col, 4);
      mat = select(coverFirmMat(biome), M_SAND,
                   y > h - loose && y <= (*col).looseTop);
    } else if (shore.onShore && shore.past < wmWaterI(wp, WM_W_MUD_WIDTH) &&
               y > h - 2) {
      // WET MUD, in the inner ring only: the transition between the bed inside
      // the disc and the biome's skin outside it. Two voxels deep, because a
      // bank gets dug into and a one-voxel skin over stone reads as painted
      // on. SOLID (not powder): a powder shell on a slope avalanches out from
      // under itself and the chunk never sleeps. A stone face in the band
      // outside the mud ring gets wet moss instead (below). The material is
      // the preset's shore.mudMaterial; a preset naming none keeps the shore
      // mud.
      let mud = wmWater(wp, WM_W_MUD_MAT);
      mat = select(M_SHORE_MUD, mud, mud != 0u);
    } else if (y > h - i32(wmBiome(biome, WM_B_SKIN_DEPTH))) {
      // The biome's ground skin (assets/biomes/<name>.json cover.skin /
      // skinDepth): grass on the forest floor, snow on the tundra, mud in the
      // marsh. USUALLY a solid -- a powder skin on a slope avalanches out from
      // under itself.
      //
      // A POWDER skin (desert and ocean author `sand`) gets the same split the
      // cap above gets -- gated on the AUTHORED CLASS, and ONLY where the biome
      // has authored a cover.firmSkin. The opt-in is the author's veto:
      // tundra's snow is a powder too, and firming part of it would break its
      // own authored claim ("99% of columns wear snow at y == h", the env-truth
      // gate) for a settle transient tundra accepts.
      let skin = wmBiome(biome, WM_B_SKIN);
      let depth = i32(wmBiome(biome, WM_B_SKIN_DEPTH));
      let loose = select(depth, looseCoverDepth(col, depth),
                         skin != MAT_AIR && materials[skin].klass == CLASS_POWDER &&
                         wmBiome(biome, WM_B_FIRM_COVER) != MAT_AIR);
      // looseTop is `h` for every column genChunk did not tighten, which
      // includes every biome coverSplits() rejects (tundra's snow), so the
      // step line cannot firm a skin whose author did not opt in.
      mat = select(coverFirmMat(biome), skin, y > h - loose && y <= (*col).looseTop);
    } else if (y > h - sed) {
      // ---- THE SEDIMENT WEDGE: topsoil over gravel over bedrock ----
      //
      // Loose powder under the skin, safe only because `sed` is SLOPE-GATED
      // in landAt and ramps continuously to zero as the ground approaches the
      // angle of repose (a constant-depth powder shell under the skin creeps
      // down every slope). With a solid skin at y == h the topmost grain is at
      // h-1, so it has a free down-diagonal only where a neighbouring column's
      // ground is 3+ voxels lower — precisely the ground the gate has already
      // taken the wedge to zero on. map.json terrain.sedSlope is the knob and
      // 0 turns the feature off.
      //
      // Topsoil first because that is the order a soil profile has, and because
      // gravel is what you want to hit when you dig a valley floor for
      // something that flows.
      if (y > h - 1 - wmTerrain(WM_H_TERRAIN_SED_TOPSOIL)) { mat = wmBiome(biome, WM_B_SUBSOIL); }
      else { mat = M_GRAVEL; }
    } else {
      mat = M_STONE;
    }
    // Caves carve the stone body, lava-filled below the magma table. No caves
    // under an authored pool/rim — a cave breaching a rim column drains the
    // pool through the tunnel system and the world never settles.
    if (mat == M_STONE && !inRim) {
      // The bands, not just the answer: caveFloraAt needs f1/f2/c2 to ask where
      // the floor and the ceiling of THIS cavern are.
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
    // Its own hash salt, like every other species here (the pond-life block
    // below says why). Chance and material are the water preset's
    // (shore.mossChance / mossMaterial); preset 0 reads 0 and grows none.
    if (GROUND_COVER && mat == M_STONE && y == h && shore.onShore) {
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
  // (the disc ponds) rather than to any fluid: the authored pool and the sea
  // are the same `fluid` machinery, and the disc pond is the only body whose
  // floor and surface are both known here as pure functions of the column.
  //
  // Everything is an INERT solid placed once at generation. Nothing grows,
  // spreads or reacts with the water it stands in — a plant that did would
  // keep every pond chunk awake forever and break the sleep budget (rule 2).
  //
  // SEPARATE HASHES PER SPECIES, not bit-slices of one. Slices (h, h>>3,
  // h>>17) share entropy, so the rolls correlate and a column that grew one
  // plant is far likelier than chance to grow another -- a scattered planting
  // collapses into a solid wall of stalks. Distinct salts cost a hash each
  // and are actually independent. Every salted roll in this file follows the
  // same rule.
  //
  // WHICH plants, at WHAT depth, HOW tall: the water preset's aquatic bands
  // (assets/water/<name>.json aquatic.emergent / floating / submerged), read
  // from the worldMap table -- the pond's OWN preset (`wp`, the row that
  // rolled it or the site that placed it). Depths are voxels of water
  // over the bed, heights cells above the bed. A band with chance 0 rolls
  // nothing (rollChance).
  if (GROUND_COVER && mat == M_WATER && pond >= 0) {
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
  if (GROUND_COVER && mat == MAT_AIR && pond >= 0 && y > pond) {
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

  // ---- trees ----
  // Only above ground, below the treeline, out of the water and never inside
  // an authored rim. The column half of this gate is colGrowsTrees, which
  // genChunk and `far` use to skip the candidate scan: keep the two in step.
  if (VEGETATION && mat == MAT_AIR && !inRim && y > h && h < treeline() && pond < 0) {
    var tm = MAT_AIR;
    if (treeValid) { tm = treeFromCands(trees, y); }
    else { tm = treeAt(x, y, z, seed, ponds); }
    if (tm != MAT_AIR) { mat = tm; }
  }

  // ---- shore cover: the marsh fringe outside the pond ----
  // The shore band genColumn narrowed (Col.shore). Placed only into cells
  // that are still AIR above the ground, so nothing here can displace
  // terrain, and -- because `shore.onShore` is only ever set outside a disc
  // -- nothing here can spill into the bowl. Running AFTER the tree block
  // means a trunk rooted on the bank keeps its cells and the marsh grows
  // around it. Everything is an INERT solid (rule 2), like the pond life.
  //
  // THE SPECIES ARE THE WATER PRESET'S (assets/water/<name>.json
  // shore.plants[], packed as WM_P_* rows). Rows are rolled IN AUTHORED
  // ORDER and the first hit wins, exactly like the biome cover stack, so the
  // author puts the water-hugging species (small reach, tall) first and the
  // ground layer that covers the whole band LAST; that ordering is what makes
  // the band read as a gradient from the water. One salt per row.
  //
  // Per row: `reach` is how far past the waterline (shore.past, voxels) it
  // still grows; `height` is the stalk, jittered per column by +-(H/6, at
  // least 1) for stalks of 3+ so a bed of stalks does not read as a fence;
  // `head`, when named, caps the top max(1, H/8) cells, on the SAME roll so a
  // head never floats over no stalk. worldmap.cpp's MaxPlantH includes the
  // jitter, so the sky-skip and far blocker ceilings cover the tallest column
  // a row can produce.
  if (GROUND_COVER && mat == MAT_AIR && shore.onShore && y > h) {
    let up = y - h;                  // voxels above this column's ground
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

  if (mat == MAT_AIR && (*col).plant.mat != MAT_AIR && y > (*col).plant.base &&
      y <= (*col).plant.top) {
    mat = (*col).plant.mat;
  }


  // ---- CACTI: the cactus biomes' metre-scale shape --------------------------
  // cactusAt, placed into air ABOVE the surface exactly the way a tree is, and
  // BEFORE the cover stack so the scrub floor fills around it. Keep-outs (the
  // site / harness keep-out, pond discs, the treeline) are enforced at the
  // SITE by cactusInfo, so a cactus rooted outside cannot lean back in; the
  // column tests here are the authored rim, the pond and the treeline.
  if (VEGETATION && mat == MAT_AIR && wmFlag(biome, WM_BF_CACTI) && !inRim && y > h && pond < 0 &&
      h < treeline()) {
    let cm = cactusAt(x, y, z, seed, ponds);
    if (cm != MAT_AIR) { mat = cm; }
  }

  // ---- THE BIOME'S OWN COVER STACK (assets/biomes/<name>.json cover.plants) --
  // Every biome's floor -- grass, flowers, undergrowth, scrub, alpine cushion
  // -- is authored rows, no shader edit per biome. Cells above the surface,
  // only where the blocks above left air, with a patch mask so the plants grow
  // in stands rather than as uniform static.
  //
  // Rows are rolled IN ORDER and the first hit wins, so an author puts the
  // common ground layer last, the way the shore set does. One salt per row
  // (see the pond-life block). The patch field is sampled at a per-row offset
  // so the rows' lattices do not line up at cell corners.
  //
  // Cost: on surface columns only, one vnoise2d + one hash per AUTHORED row
  // until a hit; a biome with no rows pays one header read. Everything placed
  // is inert (rule 2).
  //
  // No treeline gate: a row's own minY / maxY is its altitude band (the
  // alpine cushion is a row with `minY` at the treeline).
  //
  // THE CANOPY CONDITION: rows may bound undergrowthSite's cover
  // (WM_C_CANOPY_MIN / MAX). The scan runs ONCE PER COLUMN, and only for a
  // biome that authors such a row (WM_BF_CANOPY_ROWS): genChunk and `far`
  // hand the column's answer in as `canopyMemo`; the far skin lookup passes -1
  // and pays the scan here, once, for the surface cell it asks for.
  if (GROUND_COVER && mat == MAT_AIR && y > h && !inRim && pond < 0 &&
      !siteKeepOut(x, z)) {
    let up = y - h;
    let nRows = wmBiome(biome, WM_B_COVER_COUNT);
    let bThresh = i32(wmBiome(biome, WM_B_PATCH_THRESH));
    let pLog2 = wmBiome(biome, WM_B_PATCH_LOG2);
    var canopy = canopyMemo;
    if (canopy < 0 && wmFlag(biome, WM_BF_CANOPY_ROWS)) {
      canopy = undergrowthSite(x, z, seed, ponds).cover;
    }
    for (var i = 0u; i < nRows; i++) {
      let chance = wmCover(biome, i, WM_C_CHANCE);
      if (chance == 0u) { continue; }
      let hRow = hash3(seed ^ (0xC0E0u + i * 0x9E37u), bitcast<u32>(x), bitcast<u32>(z));
      if ((hRow % chance) != 0u) { continue; }
      let cMin = i32(wmCover(biome, i, WM_C_CANOPY_MIN));
      let cMax = i32(wmCover(biome, i, WM_C_CANOPY_MAX));
      if (max(canopy, 0) < cMin || max(canopy, 0) > cMax) { continue; }
      // Conditions: altitude band, steepness and water distance, per row,
      // like a species'. One compare each on the column's own numbers; the
      // water distance (waterDistAt, arithmetic on the pond set) is paid only
      // by a row that authors a bound, AFTER the chance roll.
      let minY = bitcast<i32>(wmCover(biome, i, WM_C_MIN_Y));
      let maxY = bitcast<i32>(wmCover(biome, i, WM_C_MAX_Y));
      if (minY >= 0 && h < minY) { continue; }
      if (maxY >= 0 && h > maxY) { continue; }
      let rSlope = i32(wmCover(biome, i, WM_C_MAX_SLOPE));
      if (rSlope > 0 && rSlope < 1024 && (*col).slope > rSlope) { continue; }
      let nwMax = bitcast<i32>(wmCover(biome, i, WM_C_NEAR_WATER_MAX));
      let nwMin = i32(wmCover(biome, i, WM_C_NEAR_WATER_MIN));
      if (nwMax >= 0 || nwMin > 0) {
        let d = waterDistAt(ponds, x, z, max(nwMax, nwMin));
        if (!nearWaterOk(d, nwMax, nwMin)) { continue; }
      }
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

  // Reactive seeds / stems are deliberately NOT placed by worldgen: a growth
  // source keeps chunks awake, and the garden is a brush stroke away.

  // ---- authored sites: a stamp overlays everything above its pad ----------
  // One plane read for the common case (no site); on a site's cells, the
  // template's column of runs. Non-air template voxels replace whatever the
  // terrain and cover put here; template air leaves the world alone, so a
  // stamp is a building on the ground, not a box cut out of it.
  {
    let sid = wmSiteAt(x, z);
    if (sid != 0u && y > h - 2) {
      let sm = wmStampCell(sid, x, y, z, sitePadY(sid));
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
  // be free to move on the first tick it is simulated.
  return packVox(mat, state, STAMP_NEVER);
}

// One cell of a column the caller already holds, with nothing hoisted: the
// cave bands and tree candidates are computed on demand (the `*Valid = false`
// spelling of genCellIn). The far entries' surface-skin lookup is the caller
// -- one cell per column, so there is nothing to amortize. `col` and `ponds`
// are genColumn's answer for (c.x, c.z).
fn genCellCol(col : ptr<function, Col>, ponds : ptr<function, PondSet>,
              c : vec3<i32>, seed : u32) -> u32 {
  var cave : CaveBands;
  var trees : TreeCands;
  return genCellIn(col, &cave, false, &trees, false, ponds, -1,
                   c.x, c.y, c.z, seed);
}

// ---- far-field helpers: shared by `far`, `farpatch` and `fardown` ---------
// XZ canopy footprint, ignoring height: which tree crown (if any) covers this
// column. At coarse cascade levels a whole tree is thinner than one cell, so
// the center sample loses it and distant forest degraded into bare grass; the
// far field flattens crowns into the terrain skin instead — the horizon keeps
// its canopy color even where no individual tree survives sampling. Pure
// function of (coords, seed), same as everything the sieve uses.
fn treeCanopyAt(x : i32, z : i32, seed : u32, ponds : ptr<function, PondSet>) -> u32 {
  let tx = fdiv(x, TREE_TILE);
  let tz = fdiv(z, TREE_TILE);
  for (var oz = -TREE_SCAN + unrollFence(); oz <= TREE_SCAN; oz++) {
    for (var ox = -TREE_SCAN + unrollFence(); ox <= TREE_SCAN; ox++) {
      let t = treeInfo(tx + ox, tz + oz, seed, ponds);
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
      // An airy species spreads its foliage in tufts with sky
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
// procgen), the edit patch (`farpatch`) and the downsample (`fardown`, live
// grid). The centre sample decides SHAPE (occupancy); this decides COLOUR: a
// cell that straddles the terrain surface takes the surface SKIN material
// (whatever genCellIn puts at y == h) instead of whatever body material the
// centre sample happened to land on -- otherwise a coarse cell whose centre
// sits one voxel under a 1-voxel grass skin stores STONE, and distant
// hillsides read as grey rock with green contour stripes.
//
// All three call this same pure function of (col, ponds, mat, coords, level,
// seed), so downsampled and pristine regions agree exactly at their
// boundaries (the `far-downsample` gate). `col` and `ponds` must be
// genColumn's answer at (fine.x, fine.z): reading `col.h` is what keeps the
// far field on the height contract's own definition of ground.
fn farSurfaceMat(col : ptr<function, Col>, ponds : ptr<function, PondSet>,
                 mat : u32, fine : vec3<i32>, shift : u32, seed : u32) -> u32 {
  let k = materials[mat].klass;
  if (k != CLASS_SOLID && k != CLASS_POWDER) { return mat; }  // fluids keep their ID
  var h = (*col).h;
  // "Topmost solid cell of this column": solid means center <= h, and the cell
  // above (center + 2^shift) samples past h. NOT "cell span contains h" — when
  // h lands in a cell's lower half that cell's center samples air (the cell is
  // empty) and the visible top face belongs to the cell BELOW, whose span does
  // not contain h. The span version left half of all surface cells stone-gray.
  if (fine.y > h || h >= fine.y + (1 << shift)) { return mat; }
  // canopy flattening only where cells are 2 m+ (32+ fine voxels); finer
  // levels still resolve trees as shapes and double-painting would fatten them
  if (shift >= 5u) {
    let can = treeCanopyAt(fine.x, fine.z, seed, ponds);
    if (can != MAT_AIR) { return can; }
  }
  // MEADOW COVER IS NOT FLATTENED INTO THE SKIN (tried and taken out): the
  // strand palette painted over a far meadow reads darker than the near
  // field, where the eye sees the skin between thin blades. A blade thinner
  // than a cascade cell contributes nothing to the far field (farCellIsSolid
  // drops the plant cell itself); the skin is what the far field paints.
  let skin = genCellCol(col, ponds, vec3<i32>(fine.x, h, fine.z), seed) & 0xFFFu;
  // a stamp's hollow interior can be air at y == h; keep the body mat then
  if (skin == MAT_AIR || materials[skin].klass == CLASS_GAS) { return mat; }
  return skin;
}

// ---- WHAT A CENTRE SAMPLE MAY TURN INTO A CASCADE CELL --------------------
// Shared by the sieve, the downsample and the edit patch, so the three
// producers keep their byte-for-byte agreement (the `far-downsample` gate).
// Gases are dropped (no media in the far field), and so are MICRO materials:
// a grass tuft or a flower is a mostly-air cell the renderer fills with blades
// or a sub-voxel model, and its centre sample would become a SOLID CUBE of the
// plant's palette, 20 cm at level 1 and 25 m at level 8. A blade contributes
// nothing to the far GEOMETRY; the ground under it reaches the far field
// through farSurfaceMat's skin.
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
// THE TOP OF A COLUMN, as the far field sees it: the ground contract, plus
// what stands on top of it and is not in `h` -- standing fluid (it keeps its
// id and renders opaque at distance), the tallest cover row
// (WM_H_MAX_COVER_H), and an authored site's stamp as a BOUNDING BOX (a
// conservative bit may fill a hollow interior no cascade cell could show).
//
// TREES ARE DELIBERATELY ABSENT. `treeCanopyAt` is an XZ mask with no height,
// and the only vertical bound available for it is `treeMaxAbove()`, which is
// WIDER than a level-5 cell — flagging that band would turn every forested
// column into a column of solid cubes. The canopy is carried at distance by
// `farSurfaceMat`'s flattening, which paints crown colour onto the surface
// cell at shift >= 5.
fn farColTopFrom(h : i32, fluidTop : i32, x : i32, z : i32) -> i32 {
  var top = max(h, fluidTop);
  // The biome cover stack stands ON the ground and, since the world map's P1,
  // is authored data that can be taller than a far cell: a stalk that pokes
  // into the cell above the flagged ground is a blocker the flag would
  // otherwise miss (the far-fog gate's "flag sits below the material top").
  // Global max, not per-biome: the corner-column callers hold no biome, and
  // the bit is conservative by design -- over-flagging costs nothing.
  top = max(top, h + i32(worldMap[WM_H_MAX_COVER_H]));
  top = max(top, wmSiteTopAt(x, z));   // an authored stamp
  return top;
}
// The same for a column the caller does not already hold. `landColumn`, not
// `genColumn`: the top needs the height contract and nothing else, and the
// biome / shore / tile-plant half of a Col is pure cost here.
fn farColTop(x : i32, z : i32, seed : u32) -> i32 {
  var L : LandCol;
  landColumn(x, z, seed, &L);
  return farColTopFrom(L.h, L.fluidTop, x, z);
}

// The flag for one level cell, in THREE BANDS by the cell's floor `y0`
// against `topC`, the `farColTopFrom` of the cell's CENTRE column (free at
// every call site: each already holds that column for the colour lookup):
//
//   y0 <= topC              the cell's floor is at or below the centre
//                           column's top: it is at or under the surface. Set,
//                           no further samples. (Caves included on purpose.)
//   y0 - topC >= step       more than a whole cell of air under the cell's
//                           floor: clear, no further samples.
//   otherwise               THE SURFACE BAND — exactly ONE cell per column,
//                           the one whose centre sampled air but whose span
//                           straddles the ground. Here, and only here, the
//                           four corner columns count (`btop`, farCornerTop).
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
//
// ONE RULE, TWO SHAPES OF CALLER. `far` sweeps whole columns and hoists the
// corner scan (at most one row per column is in the band, so it computes
// `btop` once); `farpatch` and `fardown` visit scattered cells and pay the
// scan only for a cell in the band (farBlockerBitAt). All three end in
// farBlockerBand, so their bytes cannot drift (the `far-downsample` gate).
fn farBlockerBand(y0 : i32, topC : i32, btop : i32, step : i32) -> u32 {
  if (y0 <= topC) { return FAR_BLOCKER_BIT; }
  if (y0 - topC >= step) { return 0u; }
  return select(0u, FAR_BLOCKER_BIT, y0 <= btop);
}
// The surface band's bound: topC and the four corner columns of the level
// cell at (ccx, ccz). One rolled loop, not four straight-line calls: each
// farColTop inlines a full landColumn, and the unroll fence keeps this body in
// the kernel once (see unrollFence(); this scan is what made `far`/`fardown`
// compile forever after the pond table grew landColumn).
fn farCornerTop(topC : i32, ccx : i32, ccz : i32, shift : u32, seed : u32) -> i32 {
  let step = 1 << shift;
  let x0 = ccx << shift; let x1 = x0 + step - 1;
  let z0 = ccz << shift; let z1 = z0 + step - 1;
  var top = topC;
  for (var ci = unrollFenceU(); ci < 4u; ci++) {
    let cx = select(x0, x1, (ci & 1u) != 0u);
    let cz = select(z0, z1, (ci & 2u) != 0u);
    top = max(top, farColTop(cx, cz, seed));
  }
  return top;
}
// The per-cell form, for the scattered-cell callers.
fn farBlockerBitAt(topC : i32, cc : vec3<i32>, shift : u32, seed : u32) -> u32 {
  let step = 1 << shift;
  let y0 = cc.y << shift;               // the cell's bottom fine voxel
  var btop = topC;
  if (y0 > topC && y0 - topC < step) { btop = farCornerTop(topC, cc.x, cc.z, shift, seed); }
  return farBlockerBand(y0, topC, btop, step);
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
// The verdict's second sweep (see the tail of genChunk): whether the chunk is
// FULL (read through workgroupUniformLoad, so the sweep's barriers sit in
// uniform control flow), and whether any cell differs from cell 0's word /
// from the JITTER synthesis at its position.
var<workgroup> wgGenFull : u32;
// Whether genChunk fills the tree tile cache (read through
// workgroupUniformLoad, because the fill ends in a barrier).
var<workgroup> wgTreeOn : u32;
var<workgroup> wgNotEq : atomic<u32>;
var<workgroup> wgNotJit : atomic<u32>;

fn storeSubOcc(slot : u32, t0 : u32, t1 : u32, b0 : u32, b1 : u32) {
  occupancy[subOccIndex(slot, 0u, 0u)] = t0;
  occupancy[subOccIndex(slot, 0u, 1u)] = t1;
  occupancy[subOccIndex(slot, 1u, 0u)] = b0;
  occupancy[subOccIndex(slot, 1u, 1u)] = b1;
}

// `actIdx` is the caller's position in genList — the index genAct is keyed on.
// `publish` is true for the streaming `list` entry point, which writes the
// verdict word (act bit + page-table class) for every position genAct holds;
// the dense `main` path passes false and its own workgroup id, publishes
// nothing and never defers.
fn genChunk(slot : u32, li : u32, actIdx : u32, publish : bool) {
  if (li == 0u) {
    atomicStore(&wgCount, 0u);
    atomicStore(&wgBlock, 0u);
    atomicStore(&wgAct, 0u);
    atomicStore(&wgNotEq, 0u);
    atomicStore(&wgNotJit, 0u);
    atomicStore(&wgSub[0], 0u);
    atomicStore(&wgSub[1], 0u);
    atomicStore(&wgSub[2], 0u);
    atomicStore(&wgSub[3], 0u);
  }
  workgroupBarrier();

  let base = slotWorldChunk(slot, T.origin) * i32(CHUNK);
  var count = 0u;
  var block = 0u;
  var act = 0u;
  var sm0 = 0u; var sm1 = 0u;   // sub-chunk TOTAL class
  var sb0 = 0u; var sb1 = 0u;   // sub-chunk BLOCKER class
  // THE CHUNK'S COLUMNS COME FROM THE COLUMN CACHE (`cols`, below; the block
  // at the top of the file). Everything about a column that is a function of
  // (x, z) alone -- genColumn's Col, the cave bands, the canopy memo, the
  // loose step line, the sky ceiling, the tree candidates' top -- was
  // evaluated ONCE per (x, z) of the list by the pre-pass, not once per
  // listed chunk on the column (~32 on a streamed X/Z plane).
  let blk = genCols[actIdx] * CC_BLOCK;
  let chunkTop = bitcast<i32>(colCache[blk + CCH_TOP]);
  // ---- THE TREE TILE CACHE, if any column of this chunk rebuilds its tree
  // candidates below: the chunk is not all sky, not above treeMaxTop() or
  // every column's candidate top, and not below every column's ground (the
  // per-column test further down, maxed / minned over the block). Decided by
  // one thread and read back uniformly, because the fill ends in a barrier.
  if (li == 0u) {
    wgTreeOn = select(0u, 1u,
        base.y <= chunkTop && base.y <= treeMaxTop() &&
        base.y <= bitcast<i32>(colCache[blk + CCH_TREE_TOP]) &&
        base.y + i32(CHUNK) - 1 > bitcast<i32>(colCache[blk + CCH_MIN_H]));
  }
  if (workgroupUniformLoad(&wgTreeOn) != 0u) {
    treeTilesFill(li, base.x, base.z, T.seed);
    workgroupBarrier();
  }
  // ---- THE SKY EARLY-OUT -------------------------------------------------
  //
  // On a streamed-in vertical plane most chunks are sky, and a sky cell would
  // still walk the whole of genCellIn to conclude nothing. Each column's
  // CEILING (CCW_TOP, computed by `cols` -- the enumeration and its proof are
  // there) bounds every non-air cell genCellIn can place over it; when the
  // chunk's lowest cell is above it the column stores sixteen zeros -- exactly
  // the word genCellIn returns for MAT_AIR (`if (mat == MAT_AIR) { return 0u;
  // }`), contributing nothing to `count`/`block`/`act` or to either
  // sub-occupancy mask, and therefore the same chunk by every observable.
  // The block header holds the MAX of its columns' ceilings: above THAT every
  // column would skip, so the whole chunk stores zeros without reading one
  // column record.
  if (base.y > chunkTop) {
    for (var i = li; i < CHUNK_VOL; i += 64u) {
      voxStore(voxWordInChunk(slot, i), 0u);
    }
  } else {
    // COLUMN-MAJOR: each column record is read ONCE and shared by the 16
    // cells stacked on it. 256 columns over 64 threads is four columns each,
    // 16 cells apiece.
    //
    // The writes stay coalesced enough: within one height step threads 0..15
    // hold x = 0..15 of the same z and write 16 CONSECUTIVE words, so the wave
    // issues four 64-byte runs instead of one 256-byte one.
    var col : Col;
    var cave : CaveBands;
    var trees : TreeCands;
    var ponds : PondSet;
    for (var ci = li; ci < CHUNK * CHUNK; ci += 64u) {
      let lx = ci % CHUNK;
      let lz = ci / CHUNK;
      let wx = base.x + i32(lx);
      let wz = base.z + i32(lz);
      let cw = blk + CC_HDR + ci * CC_WORDS;
      if (base.y > bitcast<i32>(colCache[cw + CCW_TOP])) {
        for (var ly = unrollFenceU(); ly < CHUNK; ly += 1u) {
          voxStore(voxWordInChunk(slot, lx + ly * CHUNK + lz * CHUNK * CHUNK), 0u);
        }
        continue;
      }
      // The column and its cave bands. The bands are handed to genCellIn as
      // VALID unconditionally (which drops its fallback caveBands from this
      // entry): it reads them only for a cell `y <= h` in a column that is
      // not a rim, and `cols` computed them wherever ANY listed chunk on the
      // column has such a cell (colPrologue's cave guard over the list's
      // y-range). The loose step line and the canopy memo are exact on the
      // same argument: `cols` computed each wherever a cell of a listed
      // chunk can read it, and no cell reads it anywhere else (looseRestTop's
      // band is (h - max(4, skinDepth), h]; the canopy's, colPrologue's note).
      ccLoad(cw, &col, &cave);
      // Does this chunk hold a cell above the ground? Everything that reads
      // the pond set or the tree candidates (the tree block, cacti, the cover
      // rows' water distance) is gated `y > h`.
      let above = base.y + i32(CHUNK) - 1 > col.h;
      // The pond set: genColumn's is pondScan(x, z), and an EMPTY one is
      // pondSetNone() exactly (pondScan pushes only present candidates onto
      // pondSetNone()), so only a column the cache flags as non-empty re-runs
      // the scan, and only in a chunk whose cells can read it.
      if (above && (colCache[cw + CCW_PACKED] & (CCF_PONDS << 24u)) != 0u) {
        ponds = pondScan(wx, wz, T.seed);
      } else {
        ponds = pondSetNone();
      }
      // Tree candidates: the SET, rebuilt only in a chunk that can hold a tree
      // cell. genCellIn reads `trees` in one place, the tree block, gated
      // `colGrowsTrees && y > h` (colGrowsTrees plus `y > h`), and a set
      // answers air for every y above its `top` -- so an empty set is
      // indistinguishable from the full one when
      //   * no cell of the chunk is above the ground (`above` false), or
      //   * the chunk's lowest cell is above treeMaxTop(), or
      //   * the chunk's lowest cell is above the column's own candidate top
      //     (CCW_TREE_TOP: `cols` scanned exactly this set wherever any listed
      //     chunk passes the first two tests, and stored far below any y when
      //     the column fails colGrowsTrees -- so it also carries that test).
      // The set is read from the tree tile cache, filled above whenever this
      // test can pass for any column of the chunk (its terms maxed / minned
      // over the block imply the block-level test).
      let treeTop = bitcast<i32>(colCache[cw + CCW_TREE_TOP]);
      if (above && base.y <= treeMaxTop() && base.y <= treeTop) {
        treeCandsFromTiles(&trees, wx, wz, base.x, base.z, &ponds);
      } else {
        colNoTrees(&trees);
      }
      let canopyArg = max(bitcast<i32>(colCache[cw + CCW_CANOPY]), 0);
      for (var ly = unrollFenceU(); ly < CHUNK; ly += 1u) {
        let i = lx + ly * CHUNK + lz * CHUNK * CHUNK;
        let w = genCellIn(&col, &cave, true, &trees, true, &ponds, canopyArg,
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
    // dirty set, and the same world hash.
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
    //
    // The genAct word itself is written at the very end, once the verdict's
    // second sweep below has run: it carries this act bit AND the class.
    if (T.genDeferWake != 0u) {
      atomicStore(&dirtyIn[slot], 0u);
      atomicStore(&dirtyOut[slot], 0u);
    } else if (canAct) {
      atomicOr(&dirtyIn[slot], DIRTY_R_MUTATE);
      atomicOr(&dirtyOut[slot], DIRTY_R_MUTATE);
    } else {
      atomicStore(&dirtyIn[slot], 0u);
      atomicStore(&dirtyOut[slot], 0u);
    }
    wgGenFull = select(0u, 1u, n == CHUNK_VOL);
  }

  // ---- THE GENERATION VERDICT (world.h kGenVerdict*) -------------------
  //
  // PageTable::Classify's answer for the words this workgroup just wrote,
  // reduced here instead of on the CPU — which used to read every generated
  // chunk back (16 x 32 MiB synchronous per worldgen) or, on a shift, every
  // FULL one (16 KiB each, harvested frames later). Same tests, same order:
  //
  //   EMPTY    count == 0. Exact, not a hint: genCellIn returns 0u for air
  //            and worldgen writes no stain and no bit 31, so every word is
  //            0 and Classify's stainless-air mask holds in all 4,096 cells
  //            (the argument ApplyGenVerdict's sky demote already makes).
  //   mixed    0 < count < 4,096: some cell is air and some is not, so the
  //            words are not all equal and not all one material — no
  //            sentinel form, class 0.
  //   FULL     count == 4,096: the only case that needs the words. One more
  //            sweep reads them back (storageBarrier: they were written by
  //            other threads of this workgroup) and asks the two remaining
  //            questions: all equal to cell 0's word (UNIFORM, if that word
  //            is exactly synthWord's), else every cell exactly the JITTER
  //            synthesis for cell 0's material at its world position,
  //            stamp included. Buried bulk only; sky and surface never pay.
  //
  // The CPU applies Classify's two refusals the kernel cannot see (JITTER
  // disabled, a mismatched seed) in PageTable::ClassifyGenVerdict.
  if (!publish) { return; }
  let full = workgroupUniformLoad(&wgGenFull);
  var w0 = 0u;
  if (full != 0u) {
    storageBarrier();
    let e = pageTable[slot];
    if ((e & PT_SENTINEL_BIT) == 0u) {
      let pageBase = e * CHUNK_VOL;
      w0 = voxels[pageBase];
      let jEntry = PT_SENTINEL_BIT | PT_JITTER_BIT | (w0 & PT_MAT_MASK);
      var ne = 0u;
      var nj = 0u;
      for (var ci = li; ci < CHUNK_VOL; ci += 64u) {
        let w = voxels[pageBase + ci];
        ne |= w ^ w0;
        if (nj == 0u) {
          let lc = vec3<i32>(i32(ci % CHUNK), i32((ci / CHUNK) % CHUNK),
                             i32(ci / (CHUNK * CHUNK)));
          nj |= w ^ synthWordAt(jEntry, base + lc, T.seed);
        }
      }
      if (ne != 0u) { atomicOr(&wgNotEq, 1u); }
      if (nj != 0u) { atomicOr(&wgNotJit, 1u); }
    } else {
      // No page to read (cannot happen: every genList slot is materialized
      // before the dispatch) — publish no sentinel claim.
      if (li == 0u) {
        atomicOr(&wgNotEq, 1u);
        atomicOr(&wgNotJit, 1u);
      }
    }
    workgroupBarrier();
  }
  if (li == 0u && actIdx < arrayLength(&genAct)) {
    let n = atomicLoad(&wgCount);
    let mat = w0 & PT_MAT_MASK;
    var cls = 0u;
    if (n == 0u) {
      cls = GEN_V_EMPTY;
    } else if (full != 0u) {
      if (atomicLoad(&wgNotEq) == 0u) {
        if (synthWord(PT_SENTINEL_BIT | mat) == w0) { cls = GEN_V_UNIFORM; }
      } else if (atomicLoad(&wgNotJit) == 0u) {
        cls = GEN_V_JITTER;
      }
    }
    genAct[actIdx] = select(0u, GEN_V_ACT, atomicLoad(&wgAct) > 0u) |
                     GEN_V_VALID | (cls << GEN_V_CLASS_SHIFT) |
                     (mat << GEN_V_MAT_SHIFT);
  }
}

// The whole slot space: NUM_SLOTS workgroups. Publishes no verdict (genAct is
// sized for a list, and nothing classifies a dense whole-world dispatch).
@compute @workgroup_size(64)
fn main(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  genChunk(wg.x, li, wg.x, false);
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
  genChunk(genList[wg.x], li, wg.x, true);
}

// ---- THE COLUMN CACHE PRE-PASS ----------------------------------------------
// One workgroup per CHUNK-COLUMN of the list (genCols record wg.x; the CPU
// dispatches exactly that many -- the extent is the bound, as in `pagefill`),
// four of its 256 columns per thread. Runs in the same command buffer as
// `list` / `main`, ahead of it; the pass table's ColCache edge is the barrier.
//
// Every per-column answer is computed over [yLo, yHi], the y-range of ALL
// the listed chunks on this column, and each skip in it is exact for any
// chunk inside that range (colPrologue's note; genChunk's notes say why each
// value it reads is exact for its own chunk).
//
// THE SKY CEILING (CCW_TOP) IS AN ENUMERATION, NOT AN ESTIMATE, and it is the
// only part of the skip that can be wrong. Every block in genCellIn that can
// leave `mat` non-air above the ground, with the highest y it can reach:
//
//   terrain body / skin / wet moss        h
//   standing fluid, and the pond life     fluidTop
//     placed INTO water cells
//   pond life above the waterline         max(pond + 1, h + emergent height)
//     (`bed` is min(h, pw.x) <= h, so the
//      reed test y - bed < H bounds by h)
//   trees                                 trees.top (exact, per column: see below)
//   shore rows, the biome cover stack     h + colSkyMargin
//   tile plants (fern, big toadstool)     plant.top -- NOT a term below:
//     SKY_MARGIN_MIN covers it only while the plant's centre column stands
//     within 3 of this column's h, which is not proven (`far` does add
//     plant.top). Adding it here only declines skips, but would move the
//     hash if that gap is ever hit, so it waits for a hash-moving package.
//   an authored site's stamp              wmSiteTopAt
//   cacti                                 cactusMaxTop(), cactus biomes only
//
// All the h-relative plant reaches collapse into one margin, which is
// strictly conservative: over-estimating the ceiling only declines a skip.
//
// TREES: the per-chunk skip this replaced used the candidates scanned FOR THAT
// CHUNK, empty where the chunk could hold no tree cell. This is the column's
// scan over the list's y-range, and the decision `base.y > ceiling` is the
// same for every chunk: where that chunk would have scanned, this IS its set;
// where it would not have, it was either buried (base.y <= h <= ceiling, no
// skip either way) or above treeMaxTop() (and every candidate's top is below
// that, so the term cannot decide the compare).
//
// CACTI HAVE NO COLUMN-LOCAL BOUND: a saguaro stands at a NEIGHBOURING tile's
// ground height and cactusAt clips it against that site's own `base`, not
// against this column's `h`. The bound is the world-wide one (cactusMaxTop:
// a cactus roots only below the treeline), which still lets a cactus column's
// sky skip.
var<workgroup> wgColTop : atomic<i32>;
var<workgroup> wgColTreeTop : atomic<i32>;
var<workgroup> wgColMinH : atomic<i32>;

@compute @workgroup_size(64)
fn cols(@builtin(workgroup_id) wg : vec3<u32>,
        @builtin(local_invocation_index) li : u32) {
  let r = GC_REC_BASE + wg.x * GC_REC_WORDS;
  let cx = bitcast<i32>(genCols[r]);
  let cz = bitcast<i32>(genCols[r + 1u]);
  let yLo = bitcast<i32>(genCols[r + 2u]) * i32(CHUNK);
  let yHi = bitcast<i32>(genCols[r + 3u]) * i32(CHUNK) + i32(CHUNK) - 1;
  let bx = cx * i32(CHUNK);
  let bz = cz * i32(CHUNK);
  // The block's tree tiles, once (~36 trees), for the two tile scans below.
  // Unconditional: it is cheap next to 256 genColumns, and a barrier under a
  // storage-read condition would fail the uniformity analysis.
  if (li == 0u) {
    atomicStore(&wgColTop, -1048576);
    atomicStore(&wgColTreeTop, -1048576);
    atomicStore(&wgColMinH, 1048576);
  }
  treeTilesFill(li, bx, bz, T.seed);
  workgroupBarrier();
  let blk = wg.x * CC_BLOCK;
  var top = -1048576;
  var treeTopMax = -1048576;
  var minH = 1048576;
  for (var ci = li; ci < CHUNK * CHUNK; ci += 64u) {
    let x = bx + i32(ci % CHUNK);
    let z = bz + i32(ci / CHUNK);
    var ponds : PondSet;
    var col = genColumn(x, z, T.seed, &ponds);
    var cave : CaveBands;
    var trees : TreeCands;
    let need = colPrologue(&col, x, z, yLo, yHi, &cave);
    if (need.trees) { treeCandsFromTiles(&trees, x, z, bx, bz, &ponds); } else { colNoTrees(&trees); }
    var canopy = -1;
    if (need.canopy) { canopy = undergrowthCoverFromTiles(x, z, bx, bz, &ponds); }
    // THE LOCAL STEP LINE for a loose cover (looseRestTop, above genCellIn):
    // only where the cover splits, only where a listed chunk reaches the
    // loose band (the cap is 4 deep, a skin its authored skinDepth), and only
    // where the taper left loose depth to restrict. Every other column keeps
    // genColumn's `looseTop = h`.
    let coverDepth = max(4, i32(wmBiome(col.biome, WM_B_SKIN_DEPTH)));
    if (coverSplits(col.biome) && yLo <= col.h && yHi + 1 > col.h - coverDepth &&
        looseCoverDepth(&col, coverDepth) > 0) {
      col.looseTop = looseRestTop(&col, x, z, T.seed);
    }
    var colTop = col.h + colSkyMargin(col.biome);
    colTop = max(colTop, col.fluidTop);
    colTop = max(colTop, col.pond + 1);
    colTop = max(colTop, wmSiteTopAt(x, z));
    colTop = max(colTop, trees.top);
    if (wmFlag(col.biome, WM_BF_CACTI)) { colTop = max(colTop, cactusMaxTop()); }
    ccStore(blk + CC_HDR + ci * CC_WORDS, &col, &cave, ponds.n, canopy, colTop, trees.top);
    top = max(top, colTop);
    treeTopMax = max(treeTopMax, trees.top);
    minH = min(minH, col.h);
  }
  // Max and min are order-free, so the block header does not depend on which
  // thread arrives first (rule 1).
  atomicMax(&wgColTop, top);
  atomicMax(&wgColTreeTop, treeTopMax);
  atomicMin(&wgColMinH, minH);
  workgroupBarrier();
  if (li == 0u) {
    colCache[blk + CCH_TOP] = bitcast<u32>(atomicLoad(&wgColTop));
    colCache[blk + CCH_TREE_TOP] = bitcast<u32>(atomicLoad(&wgColTreeTop));
    colCache[blk + CCH_MIN_H] = bitcast<u32>(atomicLoad(&wgColMinH));
  }
}

// ---- JITTER page materialization (world.h's JITTER block) ----------------
// Filling a page that replaces a JITTER sentinel cannot be a vkCmdFillBuffer:
// the sentinel's words vary per cell, and a fill takes ONE 32-bit pattern.
// EMPTY and UNIFORM keep the cheap one-command fill; only JITTER comes here.
//
// This lives in worldgen.wgsl rather than its own file because it needs
// exactly what genChunk needs — the slot->world mapping, T.origin, T.seed —
// and sharing the file is what keeps the two positional rules from drifting.
// It does NOT run procgen: a JITTER chunk's material is whatever the sentinel
// says (it may be the result of play, not of worldgen), and only the palette
// VARIANT follows worldgen's formula. Regenerating the cell here would silently
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
  let base = slotWorldChunk(slot, T.origin) * i32(CHUNK);
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
// Lives in this file to share the column pipeline: a level-k cascade cell is
// filled by sampling genColumn + genCellIn at the FINE-voxel centre of the
// 2^k-wide region it covers, so cascades regenerate bit-identically from
// (coords, seed) at any stride.
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
// (mat << 12) | cellIndexInLevelChunk. Read by the `farpatch` entry below.
@group(1) @binding(5) var<storage, read> farPatch : array<u32>;
// fardown's skip (world.h farSig): the far-visible matter signature each slot
// had the last time it was downsampled.
@group(1) @binding(6) var<storage, read_write> farSig : array<u32>;

var<workgroup> wgFarCount : atomic<u32>;
// Non-air cells contributed by the patch pass (`farpatch`). Kept apart from
// wgFarCount because the two are answers to different questions and only their
// SUM is safe to publish (see the farOcc note at the bottom of `farpatch`).
var<workgroup> wgFarPatchNZ : atomic<u32>;
// One plus the chunk-local row of the highest non-empty cell. Both entries use
// it — `far` for its sweep, `farpatch` for its own cells — and each initialises
// it for itself, because they are separate dispatches and workgroup memory does
// not survive one (common.wgsl FAR_OCC_TOP_SHIFT: the far readers skip the air
// above it). 0 when the chunk is empty.
var<workgroup> wgFarTop : atomic<u32>;

@compute @workgroup_size(64)
fn far(@builtin(workgroup_id) wg : vec3<u32>,
       @builtin(local_invocation_index) li : u32) {
  if (wg.x >= T.farCount) { return; }
  if (li == 0u) {
    atomicStore(&wgFarCount, 0u);
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

  // ---- THE SWEEP, COLUMN-MAJOR (the far half of the genColumn/genCellIn split)
  //
  // A level chunk is 16^3 = 4096 cells but only 256 DISTINCT (x, z) columns:
  // the sample point is `(cc << shift) + half`, so fine.x and fine.z depend on
  // cc.x and cc.z alone. Thread `li` owns ONE z row and FOUR consecutive x —
  // exactly the four cells that pack into one farVox word — so it builds each
  // of its four columns once, reuses it for the 16 rows above, and writes
  // whole 32-bit words with no byte-granular read-modify-write and no
  // cross-thread sharing. (Workgroup memory for 256 Cols would cost occupancy
  // on a kernel whose job is bandwidth.)
  var count = 0u;
  let zi = li / 4u;              // this thread's z within the level chunk
  let x0 = (li % 4u) * 4u;       // and the first of its four x
  let step = 1 << shift;
  let half = 1 << (shift - 1u);
  // The row samples of this level chunk, lowest and highest (fine voxels).
  // Column-invariant: every column of the chunk samples the same sixteen y.
  let yLo = (base.y << shift) + half;
  let yHi = ((base.y + i32(CHUNK) - 1) << shift) + half;
  // ---- COLUMN-OUTER, ROW-INNER ------------------------------------------------
  //
  // The four columns are walked ONE AT A TIME, each with its sixteen rows
  // inside, and the sixteen output words are assembled in `words` byte by
  // byte. With one column live at a time the thread can hold genChunk's whole
  // COLUMN PROLOGUE for it (pond candidates, cave bands, tree candidates, the
  // canopy memo) and call genCellIn with the valid flags set, exactly as
  // genChunk does -- the flags pick the other spelling of the SAME function,
  // so the bytes are the unhoisted ones.
  var words : array<u32, CHUNK>;   // one output word per row; zero-initialised
  var top = 0u;   // one plus the highest row with a non-empty cell, this thread
  for (var b = unrollFenceU(); b < 4u; b++) {
    let cc0 = base + vec3<i32>(i32(x0 + b), 0, i32(zi));
    // the sieve's sample column: the fine-voxel centre of the cell footprint
    let fx = (cc0.x << shift) + half;
    let fz = (cc0.z << shift) + half;
    var ponds : PondSet;
    var col = genColumn(fx, fz, T.seed, &ponds);
    // The column top for the blocker bit (farBlockerBand's centre sample).
    let topC = farColTopFrom(col.h, col.fluidTop, fx, fz);
    // Corner-max top for the ONE surface-band cell of this column, hoisted out
    // of the row loop for COMPILE cost, not run cost: farCornerTop inlines FOUR
    // farColTop, each a full landColumn, and calling it from the cell loop put
    // those copies inside the loop body the driver unrolls; after the pond
    // table grew landColumnBare the driver's compile of this entry went from
    // part of a slow minute to tens of minutes at ~10 GB. For a fixed topC
    // exactly one multiple of `step` lies inside (topC, topC + step), so the
    // middle band fires for at most one cc.y per column: compute that row's
    // corner max here, once — same values, same laziness.
    var btop = topC;
    let bcy = fdiv(topC, step) + 1;
    if (bcy * step - topC < step &&
        bcy >= base.y && bcy < base.y + i32(CHUNK)) {
      btop = farCornerTop(topC, cc0.x, cc0.z, shift, T.seed);
    }

    // ---- THE COLUMN PROLOGUE (colPrologue), with the sample rows as the
    // stack. `ponds` is genColumn's own set, by pointer into every scan and
    // into farSurfaceMat. The cave bands can be declared valid (the constant
    // `true` below, which drops genCellIn's fallback caveBands from this
    // entry): every y genCellIn is asked about here is a row sample (>= yLo)
    // or the surface skin (y == h, looked up only for a cell whose sample is
    // <= h), so wherever colPrologue declined them nothing reads them.
    var cave : CaveBands;
    var trees : TreeCands;
    let need = colPrologue(&col, fx, fz, yLo, yHi, &cave);
    if (need.trees) { treeCandsInto(&trees, fx, fz, T.seed, &ponds); } else { colNoTrees(&trees); }
    var canopy = -1;
    if (need.canopy) { canopy = undergrowthSite(fx, fz, T.seed, &ponds).cover; }
    let canopyArg = max(canopy, 0);
    let skyMargin = colSkyMargin(col.biome);

    // ---- THE SKY CEILING: the row above which this column has nothing ----
    //
    // A level box is FAR_N cells tall — 13 km at level 8 — and the terrain
    // occupies a few hundred metres of it, so most of a coarse level's fill is
    // sky. A row is skippable when its floor is above everything a cell in it
    // could report:
    //   topC + step   the column's conservative top (ground, standing fluid,
    //                 the cover stack, an authored stamp) plus one cell,
    //                 because farBlockerBand's MIDDLE band fires for a floor
    //                 in (topC, topC + step) and must not be skipped.
    //   btop          that band's corner max, which can exceed topC.
    //   trees.top     trees are NOT in farColTopFrom (see its note), and a
    //                 tree rooted on a neighbouring, higher column can lean
    //                 over this one. The candidate set IS that neighbourhood,
    //                 so its `top` is the exact per-column bound genChunk's own
    //                 sky skip uses (empty exactly where no sample can hold a
    //                 tree cell).
    //   the rest of genChunk's own column ceiling, which the global term used
    //                 to cover by being taller than anything: the h-relative
    //                 plant reach (skyMargin), the pond life (pond + 1) and the
    //                 tile plant standing on this column (cells end at
    //                 plant.top). CACTI have no column-local bound, so a
    //                 cactus-biome column adds the world-wide cactusMaxTop(),
    //                 as genChunk does.
    // Everything else genCellIn can put above ground is a gas, and
    // farCellIsSolid drops those already. Rows ascend, so the first row past
    // the ceiling ends the column: its bytes and every later row's stay 0.
    // `fardown` and `farpatch` keep the one-shot form — they visit single
    // scattered cells with no column to amortize over — and still agree byte
    // for byte (the `far-downsample` gate).
    var skyCeil = max(max(topC + step, btop), trees.top);
    skyCeil = max(skyCeil, max(col.h + skyMargin, max(col.pond + 1, col.plant.top)));
    if (wmFlag(col.biome, WM_BF_CACTI)) { skyCeil = max(skyCeil, cactusMaxTop()); }
    for (var yi = unrollFenceU(); yi < CHUNK; yi++) {
      // The row's floor.
      let yRow = (base.y + i32(yi)) << shift;
      if (yRow > skyCeil) { break; }
      let fine = vec3<i32>(fx, yRow + half, fz);
      let mat = genCellIn(&col, &cave, true, &trees, true, &ponds, canopyArg,
                          fine.x, fine.y, fine.z, T.seed) & 0xFFFu;
      // The conservative flag first: it is what a cell keeps when the centre
      // sample found nothing (common.wgsl FAR_BLOCKER_BIT), on the hoisted
      // topC/btop.
      var byteV = farBlockerBand(yRow, topC, btop, step);
      if (farCellIsSolid(mat)) {
        // shape from the center sample, color from the surface skin (phase 4).
        // What lands in the byte is the skin material's FAR PALETTE SLOT, not
        // its id (common.wgsl FAR_PAL_MASK); slots are identity while the id
        // fits in seven bits, so an unaliased material table writes exactly
        // the byte this line wrote before the palette existed.
        byteV |= matFarPal(&materials, farSurfaceMat(&col, &ponds, mat, fine, shift, T.seed));
      }
      // farOcc counts NON-EMPTY cells, which now includes blocker-only ones —
      // it gates empty-space skipping for every far reader, and a reader that
      // hits on the flag must not have its chunk skipped out from under it.
      if (byteV != 0u) { count += 1u; top = max(top, yi + 1u); }
      words[yi] |= byteV << (b * 8u);
    }
  }
  let planeBase = ((level - 1u) * FAR_VOX + slot * CHUNK_VOL) / 4u;
  // The cell index in this level chunk is x + y*CHUNK + z*CHUNK*CHUNK (see the
  // `farpatch` entry's unpack), so in words of four x-consecutive cells that is
  // x/4 + y*(CHUNK/4) + z*(CHUNK*CHUNK/4). x0 is a multiple of 4, so byte `b`
  // of the word is cell x0+b and the packing above is the same one the flat
  // form used.
  // Every row is stored UNCONDITIONALLY: this slot may hold the bytes of the
  // level chunk that used to live in it, and a row above the sky ceiling still
  // has to be cleared to the air it now is.
  for (var yi = 0u; yi < CHUNK; yi++) {
    atomicStore(&farVox[planeBase + zi * (CHUNK * CHUNK / 4u) +
                        yi * (CHUNK / 4u) + li % 4u],
                words[yi]);
  }
  atomicAdd(&wgFarCount, count);
  atomicMax(&wgFarTop, top);
  workgroupBarrier();
  if (li == 0u) {
    // farOcc only ever gates EMPTY-SPACE SKIPPING, so it must never be too
    // small and may be too large: an over-count costs one marched level chunk,
    // an under-count hides real terrain (the same conservative direction
    // `fardown`'s atomicMax takes).
    // The top row rides the same word (common.wgsl FAR_OCC_TOP_SHIFT).
    // `farpatch` below folds its own contribution into BOTH halves of this
    // word afterwards, in the same conservative direction.
    atomicStore(&farOcc[(level - 1u) * FAR_NUM_CHUNKS + slot],
                farOccPack(min(atomicLoad(&wgFarCount), CHUNK_VOL),
                           atomicLoad(&wgFarTop)));
  }
}

// ---- THE EDIT PATCH (far-field edit persistence) ---------------------------
//
// The sweep above is PRISTINE PROCGEN, and that is the whole problem this
// entry exists to fix: `fardown` writes the player's edits into these same
// cells from the live grid, and every refill of this level chunk — an
// incoming plane after the player walked out and back, a teleport, a world
// load — used to erase them. FarField::PrepareTick hands each fill entry the
// cells its CPU-side index (src/sim/faredits.h) knows were edited, taken from
// the persisted chunk store, and they are re-applied here.
//
// SAME RULE, SAME FUNCTION. The patch carries only the RAW MATERIAL at the
// cell's sample voxel; the surface-skin recolor is `farSurfaceMat`, exactly as
// in the sweep above and in `fardown`. That is what makes a patched cell
// byte-identical to what the live downsample would have written, so a region
// that flips between "resident and downsampled" and "refilled and patched"
// does not change appearance, and the sieve/downsample boundary agreement the
// `far-downsample` gate protects still holds.
//
// WHY IT IS ITS OWN ENTRY POINT (PLAN_shader_compile.md package C item 1).
// This block used to sit at the bottom of `far`, after a storageBarrier. It
// carries a SECOND full genColumn inline copy and — through farBlockerBitAt —
// four more farColTop/landColumn copies that the sweep's hoisted `tops`/
// `btops` form does not have. NVIDIA's front end charges superlinearly in
// entry-point size, so the two halves in one entry cost far more than the two
// halves apart, and Package A's PipelineBuildPool compiles separate entries in
// PARALLEL: the wall clock becomes max(sweep, patch) instead of one bigger
// whole. The barrier that used to be the in-kernel storageBarrier is now the
// pass table's edge between the two PT_FARFILL rows (src/sim/pass_table.def) —
// same hazard, same direction, generated rather than hand-written.
//
// SAME DISPATCH SHAPE as the sweep: one workgroup per farList entry, so
// `wg.x` indexes farPatch's header pairs exactly as it did inside `far`.
// An entry with no patched cells does no work and rewrites its farOcc word
// with the identical value (pnz = 0, top = 0 fold to the identity below) —
// there is deliberately no early return on `pCnt`, because it comes from a
// storage buffer and a barrier under it would fail WGSL's uniformity analysis.
//
// No cross-workgroup race, unlike `fardown`: a farVox word packs 4 cells that
// are consecutive in x WITHIN this level chunk, so every byte of every word
// this workgroup touches belongs to this workgroup alone.
@compute @workgroup_size(64)
fn farpatch(@builtin(workgroup_id) wg : vec3<u32>,
            @builtin(local_invocation_index) li : u32) {
  if (wg.x >= T.farCount) { return; }
  if (li == 0u) {
    atomicStore(&wgFarPatchNZ, 0u);
    atomicStore(&wgFarTop, 0u);
  }
  workgroupBarrier();

  let packed = farList[wg.x];
  let level = (packed >> FAR_SLOT_SHIFT) + 1u;   // 1-based
  let slot = packed & FAR_SLOT_MASK;
  let sc = vec3<i32>(vec3<u32>(slot % FAR_NCHUNK, (slot / FAR_NCHUNK) % FAR_NCHUNK,
                               slot / (FAR_NCHUNK * FAR_NCHUNK)));
  let base = farSlotToChunk(sc, F.origins[level - 1u].xyz) * i32(CHUNK);
  let shift = farCellShift(level);

  let pOff = farPatch[wg.x * 2u];
  let pCnt = farPatch[wg.x * 2u + 1u];
  var pnz = 0u;
  // ---- A CONTIGUOUS RUN PER THREAD, ONE COLUMN AT A TIME (2026-09-24) ------
  // FarEdits::Lookup hands the payload over sorted COLUMN-MAJOR — (z, x, y) of
  // the cell — so the cells of one (x, z) column are adjacent, and a thread
  // that owns a contiguous run [p0, p1) walks them column by column. The
  // column half (genColumn + the column top) is rebuilt only when the column
  // changes: an edited level chunk is typically whole columns of patched
  // cells, which used to cost one genColumn EACH (a strided pi += 64 loop
  // never sees two cells of one column). The per-cell blocker flag stays
  // per cell; its corner scan fires in at most one row per column.
  let per = (pCnt + 63u) / 64u;
  let p0 = min(li * per, pCnt);
  let p1 = min(p0 + per, pCnt);
  var pcol : Col;
  var pponds : PondSet;
  var pTop = 0;
  var haveCol = false;
  var lastX = 0;
  var lastZ = 0;
  for (var pi = p0; pi < p1; pi++) {
    let e = farPatch[FAR_PATCH_BASE + pOff + pi];
    let ci = e & 0xFFFu;                       // cell index in this level chunk
    let pmat = (e >> 12u) & 0xFFFu;            // raw material at the sample voxel
    let pl = vec3<i32>(vec3<u32>(ci % CHUNK, (ci / CHUNK) % CHUNK,
                                 ci / (CHUNK * CHUNK)));
    let pcc = base + pl;
    let pfine = (pcc << vec3<u32>(shift)) + vec3<i32>(1 << (shift - 1u));
    // Its own genColumn: a patch cell is an arbitrary cell of this level
    // chunk, so it shares no column with the sweep's. It is paid even for a
    // patch that clears the cell, because the blocker flag is a property of
    // the TERRAIN under the patch and has to survive that.
    if (!haveCol || pfine.x != lastX || pfine.z != lastZ) {
      pcol = genColumn(pfine.x, pfine.z, T.seed, &pponds);
      pTop = farColTopFrom(pcol.h, pcol.fluidTop, pfine.x, pfine.z);
      lastX = pfine.x;
      lastZ = pfine.z;
      haveCol = true;
    }
    var byteV = farBlockerBitAt(pTop, pcc, shift, T.seed);
    if (farCellIsSolid(pmat)) {
      byteV |= matFarPal(&materials, farSurfaceMat(&pcol, &pponds, pmat, pfine, shift, T.seed));
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
    // The SAME publication `far` used to do at the bottom of the merged entry,
    // read-modify-written instead of composed in registers: the sweep's word is
    // already in farOcc and only this workgroup ever touches this slot, in
    // either dispatch. The sum double-counts a patched cell that was already
    // non-air in the sweep and ignores a patch that cleared one — both land on
    // the safe (over-count) side, which is the only direction farOcc may err.
    // With pCnt == 0 this is the exact identity on the stored word.
    let oi = (level - 1u) * FAR_NUM_CHUNKS + slot;
    let cur = atomicLoad(&farOcc[oi]);
    atomicStore(&farOcc[oi],
                farOccPack(min((cur & 0xFFFFu) + atomicLoad(&wgFarPatchNZ),
                               CHUNK_VOL),
                           max(farOccTop(cur), atomicLoad(&wgFarTop))));
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
  let wc = slotWorldChunk(slot, T.origin);
  let base = wc * i32(CHUNK);

  // ---- SKIP A CHUNK WHOSE FAR-VISIBLE MATTER HAS NOT CHANGED ----------------
  // Everything below writes a pure function of (the cells farCellIsSolid
  // keeps, procgen, the level origins). Procgen is fixed per coord, so if the
  // kept cells match what this slot held at its last downsample, every byte
  // would be rewritten with the value it already has.
  //
  // THE ORIGINS ARE NOT IN THE SIGNATURE (2026-09-24). They used to be — all
  // eight hashed in — so every level-1 origin step (one per 32 voxels walked)
  // re-downsampled every dirty chunk into all eight levels. What an origin
  // step can actually change for a RESIDENT chunk is narrower: its cells are
  // inside every level's box in steady state (level 1's box is twice the
  // window), so farInBox does not flip for them, and their bytes are only
  // ever overwritten by a sieve refill of a plane or level that covers the
  // window. FarField raises the farSig clear for exactly those — a reset, and
  // a plane whose slab intersects the residency window — when that refill has
  // been DISPATCHED, which is also the moment the clear stops racing it
  // (farfield.cpp PrepareTick).
  // The dirty list cannot say that on its own: a chunk awake only for SMOKE
  // (a burning forest's ~5,000 of them, 2026-09-22) changes nothing here,
  // because gas never reaches the cascade.
  //
  // The signature is an order-free SUM of per-cell hashes, so the workgroup's
  // accumulation order cannot change it. A 32-bit collision skips one
  // downsample of render-only derived data; nothing else can observe it.
  if (li == 0u) { atomicStore(&wgFarCount, 0u); }
  workgroupBarrier();
  // The page is read DIRECTLY (i is the chunk-linear in-page index): a
  // per-cell voxWordAt re-resolves the page table for every cell, which made
  // this read cost 8x the occupancy pass's read of the same chunks. A sentinel
  // holds one material everywhere (JITTER varies only the palette nibble).
  var acc = 0u;
  let pe = pageTable[slot];
  if ((pe & PT_SENTINEL_BIT) != 0u) {
    // A sentinel is ONE material everywhere, so its signature is a function of
    // that material alone and needs no 4,096-cell sum: one closed-form term,
    // added once. It is deliberately NOT the sum a materialized page of the
    // same content would give — that sum has no closed form — so a sentinel
    // that materializes costs one redundant downsample, the direction this
    // skip is allowed to err in. EMPTY (and any non-far-visible material)
    // still contributes 0, exactly like an all-air page.
    let m = pe & PT_MAT_MASK;
    if (li == 0u && farCellIsSolid(m)) { acc = pcg(m ^ 0x5E17A1u); }
  } else {
    let pageBase = pe * CHUNK_VOL;
    for (var i = li; i < CHUNK_VOL; i += 64u) {
      let m = voxels[pageBase + i] & 0xFFFu;
      if (farCellIsSolid(m)) { acc += pcg((m << 12u) | i); }
    }
  }
  atomicAdd(&wgFarCount, acc);
  workgroupBarrier();
  if (li == 0u) {
    // The world chunk coord stays in: a slot is REUSED by another chunk after a
    // window shift, and the same content in a different place writes
    // different cells.
    var sig = pcg(hash3(u32(wc.x), u32(wc.y), u32(wc.z)) ^ atomicLoad(&wgFarCount));
    sig = max(sig, 1u);   // 0 is "never downsampled" (zeroed buffer)
    let same = farSig[slot] == sig;
    farSig[slot] = sig;
    atomicStore(&wgFarCount, select(0u, 1u, same));
  }
  if (workgroupUniformLoad(&wgFarCount) != 0u) { return; }

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
    // ONE THREAD PER COLUMN, the cells of that column in an inner loop.
    //
    // Everything procgen here is a function of (x, z) alone — genColumn, the
    // column top — and it is most of this kernel's cost. The flat
    // `i -> (ix, iy, iz)` walk this replaced evaluated it once per SAMPLE, so
    // level 1 (8x8x8 samples, 8x8 columns) paid eight genColumns per column.
    // Measured 2026-09-22 on a forest fire (~5,000 smoke-awake chunks/tick):
    // `farDown` was 15.4 ms/frame, the single largest GPU row. Same samples,
    // same bytes written (each cell owns its byte; atomics commute), so the
    // cascade is bit-identical; only the procgen count drops.
    let cols = u32(n.x * n.z);
    let origin = F.origins[level - 1u].xyz;
    for (var ci = li; ci < cols; ci += 64u) {
      let ix = i32(ci) % n.x;
      let iz = i32(ci) / n.x;
      let fx = first.x + ix * step;
      let fz = first.z + iz * step;
      // Column-level box test: x/z of the level cell do not depend on y.
      let ccx = fx >> shift;
      let ccz = fz >> shift;
      var pponds : PondSet;
      var pcol = genColumn(fx, fz, T.seed, &pponds);
      let topC = farColTopFrom(pcol.h, pcol.fluidTop, fx, fz);
      for (var iy = 0; iy < n.y; iy++) {
        // fine-voxel sample point, and the level cell it belongs to
        let fine = vec3<i32>(fx, first.y + iy * step, fz);
        let cc = vec3<i32>(ccx, fine.y >> shift, ccz);
        if (!farInBox(cc, origin)) { continue; }   // outside this cascade level

        // THE BLOCKER FLAG IS PRISTINE ON BOTH SIDES, and that is the whole
        // reason it is computed from the procgen COLUMN here rather than from
        // the live grid this entry otherwise reads. `far` has no live grid to
        // consult — it fills from procgen — so a flag derived from real voxels
        // here would differ from the flag `far` writes for the same cell, and
        // the seam between a refilled plane and a downsampled chunk would show
        // it. Same function, same arguments, same answer: the argument
        // `farSurfaceMat` already makes for the colour. The cost is that an
        // EDIT never sets or clears the flag — only the material byte below
        // records it, which is the behaviour every reader had before the flag
        // existed.
        var byteV = farBlockerBitAt(topC, cc, shift, T.seed);
        // live grid (the sample point is inside this chunk, hence resident)
        let mat = voxWordAt(fine) & 0xFFFu;
        if (farCellIsSolid(mat)) {
          // Same skin rule as the sieve — the skin is looked up from PRISTINE
          // procgen (genColumn), so a pristine chunk downsamples bit-identically
          // to the sieve's fill. An edited surface keeps its pristine skin
          // color while the cell's center voxel survives; the moment the
          // center voxel is dug away the cell empties for real. A slightly
          // stale rim color is invisible at cascade distances; a seam between
          // refilled planes and downsampled chunks is not.
          // SAME CALL, SAME ARGUMENTS as the sieve — that identity is what
          // keeps a downsampled chunk byte-identical to a refilled one at their
          // shared boundary, and it is why farSurfaceMat takes the column
          // rather than deriving a height of its own.
          byteV |= matFarPal(&materials, farSurfaceMat(&pcol, &pponds, mat, fine, shift, T.seed));
        }
        let bi = farVoxByteIndex(level, cc);
        let bsh = (bi & 3u) * 8u;
        atomicAnd(&farVox[bi >> 2u], ~(0xFFu << bsh));
        atomicOr(&farVox[bi >> 2u], byteV << bsh);
        if (byteV != 0u) {
          // Count 1 (all a reader asks of the count is non-zero) under this
          // cell's row: max() compares the row first, so a live edit that
          // stacks something above the sieve's top row raises it, and nothing
          // ever lowers it (common.wgsl, the farOcc word).
          atomicMax(&farOcc[farOccIndex(level, cc)],
                    farOccPack(1u, u32(cc.y & (i32(CHUNK) - 1)) + 1u));
        }
      }
    }
  }
}
