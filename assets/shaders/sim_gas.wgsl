// sim_gas.wgsl — gas that has left the residency window, as particles.
// docs/PLAN_gas_particles.md stage 1.
//
// INSIDE the window nothing changes: smoke is a CLASS_GAS voxel with the whole
// reaction system available to it. What this file owns is what used to be
// impossible — a gas parcel whose next move leaves the window. `tryMove`
// returned false there, indistinguishable from a wall, and the parcel sheeted
// across the top chunk plane until it decayed. sim_step's `gasLeave` now
// deletes that voxel and appends a record here instead; the edge is a SINK.
//
// A gas particle is deliberately NOT a ballistic particle:
//
//   * Its position is 24.8 with a ZERO FRACTION. It lives ON a cell and moves
//     one whole cell per tick, because it must take the moves a voxel would —
//     the same gasIntent buoyancy/wind roll and the same fourteen-step
//     fallback ladder, called out of sim_step.wgsl rather than re-typed.
//   * Its velocity words stay zero, which keeps `particlePriority` a pure
//     function of the visible state.
//   * It carries no age. Death is rolled from the material's OWN RK_DECAY
//     rules in reactions.json, keyed on (seed, tick, position-derived key) —
//     the same shape of roll the CA makes, so the decay chance an author tunes
//     for the voxel is the one the particle obeys.
//
// EVERYTHING IS BOUNDED (rule 2): the per-tick conversion budget, the pool
// cap, an outer box, a ceiling, and the authored decay. A gas particle wakes
// no chunk except when it RE-ENTERS the window, which is the one place it
// meets the page table.
//
// DETERMINISM (rule 1): every roll keys on (seed, tick, a key derived from the
// particle's CELL and payload), never on a buffer slot. Write reach is <=1
// cell and only through the claim path. The one place order does not matter
// and is not made to is `gasOuter`, which is render-only derived data the sim
// never reads and the world hash never covers.

@group(0) @binding(0)  var<storage, read_write> voxels : array<u32>;
@group(0) @binding(2)  var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3)  var<storage, read> materials : array<Material>;
@group(0) @binding(4)  var<uniform> T : TickParams;
@group(0) @binding(17) var<storage, read> pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Its own bank
// rather than a share of the ballistic kernel's: the two populations fault for
// completely different reasons — a parcel lands through the claim path where
// debris lands through a DDA — and "which kernel dropped the store" is the
// whole point of the per-kernel tally.
const PT_KERNEL : u32 = PT_K_GAS;

@group(1) @binding(0)  var<storage, read_write> gasRead : array<Particle>;
@group(1) @binding(1)  var<storage, read_write> gasWrite : array<Particle>;
@group(1) @binding(2)  var<storage, read_write> gasCounts : array<atomic<u32>>;
// The CA's outbox: GAS_SP_HDR header words then Particle-shaped records. Declared
// atomic here only because sim_step declares it so; this module's reads of the
// records are plain loads through atomicLoad.
@group(1) @binding(3)  var<storage, read_write> gasSpawn : array<atomic<u32>>;
@group(1) @binding(4)  var<storage, read_write> gasClaim : array<atomic<u32>>;
@group(1) @binding(5)  var<storage, read_write> gasArgs : array<u32>;
@group(1) @binding(6)  var<storage, read_write> gasOuter : array<atomic<u32>>;
@group(1) @binding(7)  var<storage, read> farVox : array<u32>;
@group(1) @binding(8)  var<uniform> farP : FarParams;
@group(1) @binding(9)  var<storage, read> reactions : array<Reaction>;
// CPU-authored gas spawns (the gas-reenter gate; any future emitter that is
// not a grid cell). Same shape as gasSpawn: word 0 is the count, records from
// word GAS_SP_HDR. Part of the per-tick input stream, exactly like spawnOps.
@group(1) @binding(10) var<storage, read> gasSpawnOps : array<u32>;
// The far fire-plume emitter list (world.h kGasFarEmitMax): [0] count, [1] the
// box shift it was built for, [2..3] reserved, then 4-word records. CPU-built
// from the eviction harvest and uploaded only on the ticks it changes.
@group(1) @binding(11) var<storage, read> gasFarEmit : array<u32>;
// The LONG-RANGE density box (world.h kGasFarOuterN): the same thing gasOuter
// is, at 8x the cell size and 8x the span, for the fires that are further out
// than gasOuter reaches. Render-only in exactly the same sense.
@group(1) @binding(12) var<storage, read_write> gasFarOuter : array<atomic<u32>>;

// ---- constants that must agree with sim_step.wgsl and src/sim/world.h ------
// Kept out of common.wgsl on purpose (CLAUDE.md: a constant only its consumers
// read is declared next to them; a common.wgsl edit costs the whole SPIR-V
// cache and the worldgen far-cascade recompile). check_invariants.py compares
// these against world.h.
const GAS_SP_COUNT     : u32 = 0u;
const GAS_SP_REFUSED   : u32 = 1u;
const GAS_SP_EDGE      : u32 = 2u;
const GAS_SP_POOLFULL  : u32 = 3u;  // spawn refused: the particle pool was full
const GAS_SP_REENTER   : u32 = 4u;  // particles that became voxels this tick
const GAS_SP_DIED      : u32 = 5u;  // decay / outer box / ceiling
const GAS_SP_ABOVE     : u32 = 6u;  // live particles above the window's top face
const GAS_SP_LIVE      : u32 = 7u;  // live particles after integrate
// The determinism digest: SUM of every surviving parcel's particlePriority.
// A sum because pool append order is scheduling-dependent and a digest that
// depended on it would report a false divergence on every run. Outside the
// window a parcel touches no voxel, so the world hash cannot see it and this
// is the only thing that can — see the determinism gate.
const GAS_SP_DIGEST    : u32 = 8u;
const GAS_SP_HDR       : u32 = 16u;
const GAS_SP_STRIDE    : u32 = 8u;
const GAS_SPAWN_CAP    : u32 = 65536u;   // kGasSpawnPerTick
const GAS_CPU_SPAWN_CAP: u32 = 1024u;    // kGasCpuSpawnPerTick
const GAS_PARTICLE_CAP : u32 = 262144u;  // kGasParticleCap

// The outer density box (§2.5). GAS_OUTER_N cells per axis, GAS_OUTER_SHIFT
// fine voxels per cell, so the box edge is 2x the residency window's and it is
// centred on the window. One 16-BIT COUNT per cell, two to a word (stage 1b
// widened it from a byte; world.h kGasOuterWords has the argument).
//
// DEVIATION from the plan text, which asked for 0.4 m cells AND 2x the window
// edge AND 2 MiB — three numbers that cannot all hold (128^3 bytes is 2 MiB
// and 128 * 0.4 m is 51.2 m, half the stated span). Coverage is kept; the cell
// is 8 voxels = 0.8 m and, since stage 1b, 4 MiB rather than 2.
const GAS_OUTER_N     : u32 = 128u;
const GAS_OUTER_SHIFT : u32 = 3u;
// The per-cell saturation guard. The cell is a 16-BIT count now (stage 1b:
// world.h kGasOuterWords), two to a word, and what the guard buys is unchanged
// from the byte era — that an add cannot CARRY into the neighbouring cell's
// half of the word, which would read as a bright cell one over. 60,000 leaves
// 5,535 of headroom above it, and the most simultaneous adders a cell can have
// between the load and the add is the 512 fine voxels it contains plus the
// parcels stacked in it, so the carry is out of reach rather than merely
// unlikely. A plume dense enough to reach 60,000 in a 0.8 m cell saturates,
// which is what it should look like anyway.
const GAS_OUTER_MAX   : u32 = 60000u;
// Die this many voxels above the window's top face (kGasCeilingVox). Inside
// the outer box on purpose, so the ceiling is a test that can be observed to
// fire rather than one the box would have caught anyway.
const GAS_CEILING_VOX : i32 = 192;
// RNG salts. Distinct streams so the decay roll, the motion roll and the key
// derivation cannot alias.
const GAS_KEY_SALT   : u32 = 0x9A17u;
const GAS_DECAY_SALT : u32 = 0x2C05u;
const GAS_PLUME_SALT : u32 = 0x5B31u;

