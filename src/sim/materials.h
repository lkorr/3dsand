#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Material classes — must match common.wgsl.
enum MatClass : uint32_t {
  CLASS_SOLID = 0,
  CLASS_POWDER = 1,
  CLASS_LIQUID = 2,
  CLASS_GAS = 3,
};

// Material flags — must match common.wgsl.
constexpr uint32_t kMatFlagWander = 1;  // powder scuttles laterally / hops
constexpr uint32_t kMatFlagOpaque = 2;  // liquid renders as surface hit (lava)
// Static micro-detail: the raymarcher substitutes a subdiv^3 brick for this
// material's cells (sim/microvox.h). NOT authored directly in materials.json —
// it is SET BY THE MICRO LOADER on any material whose "micro" block resolved to
// a valid brick, so the flag and the brick table can never disagree.
constexpr uint32_t kMatFlagMicro = 4;
// PASSABLE: a solid that moving bodies pass straight through — pond weed,
// reeds, kelp, and anything else authored as soft vegetation. Authored in
// materials.json as `"passable": true`.
//
// This is a COLLISION property only, and deliberately not a class. The cells
// stay ordinary solids everywhere else: the CA still runs on them, the brush,
// explosions and the laser still remove them, they still burn if flammable,
// and the renderer still draws them as solid geometry. All that changes is
// that the player capsule, mob ground probes and spell projectiles read them
// as empty space rather than as a wall.
//
// Making them a new CLASS instead was the obvious alternative and it is wrong:
// class drives density ordering and the whole displacement/settling model in
// the sim, so a "passable" class would have to re-answer every one of those
// questions for no benefit. A flag on a solid changes exactly the one thing
// that needs changing.
constexpr uint32_t kMatFlagPassable = 8;

// ---- TINTED: the state nibble is a colour, not a jitter variant -------------
// Authored in materials.json as `"tints": ["#rrggbb", ...]`, up to
// kMatTintsMax entries.
//
// A tinted material reinterprets voxel-word bits 12..15: instead of selecting
// one of the three cosmetic `colors` variants with `state % 3`, the nibble is a
// direct index into this material's run of the TINT PALETTE (world.h
// kTintPaletteBaseGpu). That is what lets a green-flesh mob's rubble stay green
// after it lands in the grid — the colour is finally something a GRID cell can
// carry, where art colour never was.
//
// WHY THE NIBBLE AND NOT NEW BITS. The voxel word is full — bits 19..23 look
// free in old comments but belong to the MPM excite seam (world.h). The nibble
// is the one span already hashed, already persisted, already carried by
// DebrisVoxel::payload and ParticleSpawn::payload, so reinterpreting it costs
// no format change anywhere: no hash-mask widen, no kPersistMask widen, no
// save-format bump, no payload growth.
//
// THE PRICE, all of it real:
//   * 16 tints per material, not 256. The body's 8-bit art colour is quantized
//     to the nearest tint when a voxel crosses into the grid.
//   * LIQUIDS CANNOT BE TINTED. Their nibble is fullness (LIQ_FULL_STATE) and
//     tinting one would corrupt flow. Refused at load.
//   * A tinted material loses its 3-variant jitter, so this is wrong for bulk
//     terrain (stone, dirt) and right for painted matter (flesh, cloth).
//   * Tinted materials must not be placed by WORLDGEN, which writes
//     `state = rnd % 3` and would scatter tints 0..2 across them.
//
// Tint 0 is the material's natural colour by convention (author it first), so a
// voxel that arrives with state 0 — a CPU path that never heard of tints, an
// old save, a worldgen cell — reads as undyed rather than as a wrong colour.
// That is what makes the flag safe to add to an existing material.
constexpr uint32_t kMatFlagTinted = 16;
// Max tints per material: the state nibble is 4 bits.
constexpr uint32_t kMatTintsMax = 16;
// ---- BURNTINT: the palette is what this voxel looked like BEFORE it caught ----
// Authored in materials.json as `"burnTint": true` on an emissive material.
//
// A burning leaf that renders as a flat orange coal stops reading as a leaf.
// A burn-tinted material keeps its `colors` as the UNBURNT palette (green
// leaves, pine needles, autumn leaves) and the RENDERER pulses each cell
// between that colour and the flame colour (render.burnTintColor, on a slow
// per-cell phase, render.burnTintRate / Min / Max), so a crown on fire still
// reads as the crown it was. Render-only: nothing in the sim reads the flag,
// every emissive grid site (primary hit, far field, secondary rays and the
// irradiance deposit) goes through burnTintAlbedo() in common.wgsl so the four
// agree, and the pulse weight also scales the emission so the "leaf" phase of
// the cycle is lit like a leaf rather than glowing green.
//
// It is a flag and three materials (leaf_burning / pine_burning /
// autumn_burning) rather than one burning material that remembers what it
// was, because the voxel word has no spare bits to remember it in (world.h);
// the material id IS the memory.
constexpr uint32_t kMatFlagBurnTint = 32;
// Where this material's tint run starts inside the shared tint palette, packed
// into the free high half of `flags` (bits 16..23) rather than added as a
// field, for the reason the wind nibbles give below: every reader tests `flags`
// with a mask, so eight more bits in it cost nothing.
constexpr uint32_t kMatTintBaseShift = 16, kMatTintBaseMask = 0xFF;

