// microbody.wgsl — dynamic microvoxel bodies (docs/PLAN_voxel_editor.md §C,
// DESIGN.md §9). One draw call, 36 vertices per micro body slot.
//
// WHY OBB RASTER + PER-FRAGMENT MARCH, rather than more cubes.
// The cube path (debris.wgsl `vsBody`) is one 36-vertex instance per VOXEL.
// That is exactly right when a limb is a dozen voxels and exactly wrong at 2x
// or 4x resolution, where the same silhouette costs 8x or 64x the instances
// and the extra triangles are all sub-pixel. Here the whole limb is ONE box:
// the vertex shader positions its 8 corners from the body transform, and the
// fragment shader marches the limb's brick in object space. Cost then scales
// with the SCREEN AREA the limb covers, which is the shape rule 2 asks for —
// a micro critter across the field costs a handful of fragments, not 6000
// instances.
//
// BACKFACES ONLY. Front faces would vanish the moment the camera entered the
// box (near-plane clipping, or the OBB being behind the fragment it should
// have generated). Drawing the FAR side instead means the box is covered for
// every camera position including one inside it, and the march simply starts
// at the ray's entry into the slab rather than at the rasterized surface.
//
// DEPTH is the shared reversed-Z convention from common.wgsl, matching
// raymarch.wgsl's `fs` exactly: viewZ = t * dot(rd, camFwd), then
// KNEAR / max(viewZ, KNEAR). That is what lets hardware GreaterEqual depth
// testing composite these bodies against the raymarched world, the particle
// cubes, the ordinary body cubes and the sprites with no sorting anywhere.
//
// SHADOWS: RECEIVED since 2026-09-04, still CAST by nothing. The distinction
// is the whole design. This pass casts one sun ray per fragment against the
// VOXEL GRID (bodySunShadow, common.wgsl), so a limb standing in a building's
// shade is lit like the ground it stands on — before that it kept 100% of the
// key light and a mob indoors read ~10x too bright. What has NOT changed is
// the old rule that shadow rays must never iterate MODELS: the ray marches
// world voxels only, so a body cannot shadow itself or another body, and the
// per-fragment cost is independent of how many micro bodies exist. A coarse
// render-side occupancy proxy is still the stretch goal for body-on-body.
//
// DETERMINISM: render-only. These buffers are bound here and nowhere else.

// The sub-chunk occupancy mask + the openness grid (docs/PLAN_gi.md §2). A
// BODY is not in the voxel grid, so its own block has no openness entry; the
// mask is what lets opennessScaleAtBody find the ground under the fragment and
// take that surface's sky visibility. Without this a mob in a cave keeps full
// daylight ambient while the cave around it goes dark, which is the failure
// PLAN_gi.md §1 names by hand.
// The world grid and its page table, for ONE thing: the sun-shadow ray this
// pass casts per fragment (bodySunShadow, common.wgsl). A micro body is not in
// the grid, so nothing shadows it unless it asks, and before 2026-09-04 it
// never asked — a mob standing in a building's shade was lit as if it were in
// open sun. Declaring `voxels` is also what keeps the page-table block of
// common.wgsl from being stripped out of this shader (BodyAddressesVoxels in
// src/gpu/resources.cpp is a content predicate — it reads the declaration, not
// a list), which is where traceOpaque and voxWordAt come from. Fragment-only
// visibility in renderBGL_ is exactly what this pass needs; it shades in fs.
@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(9) var<storage, read> pageTable : array<u32>;
@group(0) @binding(17) var<storage, read> openness    : array<u32>;
@group(0) @binding(18) var<storage, read> opennessGen : array<u32>;
// The glow field (src/sim/world.h kGlowBytes, common.wgsl THE GLOW FIELD).
// ONE buffer load per shaded point, no ray and no voxel read, which is the only
// shape of emitter light this path can consume.
@group(0) @binding(20) var<storage, read> glow : array<u32>;
// The water veil (common.wgsl THE WATER VEIL): how the liquid over each pixel
// transforms light coming up from below. Written by raymarch.wgsl in the half
// of the world pass before this one draws.
@group(0) @binding(24) var<storage, read> waterVeil : array<u32>;

// THE CLOUDS (common.wgsl cloudSunAt): the shadow + env maps and the cloud
// uniform, so a body standing in a cloud's shadow is in it too — the same
// lookup the terrain around it makes.
@group(0) @binding(26) var<storage, read> cloudMaps : array<u32>;
@group(0) @binding(27) var<uniform> CL : CloudParams;

struct BodyXform {
  pos : vec3f, _p : f32,         // world voxels
  quat : vec4f,                  // x,y,z,w
};
@group(1) @binding(0) var<storage, read> bodyXf : array<BodyXform>;
@group(1) @binding(1) var<storage, read> models : array<MicroBodyModel>;
@group(1) @binding(2) var<storage, read> pool : array<u32>;
// One entry per micro body drawn this frame: the render SLOT it occupies in
// bodyXf, and the model index. Compacted on the CPU so the draw's instance
// count is exactly the number of micro bodies — zero of them means zero
// instances and the pass is skipped entirely (rule 2).
// `flash_bits` is the HIT FLASH as a float bit pattern (game/mob.h
// MobLimb::hitFlash, packed by Mob::AppendMicroInsts). A reused padding word,
// not a widened struct: the CPU mirror is sim/microbody.h MicroBodyInstGpu and
// its 16-byte static_assert is the only mechanical guard the pair has — nothing
// checks this layout, so the struct must not grow. 0 bitcasts to 0.0, which is
// what debris bodies keep passing.
// `dye_bits` is the garment DYE (src/game/dye.h): bit 24 set = dyed, low 24
// bits the colour in unpackColor's own byte order. 0 = undyed, which is every
// body limb, every piece of debris and every garment nobody has coloured.
struct MicroBodyInst {
  slot : u32, model : u32, flash_bits : u32, dye_bits : u32,
};
@group(1) @binding(3) var<storage, read> insts : array<MicroBodyInst>;

// quatRotate / axisUnit / unpackColor / litColor / emberFlicker are shared with
// debris.wgsl and live in common.wgsl — the two paths draw the same limbs (a
// live mob's arm and the severed one beside it), so their shading must be one
// definition, not two that happen to agree today.