// ---- FAR FIRE PLUMES (world.h's kGasFarEmitMax block) ----------------------
// The emitter list's layout, which the CPU writes and this file reads.
// check_invariants.py pins these against world.h.
const GAS_FAR_EMIT_HDR    : u32 = 4u;
const GAS_FAR_EMIT_STRIDE : u32 = 4u;
const GAS_FAR_EMIT_MAX    : u32 = 256u;   // kGasFarEmitMax
// The most hot voxels one emitter can stand for: a gasOuter cell in x/z by a
// fine chunk in y. The strength is divided by it, so "a fully burning column"
// is 1.0 whatever the cell size becomes.
const GAS_FAR_STRENGTH_MAX : u32 = 1024u; // kGasFarEmitStrengthMax
// The strength word's layout (world.h kGasFarEmitStrengthMask / WeightShift):
// low 24 bits the count, top byte the crossfade weight.
const GAS_FAR_STRENGTH_MASK : u32 = 0xFFFFFFu;
const GAS_FAR_WEIGHT_SHIFT : u32 = 24u;
// The WIDE section of the same buffer (world.h kGasFarEmitMaxWide /
// kGasFarEmitWideBase): the emitters for the long-range box, at a FIXED word
// offset so this kernel can address them without first reading the fine count.
const GAS_FAR_WIDE_MAX  : u32 = 256u;
const GAS_FAR_WIDE_BASE : u32 = 1028u;
// The long-range box itself (world.h kGasFarOuterN / kGasFarOuterShift).
const GAS_FAROUT_N     : u32 = 128u;
const GAS_FAROUT_SHIFT : u32 = 6u;
const_assert (GAS_FAROUT_N << GAS_FAROUT_SHIFT) == 16u * WORLD_N;
// The cell is ANISOTROPIC (world.h kGasFarOuterShiftY): 64 voxels in x/z and 8
// in y, so the box's vertical span is the fine box's ±409.6 m. A 51.2 m-tall
// cell put a canopy-height plume base in a cell that reached the ground.
const GAS_FAROUT_SHIFT_Y : u32 = 3u;
const_assert (GAS_FAROUT_N << GAS_FAROUT_SHIFT_Y) == 2u * WORLD_N;
const GAS_FAROUT_OFF_Y : i32 = i32((GAS_FAROUT_N << GAS_FAROUT_SHIFT_Y) / 2u) -
                               i32(WORLD_N / 2u);
const GAS_FAROUT_SHIFTS : vec3<u32> =
    vec3<u32>(GAS_FAROUT_SHIFT, GAS_FAROUT_SHIFT_Y, GAS_FAROUT_SHIFT);
// The centring offset, floored-origin (world.h kGasFarOuterOffsetVox). The
// fine box does not need the floor and this one does: the window origin is a
// multiple of 16 voxels and this cell is 64, so without it the lattice would
// slide 1.6 m sideways on every window shift and a settled plume would visibly
// re-quantise as the player walked.
const GAS_FAROUT_OFF : i32 = i32((GAS_FAROUT_N << GAS_FAROUT_SHIFT) / 2u) -
                             i32(WORLD_N / 2u);
// How much taller a WIDE plume gets for being a bigger fire. A wide emitter is
// a whole 6.4 m column footprint, aggregated from up to 256 fine columns, and
// its strength is the SUM of theirs -- so unlike the fine list it carries real
// information about how large the fire is, and a large fire makes a tall
// column. sqrt(columns), capped: one burning column is 1x, sixteen is 4x.
const FAR_PLUME_WIDE_HMAX : f32 = 4.0;
// ...AND IT NO LONGER FLOORS AT ONE (2026-09-19). `cols` is strength / 1024, so
// hMul = 1 means ONE FULL FINE COLUMN of fire -- a whole 16-voxel-deep chunk
// column saturated. Flooring the clamp there gave a chunk with THREE hot voxels
// in it the same 28 m column at the same 4.3 m radius as a burning tree, only
// fainter, because only the MASS scaled with `cols` and never the SIZE.
//
// That was survivable only as long as the renderer eroded faint columns away,
// and it stopped being survivable the moment GAS_CORE_COUNT_WIDE fixed the
// threshold that was doing it: the report went straight from "small fires have
// no smoke at distance" to "billowing clouds from places with no fire". Both
// are the same defect seen from either side -- size that does not follow the
// fire, with an erosion threshold accidentally standing in for it.
//
// The floor is now a minimum VISIBLE wisp rather than a full column. Anything
// smaller deposits a mass that rounds to zero anyway (a one-voxel chunk is
// cols = 0.001, i.e. 0.05 of a count), so this bound is about keeping the puff
// radius off exactly zero, not about keeping tiny fires alive.
const FAR_PLUME_WIDE_HMIN : f32 = 0.05;