// ---- wind coupling, packed into the SAME flags word ------------------------
// docs/RESEARCH_wind.md §4.5, invariant 7. Bits 0..7 are the MATF_* booleans
// above (6 used, 2 spare); bits 8..11 and 12..15 are two authored 4-bit
// numbers; 16..31 are free.
//
// Packed rather than added as fields because growing a struct every sim thread
// reads to buy eight bits is the worse trade — the call stainPack already made.
// (MaterialGpu is 80 bytes now, not the 64 this used to cite; it grew for
// `repose`. The trade stands without the "no spare word" half.) `flags`
// specifically is safe because every reader on both sides of the language
// boundary tests it with a MASK; not one compares the word whole.
//
// response: how hard the field pushes this material, 0 (wind does not touch it,
//   and every consumer early-outs) to 15 (it goes where the air goes).
// friction: the ENTRAINMENT threshold — how hard a per-axis wind must blow to
//   pull a SETTLED grain loose. A different axis from response, not a scaling
//   of it: snow lifts in a breeze and then flies far, wet sand needs a gale and
//   then barely moves.
//
// Authored in materials.json as `"wind": {"response": n, "friction": n}`;
// absent means DeriveWindResponse/DeriveWindFriction below pick a default from
// density and class. Never hardcoded per material in a shader (invariant 7).
constexpr uint32_t kMatWindRespShift = 8, kMatWindRespMask = 0xF;
constexpr uint32_t kMatWindFricShift = 12, kMatWindFricMask = 0xF;
constexpr uint32_t kMatWindMax = 15;

// ---- far palette slot, bits 24..30 of the SAME flags word ----------------
// Which of the far-field cascade's 128 palette slots this material paints at
// distance. A far cell is one byte -- seven bits and a conservative blocker
// flag (common.wgsl FAR_PAL_MASK / FAR_BLOCKER_BIT) -- and those seven bits
// used to BE a material id, which is the only reason this loader ever refused
// a 129th material. They are now an index into the far palette (world.h
// kFarPaletteBaseGpu), so 128 bounds how many things can look DIFFERENT at
// cascade scale rather than how many materials may exist.
//
// AssignFarSlots below hands them out identity-first, so a table whose
// materials all fit in seven bits writes byte-for-byte what it wrote before
// the palette existed. Materials that are indistinguishable at 50 m share a
// slot by authoring `"far": "<material>"`. Mirrors MATF_FAR_PAL_SHIFT in
// common.wgsl; worldgen's three far-cell writers read it through matFarPal().
//
// Packed into `flags` for the reason the wind nibbles and the tint base give:
// MaterialGpu is exactly 64 bytes with no spare word. Bit 31 is now the last
// free bit in it.
constexpr uint32_t kMatFarPalShift = 24, kMatFarPalMask = 0x7F;
constexpr uint32_t kMatFarPalSlots = 128;   // == world.h kFarPaletteSlotsGpu