// Inverse rotation = rotation by the conjugate. Used to bring the camera ray
// into object space; the direction is deliberately left UNNORMALIZED so `t`
// stays in world-voxel units and feeds the depth formula directly.
fn quatRotateInv(q : vec4f, v : vec3f) -> vec3f {
  return quatRotate(vec4f(-q.xyz, q.w), v);
}

// Everything the fragment march needs that is constant across the instance is
// computed ONCE per vertex and interpolated flat. The alternative — refetching
// insts -> models -> bodyXf per fragment — is three dependent storage loads and
// a quaternion rotation before the slab test can even start, on every one of the
// thousands of pixels a limb covers.
struct VSOut {
  @builtin(position) pos : vec4f,
  @location(0) worldDir : vec3f,                    // camPos -> fragment
  @location(1) @interpolate(flat) roM : vec3f,      // ray origin, MICRO units
  @location(2) @interpolate(flat) quat : vec4f,     // body rotation
  @location(3) @interpolate(flat) dims : vec3<i32>, // brick extent, micro voxels
  @location(4) @interpolate(flat) base : u32,       // pool word offset
  @location(5) @interpolate(flat) scale : f32,      // micro voxels per world voxel
  @location(6) @interpolate(flat) slot : u32,       // ember flicker phase key
  // Hit flash, linear HDR, added after lighting. FLAT and carried on the
  // vertex output for the same reason everything else here is: refetching
  // insts[] per fragment is a dependent storage load on every pixel a limb
  // covers, for a value that is constant across the whole instance.
  @location(7) @interpolate(flat) flash : f32,
  // The model's 6-bit joint mask (microBodyCutFaces). Flat and carried here for
  // the same reason `base`/`dims` are: refetching insts -> models per fragment
  // is two dependent storage loads for a value constant across the instance.
  @location(8) @interpolate(flat) cut : u32,
  // Pool word where this model's STAIN LATTICE starts (16 bits per micro
  // voxel -- coat byte, bruise byte -- 2 per word, same idx order as the
  // payload), or 0 when the block
  // carries none -- bit 30 of the dims word (sim/microbody.h). A shared def
  // model never has one; a body grows one the first time it is bloodied.
  @location(9) @interpolate(flat) stainBase : u32,
  // THE DYE, already unpacked, and its flag as a separate float so the
  // fragment does no bit work at all. Flat and carried here for the same
  // reason `flash` is: it is constant across the instance, and refetching
  // insts[] per fragment is a dependent storage load on every pixel a garment
  // covers. `dyeOn` is 0.0 or 1.0 so the blend below is a mix() rather than a
  // branch — a branch on a value that is uniform across the whole draw call
  // still costs the divergent path on some drivers, and mix() cannot.
  @location(10) @interpolate(flat) dye : vec3f,
  @location(11) @interpolate(flat) dyeOn : f32,
  // A VESSEL'S CONTENTS (src/phys/fillview.h, which documents the word). A
  // word with the dye flag CLEAR but not zero is a fill: `fillMat` the
  // contents' MATERIAL, `fillSlices` the see-through x-slices (0 = no fill,
  // which is every limb, garment and body that is not a filled flask), and a
  // cell is under the surface when its centre's height along
  // `fillPlane.xyz` (world up, in the brick's frame) is at most `fillPlane.w`.
  @location(12) @interpolate(flat) fillMat : u32,
  @location(13) @interpolate(flat) fillSlices : i32,
  @location(14) @interpolate(flat) fillPlane : vec4f,
};

// ---- THE DYE REFERENCE TONE (src/game/dye.h kDyeRef) ------------------------
//
// A garment authored for dyeing is painted in GREYSCALE, and each cell's grey
// is a MULTIPLIER rather than a colour: a cell at this value renders as exactly
// the colour the player picked, one at half it renders at half, and the weave,
// the seams and the hems all survive being recoloured because they are ratios
// rather than pigments. One divide, at the one place albedo is decided.
//
// STATED IN THREE PLACES AND CHECKED IN TWO: here (the GPU), kDyeRef in
// src/game/dye.h (the UI's preview), and DYE_REF_GREY in
// scripts/gen_peasant_clothes.py (what the art is actually painted at, which
// asserts against its own ramp). A disagreement is not a crash — it is every
// dyed garment in the game coming out uniformly too bright or too dark with
// the cause two files away — so `--gate items` compares this literal against
// the header's.
//
// DECLARED HERE, NOT IN common.wgsl. This is the only shader that dyes
// anything, and a common.wgsl edit misses the SPIR-V cache for every shader in
// the engine — measured at 536 s of pipeline compile for eight constants
// (CLAUDE.md, "What needs a rebuild").
const DYE_REF : f32 = 0.70;

// game/container.h kHeldFillLevelShift. Here and not in common.wgsl for the
// same reason DYE_REF is: this is its only reader.
const HELD_FILL_LEVEL_SHIFT : u32 = 25u;
// How far a filled cell's glass gives way to the colour of what is in it. Not
// 1.0: the vessel's own tone (its jitter, a stopper's shadow) still reads
// through, which is what makes it liquid IN glass rather than a painted band.
const HELD_FILL_MIX : f32 = 0.8;