// One thread per CELL of height. 64 is the whole budget: the tuning clamp
// keeps render.farPlumeHeight inside the box's half-extent (51.2 m = 64 cells),
// so a thread per height covers the tallest legal plume with no loop.
const FAR_PLUME_STEPS : u32 = 64u;
// ---- THE PUFFS, and the one afternoon they were something else -------------
//
// The deposit is hash-scattered PUFFS per height step: FAR_PLUME_PUFFS points
// inside a radius that grows with height, each rounded to a cell. For one
// afternoon on 2026-09-19 it was an enumerated disc footprint that filled every
// cell the plume's cross-section touched, with a wander on the column, and it
// looked "completely different and worse": a broad soft body whose base was
// several chunks across, so the smoke no longer read as coming FROM the fire.
// The puff look — a thin, broken, wispy column at 6.4 m cells — was the one the
// player liked; it just needed to be THICKER. So the puffs are back, and the
// thickness comes from what that afternoon did leave behind:
//
//   ONE EMITTER ADDS TO A GIVEN CELL AT MOST ONCE. Puffs of one thread that
//   round to the same cell are MERGED before the add (gasFarPlume's `seen`
//   loop), and the wide box deposits one bilinear puff whose four cells are
//   distinct by construction. That divides the anti-carry worst case by the
//   puff count, which is what let FAR_PLUME_ADD_MAX go 80 -> 240 and the
//   per-puff density 72 -> 240 — the thickness — with the proof below getting
//   STRONGER rather than weaker.
//
// Puffs per height step. Five, not three: with the merge above the count no
// longer appears in the proof, so it is purely how many cells of the upper
// column get smoke (the base rounds them all into one cell whatever the
// number). Five is what fills the top of the old radius without the column
// turning into the solid body the footprint was.
const FAR_PLUME_PUFFS : u32 = 5u;
// How wide the column gets by the top and how wide it starts, in METRES.
// Metres and not cells, because there are two boxes and a radius in cells
// would make the SAME fire eight times wider in the coarse box than in the
// fine one -- the shape has to be a property of the plume, not of the grid it
// is drawn on. These are the numbers the liked look had; they are under one
// 6.4 m cell for most of the column, which is WHY it reads as a wisp.
const FAR_PLUME_R0_M     : f32 = 0.32;
const FAR_PLUME_SPREAD_M : f32 = 4.0;
// Density per PUFF at the base of a fully burning column at
// render.farPlumeStrength 1. The renderer divides a cell's count by (1<<SHIFT)^3
// = 512 to get a volume fraction, so 240 is a cell 47% smoke by volume.
//
// 72 -> 240 (2026-09-19) is the "thicker". At the BASE it changes little: the
// old three puffs of 72 all rounded into one cell and stacked to 216. In the
// UPPER column, where the puffs scatter into separate cells, each cell used to
// get ONE puff of 72 and now gets one of 240 — 3.3x — and that upper column is
// what a distant fire mostly shows. It is exactly the 4x the renderer used to
// apply as a distance-ramped gain, moved into the data so it holds at every
// distance instead of switching on fifty metres past the window face.
//
// Equal to FAR_PLUME_ADD_MAX on purpose: strength 1 is the top of the range and
// render.farPlumeStrength past 1 clips per-add (silently — nothing visible
// breaks, the knob just stops doing anything). Raise ADD_MAX to go further,
// and re-check the proof below.
const FAR_PLUME_DENSITY : f32 = 240.0;
// THE WIDE BOX'S CHORD, in fine cells per wide cell: 1 << (6 - 3) = 8. A ray
// crosses 64 voxels of a wide cell against 8 of a fine one, so the same count
// is eight times the optical depth; the wide deposit divides its mass by this
// so the coarse box renders the fine box's column to the same opacity. This
// was a hand-ramped 1/4..1/12 in the RENDERER until 2026-09-19; it is a
// property of the two grids and belongs where the counts are written.
const FAR_PLUME_WIDE_CHORD : f32 = f32(1u << (GAS_FAROUT_SHIFT - GAS_OUTER_SHIFT));
// The wide cell is only wider in x/z; in y it is the fine cell, so the two
// boxes hold a plume at the same number of height cells and one rise speed.
const_assert GAS_FAROUT_SHIFT_Y == GAS_OUTER_SHIFT;
// ...and how many fine cells a ray crosses of the fine column, on average over
// its height: one at the base (every puff in one cell), about two at the top
// (five puffs over a disc 1.3 cells across). The wide deposit is ONE puff per
// height step against the fine box's five, so its mass is scaled by this over
// the chord to match the fine box's LINE INTEGRAL rather than its puff count.
// Measured, not derived: the gas-farplume gate's fine column against the
// gas-farplume2 gate's wide one.
const FAR_PLUME_WIDE_FILL : f32 = 1.5;
// ---- THE ANTI-CARRY BOUND, and it is a PROOF rather than a margin ----------
// gasOuter packs two 16-bit cells per word, so an add that overflows its half
// carries into the neighbour's — a bright cell one over, on some runs only.
// The parcel splat guards this by adding exactly 1; this one adds a WEIGHT, so
// the guard has to be sized.
//
// ONE EMITTER CAN REACH A CELL ONLY ONCE, and that is what sizes this. A thread
// owns one cell of HEIGHT; the fine kernel MERGES puffs that round to one cell
// before adding, and the wide kernel's single bilinear puff touches four
// distinct cells. So no two adds of one workgroup can land in the same cell. Across emitters they can, and the
// bound on how many is the list cap itself. So the worst reachable value after
// a race is
//
//     FAR_PLUME_CEIL + (GAS_FAR_EMIT_MAX - 1) * FAR_PLUME_ADD_MAX
//
// and the const_assert below is that it still fits 16 bits. That is a factor of
// FAR_PLUME_PUFFS tighter than the unmerged version's bound, and spending
// it on ADD_MAX is what pays for a physical density instead of a renderer-side
// gain: 4,096 + 255 x 240 = 65,296, i.e. the 240 is the largest per-add this
// packing admits at a 256-emitter cap and there is no margin left to take.
//
// FAR_PLUME_CEIL is where this kernel stops adding at all: 4,096 is eight times
// a fully solid cell, i.e. far past opaque, so nothing visible is lost there.
const FAR_PLUME_CEIL    : u32 = 4096u;
const FAR_PLUME_ADD_MAX : u32 = 240u;
const_assert FAR_PLUME_CEIL +
    (GAS_FAR_EMIT_MAX - 1u) * FAR_PLUME_ADD_MAX <= 0xFFFFu;
// ...and the SAME proof for the long-range box, restated against its own cap
// rather than shared with the one above. The two caps are equal today; stating
// it twice is what makes raising one of them a compile error instead of a
// carry into a neighbouring cell on some runs.
const_assert FAR_PLUME_CEIL +
    (GAS_FAR_WIDE_MAX - 1u) * FAR_PLUME_ADD_MAX <= 0xFFFFu;
// How fast a plume packet rises, in VOXELS per tick — not in cells, which is
// what this constant used to be and was a bug the moment a second box existed.
// A gas voxel takes one whole voxel per tick in the CA's ladder, so 1.0 is the
// parcel's own speed and the far plume climbs at the speed the near smoke does.
//
// IT HAS TO BE VOXELS BECAUSE THE TWO BOXES INDEX HEIGHT IN THEIR OWN CELLS.
// `risen` is subtracted from `s`, and `s` counts 0.8 m cells in the fine box
// and 6.4 m cells in the wide one. Expressed in cells, the same number made the
// wide box's billows climb EIGHT TIMES too fast — 48 m/s rather than 6 — so a
// distant plume scrolled like a shimmer instead of rising like smoke. Dividing
// by the caller's own cellVox is what makes it one physical speed; the fine box
// still evaluates to exactly the 0.125 it always had, so its contents do not
// move. Consistent with FAR_PLUME_RISE_VPS below: 1 voxel/tick x 60 tps = 60.
const FAR_PLUME_RISE_VOX : f32 = 1.0;
// Voxels per second a plume rises, used ONLY to turn the wind speed into a
// tilt: lateral cells per vertical cell is (wind vox/s) / this. 1 voxel/tick at
// 60 ticks/s.
const FAR_PLUME_RISE_VPS : f32 = 60.0;

// (a) RESIDENCY, in the tickets-P0 sense (docs/tickets_p0_audit.md): every
// caller here asks "may I read or write this cell", never "where is the window
// box". Re-entry proposes a voxel, gasBlocked asks whether the GRID is the
// authority at a cell, and the splat asks whether the parcel is already being
// drawn as a voxel — all three are true for a ticket chunk, which is resident
// and simulated and rendered even though it is nowhere near the window.
//
// The WINDOW-BOX questions in this file are gasOuterOrigin/gasCeilY, and they
// deliberately keep saying WORLD_N: the outer density box really is two window
// edges centred on the window, and the kill ceiling really is measured from the
// window's top face. That is class (c) and it does not move.
fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, T.origin); }

fn gasLive(page : u32) -> u32 {
  return min(atomicLoad(&gasCounts[page]), GAS_PARTICLE_CAP);
}

fn gasAppend(p : Particle) {
  let slot = atomicAdd(&gasCounts[1u - T.page], 1u);
  if (slot < GAS_PARTICLE_CAP) { gasWrite[slot] = p; }
}

// The gas motion model — gasRndK, windLateralStartK, gasIntentK, gasLadderStep
// and the GasIntent struct — is in common.wgsl, which is where it belongs and
// where the CA reads it from too. That is the plan's §2.2 requirement made
// structural: a parcel takes the moves a voxel would because it calls the same
// function, not because two copies were kept in step.
//
// The one thing this kernel supplies differently is the roll's inputs. The CA
// keys on its cell's slot index and rolls once per gravity SUBSTEP; a parcel
// keys on gasKey (a hash of its cell and payload) and rolls once per TICK, so
// every call below passes substep 0.

// ---- the outer box ---------------------------------------------------------
// T.origin is in CHUNK units. The box edge is 2x the window's and centred on
// it, so its min corner sits half a window below the window's min corner. The
// window origin is chunk-aligned (a multiple of 16 voxels) and half a window
// is 256, so this is always a multiple of the cell size: no rounding, and the
// mapping the renderer reproduces is exactly this expression.
fn gasOuterOrigin() -> vec3<i32> {
  return T.origin * i32(CHUNK) - vec3<i32>(i32(WORLD_N) / 2);
}