// The default when a material authors no "wind" block, so that adding wind did
// not mean editing 96 materials — and so that a NEW material is windy on the
// day it is added rather than inert until someone remembers.
//
// Response goes as 1/density, which is what acceleration under a fixed drag
// force does at a fixed voxel size. The constant is set so a gas saturates at
// 15, dust and snow sit near the top, sand and gravel near the bottom, and
// stone is a nudge. It is a STARTING POINT, not a law: real-world wind
// susceptibility is area-over-mass, i.e. SIZE, and a uniform grid has erased
// size — so iron filings and an iron bar are the same material here and only an
// author can say which one this is. That is why the Powder Toy hand-tunes
// `Advection` per element rather than computing it, and why this is a default
// with an override rather than a formula.
inline uint32_t DeriveWindResponse(int32_t density) {
  int32_t d = density > 0 ? density : 1;
  int32_t r = 4800 / d;
  return (uint32_t)(r > (int32_t)kMatWindMax ? (int32_t)kMatWindMax : r);
}
// Friction rises with density: a settled snowflake lifts in a breeze, gravel
// does not lift at all. Floored at 1 because 0 would mean the faintest
// air movement entrains this material, and "wind never moves it" is what
// response 0 says — two spellings of the same thing is one too many.
inline uint32_t DeriveWindFriction(int32_t density) {
  int32_t d = density > 0 ? density : 1;
  int32_t f = 1 + d / 400;
  return (uint32_t)(f > (int32_t)kMatWindMax ? (int32_t)kMatWindMax : f);
}

// ---- ANGLE OF REPOSE (MaterialGpu.repose) ----------------------------------
// The slope a PILE of this powder rests at, authored in materials.json as one
// optional integer:
//
//   "repose": 34            // degrees, 18..72, POWDERS ONLY
//
// Absent (or 45) compiles to the word ZERO, which the kernel reads as "one down
// one across" — the single rule every powder had before this existed. That is
// what makes the default bit-identical to the old sim rather than merely close
// to it, and it is why the encoding is biased so that a zeroed MaterialGpu (air,
// the stain palette run, the art palette run, an unfilled slot) reads as 45°
// instead of as the flowiest tier.
//
// THE ANGLE IS A RUN:RISE RATIO, because a lattice CA has no other way to hold
// one. sim_step.wgsl's powder chain moves a grain one cell per tick, so the only
// angles it can express exactly are the ones whose tangent is a ratio of small
// integers. Five TIERS, in two families either side of the rule that was
// already there:
//
//   code            run:rise   angle    what it does in the kernel
//   kRepose3To1     3 : 1      18.43°   lateral slide toward a drop 3 out
//   kRepose2To1     2 : 1      26.57°   lateral slide toward a drop 2 out
//   kRepose1To1     1 : 1      45°      the bare down-diagonal (the old rule)
//   kRepose1To2     1 : 2      63.43°   down-diagonal only if it drops >= 2
//   kRepose1To3     1 : 3      71.57°   down-diagonal only if it drops >= 3
//
// BOTH FAMILIES READ CELLS 2 AND 3 AWAY, which the 3x3x3 colour lattice cannot
// make race-free on LIVE voxels: same-colour write boxes tile space, so a read
// at distance >= 2 lands in another acting cell's box and its value depends on
// GPU scheduling. They read the REPOSE OCCUPANCY SNAPSHOT instead (world.h
// kReposeSnap*) — one bit per voxel, taken at tick start by a prepass over the
// dirty list plus the nine neighbours a probe can reach, identical in every
// run of the same tick. That
// snapshot is the ONLY licence in the CA for a read past distance 1.
//
// ANGLES BETWEEN TIERS ARE A MIXTURE, not a rounding. The word carries TWO
// codes and a blend: blend/256 of the grains use code B and the rest use code
// A, chosen by a POSITION-KEYED hash with no tick in it, so a grain that has
// not moved makes the same choice every tick and a settled pile still sleeps
// (CLAUDE.md rule 2). Sand at 34° is 60% of grains at 2:1 and 40% at 1:1, and
// the pile it builds rests between the two — which is the whole reason the
// authored surface can be a plain number of degrees rather than a tier name.
//
// Two codes and an explicit blend rather than "tier + fraction of the next one
// up" because that spelling needs the codes to be ORDERED BY ANGLE, and
// then the code that must be zero (45°, the middle of the ladder) cannot be.
// A (codeA, codeB, blend) triple is ordering-free, so the default falls out as
// an all-zero word for free.
// One constant per `constexpr` statement, deliberately: ShaderConstantPrelude()
// emits these and scripts/check_shaders.sh scrapes them to assemble the same
// prelude without a build, and its scraper reads one name per statement.
constexpr uint32_t kMatReposeCodeAShift = 0;
constexpr uint32_t kMatReposeCodeAMask = 0x7;
constexpr uint32_t kMatReposeCodeBShift = 3;
constexpr uint32_t kMatReposeCodeBMask = 0x7;
// 0..255; the fraction of grains using code B is blend/256, so 255 is 99.6%
// and NOT "all of them" — the loader normalizes a saturated blend to a pure
// code rather than leaving one grain in 256 on the wrong tier.
constexpr uint32_t kMatReposeBlendShift = 6;
constexpr uint32_t kMatReposeBlendMask = 0xFF;