// vi in 0..35 -> a corner of the unit box. Every face must wind the SAME way
// around its own outward normal, or `cullMode: Front` keeps a different subset
// of faces depending on the view direction — and in the octant where the three
// mis-wound faces are exactly the three far ones, NOTHING survives and the body
// disappears completely. That was a real bug: with a fixed (t1, t2) basis the
// triple (t1, t2, n) flips handedness with `sgn`, so the three negative faces
// wound backwards and each micro body was invisible from 1/8 of all view
// directions (measured: 12.7% of the sphere, the +++ octant in object space).
//
// The fix is to swap the two tangents on the negative faces, which re-orients
// the sweep so (t1, t2, n) is right-handed for all six. The selftest's
// single-body probe orbits one identity-rotated body through the 8 octants and
// the 6 axes and asserts every one draws.
fn boxCorner(vi : u32) -> vec3f {
  let face = vi / 6u;
  let axis = face / 2u;
  let sgn = 1.0 - 2.0 * f32(face % 2u);
  let n = axisUnit(axis) * sgn;
  // Swap tangents on negative faces so the cross product t1 x t2 always points
  // along +n, making the winding consistent across the whole cube.
  let a1 = axisUnit((axis + 1u) % 3u);
  let a2 = axisUnit((axis + 2u) % 3u);
  let t1 = select(a2, a1, sgn > 0.0);
  let t2 = select(a1, a2, sgn > 0.0);
  var quad = array<vec2f, 6>(
      vec2f(0.0, 0.0), vec2f(1.0, 0.0), vec2f(0.0, 1.0),
      vec2f(0.0, 1.0), vec2f(1.0, 0.0), vec2f(1.0, 1.0));
  let q = quad[vi % 6u];
  // 0..1 box coordinates: n*0.5 picks the face, the tangents sweep it.
  return vec3f(0.5) + n * 0.5 + t1 * (q.x - 0.5) + t2 * (q.y - 0.5);
}

@vertex
fn vs(@builtin(vertex_index) vi : u32,
      @builtin(instance_index) inst : u32) -> VSOut {
  let m = models[insts[inst].model];
  let slot = insts[inst].slot;
  let xf = bodyXf[slot];
  let dims = microBodyDims(m);
  let scale = f32(max(m.scale, 1u));

  // Object-space box, in WORLD voxels: the limb is dims SKIN voxels across at
  // 1/scale world voxels each. `m.scale` is the SKIN resolution — the brick is
  // packed from the skin lattice — and it is NOT necessarily the pitch the
  // collider was built at. Since the skin/collider split those are two
  // resolutions (mob.h MobDef::skinScale vs physScale, phys/lattice.h): the
  // collider is derived coarser, and the renderer neither knows nor cares.
  let extent = vec3f(dims) / scale;
  // Half a micro voxel of skin, grown symmetrically about the box centre.
  // Without it a ray grazing a face can rasterize a fragment whose slab entry
  // lands an epsilon OUTSIDE the brick and discards, leaving a one-pixel crack
  // along every silhouette edge. The march itself is unaffected: the slab test
  // uses the true 0..dims box, so the skin only ever adds fragments that then
  // discard for real.
  let pad = 0.5 / scale;
  let local = boxCorner(vi) * (extent + pad * 2.0) - pad;

  let world = xf.pos + quatRotate(xf.quat, local);
  var out : VSOut;
  out.pos = projectView(world - R.camPos, R);
  out.worldDir = world - R.camPos;
  // Ray origin in MICRO units: the eye, brought into object space and scaled.
  out.roM = quatRotateInv(xf.quat, R.camPos - xf.pos) * scale;
  out.quat = xf.quat;
  out.dims = dims;
  out.base = m.base;
  out.scale = scale;
  out.slot = slot;
  out.flash = bitcast<f32>(insts[inst].flash_bits);
  let dyeBits = insts[inst].dye_bits;
  // unpackColor is the same decode the art palette uses, which is exactly why
  // the CPU packs the dye in that byte order (src/game/dye.h).
  out.dye = unpackColor(dyeBits & 0xFFFFFFu);
  out.dyeOn = select(0.0, 1.0, (dyeBits & 0x1000000u) != 0u);
  let isFill = (dyeBits & 0x1000000u) == 0u && dyeBits != 0u;
  out.fillSlices = select(0, i32((dyeBits >> 16u) & 0x7Fu), isFill);
  out.fillMat = select(0u, dyeBits & 0xFFFu, isFill);
  // The CPU quantized the surface over +-half the brick's diagonal, 0..127.
  let halfDiag = 0.5 * length(vec3f(dims));
  out.fillPlane = vec4f(quatRotateInv(xf.quat, vec3f(0.0, 1.0, 0.0)),
                        (f32(dyeBits >> HELD_FILL_LEVEL_SHIFT) / 127.0 * 2.0 - 1.0) *
                            halfDiag);
  out.cut = microBodyCutFaces(m);
  // Payload is 2 voxels per word; the stain lattice sits right after it.
  let cells = u32(dims.x * dims.y * dims.z);
  out.stainBase = select(0u, m.base + (cells + 1u) / 2u,
                         (m.dims & MB_DIMS_STAIN_BIT) != 0u);
  return out;
}

// sim/microbody.h kMicroBodyDimsStainBit. Declared HERE, not in common.wgsl:
// this is the only shader that reads it, and a common.wgsl edit misses the
// SPIR-V cache for every shader (CLAUDE.md, "What needs a rebuild").
const MB_DIMS_STAIN_BIT : u32 = 0x40000000u;

// ---- BLOOD ON A BODY (DESIGN.md section 7) ----------------------------------
//
// The 16-bit stain cell of a hit voxel, two per word: the COAT byte low and
// the BRUISE byte high (sim/microbody.h, dims bit 30). Each byte is amount in
// the low nibble and stain TYPE (the same palette slot the voxel word's bits
// 28..30 carry) above it. Read once, at the hit only, from the lattice after
// the payload. 0 when the model has no lattice or the voxel is clean.
fn poolStainAt(stainBase : u32, dims : vec3<i32>, p : vec3<i32>) -> u32 {
  if (stainBase == 0u) { return 0u; }
  let idx = u32((p.z * dims.y + p.y) * dims.x + p.x);
  let w = stainBase + (idx >> 1u);
  if (w >= MICRO_BODY_POOL_WORDS) { return 0u; }
  return (pool[w] >> ((idx & 1u) * 16u)) & 0xFFFFu;
}