// The kill ceiling, in world voxels.
fn gasCeilY() -> i32 {
  return T.origin.y * i32(CHUNK) + i32(WORLD_N) + GAS_CEILING_VOX;
}

fn gasOuterCell(c : vec3<i32>) -> vec3<i32> {
  return (c - gasOuterOrigin()) >> vec3<u32>(GAS_OUTER_SHIFT);
}

fn gasInOuterBox(c : vec3<i32>) -> bool {
  let d = gasOuterCell(c);
  let n = i32(GAS_OUTER_N);
  return d.x >= 0 && d.y >= 0 && d.z >= 0 && d.x < n && d.y < n && d.z < n;
}

// ---- blocking outside the window (§2.4) ------------------------------------
// The far cascade's level-1 byte, which is the same test the far march uses to
// decide the ray hit something. A dense toroidal array addressed directly —
// NOT the page table, which does not extend past the window and has no
// business being consulted for a parcel of smoke 60 m away. Coarse (one byte
// per 2^(1+FAR_SHIFT_BASE) fine voxels) and right for a plume: what it is for
// is "does the plume go around the hill", not per-voxel collision.
//
// OUTSIDE THE CASCADE reads as OPEN, not as blocked. The conservative
// direction elsewhere in the engine is "assume solid" (the residency edge is
// inert), but here the parcel is already outside the simulated world and the
// alternative is a wall at the cascade boundary that would pin every plume
// against an invisible surface.
fn gasFarBlocked(c : vec3<i32>) -> bool {
  let cell = c >> vec3<u32>(farCellShift(1u));
  if (!farInBox(cell, farP.origins[0].xyz)) { return false; }
  let bi = farVoxByteIndex(1u, cell);
  let b = (farVox[bi >> 2u] >> (8u * (bi & 3u))) & 0xFFu;
  if (b == 0u) { return false; }               // air
  // The blocker flag alone is terrain the centre sample missed — solid, and
  // it carries no material of its own. (This used to fall out of indexing the
  // material table with the whole byte: 128..255 are unwritten entries, whose
  // zeroed klass is CLASS_SOLID. Same answer, said on purpose.)
  if ((b & FAR_BLOCKER_BIT) != 0u) { return true; }
  // A gas does not block a gas — smoke downsampled into the cascade must not
  // wall its own plume off. The byte is a far PALETTE SLOT, so translate it
  // back to a material first (common.wgsl FAR_PAL_MASK).
  return materials[farPalMat(&materials, b & FAR_PAL_MASK)].klass != CLASS_GAS;
}

// Can this parcel enter cell `c`? Inside the window the grid answers (and the
// caller then RE-ENTERS rather than moving); outside, the cascade does.
// Gas particles do not exclude each other: several may share a cell, which is
// what makes the outer density box a density instead of an occupancy map.
fn gasBlocked(c : vec3<i32>) -> bool {
  if (inBounds(c)) {
    let mm = voxMat(voxWordAt(c));
    if (mm == MAT_AIR) { return false; }
    return materials[mm].klass != CLASS_GAS;
  }
  return gasFarBlocked(c);
}

// ---- the splat (§2.5) ------------------------------------------------------
// RENDER-ONLY DERIVED DATA. The sim never reads gasOuter, the world hash never
// covers it, and it is rebuilt from scratch every tick — so the load-then-add
// below is a benign race and is stated as one rather than defended. See
// GAS_OUTER_MAX above for what the guard buys.
//
// TWO SPLATTERS SHARE THIS BOX (stage 1b). This one takes a PARCEL, and the
// CA's gasOuterSplat in sim_step.wgsl takes an in-window VOXEL, once per tick.
// They are separate functions because the two kernels bind gasOuter at
// different numbers and neither may declare it in common.wgsl (the SPIR-V
// cache), but the CELL MAPPING is the same expression in both and
// check_invariants.py pins the constants it is built from.
fn gasSplat(c : vec3<i32>) {
  let d = gasOuterCell(c);
  let n = i32(GAS_OUTER_N);
  if (d.x < 0 || d.y < 0 || d.z < 0 || d.x >= n || d.y >= n || d.z >= n) { return; }
  let li = (u32(d.z) * GAS_OUTER_N + u32(d.y)) * GAS_OUTER_N + u32(d.x);
  let word = li >> 1u;
  let sh = 16u * (li & 1u);
  if (((atomicLoad(&gasOuter[word]) >> sh) & 0xFFFFu) >= GAS_OUTER_MAX) { return; }
  atomicAdd(&gasOuter[word], 1u << sh);
}

// The far fire plume's add: a WEIGHT into one CELL, where gasSplat takes a
// voxel and adds one. Separate rather than a parameter on gasSplat because the
// two have different overflow arguments and the argument is the interesting
// part of both — see FAR_PLUME_CEIL above.
fn gasOuterAddCell(d : vec3<i32>, amount : u32) {
  let n = i32(GAS_OUTER_N);
  if (d.x < 0 || d.y < 0 || d.z < 0 || d.x >= n || d.y >= n || d.z >= n) { return; }
  if (amount == 0u) { return; }
  let li = (u32(d.z) * GAS_OUTER_N + u32(d.y)) * GAS_OUTER_N + u32(d.x);
  let word = li >> 1u;
  let sh = 16u * (li & 1u);
  if (((atomicLoad(&gasOuter[word]) >> sh) & 0xFFFFu) >= FAR_PLUME_CEIL) { return; }
  atomicAdd(&gasOuter[word], min(amount, FAR_PLUME_ADD_MAX) << sh);
}

// ---- the LONG-RANGE box ----------------------------------------------------
// The same three functions as the fine box, against a different binding and a
// different cell size. Duplicated rather than parameterised because WGSL has no
// portable way to hand a kernel a pointer to one of two storage bindings, and
// because the ORIGIN RULE genuinely differs: this one is floored to the cell.
fn gasFarOuterOrigin() -> vec3<i32> {
  let off = vec3<i32>(GAS_FAROUT_OFF, GAS_FAROUT_OFF_Y, GAS_FAROUT_OFF);
  return (((T.origin * i32(CHUNK)) - off) >> GAS_FAROUT_SHIFTS)
         << GAS_FAROUT_SHIFTS;
}

fn gasFarOuterCell(c : vec3<i32>) -> vec3<i32> {
  return (c - gasFarOuterOrigin()) >> GAS_FAROUT_SHIFTS;
}

fn gasFarOuterAddCell(d : vec3<i32>, amount : u32) {
  let n = i32(GAS_FAROUT_N);
  if (d.x < 0 || d.y < 0 || d.z < 0 || d.x >= n || d.y >= n || d.z >= n) { return; }
  if (amount == 0u) { return; }
  let li = (u32(d.z) * GAS_FAROUT_N + u32(d.y)) * GAS_FAROUT_N + u32(d.x);
  let word = li >> 1u;
  let sh = 16u * (li & 1u);
  if (((atomicLoad(&gasFarOuter[word]) >> sh) & 0xFFFFu) >= FAR_PLUME_CEIL) { return; }
  atomicAdd(&gasFarOuter[word], min(amount, FAR_PLUME_ADD_MAX) << sh);
}