// The tier codes. ShaderConstantPrelude() emits these as the REPOSE_* consts
// sim_step.wgsl reads, so there is ONE definition rather than two that must
// agree. THE ORDER OF THE VALUES IS LOAD-BEARING there: the kernel tests
// `code < REPOSE_1_2` to mean "not a steep tier", so the two steep codes have
// to be the two largest.
constexpr uint32_t kRepose1To1 = 0;  // 45 degrees: the word 0, the old rule
constexpr uint32_t kRepose2To1 = 1;
constexpr uint32_t kRepose3To1 = 2;
constexpr uint32_t kRepose1To2 = 3;
constexpr uint32_t kRepose1To3 = 4;

// The authorable range, both ends REFUSED at load rather than clamped — a clamp
// looks like the feature not working, and the first cut of this DID silently
// clamp 10..18 to 18.43 while claiming three lines up that it refused. 18 is
// the flattest face the lattice can hold (3:1) and 72 the steepest (1:3), and
// the loader diagnostic names which end was crossed.
constexpr int32_t kReposeDegMin = 18;
constexpr int32_t kReposeDegMax = 72;
constexpr int32_t kReposeDegDefault = 45;

// The tier ladder in HUNDREDTHS OF A DEGREE, ascending. Integer so the compile
// from degrees to (codeA, codeB, blend) needs no floating point anywhere: the
// authored value is a whole number of degrees, so deg*100 is exact and the
// blend is one integer divide.
//   1843 = atan(1/3), 2657 = atan(1/2), 4500 = 45, 6343 = atan(2),
//   7157 = atan(3)
struct ReposeTier {
  int32_t centideg;
  uint32_t code;
};
constexpr int kReposeTierCount = 5;
constexpr ReposeTier kReposeTiers[kReposeTierCount] = {
    {1843, kRepose3To1}, {2657, kRepose2To1}, {4500, kRepose1To1},
    {6343, kRepose1To2}, {7157, kRepose1To3},
};

// Compiles authored degrees to the packed word. Out-of-range input is the
// caller's problem (LoadMaterialsJson refuses it); this clamps so a bad value
// cannot produce a nonsense code.
inline uint32_t PackRepose(int32_t deg) {
  int32_t cd = deg * 100;
  if (cd <= kReposeTiers[0].centideg) cd = kReposeTiers[0].centideg;
  if (cd >= kReposeTiers[kReposeTierCount - 1].centideg)
    cd = kReposeTiers[kReposeTierCount - 1].centideg;
  uint32_t a = kReposeTiers[kReposeTierCount - 1].code, b = a;
  uint32_t blend = 0;
  for (int i = 0; i < kReposeTierCount - 1; i++) {
    const int32_t lo = kReposeTiers[i].centideg, hi = kReposeTiers[i + 1].centideg;
    if (cd < lo || cd >= hi) continue;
    a = kReposeTiers[i].code;
    b = kReposeTiers[i + 1].code;
    // Rounded, and deliberately to 0..255 rather than 1..255: cd == lo is the
    // tier exactly and must compile to the pure tier.
    blend = (uint32_t)(((cd - lo) * 255 + (hi - lo) / 2) / (hi - lo));
    break;
  }
  // Normalize the two degenerate mixtures to a pure code, so that (a) 45
  // degrees authored explicitly is the same all-zero word as 45 degrees left
  // out, and (b) no material ever pays the blend hash for a decision that is
  // already made.
  if (blend == 0) b = a;
  if (blend >= 255) { a = b; blend = 0; }
  if (a == b) blend = 0;
  if (a == kRepose1To1 && b == kRepose1To1) return 0;
  return ((a & kMatReposeCodeAMask) << kMatReposeCodeAShift) |
         ((b & kMatReposeCodeBMask) << kMatReposeCodeBShift) |
         ((blend & kMatReposeBlendMask) << kMatReposeBlendShift);
}