// The same look as the ground's stain (raymarch.wgsl applyStain), on purpose:
// blood that ran off an arm onto the floor must not change colour on the way
// down. Same palette entry, same mottle threshold, same multiply-then-lerp,
// same render.stain* knobs. Only the noise domain differs: the mottle is
// sampled in MICRO cells scaled back to world pitch, so a stain on a scale-8
// limb breaks up at the same physical size as one on the ground beside it.
// The value noise itself is common.wgsl's valueNoise — the one the ground's
// stain mottle reads, so the two cannot drift.
// ---- A COAT THAT GLOWS AND BREATHES (2026-09-23) ---------------------------
// The stain palette entry's spare `_r2` word is the coat's glow + pulse
// (materials.json coat.glow / coat.pulse; packed by Simulation::UploadTables,
// layout materials.h kCoatGlow* -- bits 0..7 glow 0..255, bits 8..19 pulse in
// centi-Hz). Zero for every coat but a glowing one, so blood, water, rot and
// bruises take the `pulse == 0` branch and draw exactly as before.
//
// The wave is 0..1. A small per-cell phase from the same mottle the coverage
// uses, so a drenched arm shimmers as one film rather than blinking as a slab.
fn bodyCoatWave(glowWord : u32, mottle : f32, time : f32) -> f32 {
  let hz = f32((glowWord >> 8u) & 0xFFFu) * 0.01;
  if (hz <= 0.0) { return 1.0; }
  return 0.5 + 0.5 * sin(time * hz * 6.2831853 + mottle * 1.6);
}

// How much of this voxel the coat covers, 0..1, before any pulse. Shared by the
// tint and the glow so the two can never disagree about where the coat is.
fn bodyStainCover(stain : u32, cell : vec3<i32>, scale : f32) -> vec2f {
  let amtI = stain & 0xFu;
  if (amtI == 0u) { return vec2f(0.0); }
  let amt = f32(amtI) / f32(STAIN_AMT_MAX);
  let packed = materials[STAIN_PALETTE_BASE + ((stain >> 4u) & 0x7u)].stainColor;
  // The one place a body differs from the ground: the coat's authored
  // `opacity` (materials.json coat block, packed in the colour's alpha byte by
  // ParseCoat) scales the coverage, so water can soak a limb to full amount
  // and still read as a faint dampening rather than a grey statue.
  let opacity = f32(packed >> 24u) / 255.0;
  let mottle = valueNoise(vec3f(cell), TUNE_STAIN_MOTTLE_SCALE * scale);
  let cover = opacity *
              clamp((amt * (1.0 + TUNE_STAIN_MOTTLE) - mottle * TUNE_STAIN_MOTTLE) *
                    TUNE_STAIN_COVERAGE, 0.0, 1.0);
  return vec2f(cover, mottle);
}

// Emission a glowing coat adds to this voxel (scalar, like material emission):
// glow x coverage, breathing between 35% and 100% on the coat's pulse.
fn bodyCoatGlow(stain : u32, cell : vec3<i32>, scale : f32, time : f32) -> f32 {
  if ((stain & 0xFu) == 0u) { return 0.0; }
  let glowWord = materials[STAIN_PALETTE_BASE + ((stain >> 4u) & 0x7u)]._r2;
  let glow = f32(glowWord & 0xFFu) / 255.0;
  if (glow <= 0.0) { return 0.0; }
  let cm = bodyStainCover(stain, cell, scale);
  return glow * cm.x * mix(0.35, 1.0, bodyCoatWave(glowWord, cm.y, time));
}

fn bodyStainTint(albedo : vec3f, stain : u32, cell : vec3<i32>, scale : f32,
                 time : f32) -> vec3f {
  let cm = bodyStainCover(stain, cell, scale);
  if (cm.x <= 0.0) { return albedo; }
  let pal = materials[STAIN_PALETTE_BASE + ((stain >> 4u) & 0x7u)];
  let stainCol = unpackColor(pal.stainColor);
  // A pulsing coat's COLOUR breathes too (70%..100% of its cover), so the film
  // itself swells and thins rather than only its light.
  let cover = cm.x * mix(0.7, 1.0, bodyCoatWave(pal._r2, cm.y, time));
  let soaked = albedo * mix(vec3f(1.0), stainCol * TUNE_STAIN_DARKEN, cover);
  return mix(soaked, stainCol, cover * TUNE_STAIN_OPACITY);
}

// One micro voxel: bits 0..7 material id, bits 8..15 art colour slot (0 = use
// the material's own colour). Two per word — see MicroBodyModelGpu::base.
fn poolVoxAt(base : u32, dims : vec3<i32>, p : vec3<i32>) -> u32 {
  let idx = u32((p.z * dims.y + p.y) * dims.x + p.x);
  let w = base + (idx >> 1u);
  if (w >= MICRO_BODY_POOL_WORDS) { return 0u; }  // defensive
  return (pool[w] >> ((idx & 1u) * 16u)) & 0xFFFFu;
}

// ---- the smooth body normal (2026-09-11) ------------------------------------
//
// A limb's face normal comes from the DDA's last-stepped axis, so a rounded arm
// made of 8 mm skin voxels shades as a staircase of six flat tones — the same
// defect `shadeViscous` fixes for blood in raymarch.wgsl, and the same one the
// Grimorium renderer's slime shader addresses by treating the body as a density
// field. The cure is the same one: differentiate the OCCUPANCY field the brick
// already stores and shade against its gradient, so neighbouring micro voxels
// agree on their normal and the cube structure dissolves.
//
// 26 taps (the 3^3 neighbourhood less the centre) weighted by 1/|d|, NOT a
// 6-tap central difference: on a binary field a 6-tap gradient can only take
// values in {-1, 0, 1} per axis, which quantises the normal to the same 26
// directions the staircase already had. The diagonal taps are what make it
// continuous.
//
// A BLEND, not a replacement. At 1.0 a one-voxel spur reads as a sphere and a
// deliberately square limb (a shield, an iron pauldron) loses its edges; the
// face normal carries the silhouette's intent and the gradient carries the
// curvature. BODY_SMOOTH_N is the mix.
//
// Declared here rather than as a TUNE_ row for CLAUDE.md's reason: a constant
// only one shader reads is declared in that shader. It is NOT in common.wgsl,
// where it would cost every other shader a SPIR-V cache miss.
//
// COST: 26 pool reads on the primary body fragment only — this shader has no
// secondary rays. Set to 0.0 and the whole block const-folds away, leaving the
// face normal bit-identical to what shipped before.
const BODY_SMOOTH_N : f32 = 0.55;