// Deposit one puff at a FRACTIONAL x/z cell offset, bilinearly across the four
// cells that straddle it. See PlumePuff's `fxz`: at 51.2 m cells a rounded
// puff makes the column a bar.
//
// Y IS EXACT AND STAYS EXACT, which is not a detail — it is what keeps the
// anti-carry proof above true word for word: a thread owns one cell of HEIGHT,
// so its four adds are four distinct cells and no other thread of the
// workgroup can reach them. Spreading in y would let two threads reach one
// cell and the bound would have to be re-derived.
//
// The clamp happens BEFORE the split: `a` is already <= FAR_PLUME_ADD_MAX, the
// four weights are in [0,1] and sum to 1, so every individual add is <=
// ADD_MAX (what the const_assert counts). Clamping after the split would have
// let a puff deposit up to 4x ADD_MAX and quietly invalidated the bound.
fn gasFarOuterAddBilinear(c0 : vec3<i32>, fxz : vec2f, y : i32, amount : u32) {
  let a = f32(min(amount, FAR_PLUME_ADD_MAX));
  if (a <= 0.0) { return; }
  let b = floor(fxz);
  let f = fxz - b;
  let bi = vec2<i32>(b);
  let g = 1.0 - f;
  // u32() truncates, so the four parts sum to slightly UNDER `a`. That is the
  // conservative direction for the bound.
  gasFarOuterAddCell(c0 + vec3<i32>(bi.x,      y, bi.y     ), u32(a * g.x * g.y));
  gasFarOuterAddCell(c0 + vec3<i32>(bi.x + 1,  y, bi.y     ), u32(a * f.x * g.y));
  gasFarOuterAddCell(c0 + vec3<i32>(bi.x,      y, bi.y + 1 ), u32(a * g.x * f.y));
  gasFarOuterAddCell(c0 + vec3<i32>(bi.x + 1,  y, bi.y + 1 ), u32(a * f.x * f.y));
}

// Next-tick dirty mark incl. boundary neighbours — the gas passes run after
// the CA, so a re-entry landing is next tick's business. Same rule and the
// same reason bit as sim_particle.wgsl's markDirtyNext: a gas particle
// rejoining the grid IS a particle landing.
fn gasMarkDirtyNext(c : vec3<i32>) {
  for (var k = 0u; k < 8u; k++) {
    let ns = dirtyFanSlot(c, T.origin, k);  // common.wgsl: own + bordered chunks
    if (ns != SLOT_NONE) { atomicOr(&dirtyOut[ns], DIRTY_R_PARTICLE); }
  }
}

// ---- the identity key ------------------------------------------------------
// Derived from the parcel's CELL and its payload, never from its buffer slot
// (rule 1, and the particle system's standing rule). Two parcels sharing a
// cell AND a payload therefore roll identically and move together, which is
// correct rather than merely tolerable: they are indistinguishable states, and
// making them diverge would need per-particle identity the 32-byte record does
// not carry. The payload's state nibble (worldgen's palette variant) gives a
// plume a few independent sub-populations for free.
fn gasKey(c : vec3<i32>, payload : u32) -> u32 {
  return hash3(GAS_KEY_SALT ^ payload, bitcast<u32>(c.x),
               bitcast<u32>(c.y) ^ pcg(bitcast<u32>(c.z)));
}

// ---- decay, from the material's own bucket (§2.7) --------------------------
// Walks reactOffset..+reactCount for RK_DECAY entries and rolls each once,
// first hit wins — the same shape as doReactions. Returns 0xFFFFFFFF for "no
// rule fired"; otherwise the product material id (0 = air = die).
//
// TWO KINDS OF RULE ARE SKIPPED, both because the information is not out here:
//   * neighbour-COUNT scaled rules (RSCALE_ON). Outside the window a parcel
//     has no neighbours to count, and scaledChance's answer would be a
//     fabrication. Skipping is the conservative direction: the rule simply
//     does not apply to gas that has left.
//   * nothing else. RCOND_SKY is SATISFIED by construction — a parcel above
//     the window has nothing over it — and the day/night gates read T.dayPhase
//     exactly as the CA does.
fn gasDecayProduct(m : Material, key : u32) -> u32 {
  let day = daylightStrength(T.dayPhase);
  for (var ri = 0u; ri < m.reactCount; ri++) {
    let rule = reactions[m.reactOffset + ri];
    if ((rule.packed & 3u) != RK_DECAY) { continue; }
    if ((rule.cond & RSCALE_ON) != 0u) { continue; }
    let cond = rule.cond & 0xFFu;
    if (cond != 0u) {
      if ((cond & RCOND_DAY) != 0u && day == 0u) { continue; }
      if ((cond & RCOND_NIGHT) != 0u && day != 0u) { continue; }
      if (day < ((rule.cond >> 8u) & 0xFFu)) { continue; }
    }
    let rr = hash3(key, ri, GAS_DECAY_SALT);
    if ((rr % REACT_CHANCE_DEN) < rule.chance) { return rule.prodSelf; }
  }
  return 0xFFFFFFFFu;
}

// ============================== entry points ================================

// pArgs-shaped: [4..6] indirect dispatch {groups, 1, 1}. Gas particles are not
// drawn as instances (they render through gasOuter, in the far march), so the
// draw-args words the ballistic pair carries are absent here.
@compute @workgroup_size(1)
fn gasArgs1() {
  // ZERO THE WRITE PAGE'S CURSOR, here and only here. This kernel is the one
  // point in the tick that runs after the last read of the write page's old
  // value (last tick's resolve) and before the first append into it (this
  // tick's integrate) — so the gas pool needs no per-tick buffer fill, and the
  // "who resets the count" question has one answer instead of a convention.
  atomicStore(&gasCounts[1u - T.page], 0u);
  let n = gasLive(T.page);
  gasArgs[4] = (n + 63u) / 64u;
  gasArgs[5] = 1u;
  gasArgs[6] = 1u;
}

@compute @workgroup_size(1)
fn gasArgs2() {
  let n = gasLive(1u - T.page);
  gasArgs[4] = (n + 63u) / 64u;
  gasArgs[5] = 1u;
  gasArgs[6] = 1u;
  atomicStore(&gasSpawn[GAS_SP_LIVE], n);
}

// Drains BOTH spawn streams into the READ page, before gasArgs1 sizes the
// integrate dispatch — so a parcel that left the grid this tick flies this
// tick, the same latency the ballistic `spawn` path has.
//
// One dispatch covers both lists: [0, GAS_SPAWN_CAP) is the CA's outbox and
// [GAS_SPAWN_CAP, +GAS_CPU_SPAWN_CAP) is the CPU's. Threads past either
// list's live count return immediately, so the fixed dispatch costs one load
// per idle thread and nothing else.
@compute @workgroup_size(64)
fn gasSpawnStep(@builtin(global_invocation_id) gid : vec3<u32>) {
  var p : Particle;
  if (gid.x < GAS_SPAWN_CAP) {
    if (gid.x >= min(atomicLoad(&gasSpawn[GAS_SP_COUNT]), GAS_SPAWN_CAP)) { return; }
    let b = GAS_SP_HDR + gid.x * GAS_SP_STRIDE;
    p.px = bitcast<i32>(atomicLoad(&gasSpawn[b + 0u]));
    p.py = bitcast<i32>(atomicLoad(&gasSpawn[b + 1u]));
    p.pz = bitcast<i32>(atomicLoad(&gasSpawn[b + 2u]));
    p.payload = atomicLoad(&gasSpawn[b + 6u]);
  } else {
    let i = gid.x - GAS_SPAWN_CAP;
    if (i >= min(gasSpawnOps[GAS_SP_COUNT], GAS_CPU_SPAWN_CAP)) { return; }
    let b = GAS_SP_HDR + i * GAS_SP_STRIDE;
    p.px = bitcast<i32>(gasSpawnOps[b + 0u]);
    p.py = bitcast<i32>(gasSpawnOps[b + 1u]);
    p.pz = bitcast<i32>(gasSpawnOps[b + 2u]);
    p.payload = gasSpawnOps[b + 6u];
  }
  // Velocity is ZERO for every gas particle, always: this population moves in
  // whole cells by the CA's ladder, and a nonzero velocity word would change
  // particlePriority without changing anything visible.
  p.vx = 0; p.vy = 0; p.vz = 0;
  // Liveness and pendingness are FORCED, exactly as sim_particle's `spawn`
  // forces them: a malformed op must not be able to inject a particle that is
  // already claiming a cell.
  p.flags = PFLAG_ALIVE | PFLAG_GAS;
  let slot = atomicAdd(&gasCounts[T.page], 1u);
  if (slot >= GAS_PARTICLE_CAP) {
    // At the cap the parcel vaporizes. Degradation, not failure — and it is
    // counted, so "the pool was the bound" is a printed number rather than an
    // inference from a plume that looks thin.
    atomicAdd(&gasSpawn[GAS_SP_POOLFULL], 1u);
    return;
  }
  gasRead[slot] = p;
}