// GPU-side layout, 80 bytes — must match struct Material in common.wgsl.
//
// GREW FROM 64 TO 80 for `repose`, which is the first field added here that
// could not be packed into a word that already existed. `flags` is full in the
// low half and its high half is the tint-run base; `stainPack` is full;
// `hardness` is 0..255 and its upper 24 bits were the obvious squat, but every
// blast and dig path compares hardness as a WHOLE WORD, and a masked reader in
// nine places to save 16 bytes in a 4096-entry table (256 KiB -> 320 KiB, once,
// in one buffer) is the worse trade. Three reserved words come with it so the
// NEXT field costs nothing: everything that addresses this table does so by
// index through sizeof(MaterialGpu), so the stride is written down nowhere else.
struct MaterialGpu {
  uint32_t klass;
  int32_t density;
  uint32_t color0, color1, color2;  // RGBA8
  uint32_t emission;                // 0..255 glow
  uint32_t flags;
  uint32_t tagMask;
  uint32_t reactOffset, reactCount; // bucket into the flat reaction array
  uint32_t moveEvery;               // viscosity: move only when tick % moveEvery == 0
  uint32_t opacity;                 // 0..255 media absorbance (liquids/gases)
  uint32_t hardness;                // 0..255 blast/dig resistance (DESIGN.md §7)
  uint32_t molten;                  // laser/heat product ID (0 = vaporize to air)
  uint32_t stainPack;               // packed staining behaviour (see below)
  uint32_t stainColor;              // RGBA8 the renderer paints for this stain
  uint32_t repose = 0;              // powder pile slope, packed (see above); 0 = 45 degrees
  uint32_t _r1 = 0, _r2 = 0, _r3 = 0;  // reserved: the next field costs no struct grow
};
static_assert(sizeof(MaterialGpu) == 80, "must match common.wgsl Material");

// ---- staining (MaterialGpu.stainPack) --------------------------------------
// A staining liquid marks the voxels it touches with a stain type + amount in
// the voxel word's spare bits (kStain* in world.h), and may CONSUME what it
// stains. Authored in materials.json:
//
//   "stain": { "type": "blood", "color": "#5e0d0d", "amount": 5,
//              "chance": 60, "consume": 8 }
//
// Packed into ONE u32 rather than four, because MaterialGpu had exactly two
// spare words at the time and growing a struct every sim thread reads is a
// worse trade than four bit-shifts. (It DID later grow, 64 -> 80 bytes, for
// `repose`, which could not be packed into any word that already existed. That
// does not make this packing wrong — it makes "there is no room" the weak half
// of the argument and "every reader masks anyway" the load-bearing half.)
// Layout:
//   bits 0..2   : stain type 1..7 (0 = does not stain) — a palette slot, NOT a
//                 material id; slots are assigned at load in file order so the
//                 renderer can hold a small stain table.
//   bits 3..6   : amount added per contact, 1..15 (voxel amounts saturate at 15)
//   bits 7..16  : per-mille chance per tick to stain a touching neighbour
//   bits 17..26 : per-mille chance a stain CONSUMES the voxel it stained
//   bits 27..30 : ABSORB CAPACITY, 0..15 — see below. Authored on the SUBSTRATE,
//                 not on the stainer, so it shares this word only because there
//                 was room; the two halves are read by opposite sides of the
//                 rule and never by the same material.
//   bit  31     : WASHES — this liquid scrubs FOREIGN stains away instead of
//                 overwriting them with its own (water rinsing blood off).
// 10 bits per chance = 0..1023, which covers the 0..1000 per-mille range the
// rest of the reaction system already speaks.
constexpr uint32_t kStainPackTypeShift = 0, kStainPackTypeMask = 0x7;
constexpr uint32_t kStainPackAmtShift = 3, kStainPackAmtMask = 0xF;
constexpr uint32_t kStainPackChanceShift = 7, kStainPackChanceMask = 0x3FF;
constexpr uint32_t kStainPackConsumeShift = 17, kStainPackConsumeMask = 0x3FF;
constexpr uint32_t kStainPackAbsorbShift = 27, kStainPackAbsorbMask = 0xF;
constexpr uint32_t kStainPackWashesBit = 1u << 31;
constexpr uint32_t kStainChanceMax = 1000;