// ---- TWO BRICKS THAT OWN THE SAME CELL (2026-09-12) ------------------------
//
// WHAT THIS FIXES. A garment's panels deliberately OVERLAP. A robe's sleeve is
// a tube around the arm and its body panel is a tube around the torso; the two
// meet in the armpit and, where the torso tapers at the waist, along a whole
// column, and scripts/gen_stock_armor.py gives those cells to BOTH on purpose —
// whichever side cedes them shows a stripe of bare skin through half the gait,
// because the sleeve's inner wall is exactly what you see when the arm swings
// forward and the torso's side column is exactly what you see when it swings
// back.
//
// That was free while both bricks shaded a shared cell identically: the note
// in the generator says as much — "z-fighting is only a defect between things
// that look different". BODY_SMOOTH_N made them look different. Each brick
// differentiates its OWN occupancy field and can see no other, so the sleeve's
// copy of an armpit cell gets a normal pointing away from the arm and the
// torso's copy gets one pointing away from the chest. Now the depth tie decides
// which of two visibly different shadings you see.
//
// AND THE TIE IS NOISE. The two panels ride different limbs, so the same world
// plane is reached through two different quaternions and two different brick
// origins; `tCur` for the shared cell agrees only to float rounding, the
// GreaterEqual test therefore picks a winner per PIXEL, and every idle-sway
// frame re-rolls it. That is the owner report of 2026-09-12: the overlapping
// parts of a robe pulsing, flashing and swapping with each other.
//
// SO THE ORDER IS MADE EXPLICIT. Each body is pulled toward the camera by a
// relative slice of its view depth keyed on its render SLOT, which is stable
// frame to frame (mob.cpp AppendMicroInsts walks limbs in a fixed order). Any
// consistent winner removes the flicker — both panels still cover the body, so
// the choice only decides which one's shading you see at the seam, and a seam
// that holds still reads as a seam instead of as a rendering fault.
//
// RELATIVE, NOT ABSOLUTE, so it is a fixed number of depth-buffer steps at any
// distance. The largest bias is 63 * this = 1.9e-3 of view depth — four
// thousand times the ~1e-6 rounding it has to beat, and still under a
// hundredth of a voxel at arm's length, so nothing sinks into or floats off the
// terrain it is composited against. 64 distinct priorities cover every slot of
// a dressed humanoid (about 15 limbs plus 6 robe panels), which is all that is
// asked of it: two bodies far enough apart in slot index to alias are two
// bodies that are not sharing a cell.
const BODY_Z_PRIORITY : f32 = 3.0e-5;

// ---- WHAT LIES OUTSIDE THE BRICK (the cut-face mask, 2026-09-11) -----------
//
// MicroBodyModel.cutFaces (src/sim/microbody.h, filled by mob.cpp
// LimbCutFaces). Six bits, `axis * 2 + positive`, one per boundary plane of
// the brick, set when ANOTHER limb of the same prefab is pressed against that
// plane — i.e. when the plane is a JOINT rather than the end of the model.
// (The common.wgsl mirror called it `_pad` until 2026-09-24.)
fn microBodyCutFaces(m : MicroBodyModel) -> u32 { return m.cutFaces; }

// ---- SEE-THROUGH ART (wisps, 2026-09-24) ------------------------------------
//
// An art colour's alpha byte (color0 >> 24, written by ArtRgbToGpu from the
// .vox RGBA chunk's own alpha) is how much of a voxel painted in it is THERE.
// Long hair uses it: the strands thin toward their tips (mobgen.js
// ART.HAIR_WISP*), so the last hand-span of a mane reads as wisps rather than
// as a cut plank.
//
// SCREEN-DOOR, NOT BLENDING. This pass writes depth and is composited against
// the world, the cube bodies and the sprites by the depth test alone, with no
// sorting anywhere; a blended fragment would need all of that to change. So a
// partly-covered voxel is either hit or passed THROUGH, per pixel, and the
// march carries on to whatever is behind it (more hair, the neck, nothing).
// The fraction of pixels that hit is the alpha.
//
// THE PATTERN is interleaved gradient noise on the pixel, offset by a hash of
// the CELL. The pixel term makes coverage fine-grained within a voxel; the
// cell term decorrelates neighbouring voxels so two tiers of wisp do not line
// up into one screen-fixed grid, and so a wisp's holes move with the strand
// when the head turns rather than crawling across it.
//
// Opaque art (alpha 255, every colour but the wisps) exits on the first
// compare, and unpainted voxels never get here at all.
fn artCovers(art : u32, c : vec3<i32>, px : vec2f, slot : u32) -> bool {
  let a = materials[ART_PALETTE_BASE + (art - 1u)].color0 >> 24u;
  if (a >= 255u) { return true; }
  let ign = fract(52.9829189 * fract(dot(floor(px), vec2f(0.06711056, 0.00583715))));
  let h = f32(pcg(u32(c.x * 73856093) ^ u32(c.y * 19349663) ^
                  u32(c.z * 83492791) ^ (slot * 2654435761u)) & 0xFFFFu) / 65536.0;
  return fract(ign + h) * 255.0 < f32(a);
}

// Sample the brick's occupancy field, deciding what a sample OUTSIDE the brick
// means.
//
// Past a face that is NOT a joint, the answer is EMPTY, deliberately: that is
// what makes a silhouette voxel's gradient point outward and round the edge,
// instead of the brick's bounding box reading as a solid wall.
//
// Past a JOINT face the answer is the boundary cell itself — the field
// CONTINUES. Treating a joint as air instead was the "limb seams pulse and
// flash" defect (owner report 2026-09-11): the last ring of voxels on an upper
// arm got a gradient pointing straight out along the limb, i.e. a rounded end
// CAP, while the forearm's first ring rounded the opposite way. That put a hard
// bright/dark band across every shoulder, elbow, hip and knee, and because each
// cap's normal swings with its own limb, the band's shading swung with the
// animation while the two overlapping bricks traded depth ties under the TAA
// jitter. The model has no cap there; nothing should be shaded as if it did.
//
// Clamping across a joint is exact for the axis that crosses it and costs
// nothing for the others: a corner tap that leaves the brick through a joint
// AND through an open face is still air, because the open face decides.
fn bodySolidAt(base : u32, dims : vec3<i32>, p : vec3<i32>, cut : u32) -> f32 {
  let lo = p < vec3<i32>(0);
  let hi = p >= dims;
  let cutLo = vec3<bool>((cut & 1u) != 0u, (cut & 4u) != 0u, (cut & 16u) != 0u);
  let cutHi = vec3<bool>((cut & 2u) != 0u, (cut & 8u) != 0u, (cut & 32u) != 0u);
  if (any(lo & !cutLo) || any(hi & !cutHi)) { return 0.0; }
  let q = clamp(p, vec3<i32>(0), dims - vec3<i32>(1));
  return select(0.0, 1.0, (poolVoxAt(base, dims, q) & 0xFFu) != 0u);
}