@compute @workgroup_size(64)
fn gasIntegrate(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= gasLive(T.page)) { return; }
  var p = gasRead[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }

  let c = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);

  // ---- kill bounds (rule 2). All three are stateless tests on the parcel's
  // own position, so nothing has to remember anything about it.
  if (!gasInOuterBox(c) || c.y >= gasCeilY()) {
    atomicAdd(&gasSpawn[GAS_SP_DIED], 1u);
    return;                       // not appended: the slot is reclaimed
  }
  if (c.y >= T.origin.y * i32(CHUNK) + i32(WORLD_N)) {
    atomicAdd(&gasSpawn[GAS_SP_ABOVE], 1u);
  }

  let key = gasKey(c, p.payload);
  var mat = p.payload & 0xFFFu;
  var m = materials[mat];

  // ---- decay, before motion: a parcel that dies this tick does not move ----
  let prod = gasDecayProduct(m, key);
  if (prod != 0xFFFFFFFFu && prod != PROD_KEEP) {
    if (prod == 0u) {                       // -> air: gone
      atomicAdd(&gasSpawn[GAS_SP_DIED], 1u);
      return;
    }
    let pm = materials[prod];
    if (pm.klass == CLASS_GAS) {
      // Morph and keep going. The state nibble is re-derived from the key so
      // the new material gets its own palette variant rather than inheriting
      // an index that means something else in its table.
      mat = prod;
      m = pm;
      p.payload = prod | (((key >> 24u) % 3u) << 12u);
    } else {
      // A grid product (steam -> water). It can only land where the grid
      // exists: inside the window it proposes itself through the claim path
      // below; outside, it is rain on unloaded space, which is nothing today
      // either.
      if (!inBounds(c)) {
        atomicAdd(&gasSpawn[GAS_SP_DIED], 1u);
        return;
      }
      p.payload = prod | (((key >> 24u) % 3u) << 12u);
      p.flags |= PFLAG_PENDING;
      atomicMax(&gasClaim[claimSlot(cellIndexW(c))], particlePriority(p));
      gasAppend(p);
      return;
    }
  }

  // ---- motion: the grid model, verbatim (§2.2) -----------------------------
  let rnd = gasRndK(key, 2u, 0u, &T);
  let g = gasIntentK(c, m, key, rnd >> 10u, 0u, &T);
  var tgt = c;
  var found = false;
  for (var i = 0u; i < GAS_LADDER_RING; i++) {
    let s = gasLadderStep(g, 0u, 0u, i);
    if (s.w == 0) { continue; }
    let n = c + s.xyz;
    if (!gasBlocked(n)) { tgt = n; found = true; break; }
  }
  if (!found) {
    let rUp  = windLateralStartK(c, rnd >> 10u, m, key, 0u, &T);
    let rLat = windLateralStartK(c, rnd >> 14u, m, key, 0u, &T);
    for (var i = GAS_LADDER_RING; i < GAS_LADDER_N; i++) {
      let n = c + gasLadderStep(g, rUp, rLat, i).xyz;
      if (!gasBlocked(n)) { tgt = n; found = true; break; }
    }
  }
  // Nowhere to go: stay put and try again next tick. Bounded by the decay.

  p.px = tgt.x << 8u;
  p.py = tgt.y << 8u;
  p.pz = tgt.z << 8u;

  // ---- re-entry = RECONVERT (§2.6) ----------------------------------------
  // A parcel whose next cell is inside the window has nowhere to render (there
  // is no inner density volume in stage 1) and, more to the point, it has
  // rejoined a place where reactions happen. So it proposes itself as a voxel
  // through the SAME atomicMax claim path the ballistic particles use: one
  // claim per parcel per tick, deterministic winner, losers retry.
  if (inBounds(tgt)) {
    p.flags |= PFLAG_PENDING;
    atomicMax(&gasClaim[claimSlot(cellIndexW(tgt))], particlePriority(p));
  }
  gasAppend(p);
}

@compute @workgroup_size(64)
fn gasResolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= gasLive(1u - T.page)) { return; }
  var p = gasWrite[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }
  let cell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);

  if ((p.flags & PFLAG_PENDING) != 0u) {
    // TWO BASES, the same rule as sim_particle's resolve: the claim hashes on
    // the SLOT cell index (an identity), and only the store uses the physical
    // word index (an address).
    let tgtSlot = cellIndexW(cell);
    let won = atomicLoad(&gasClaim[claimSlot(tgtSlot)]) == particlePriority(p);
    if (won && inBounds(cell) && voxMat(voxWordAt(cell)) == MAT_AIR) {
      let mat = p.payload & 0xFFFu;
      var state = (p.payload >> 12u) & 0xFu;
      if (materials[mat].klass == CLASS_LIQUID) { state = LIQ_FULL_STATE; }
      // STAMP_NEVER: it has not acted as a grid voxel yet, so it may move on
      // the tick it lands.
      voxStore(voxWordIndex(cell), packVox(mat, state, STAMP_NEVER));
      gasMarkDirtyNext(cell);
      atomicAdd(&gasSpawn[GAS_SP_REENTER], 1u);
      p.flags = 0u;                       // dead: it is a voxel now
      gasWrite[gid.x] = p;
      return;
    }
    // Lost the claim, or the cell was taken between integrate and resolve.
    // Stay a parcel and retry next tick; it is not stuck, because next tick
    // the cell reads non-air and the ladder routes it somewhere else.
    p.flags = PFLAG_ALIVE | PFLAG_GAS;
    gasWrite[gid.x] = p;
  }

  // The splat, folded into resolve rather than given its own dispatch: it is
  // one atomic per live parcel and this is already the pass that walks every
  // live parcel exactly once. Only cells OUTSIDE the window splat — inside,
  // smoke is a voxel and renders as one, and counting it twice would put a
  // bright halo on the window boundary.
  if (!inBounds(cell)) { gasSplat(cell); }

  // The determinism digest. Every parcel that is still a parcel after this
  // pass contributes; one that became a voxel returned above and is covered by
  // the world hash instead, so the two never double-count a parcel and never
  // drop one.
  atomicAdd(&gasSpawn[GAS_SP_DIGEST], particlePriority(p));
}