// ---- absorption (MaterialGpu.stainPack bits 27..30) ------------------------
// How much staining liquid a GROUND material soaks up before the liquid starts
// to persist on top of it as a pool. Authored on the substrate:
//
//   "absorb": { "capacity": 12 }
//
// Capacity is measured in the same 0..15 units as a voxel's stain amount, so
// "grass absorbs 12" literally means "grass accepts stain up to level 12".
// Absent (or 0) = this material never absorbs, and a liquid touching it pools
// immediately — which is every material that predates the feature, including
// every kind of stone.
//
// Why it lives on the substrate and not on the liquid: the liquid's `amount` is
// the per-contact STEP (how fast it soaks in), and capacity is the CEILING (how
// much this ground can hold). Those are genuinely different axes — the same
// rain soaks into sand quickly but shallowly, and into loam slowly but deeply —
// and authoring the ceiling per material PAIR is the N x M explosion tags exist
// to avoid.
constexpr uint32_t kAbsorbCapacityMax = 15;

// The stain palette lives at kStainPaletteBase in the material table — see
// world.h, which holds it because the WGSL prelude is generated from that file.

// Reaction chance resolution — must match REACT_CHANCE_* in common.wgsl.
//
// Chances are AUTHORED in per-mille but stored in units of 1/kReactChanceDen,
// which buys two things: the neighbour-count ramp keeps 6 distinct steps even
// at chance 1 (see kScale* below), and a rule can be authored far below 1
// per-mille. Plain per-mille bottoms out at a mean wait of 1000 ticks ~= 33 s
// at 30 Hz — much too frequent for a "rare ambient event" rule, which is why
// the authored value is allowed to be fractional.
//
// kReactChanceScale must stay a multiple of kScaleMulUnit*5 (= 20) so the ramp
// divide in scaledChance() is exact. At 2000 the finest authorable chance is
// 0.0005 per-mille, a mean wait of ~2e9 ticks (~18.5 h at 30 Hz), and the
// worst-case ramp numerator (den * 95) is ~1.9e8 — 22x inside u32.
constexpr uint32_t kReactChanceScale = 2000;
constexpr uint32_t kReactChanceDen = 1000 * kReactChanceScale;
constexpr double kReactChanceMinMille = 1.0 / (double)kReactChanceScale;

// Formats a per-mille chance for diagnostics without trailing-zero noise
// (0.0005 rather than 0.000500).
std::string FormatMille(double mille);

// Reaction kinds / direction bits — must match common.wgsl.
constexpr uint32_t kReactPair = 0, kReactDecay = 1, kReactEmit = 2;
constexpr uint32_t kDirDown = 1, kDirUp = 2, kDirSide = 4, kDirAny = 7;
constexpr uint32_t kProdKeep = 0xFFFF;
constexpr uint32_t kNbrAny = 0xFFFF;

// Reaction light conditions (ReactionGpu.cond bits 0..7) — must match the
// RCOND_* consts in common.wgsl. Authored in reactions.json as
// "needsSky": true, "when": "day"|"night", "minLight": 0..255.
constexpr uint32_t kCondSky = 1, kCondDay = 2, kCondNight = 4;

// Neighbour-count scaling (ReactionGpu.cond bits 16..31) — see the RSCALE_*
// consts in common.wgsl. Authored as "scaleByNeighbors": {...}.
//
// The rule's chance is scaled by how many of the 6 face neighbours satisfy a
// predicate: 0 matching neighbours means the rule CANNOT fire, and a full 6
// means `scaleMax` times the base chance. That is what turns a uniform
// nucleation rule into a frontier that spreads — a pond freezes from its
// banks inward rather than icing over all at once.
//
// The predicate reuses nbrMat/nbrTags/nbrClass, which a DECAY rule otherwise
// leaves unused, so the counted set is expressed with exactly the vocabulary
// pair rules already use ("neighbor": "water" / "tag:organic" / class list)
// and no new field is needed for the 12-bit id.
constexpr uint32_t kScaleEnable = 1u << 16;  // bit 16: scaling armed
constexpr uint32_t kScaleInvert = 1u << 17;  // count neighbours NOT matching
// Bits 18..19: minCount-1, a floor on the matching count. The default 0 means
// "any count >= 1 fires", which is the original behaviour. Raising it makes a
// rule need a WIDER frontier before it can fire at all, which is what lets
// evaporation say "an exposed droplet boils off, but the flat surface of a
// pond does not". Without it the ramp's only hard gate is at count 0, so a
// pond surface (5 water neighbours, 1 air) always fires at the base rate.
//
// Stored biased by 1 so the common no-threshold case stays the zero value:
//   stored = minCount - 1, range 1..4.
constexpr uint32_t kScaleMinShift = 18, kScaleMinMask = 0x3u;
constexpr uint32_t kScaleMinCountMin = 1, kScaleMinCountMax = 4;
// Bits 20..23 hold the multiplier at a full count of 6, in
// quarters BIASED BY 1.0 — stored = round(scaleMax*4) - 4. A multiplier below
// 1.0x is meaningless (the count gate is what suppresses, not the scale), so
// spending codes on it would waste the field; the bias buys 1.0x..4.75x out
// of 4 bits, which covers the 4x this was built for.
constexpr uint32_t kScaleMulShift = 20, kScaleMulMask = 0xFu;
constexpr uint32_t kScaleMulUnit = 4;  // quarters per 1.0x
constexpr float kScaleMulMin = 1.0f, kScaleMulMax = 4.75f;