// Object-space outward normal from the occupancy gradient, or the zero vector
// when the neighbourhood is uniform (a fully buried voxel, or an isolated one)
// — the caller keeps its face normal in that case rather than normalizing 0.
fn bodyFieldNormal(base : u32, dims : vec3<i32>, c : vec3<i32>,
                   cut : u32) -> vec3f {
  var g = vec3f(0.0);
  for (var dz = -1; dz <= 1; dz++) {
    for (var dy = -1; dy <= 1; dy++) {
      for (var dx = -1; dx <= 1; dx++) {
        if (dx == 0 && dy == 0 && dz == 0) { continue; }
        let d = vec3f(f32(dx), f32(dy), f32(dz));
        let s = bodySolidAt(base, dims, c + vec3<i32>(dx, dy, dz), cut);
        // Density rises INTO the body, so the outward direction is -grad.
        g -= d * (s / length(d));
      }
    }
  }
  return g;
}

struct FSOut {
  @location(0) color : vec4f,
  @builtin(frag_depth) depth : f32,
};

@fragment
fn fs(in : VSOut) -> FSOut {
  // Everything instance-uniform arrives as a flat interpolant (see VSOut), so
  // this shader touches no storage buffer until the DDA's first brick fetch.
  let dims = in.dims;
  let scale = in.scale;
  let roM = in.roM;

  // ---- world ray -> object space ----
  // `rd` is NOT normalized: it is the camera-to-fragment vector, so any `t`
  // along it is in the same units the depth formula expects. Rotating it by the
  // conjugate quaternion (no scaling anywhere) preserves that, which is the
  // whole reason to avoid normalizing here.
  //
  // Working the slab test and the DDA in MICRO units makes them integer-indexed
  // and identical in shape to the world DDA; `t` is a fraction of rdWorld
  // either way, because roM and rdM are scaled by the SAME factor.
  let rdWorld = in.worldDir;
  let rdM = quatRotateInv(in.quat, rdWorld) * scale;
  let boxHi = vec3f(dims);
  // Clamp the direction's magnitude away from zero, keeping its sign, so an
  // axis-aligned ray yields a huge-but-finite tDelta instead of a NaN.
  let inv = 1.0 / select(rdM, sign(rdM + 1e-30) * 1e-9, abs(rdM) < vec3f(1e-9));
  let t0 = (vec3f(0.0) - roM) * inv;
  let t1 = (boxHi - roM) * inv;
  let tsmall = min(t0, t1);
  let tbig = max(t0, t1);
  // The camera may be INSIDE the box (that is exactly why we draw backfaces),
  // in which case tEnter is negative and the march starts at the eye.
  let tEnter = max(max(tsmall.x, tsmall.y), max(tsmall.z, 0.0));
  let tExit = min(tbig.x, min(tbig.y, tbig.z));
  if (tExit <= tEnter) { discard; }

  // ---- Amanatides-Woo over the brick ----
  // Nudge past the entry face before flooring: a ray entering at exactly x = 0
  // otherwise floors to -1 or 0 depending on float noise, and the silhouette
  // loses its first row of voxels.
  var p = roM + rdM * (tEnter + 1e-4);
  var c = vec3<i32>(floor(p));
  c = clamp(c, vec3<i32>(0), dims - vec3<i32>(1));
  let stepv = vec3<i32>(sign(rdM));
  let tDelta = abs(inv);
  var tMax = (vec3f(c) + select(vec3f(0.0), vec3f(1.0), rdM > vec3f(0.0)) - roM) * inv;

  // Seed `axis` with the ENTRY face, not with 0. If the very first cell the ray
  // lands in is solid — which is the common case for a limb whose surface is
  // its bounding box, e.g. a leg — the loop never steps, and a hardcoded 0
  // would light every such fragment as if it faced ±x. (When the camera is
  // inside the box tEnter is 0 and no slab bound matches; the fallback of 1
  // gives an up-facing normal, which is the least wrong choice for a camera
  // buried inside a creature and is never seen from outside.)
  var axis = 1;
  if (tEnter > 0.0) {
    if (tsmall.x >= tsmall.y && tsmall.x >= tsmall.z) { axis = 0; }
    else if (tsmall.y >= tsmall.z) { axis = 1; }
    else { axis = 2; }
  }
  var tCur = tEnter;
  var hitMat = 0u;
  var hitArt = 0u;   // art colour slot of the hit voxel, 0 = material colour
  // HARD CAP (rule 2: bound every emergent process). 3*maxDim covers a full
  // diagonal traverse of the brick; +4 is slack for the entry rounding above.
  // No data-dependent loop bound anywhere in this shader.
  let maxDim = max(dims.x, max(dims.y, dims.z));
  let maxSteps = 3 * maxDim + 4;
  for (var i = 0; i < maxSteps; i++) {
    if (c.x < 0 || c.y < 0 || c.z < 0 ||
        c.x >= dims.x || c.y >= dims.y || c.z >= dims.z) { break; }
    let v = poolVoxAt(in.base, dims, c);
    if ((v & 0xFFu) != 0u) {
      let art = (v >> 8u) & 0xFFu;
      // A see-through art voxel this pixel misses is empty to the march
      // (artCovers, above): step on to whatever lies behind it.
      if (art == 0u || artCovers(art, c, in.pos.xy, in.slot)) {
        hitMat = v & 0xFFu;
        hitArt = art;
        break;
      }
    }
    if (tMax.x < tMax.y && tMax.x < tMax.z) {
      c.x += stepv.x; tCur = tMax.x; tMax.x += tDelta.x; axis = 0;
    } else if (tMax.y < tMax.z) {
      c.y += stepv.y; tCur = tMax.y; tMax.y += tDelta.y; axis = 1;
    } else {
      c.z += stepv.z; tCur = tMax.z; tMax.z += tDelta.z; axis = 2;
    }
    if (tCur > tExit) { break; }
  }
  if (hitMat == 0u) { discard; }

  // ---- shading ----
  // Object-space face normal from the last-stepped axis, back to world space,
  // rounded toward the occupancy field's gradient (see BODY_SMOOTH_N).
  let nFace = axisVec(axis, -f32(axisPickI(stepv, axis)));
  var nLocal = nFace;
  if (BODY_SMOOTH_N > 0.0) {
    let g = bodyFieldNormal(in.base, dims, c, in.cut);
    let gl = length(g);
    // A uniform neighbourhood gives g == 0 and no opinion; keep the face
    // normal rather than normalizing a zero vector.
    if (gl > 1e-4) {
      nLocal = normalize(mix(nFace, g / gl, BODY_SMOOTH_N));
    }
  }
  let n = quatRotate(in.quat, nLocal);

  let mat = materials[hitMat];
  // A painted voxel shows its ART colour; an unpainted one falls back to the
  // material's 3-variant palette, keyed on the micro CELL rather than on the
  // instance so a limb's texture does not crawl when it rotates (and is
  // identical on every machine — render-only, but a replay should still look
  // the same). Art colour is what lets a creature be one material all over and
  // still be painted; the material is what a severed limb becomes.
  // `hitArt` is a 1-BASED merged art index (0 = unpainted), the exact same
  // encoding debris.wgsl's cube path uses — so both body passes now decode
  // colour identically and index 1 means the same colour in each. It used to be
  // a raw .vox palette slot rebased by ART_SLOT_MIN, which capped the merged
  // palette at 128 for no reason but the rebasing (see world.h).
  //
  // paletteJitter, NOT paletteColor: `c` is a brick cell, and a brick cell has
  // no state nibble at all — the number below is a positional hash this shader
  // invents for texture. paletteColor() would read it as a MATF_TINTED
  // material's DYE INDEX, which is what made a steel blade and every
  // one-material-per-colour mob draw as a zebra (common.wgsl).
  var albedo : vec3f;
  if (hitArt != 0u) {
    albedo = unpackColor(materials[ART_PALETTE_BASE + (hitArt - 1u)].color0);
  } else {
    albedo = paletteJitter(mat, u32(c.x * 7 + c.y * 13 + c.z * 29));
  }
  // ---- THE DYE (src/game/dye.h) --------------------------------------------
  //
  // A dyed instance's art is a greyscale weave, so its albedo carries no hue
  // of its own and everything it does carry is TONE: the base cloth, the
  // shadow threads, the hem, the lacing, the patch. Multiplying the picked
  // colour by that tone over the reference grey reproduces the picked colour
  // exactly where the cloth is plain and keeps every one of those markings as
  // a ratio of it. Nine patterns times any colour is the whole wardrobe.
  //
  // LUMINANCE, not a per-channel multiply. The two are identical on the
  // greyscale art this is for, and they differ on art that is NOT greyscale —
  // where per-channel would tint (the robe's black stays black, its gold trim
  // turns a muddy version of the dye) and luminance re-colours outright. The
  // second is the behaviour somebody dyeing a thing expects, and it means a
  // dye applied to an un-dyeable piece is merely wrong rather than invisible.
  //
  // BEFORE the stain, deliberately: blood goes ON the cloth, so it must not be
  // scaled by the cloth's colour. Same order the ground uses for its own
  // stains, and the same reason.
  albedo = mix(albedo,
               in.dye * (dot(albedo, vec3f(0.2126, 0.7152, 0.0722)) /
                         DYE_REF),
               in.dyeOn);
  // A vessel's contents, up to their surface: every see-through cell whose
  // centre lies at or under the level plane is shaded AS THE CONTENTS'
  // MATERIAL -- its palette here, and below its emission, burn tint and ember
  // flicker, through the very functions the hit material goes through. So
  // lava in a flask glows because lava glows, and whatever glows next will
  // too; nothing here names a substance. The plane is normal to WORLD up, so
  // the liquid stays level as the flask tips. fillSlices is 0 for everything
  // else, so this is one compare there.
  var shadeMat = mat;
  var shadeWeight = 1.0;   // how much of this cell's emission is the shade material's
  let fillH = dot(vec3f(c) + vec3f(0.5) - vec3f(dims) * 0.5, in.fillPlane.xyz);
  if (c.x < in.fillSlices && fillH <= in.fillPlane.w + 1e-3) {
    shadeMat = materials[in.fillMat];
    albedo = mix(albedo, paletteJitter(shadeMat, u32(c.x * 7 + c.y * 13 + c.z * 29)),
                 HELD_FILL_MIX);
    // Seen through the glass: the contents' light by the share of the cell
    // they are, the same share their colour got.
    shadeWeight = HELD_FILL_MIX;
  }

  // Blood (or whatever else soaked in) OVER the art, before lighting, exactly
  // where the ground applies its own stain: a stain is a change to what the
  // surface is, and it has to take the scene's light like the skin under it.
  // One pool load, and only for models that carry a lattice at all.
  //
  // THE BRUISE FIRST, THEN THE COAT (2026-09-26). A bruise is the skin itself
  // discoloured, not something on it, so it is tinted into the albedo before
  // anything that sits on top: blood, water or mud over a bruise covers it the
  // way it would cover unhurt skin, and washing the coat off shows the bruise
  // still there. Same palette entry, mottle and knobs as a coat -- the look is
  // unchanged, only the layering is.
  let stainCell = poolStainAt(in.stainBase, dims, c);
  let coatWord = stainCell & 0xFFu;
  albedo = bodyStainTint(albedo, stainCell >> 8u, c, scale, R.time);
  albedo = bodyStainTint(albedo, coatWord, c, scale, R.time);

  // `tCur` is already the parameter along the UNNORMALIZED camera-to-fragment
  // vector, and that is the whole point of never normalizing anything: `ro/rd`
  // are `R.camPos - xf.pos` and `rdWorld` rotated by the conjugate (a rigid
  // rotation preserves magnitude), and `roM/rdM` are both scaled by the SAME
  // factor, so the micro-unit parametrization and the world one share `t`. The
  // hit is therefore just camPos + rdWorld * t, with no conversion.
  let worldPos = R.camPos + rdWorld * tCur;

  // emissive body voxels (embers) flicker exactly like their grid counterparts
  // and like the cube path's — one shared definition, in common.wgsl
  let fh = pcg(u32(c.x * 2917 + c.y * 131 + c.z * 7919) + in.slot * 977u);
  // ...and burn-tinted matter breathes toward the flame colour on the same key
  // (burnTint, common.wgsl) before it flickers. No mob material carries the
  // flag today; the rule is that no path may shade emission without it.
  let bt = burnTint(shadeMat, albedo, f32(shadeMat.emission) / 255.0 * shadeWeight,
                    burnTintWeightH(fh, R.time));
  albedo = bt.albedo;
  // ...plus a glowing coat's own light (acid), which breathes on its pulse
  // rather than flickering like an ember.
  let emis = emberFlicker(bt.emis, fh, R.time) +
             bodyCoatGlow(coatWord, c, scale, R.time);
  // The ambient's spatial term. One downward probe per FRAGMENT (at most six
  // mask words), which is where a body's shading has to happen. `.x` is the
  // ambient multiplier, `.y` the raw openness the shadow lift is capped by —
  // one walk, both consumers (opennessAtBody, common.wgsl).
  let open = opennessAtBody(worldPos, &occupancy, &openness, &opennessGen);
  // THE SUN-SHADOW RAY. The whole reason this pass reaches the voxel grid: a
  // limb in shade used to keep full key light while the ground it stood on
  // went dark, so a mob read as lit from a sun the terrain could not see.
  // Same ray, same softening law and same lift cap as the terrain beside it
  // (bodySunShadow -> shadowFromOpaqueHit, common.wgsl).
  var sh = bodySunShadow(worldPos, n, R, &occupancy, &materials);
  if ((R.weatherFlags & RWF_CLOUDS) != 0u) {
    sh *= cloudSunAt(worldPos, keyLightDirP(R), &CL, &cloudMaps);
  }
  // Unfogged: the air fog is applied at the end, by the water veil when the
  // fragment is under a liquid surface and by bodyAirFog when it is not.
  let lit = litColorSNoFog(albedo, n, worldPos, emis, R, open.x, open.y, sh);
  // Emitter light from the glow field (common.wgsl THE GLOW FIELD). ONE buffer
  // load, no ray: this is the term that lights a mob standing in a lava pit or
  // beside a burning tree, which nothing did before. It cannot come from the
  // irradiance grid the terrain uses — `giGather` is nine coarse DDA rays and
  // this shader already pays for a sun ray and an openness probe per fragment.
  //
  // `open.x` as the occlusion, not 1.0: unlike a loose particle a limb is a
  // solid body with creases, and this is the same multiplier the ambient took
  // one line up.
  var add = glowLight(albedo, open.x, glowAtPos(worldPos, &glow),
                     TUNE_GLOW_STRENGTH);

  // ---- THE HIT FLASH -------------------------------------------------------
  // ADDITIVE, and BEFORE the tonemap, because litColor's output is linear HDR
  // and the tonemap below has to match the cube path's exactly (see the note on
  // out.color) — adding after it would brighten this pass on a curve the
  // severed limb beside it is not on.
  //
  // A WARM WHITE rather than pure white: a blow reads as a bright bloom shot
  // through with the colour of what is being hit, and a flat vec3f(flash) on a
  // dark limb reads as a lighting bug instead. The albedo term is what carries
  // that — it is added on TOP of a constant, so a black limb still flashes.
  //
  // Nothing is done when flash is 0, which is every micro body in the world
  // except the ones struck in the last fifth of a second. The branch is
  // uniform across the instance (the value is flat-interpolated), so it costs
  // nothing on the ones that skip it.
  if (in.flash > 0.0) {
    add += (vec3f(1.0, 0.86, 0.78) + albedo) * in.flash;
  }

  // ---- UNDER WATER ---------------------------------------------------------
  // A limb below a liquid surface is seen through it: dimmed and tinted by the
  // column between it and the surface, lit by the caustic web, and behind the
  // surface's own Fresnel reflection and foam — the same equation the lake bed
  // under it was shaded with (common.wgsl THE WATER VEIL). Above the surface
  // it takes the ordinary air fog, with glow and flash on top unfogged as
  // they always were.
  let dist = length(worldPos - R.camPos);
  let veil = waterVeilAt(&waterVeil, in.pos.xy, dist, &R);
  var col : vec3f;
  if (veil.on) {
    col = waterVeilApply(veil, lit + add, dist);
  } else {
    col = bodyAirFog(lit, worldPos, R) + add;
  }

  // ---- reversed-Z depth, EXACTLY raymarch.wgsl's convention ----
  // dot(rdWorld, camFwd) is the fragment's view-space Z at t = 1, so scaling it
  // by t gives the hit's view Z in world voxels — the same `t * dot(rd, camFwd)`
  // the raymarcher writes, just with an unnormalized rd on both sides. Any
  // deviation here (a normalized direction, a different near constant) shows up
  // as micro bodies punching through terrain or sinking into it.
  // The slot priority (BODY_Z_PRIORITY) is folded in here and nowhere else:
  // shrinking the view depth is what "nearer" means under reversed-Z, and
  // doing it to viewZ rather than to the packed depth keeps the one conversion
  // this file shares with raymarch.wgsl byte for byte.
  let viewZ = tCur * dot(rdWorld, R.camFwd) *
              (1.0 - f32(in.slot & 63u) * BODY_Z_PRIORITY);
  var out : FSOut;
  // litColor is linear HDR; same tonemap as terrain + the cube path, or a
  // live limb and the severed one beside it would shade differently.
  out.color = vec4f(tonemapHdr(col), 1.0);
  out.depth = clamp(KNEAR / max(viewZ, KNEAR), 0.0, 1.0);
  return out;
}