// ======================= FAR FIRE PLUMES ====================================
//
// The smoke of a fire the residency window has LEFT BEHIND. Such a fire is
// frozen mid-burn: its embers were downsampled into the far cascade by the last
// `fardown` and stay there, visibly orange, for the rest of the session — while
// its smoke dies within about four seconds, because a smoke parcel is only ever
// born at the window face by the running CA and then decays at the material's
// authored rate. These two kernels put the smoke back.
//
// TWO KERNELS, TWO BOXES, ONE SHAPE. `gasFarPlume` draws the fires inside
// gasOuter's ±51.2 m; `gasFarPlumeWide` draws the ones past it, into the
// long-range box, out to ±409.6 m. The emitter lists are DISJOINT (the CPU
// splits them in the max norm at the fine box's own half-extent), so no fire is
// ever in both and "the crossfade cannot double-brighten" is a property of the
// data rather than of a blend weight the renderer has to get right.
//
// ONE WORKGROUP PER EMITTER, ONE THREAD PER CELL OF HEIGHT. The lists are short
// and CPU-built, so both dispatches are direct: nothing here needs a GPU-side
// count or an indirect args buffer.
//
// WHAT THEY MAY TOUCH: the two density boxes, and nothing else. Both are
// render-only derived data — the sim never reads them, the world hash never
// covers them, and they are rebuilt from scratch every tick. That is why these
// kernels may use f32 at all (CLAUDE.md rule 1's integer-only requirement is
// about the CA and about anything the hash can see) and why a frozen fire
// cannot move the world. They do NOT create parcels: the parcel pool is
// deterministic sim state pinned by GAS_SP_DIGEST, and a parcel that drifts
// back in writes a hashed voxel.
//
// THE ANIMATION IS STATELESS. There is no per-plume state anywhere: the
// billowing comes from hashing (emitter index, height packet), and the packet
// index slides downward with the tick, so a given lump of turbulence RISES
// through the column at FAR_PLUME_RISE_VOX voxels a tick instead of the column
// flickering in place — one physical speed in both boxes, which is the whole
// point of that constant being in voxels rather than in cells.

// One puff: which cell OFFSET from the emitter's own cell, and how much.
// Offsets rather than absolute cells so the one shape function serves both
// boxes — the caller adds its own `c0`, which is the only thing that differs.
struct PlumePuff {
  off : vec3<i32>,
  // The SAME x/z offset, unrounded, for the wide box's bilinear deposit: its
  // cell is 51.2 m against a plume radius under 5 m, so round() would send
  // every puff of every height step to one cell and the column would be a bar
  // with no taper, no per-height wander and no wind tilt until the wind was
  // strong enough to jump a whole cell.
  fxz : vec2f,
  amt : f32,
};

// THE SHAPE, shared by both scales. `cellVox` is the box's cell size in voxels,
// and it is the ONLY scale-bearing input: the radius and the height are carried
// in METRES and divided by it here, so the same fire is the same physical
// column in either box rather than eight times wider in the coarse one.
//
// `hMul` stretches the column for a bigger fire. It is 1 for the fine list
// (whose emitter is one 0.8 m column footprint and carries no information about
// how large the fire really is) and sqrt(fine columns) for the wide list, whose
// emitter aggregates up to 256 of them.
//
// THE COLUMN STANDS ON THE FIRE. The only lateral terms are the wind tilt (zero
// at s = 0) and the puff scatter (radius 0.32 m at s = 0, a twentieth of a
// cell), so the base of the plume is the emitter's own cell and nothing else.
// The footprint pass added a height-keyed snake here and it put the base a cell
// sideways at s = 0 (cos 0 = 1), which with the disc on top of it is what made
// the smoke look as though it came from chunks away. No snake.
// `cellXZ` / `cellY` are the box's cell sizes in voxels, and they are the ONLY
// scale-bearing inputs: the radius, the tilt and the rise are carried in
// physical units and divided by them here. They differ for the wide box (64
// across, 8 tall) and not for the fine one.
fn plumePuff(burn : f32, hMul : f32, s : u32, hCells : u32, idx : u32,
             k : u32, cellXZ : f32, cellY : f32) -> PlumePuff {
  var o : PlumePuff;
  let t = f32(s) / f32(hCells);          // 0 at the fire, 1 at the top
  // Wind tilt: lateral cells per vertical cell is the wind speed over the rise
  // speed, both in voxels/s — a RATIO, so it is the same number in either box
  // and needs no cellVox. windDirQ is the unit downwind XZ the CA and the
  // renderer already share, so a far plume leans the way the near grass does.
  var tilt = vec2f(0.0, 0.0);
  if (T.windMode != WIND_MODE_OFF) {
    let dir = vec2f(f32(T.windDirQ.x), f32(T.windDirQ.y)) / 65536.0;
    let spd = f32(T.windSpeedQ) / 65536.0;          // voxels/s
    // ...times this height in VOXELS, over the x/z cell: the ratio is voxels
    // per voxel, and the two cell sizes need not agree.
    tilt = dir * (spd / FAR_PLUME_RISE_VPS) * (f32(s) * cellY / cellXZ);
  }
  // The column widens and dilutes with height, which is the whole reason it
  // reads as smoke and not as a bar.
  let radM = (FAR_PLUME_R0_M + FAR_PLUME_SPREAD_M * t) * hMul;
  let rad = radM / (VOXEL_METERS * cellXZ);
  // Density taper. Not to zero at the top: a plume that vanishes at a hard
  // height reads as a cut-off, and the renderer's fog is what should finish it.
  // 0.4 leaves the top at 60%, so the upper column — most of what you see of a
  // distant fire — is a body of smoke rather than a taper to nothing.
  let taper = 1.0 - 0.4 * t;

  // ---- the packet, which is what MOVES -------------------------------------
  // A lump of turbulence born at the fire has risen FAR_PLUME_RISE_VOX voxels a
  // tick since — (FAR_PLUME_RISE_VOX / cellY) of THIS box's height cells, which
  // is what keeps the two boxes at one physical speed. So the lump now at
  // height s was born (s - risen) cells ago, and hashing on that difference
  // makes the pattern travel up the column at exactly the speed a parcel would,
  // with no state.
  //
  // CONTINUOUS, NOT AN INTEGER (2026-09-19). `risen` used to be truncated, so
  // every puff held its cell for eight ticks and then every puff in the column
  // re-rolled at once — smoke that stepped rather than rose, the "choppy
  // framerate" report. Now the lump index is fractional: the lump just below
  // this height (pA, at s - f) and the one just above (pA + 1) are both rolled
  // and their offsets and billow are MIXED by f. As the column rises f runs
  // 1 -> 0, pA steps down and f wraps to 1 — and at the wrap both sides of
  // the mix evaluate to the same lump, so nothing jumps. Two hashes a puff.
  // The bias keeps the u32 conversion away from a wrap at low tick counts.
  let risenF = f32(T.tick) * (FAR_PLUME_RISE_VOX / cellY);
  let packetF = f32(s) - risenF;
  let pA = floor(packetF);
  let f = packetF - pA;
  let salt = GAS_PLUME_SALT ^ (idx * 2654435761u);
  let hA = hash3(salt, u32(i32(pA) + 0x40000), k);
  let hB = hash3(salt, u32(i32(pA) + 0x40001), k);
  // Two signed unit-ish offsets and one density roll out of each hash.
  let oA = vec2f(f32(hA & 0xFFu), f32((hA >> 8u) & 0xFFu)) / 127.5 - 1.0;
  let oB = vec2f(f32(hB & 0xFFu), f32((hB >> 8u) & 0xFFu)) / 127.5 - 1.0;
  let oxz = mix(oA, oB, f) * rad;
  // 0.35..1.0 — the billow. Without it every puff is the same brightness and
  // the column is a smooth cone.
  let bill = 0.35 + 0.65 * mix(f32((hA >> 16u) & 0xFFu),
                               f32((hB >> 16u) & 0xFFu), f) / 255.0;
  let ox = oxz.x;
  let oz = oxz.y;
  o.amt = TUNE_FAR_PLUME_STRENGTH * FAR_PLUME_DENSITY * burn * taper * bill;
  o.fxz = vec2f(tilt.x + ox, tilt.y + oz);
  o.off = vec3<i32>(i32(round(o.fxz.x)), i32(s), i32(round(o.fxz.y)));
  return o;
}

// How many cells of height this plume gets. `hMul` scales it, and the min
// against FAR_PLUME_STEPS is the belt to the tuning clamp's brace: a knob
// edited past the clamp costs a shorter plume, never an out-of-bounds thread.
fn plumeHeightCells(hMul : f32, cellY : f32) -> u32 {
  let m = TUNE_FAR_PLUME_HEIGHT * hMul;
  return min(u32(max(m / (VOXEL_METERS * cellY), 1.0)), FAR_PLUME_STEPS);
}