// GPU-side layout, 32 bytes — must match struct Reaction in common.wgsl.
struct ReactionGpu {
  uint32_t packed;    // bits 0..1 kind, bits 2..4 dir mask
  uint32_t nbrMat;    // exact neighbor id, or kNbrAny
  uint32_t nbrTags;   // tag mask (nonzero => neighbor matches on any shared tag)
  uint32_t nbrClass;  // bit-per-class filter (1<<klass); 0 = any class
  uint32_t chance;    // per tick, in units of 1/kReactChanceDen (see above)
  uint32_t prodSelf;  // kProdKeep = unchanged, 0 = air
  uint32_t prodNbr;   // pair: neighbor product; emit: emitted material
  // Light/day-phase condition + neighbour-count scaling (was pad — no struct
  // growth).
  //   bits 0..7   : kCondSky | kCondDay | kCondNight
  //   bits 8..15  : minimum daylight strength 0..255 (0 = no floor)
  //   bits 16..19 : kScaleEnable | kScaleInvert | (minCount-1)
  //   bits 20..23 : the multiplier at a full count of 6, quarters biased by 1
  // Zero means unconditional, which is every pre-existing rule.
  uint32_t cond;
};
static_assert(sizeof(ReactionGpu) == 32, "must match common.wgsl Reaction");

constexpr uint32_t kMaxReactions = 4096;

struct MaterialDef {
  std::string name;
  MaterialGpu gpu{};
  std::vector<std::string> tags;
  // What sub-8-voxel islands of this material crumble into (DESIGN.md §7).
  // Empty = default (dust for organics/flammables, gravel otherwise).
  std::string rubble;
  // What the laser/heat melt mode converts this into (stone -> lava,
  // sand -> molten_glass, wood -> fire ...). Empty = vaporize to air.
  std::string molten;
  // ARCANE VALUE: the per-voxel base of the spell tariff (docs/
  // PLAN_magic_grammar.md section 4; materials.json "arcane"). One integer
  // per material rather than a from x to table, so hundreds of materials stay
  // O(N) and a modder prices a new one with one key. 0 for air, small for
  // dirt/sand/water, large for gold. Authored, or derived from density when
  // absent (DeriveArcane), so every material has a price the day it exists.
  int32_t arcane = 0;
  // Name of the stain this material leaves, if any (materials.json "stain":
  // {"type": ...}). Shared across materials: two liquids naming the same stain
  // get the same palette slot. Empty = this material does not stain.
  std::string stain;
  // How much staining liquid this material soaks up before the liquid pools on
  // top (materials.json "absorb": {"capacity": ...}), in the same 0..15 units
  // as a voxel's stain amount. 0 = never absorbs. Mirrors the top nibble of
  // gpu.stainPack; kept unpacked here for the tuner and the wiki.
  uint32_t absorbCapacity = 0;
  // GRID colours for a MATF_TINTED material (materials.json "tints"), packed
  // 0x00RRGGBB, at most kMatTintsMax. Entry i is what a voxel of this material
  // with state nibble i renders as; entry 0 is the natural colour by
  // convention. Empty = not tinted, and the nibble keeps its jitter meaning.
  //
  // Kept unpacked here, like absorbCapacity, because the packed side is a run
  // in a reserved region of the GPU material table (world.h kTintPaletteBaseGpu)
  // that the tuner and the wiki cannot read back.
  std::vector<uint32_t> tints;
  // Unpacked mirrors of gpu.flags bits 8..15 (see kMatWind* above), kept for
  // the tuner and the wiki the way absorbCapacity is — the packed word is the
  // truth, these are for anything that wants to READ the value back without
  // knowing the layout. Always populated, whether authored or derived.
  uint32_t windResponse = 0;
  uint32_t windFriction = 0;
  // FAR-FIELD LOOK-ALIKE (materials.json "far": "<material name>"). Names the
  // material whose far palette slot this one shares -- "at cascade distance I
  // am that". Empty = this material owns a slot of its own. Resolved by name
  // at load; an alias may not point at another alias (one hop, so the slot a
  // byte names is always a material that actually paints itself).
  std::string farAlias;
  // Unpacked mirror of gpu.flags bits 24..30 (kMatFarPalShift), kept like
  // windResponse for anything that wants to read the slot back without knowing
  // the layout -- including UploadTables, which builds the REVERSE table from
  // it. The packed word is the truth.
  uint32_t farPalSlot = 0;
  // True for the material that OWNS farPalSlot -- i.e. the one the reverse
  // table maps that slot back to. Exactly one material per live slot has it,
  // and it is what UploadTables walks: an alias must not overwrite the entry
  // for the slot it borrowed.
  bool farPalOwner = false;
  // The AUTHORED angle of repose in degrees, mirrored unpacked for the tuner
  // and the wiki exactly as absorbCapacity and the wind nibbles are. Always
  // populated: a powder that authors nothing reads 45, and a non-powder reads
  // 45 too (the field means nothing for it, and 0 would read as a legal angle).
  int32_t reposeDeg = kReposeDegDefault;
  // Sound sets for this surface, keyed by SLOT ("footstep", "impact",
  // "break", ...). Each value names a set relative to the slot's namespace, so
  // "footstep": "leaf" resolves to the set "footsteps/leaf" — one FOLDER under
  // assets/sounds/ whose files are the interchangeable variants.
  //
  // Authored either as a "sounds" object or, for footsteps only, as the older
  // flat "footstep": "leaf" key, which is still read (and still written by the
  // tuner for materials that already use it). assets/sound_schema.js is the
  // list of slots the tuner offers; the engine only cares that a key it looks
  // up is present.
  //
  // A missing slot is NOT an error: cues.cpp falls back by tag, so a new
  // material is audible the day it is added. Purely presentation — the sim
  // never reads any of this, and an unknown set name is a diagnostic.
  std::map<std::string, std::string> sounds;

  // The named slot, or "" if this material does not author one.
  const std::string& Sound(const char* slot) const {
    static const std::string kNone;
    auto it = sounds.find(slot);
    return it == sounds.end() ? kNone : it->second;
  }
};

// Loads materials.json + reactions.json and compiles them into GPU tables:
// tag strings become bits of a shared registry, reactions are grouped into
// per-material buckets preserving file order (first matching rule wins on the
// GPU, so specific rules must precede tag rules in the file). Returns false
// (with errors filled) on validation failure — modders get diagnostics, not
// silent breakage (DESIGN.md §6). Index in mats == 12-bit material ID; slot 0
// is air.
bool LoadAssets(const std::string& materialsPath, const std::string& reactionsPath,
                std::vector<MaterialDef>& mats, std::vector<ReactionGpu>& reactions,
                std::string& errors);

// Builds the material-id -> COLLISION class table that World::KindAt reads.
//
// This is not just `m.gpu.klass` per material, and the difference is the whole
// point: a material flagged PASSABLE (soft vegetation — reeds, kelp, lilypads)
// is reported as CLASS_GAS here, so KindAt returns Gas and every CPU collision
// path already treats it as empty space. The player capsule sweep, the mob and
// avatar ground probes and the spell projectile march all test against Solid,
// so none of them needs to learn about the flag.
//
// Gas rather than a new kind, because Gas is the class those paths ALREADY
// mean "you can move through this" for — smoke and steam. Reusing it means the
// behaviour drops out of the existing tests instead of adding a fourth case to
// each of them, and a caller that forgets to handle passable simply cannot
// exist.
//
// Everything else still sees a solid: the CA, fire, the brush, explosions, the
// laser and the renderer all read gpu.klass directly and are untouched.
std::vector<uint32_t> BuildCollisionClasses(const std::vector<MaterialDef>& mats);