@compute @workgroup_size(64)
fn gasFarPlume(@builtin(workgroup_id) wg : vec3<u32>,
               @builtin(local_invocation_id) li : vec3<u32>) {
  let n = min(gasFarEmit[0], GAS_FAR_EMIT_MAX);
  if (wg.x >= n) { return; }

  let cellVox = f32(1u << GAS_OUTER_SHIFT);
  let hCells = plumeHeightCells(1.0, cellVox);
  let s = li.x;
  if (s >= hCells) { return; }

  let b = GAS_FAR_EMIT_HDR + wg.x * GAS_FAR_EMIT_STRIDE;
  let base = vec3<i32>(bitcast<i32>(gasFarEmit[b + 0u]),
                       bitcast<i32>(gasFarEmit[b + 1u]),
                       bitcast<i32>(gasFarEmit[b + 2u]));
  // 0..1: how much of this column footprint is actually on fire, times the
  // CROSSFADE WEIGHT in the word's top byte (world.h kGasFarBlendVox): an
  // emitter in the shell inside the fine box's face fades out here at full
  // size while its wide twin fades in.
  let sw = gasFarEmit[b + 3u];
  let burn = clamp(f32(sw & GAS_FAR_STRENGTH_MASK) / f32(GAS_FAR_STRENGTH_MAX),
                   0.0, 1.0) * (f32(sw >> GAS_FAR_WEIGHT_SHIFT) / 255.0);
  // The emitter's own cell, from the SAME expression the CPU filtered the list
  // with and the renderer samples the box with (gasOuterCell above).
  let c0 = gasOuterCell(base);

  // THE MERGE. Every (cell, amount) a puff wants to deposit goes through one
  // list that sums same-cell entries, and each cell is added ONCE — so an
  // emitter reaches a cell at most once per height, which is the fact the
  // anti-carry proof at FAR_PLUME_ADD_MAX rests on. The sum is clamped by
  // gasOuterAddCell's own min() against ADD_MAX, which is what keeps a stacked
  // base cell at the old three-puff density rather than five times it.
  //
  // BILINEAR, like the wide box, since the packet went continuous: a puff whose
  // position glides has to be able to glide between cells, and a rounded
  // deposit would hop a whole 6.4 m cell at a time. Four entries per puff.
  var seen : array<vec2<i32>, 4u * FAR_PLUME_PUFFS>;
  var amt : array<f32, 4u * FAR_PLUME_PUFFS>;
  var nSeen = 0u;
  for (var k = 0u; k < FAR_PLUME_PUFFS; k++) {
    let sp = plumePuff(burn, 1.0, s, hCells, wg.x, k, cellVox, cellVox);
    let b = floor(sp.fxz);
    let fr = sp.fxz - b;
    let bi = vec2<i32>(b);
    for (var q = 0u; q < 4u; q++) {
      let cell = bi + vec2<i32>(i32(q & 1u), i32(q >> 1u));
      let w = select(1.0 - fr.x, fr.x, (q & 1u) == 1u) *
              select(1.0 - fr.y, fr.y, (q >> 1u) == 1u);
      let a = sp.amt * w;
      var merged = false;
      for (var m = 0u; m < nSeen; m++) {
        if (all(seen[m] == cell)) { amt[m] += a; merged = true; break; }
      }
      if (!merged) { seen[nSeen] = cell; amt[nSeen] = a; nSeen++; }
    }
  }
  for (var m = 0u; m < nSeen; m++) {
    gasOuterAddCell(c0 + vec3<i32>(seen[m].x, i32(s), seen[m].y),
                    u32(max(amt[m], 0.0)));
  }
}

// The same plume, one LOD out. Everything that differs is in the first eight
// lines: the list section, the box mapping, the cell size and the height
// multiplier. The shape, the wind tilt, the billow and the anti-carry guard are
// the same code, which is what keeps the two boxes from drifting into two
// different-looking plume systems.
@compute @workgroup_size(64)
fn gasFarPlumeWide(@builtin(workgroup_id) wg : vec3<u32>,
                   @builtin(local_invocation_id) li : vec3<u32>) {
  let n = min(gasFarEmit[2], GAS_FAR_WIDE_MAX);
  if (wg.x >= n) { return; }

  let b = GAS_FAR_WIDE_BASE + wg.x * GAS_FAR_EMIT_STRIDE;
  let base = vec3<i32>(bitcast<i32>(gasFarEmit[b + 0u]),
                       bitcast<i32>(gasFarEmit[b + 1u]),
                       bitcast<i32>(gasFarEmit[b + 2u]));
  // A WIDE emitter aggregates every fine column in its 6.4 m footprint, so its
  // strength is the SUM and can be far more than one full column. Two different
  // things are read out of it, which is the whole reason the wide list is worth
  // aggregating rather than just truncating:
  //   cols  how many full fine columns it stands for, which scales the MASS
  //         (see `stack` below) -- the fine box stacks those columns' counts
  //         in its own cells, and the wide box has to stack them too or the
  //         handover drops to a fraction of the fine box's opacity.
  //   hMul  sqrt(columns), capped. One burning tree is a wisp; a burning
  //         hillside is a column four times as tall and proportionally wide.
  let sw = gasFarEmit[b + 3u];
  let cols = f32(sw & GAS_FAR_STRENGTH_MASK) / f32(GAS_FAR_STRENGTH_MAX);
  // The crossfade weight scales the MASS only (below): the height and width
  // come from the unweighted column count, so a plume fading in across the
  // shell fades in at its full size rather than growing into it.
  let blend = f32(sw >> GAS_FAR_WEIGHT_SHIFT) / 255.0;
  let hMul = clamp(sqrt(max(cols, 0.0)), FAR_PLUME_WIDE_HMIN,
                   FAR_PLUME_WIDE_HMAX);

  let cellXZ = f32(1u << GAS_FAROUT_SHIFT);
  let cellY = f32(1u << GAS_FAROUT_SHIFT_Y);
  let hCells = plumeHeightCells(hMul, cellY);
  let s = li.x;
  if (s >= hCells) { return; }

  let c0 = gasFarOuterCell(base);
  // ONE bilinear puff per height step, not five: four cells that are distinct
  // by construction, which is this box's half of the one-add-per-cell rule the
  // anti-carry proof rests on. Its mass is the fine box's five puffs' worth of
  // LINE INTEGRAL — five over the chord, times the fine cells a ray crosses —
  // so the coarse box shows the fine box's column at the fine box's opacity
  // and the renderer folds both into one accumulator with no scale at all.
  //
  // AND IT STACKS. The fine box draws a burning chunk as up to four emitters
  // whose puffs land in the same or adjacent cells and ADD; a hillside is
  // dozens of fine columns per 51.2 m cell, all of them summing there. The
  // wide emitter is the aggregate of those columns, so its mass is one
  // column's times how many it stands for -- `cols`, not `burn` (which
  // saturates at one and was measured 3-4x too faint against the fine box on
  // the gas-farplume2 fixture). Capped at the 8x8 fine columns a wide cell
  // holds; the per-add clamp in gasFarOuterAddCell saturates a real hillside
  // at 47% smoke by volume long before that, which is opaque.
  let sp = plumePuff(1.0, hMul, s, hCells, wg.x + 0x9E37u, 0u, cellXZ, cellY);
  let stack = clamp(cols, 0.0, FAR_PLUME_WIDE_CHORD * FAR_PLUME_WIDE_CHORD) * blend;
  let m = sp.amt * stack * (FAR_PLUME_WIDE_FILL / FAR_PLUME_WIDE_CHORD);
  gasFarOuterAddBilinear(c0, sp.fxz, i32(s), u32(max(m, 0.0)));
}
