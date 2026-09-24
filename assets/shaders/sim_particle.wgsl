// sim_particle.wgsl — ballistic voxels-in-flight (DESIGN.md §5).
// Runs after the CA color passes each tick, double-buffered:
//   args1    -> integrate (read page, indirect) -> args2 -> resolve (write page)
// integrate: gravity + fixed-point flight, sampling the grid every <=half
// voxel; a particle that would enter a blocking cell proposes a reinsertion at
// the last empty cell by atomicMax-ing a state-derived priority into a claim
// hash. resolve: claim winners write themselves back into the grid; losers
// (two particles wanting one cell, or a hash collision) rest one tick and
// retry. Everything is integer and keyed on particle state + tick, never on
// buffer slot order, so the grid stays bit-deterministic (DESIGN.md §2/§4).

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(15) var<storage, read_write> supportOut : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
// This module's page-fault identity (common.wgsl's PT_K_* block). Every
// shader that declares `read_write> voxels` must define this: gPtKernel's
// initializer references it, so omitting it is a compile error rather than
// a fault that reports as "unknown".
const PT_KERNEL : u32 = PT_K_PARTICLE;

@group(1) @binding(0) var<storage, read_write> pRead  : array<Particle>;
@group(1) @binding(1) var<storage, read_write> pWrite : array<Particle>;
@group(1) @binding(2) var<storage, read_write> counts : array<atomic<u32>>;
@group(1) @binding(3) var<storage, read_write> claim  : array<atomic<u32>>;
@group(1) @binding(4) var<storage, read_write> pArgs  : array<u32>;
@group(1) @binding(7) var<storage, read>       spawnOps : array<Particle>;

fn inBounds(c : vec3<i32>) -> bool { return cellResident(c, T.origin); }

// ---- wind (docs/RESEARCH_wind.md §4.6, phase 3) -----------------------------
// THE FIELD SCALE, shared by both fields this kernel reads. windAtQ and
// currentAtQ both speak Q16.16 world cells per SECOND; particles speak Q24.8
// cells per TICK. 65536/256 = 256 fixed-point units and 30 ticks a second, so
// the divisor is 256*30. A divisor rather than a multiply-shift because it is
// exact at both ends and this runs once per particle per tick, not once per
// cell. One constant for both because they are the same transcription — a
// second one named for water would be the same number with a second place to
// keep it in step.
const PART_FIELD_SCALE : i32 = 7680;
// Fraction of the gap between a particle's velocity and the local wind that
// closes in ONE TICK at a material's full windResponse of 15, in Q16. Human
// units in, integer out, at shader compile time — the sim_fluid.wgsl discipline
// (IEEE-exact const folding, so the kernel stays integer and deterministic).
const PART_WIND_DRAG : i32 =
    i32(round(clamp(TUNE_WIND_DRAG, 0.0, 30.0) * 65536.0 / 30.0));

// ---- liquids (materials.json "fluid", kFluidPack* in src/sim/materials.h) ---
// A voxel in flight used to stop dead at a water surface — "splash = plop onto
// the surface" — and that single line is what built rafts of exploded tree in
// mid-air over a pond: the first chip landed on the film, the next was blocked
// by the first, and the CA has no rule that moves a solid touching another
// solid, so the tower stood there forever.
//
// A liquid is now something a particle flies THROUGH, with two forces acting on
// it while it is in there, and the shape of both comes out of `density` rather
// than out of a knob: an upward push of g * rhoFluid / rhoSelf (gravity is
// already subtracted, so the net is g * (rhoFluid - rhoSelf) / rhoSelf, which is
// downward for stone and upward for wood with nothing to author either way), and
// a per-tick viscous damping. Damping is what makes the water read as water and
// not as thin air, and it is also the thing that ENDS the motion: a buoyant chip
// rises, overshoots into the air, falls back, and each cycle is smaller, so it
// converges to a bob at the waterline instead of oscillating forever.
//
// `lift == 0` is the opt-out and keeps every older behaviour exactly: micro
// spray (a droplet dying on a surface is right — it has no voxel to sink) and
// liquid/gas ejecta (a water voxel landing in water is a MERGE, whose fullness
// has to go somewhere, and that is a different rule than this one).
const PART_BUOY_MAX      : i32 = TUNE_PART_BUOY_MAX;
const PART_SETTLE_SPEED  : i32 = TUNE_PART_SETTLE_SPEED;
const PART_FLOAT_PATIENCE : u32 = TUNE_PART_FLOAT_PATIENCE;

// FLOAT PATIENCE lives in the micro LIFE bits. They are spare here by
// construction: `isMicro` and the float path are mutually exclusive (a micro
// particle never has lift, see above), so the two never read the same bits in
// the same particle. Same trick as the micro fields themselves — spare room in
// `flags`, no growth of the 32-byte Particle.
// ---- CALM: poured, not thrown (game/container.h) --------------------------
// A stream out of a flask is a rope of water, not spray, and the wind does not
// carry it off. Measured before this: a flask poured from ten cells up put
// every drop outside a 25x25 catch tray and some of them still aloft 80 ticks
// later, and the owner saw the stream "fly off at a weird angle". The bit
// skips the wind drag and nothing else -- gravity, landing, buoyancy and the
// claim are exactly an ordinary particle's. Bit 14: 0..12 are ALIVE / PENDING
// / MICRO + the micro fields and 13 is PFLAG_GAS (common.wgsl). Declared here,
// beside its only reader; world.h kPFlagCalm must agree (check_invariants).
const PFLAG_CALM : u32 = 16384u;
// ---- DRIP: off a wet body, lands without a mark (MobSystem::WetOneLimb) ----
// A micro droplet that dies on contact like any other and skips the stain it
// would have deposited: water's world stain never dries, so a creature
// dripping as it walks would otherwise leave a permanent wet trail. Bit 15,
// the next free one after CALM. world.h kPFlagDrip must agree.
const PFLAG_DRIP : u32 = 32768u;

fn floatTicksOf(flags : u32) -> u32 {
  return (flags >> PMICRO_LIFE_SHIFT) & PMICRO_LIFE_MASK;
}
fn withFloatTicks(flags : u32, t : u32) -> u32 {
  return (flags & ~(PMICRO_LIFE_MASK << PMICRO_LIFE_SHIFT)) |
         ((min(t, PMICRO_LIFE_MASK)) << PMICRO_LIFE_SHIFT);
}

// Blocks flight: solids and powders always; liquids only for a particle that
// does not interact with them (lift 0 — see above).
fn blocksParticle(c : vec3<i32>, passLiquid : bool) -> bool {
  let w = voxWordAt(c);
  let mat = voxMat(w);
  if (mat == MAT_AIR) { return false; }
  let k = materials[mat].klass;
  if (k == CLASS_GAS) { return false; }
  if (passLiquid && k == CLASS_LIQUID) { return false; }
  return true;
}

// May a particle rejoin the grid in the cell holding word `w`?
//
// Air always. A LIQUID only if the particle is denser than it — which is the
// same statement the brush makes when it paints a stone into a pond (a cell
// written over a liquid keeps no memory of the liquid, sim_mutate.wgsl), and
// the reason the rule has to exist at all is that a sinking rock comes to rest
// at the BED, where the cell it stops in is water, not air. Without it a rock
// that made it to the bottom of a pond would propose a cell it could never be
// given and retry forever.
//
// The density test is what stops the opposite abuse: a plank must not "settle"
// by overwriting the water it is floating on, which would look exactly like
// sinking. `patient` is the escape hatch at the end of PART_FLOAT_PATIENCE —
// see the settle block in integrate.
fn canOccupy(w : u32, myDensity : i32, patient : bool) -> bool {
  let m = voxMat(w);
  if (m == MAT_AIR) { return true; }
  let mm = materials[m];
  if (mm.klass != CLASS_LIQUID) { return false; }
  return patient || myDensity > mm.density;
}

// Is there something under this cell worth coming to rest ON?
//
// Solid or powder is ordinary ground. A LIQUID counts only when the particle
// floats in it, and that is the whole definition of a waterline: the cell above
// the topmost liquid is where a floater belongs. A liquid it would SINK through
// is not support, so a rock passing the surface keeps going, and neutrally
// buoyant matter mid-column never converts to a voxel hanging in the water —
// which would be the old bug wearing new clothes.
// Sideways drift for something already floating (materials.json "fluid":
// {"wander": n}, in 1/256 voxel per tick). A leaf skitters, a log barely moves.
//
// Keyed on the particle's POSITION and the tick, never on its ring slot — the
// rule every other random decision in this file follows (DESIGN.md §4). Two
// particles cannot occupy one position, so position is a usable identity for a
// population that has no stable id of its own.
fn fluidWander(p : ptr<function, Particle>, m : Material) {
  let wand = i32(matFluidWander(m));
  if (wand == 0) { return; }
  let h = hash3(T.seed ^ 0xF10A7u, T.tick,
                u32((*p).px) ^ pcg(u32((*p).pz) ^ pcg(u32((*p).py))));
  (*p).vx += ((i32(h & 0xFFu) - 128) * wand) / 128;
  (*p).vz += ((i32((h >> 8u) & 0xFFu) - 128) * wand) / 128;
}

fn settleSupported(c : vec3<i32>, myDensity : i32) -> bool {
  let b = c + vec3<i32>(0, -1, 0);
  if (!inBounds(b)) { return false; }
  let bm = voxMat(voxWordAt(b));
  if (bm == MAT_AIR) { return false; }
  let m = materials[bm];
  if (m.klass == CLASS_SOLID || m.klass == CLASS_POWDER) { return true; }
  if (m.klass == CLASS_LIQUID) { return m.density > myDensity; }
  return false;
}

// Is the cell under this one a liquid — i.e. is a particle sitting HERE riding
// a water surface rather than flying through open air?
//
// This exists because A FLOATER AT THE WATERLINE IS NOT SUBMERGED. The cell it
// bobs in is the AIR above the topmost water cell, which is the whole point of
// `settleSupported`'s liquid arm, and it meant every fluid force in `integrate`
// — all of which tested the cell the particle is IN — switched off at exactly
// the moment the particle started floating. What was left acting on a chip of
// exploded tree sitting on a pond was the wind, at wood's derived response of 8
// and a leaf's of 15, with nothing pulling the other way. That is the owner's
// report: rafts of debris skidding across open water.
//
// It costs one extra voxWordAt per tick for a particle that has lift and is not
// already submerged, and there is no cheaper honest test: "am I on the water" is
// a question about a cell this kernel otherwise never reads. Gating it on the
// particle being SLOW would have been free and wrong — a chip the wind has
// already got hold of is not slow, which is the case that has to be caught.
fn liquidBelow(c : vec3<i32>) -> bool {
  let b = c + vec3<i32>(0, -1, 0);
  if (!inBounds(b)) { return false; }
  let bm = voxMat(voxWordAt(b));
  return bm != MAT_AIR && materials[bm].klass == CLASS_LIQUID;
}

// Next-tick dirty mark incl. boundary neighbors (particles run post-CA, so
// their writes are next tick's business).
fn markDirtyNext(c : vec3<i32>) {
  let lo = c & vec3<i32>(CHUNK_MASK);
  let ch = worldChunkOf(c);
  var xs = array<i32, 2>(0, 0);
  var ys = array<i32, 2>(0, 0);
  var zs = array<i32, 2>(0, 0);
  if (lo.x == 0) { xs[1] = -1; } else if (lo.x == CHUNK_MASK) { xs[1] = 1; }
  if (lo.y == 0) { ys[1] = -1; } else if (lo.y == CHUNK_MASK) { ys[1] = 1; }
  if (lo.z == 0) { zs[1] = -1; } else if (lo.z == CHUNK_MASK) { zs[1] = 1; }
  for (var i = 0; i < 2; i++) {
    for (var j = 0; j < 2; j++) {
      for (var k = 0; k < 2; k++) {
        let n = ch + vec3<i32>(xs[i], ys[j], zs[k]);
        let ns = chunkSlotOf(n, T.origin);
        if (ns != SLOT_NONE) {
          atomicOr(&dirtyOut[ns], DIRTY_R_PARTICLE);
        }
      }
    }
  }
}

// ---- debris that lands with nothing under it --------------------------------
// A particle backed off from a blocked flight comes to rest in the last cell it
// could be in, and when the thing that blocked it was a WALL rather than the
// ground, that cell has air underneath. One such voxel is a one-voxel island and
// the CA drops it for free (`soloSolid`, sim_step.wgsl). TWO of them side by
// side are not: a solid touching another solid never moves in the CA, so they
// hang on the wall until something else disturbs them, and nothing ever flags
// them because they did not TAKE support from anything — they arrived without
// any. That is the one hole in flagSupportLoss's coverage, and it is on the
// landing side rather than the vacating side.
//
// So: air below, and a solid on some other face (the case the cheap CA rule
// cannot decide), raises the same flag a collapse does and island detection
// judges it. Deliberately NOT raised when the cell below is LIQUID — a raft
// floating at the waterline IS supported, and flagging it would hand it to
// island detection, which counts liquid as empty, converts the raft to a
// rigidbody, settles it back to the grid, and flags it again forever.
fn flagLandedUnsupported(c : vec3<i32>, mat : u32) {
  if (materials[mat].klass != CLASS_SOLID) { return; }
  let b = c + vec3<i32>(0, -1, 0);
  if (!inBounds(b)) { return; }
  if (voxMat(voxWordAt(b)) != MAT_AIR) { return; }  // ground, powder or water
  var off = array<vec3<i32>, 5>(
      vec3<i32>(1, 0, 0), vec3<i32>(-1, 0, 0), vec3<i32>(0, 0, 1),
      vec3<i32>(0, 0, -1), vec3<i32>(0, 1, 0));
  for (var i = 0u; i < 5u; i = i + 1u) {
    let n = c + off[i];
    if (!inBounds(n)) { continue; }
    let nm = voxMat(voxWordAt(n));
    if (nm != MAT_AIR && materials[nm].klass == CLASS_SOLID) {
      atomicStore(&supportOut[chunkIndexW(c)], 1u);
      return;
    }
  }
}

fn liveCount(page : u32) -> u32 {
  return min(atomicLoad(&counts[page]), PARTICLE_CAP);
}

// pArgs layout: [0..3] indirect draw {36, instances, 0, 0},
//               [4..6] indirect dispatch {groups, 1, 1}
@compute @workgroup_size(1)
fn args1() {
  let n = liveCount(T.page);
  pArgs[4] = (n + 63u) / 64u;
  pArgs[5] = 1u;
  pArgs[6] = 1u;
}

@compute @workgroup_size(1)
fn args2() {
  let n = liveCount(1u - T.page);
  pArgs[0] = 36u;
  pArgs[1] = n;
  pArgs[2] = 0u;
  pArgs[3] = 0u;
  pArgs[4] = (n + 63u) / 64u;
  pArgs[5] = 1u;
  pArgs[6] = 1u;
}

fn append(p : Particle) {
  let slot = atomicAdd(&counts[1u - T.page], 1u);
  if (slot < PARTICLE_CAP) { pWrite[slot] = p; }
}

// CPU-authored spawns (debris shatter: body fragments re-entering the world
// as voxels-in-flight). Appended to the READ page before args1/integrate —
// same page explosion ejecta uses — so they fly this very tick. The op data
// is part of the tick's input stream, so replays capture it for free.
@compute @workgroup_size(64)
fn spawn(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= T.spawnCount) { return; }
  let slot = atomicAdd(&counts[T.page], 1u);
  if (slot >= PARTICLE_CAP) { return; }  // ring full: fragment just vaporizes
  var p = spawnOps[gid.x];
  // Keep the CPU's micro bits (PFLAG_MICRO, scale, life) and force only the
  // liveness/pending state, so a malformed op cannot inject a particle that is
  // already claiming a cell. Masking to the fields the CPU is allowed to
  // author is what keeps this an input stream rather than raw state injection.
  //
  // The LIFE field is admitted only for a micro particle. Bits 5..12 mean two
  // things now — a droplet's remaining life, and a floater's patience — and
  // they are safe to share only because `isMicro` separates the two
  // populations completely. That argument holds inside the shader by
  // construction; here it is a CPU value arriving, so it is enforced rather
  // than assumed, and a whole-voxel spawn starts its patience at zero whatever
  // the producer put in the word.
  var keep = p.flags & (PFLAG_MICRO | PFLAG_CALM | PFLAG_DRIP |
                        (PMICRO_SCALE_MASK << PMICRO_SCALE_SHIFT));
  if ((p.flags & PFLAG_MICRO) != 0u) {
    keep |= p.flags & (PMICRO_LIFE_MASK << PMICRO_LIFE_SHIFT);
  }
  p.flags = PFLAG_ALIVE | keep;
  pRead[slot] = p;
}

@compute @workgroup_size(64)
fn integrate(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveCount(T.page)) { return; }
  var p = pRead[gid.x];
  if ((p.flags & PFLAG_ALIVE) == 0u) { return; }

  let startCell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
  if (!inBounds(startCell)) { return; }  // fell out of the world: gone

  // This particle's own material, read once: every fluid decision below is a
  // property of the stuff in flight, and re-indexing the table per test is the
  // kind of thing that turns one branch into five loads.
  let myMat = p.payload & 0xFFFu;
  let myDensity = materials[myMat].density;
  let myLift = matFluidLift(materials[myMat]);
  // Micro spray is excluded by construction rather than by authoring — see the
  // note on blocksParticle. Everything below is gated on this one bool.
  let inFluid = !isMicro(p) && myLift > 0u;

  // ---- micro particles: age out ----
  // Spray is an effect with a finite budget, not conserved matter. Expiring in
  // mid-air is the common exit for a droplet that never hits anything, and it
  // is what guarantees a fight settles back to zero live particles (rule 2).
  if (isMicro(p)) {
    let life = microLifeOf(p.flags);
    if (life == 0u) { return; }  // dead: not appended, slot reclaimed
    p.flags = withMicroLife(p.flags, life - 1u);
  }

  // The cell the particle is standing in, read ONCE: the burial test below and
  // the buoyancy term after it both want it, and a second voxWordAt here is a
  // second page-table translation per particle per tick.
  let startWord = voxWordAt(startCell);
  let startMat = voxMat(startWord);
  let startKlass = materials[startMat].klass;  // air is a gas in the table

  // buried (CA moved material onto us): rise one voxel per tick until free.
  // Water flowing over a sinking rock is not burial, which is why a liquid
  // counts as blocking only for something that does not interact with liquids.
  if (startMat != MAT_AIR && startKlass != CLASS_GAS &&
      !(inFluid && startKlass == CLASS_LIQUID)) {
    // A micro particle has no voxel to dig out to. Being buried means the CA
    // flowed over it, so it is inside something now — stain that something and
    // be gone, rather than tunnelling upward through solid rock.
    if (isMicro(p)) {
      p.flags |= PFLAG_PENDING;
      atomicMax(&claim[claimSlot(cellIndexW(startCell))], microStainPriority(p));
      append(p);
      return;
    }
    p.py += PART_ONE;
    p.vx = 0; p.vy = 0; p.vz = 0;
    append(p);
    return;
  }

  // gravity + clamp
  p.vy -= PART_GRAVITY;

  // ---- submerged: Archimedes, then viscosity ------------------------------
  // Evaluated on the cell the particle STARTED the tick in, once, for the same
  // reason gravity is: the substep loop below samples the path for BLOCKAGE,
  // and a force re-evaluated per sample would make the acceleration a function
  // of how fast the particle happened to be going.
  let submerged = inFluid && startMat != MAT_AIR && startKlass == CLASS_LIQUID;
  if (submerged) {
      // g * rhoFluid / rhoSelf, capped. The cap is not cosmetic: a leaf
      // (density 200) in water is a five-gravity rocket and would be fired out
      // of the pond, and a future material at density 20 would overflow the
      // velocity clamp in one tick. Scaled by lift last so that the ceiling is
      // on the FORCE, not on the authored fraction of it.
    let ds = max(myDensity, 1);
    let buoy = min(PART_GRAVITY * materials[startMat].density / ds, PART_BUOY_MAX);
    p.vy += buoy * i32(myLift) / 15;
  }

  // Riding the surface: not IN the liquid, but touching it. See liquidBelow.
  // Buoyancy above deliberately stays on `submerged` — Archimedes is a function
  // of the fluid a voxel has actually displaced, and a floater at the waterline
  // is on the air limb of its own bob. What `afloat` buys is the two statements
  // below: the water still has hold of it, and the air does not.
  let afloat = inFluid && !submerged && liquidBelow(startCell);
  let wet = submerged || afloat;

  // ---- the water drags it toward wherever the water is GOING ---------------
  // Viscous damping, k/16 of the gap between the particle and the local current
  // per tick. "Toward a standstill" was the same statement written for still
  // water only, and re-aiming it at the field is the whole of this change:
  // `currentAtQ` returns the zero vector whenever `sim.currentMode` is off, so
  // the shipping default is an EXACT identity with the pure damping it replaces
  // and the day the field is switched on a floating log rides it with nothing
  // further to write. That identity is why this is one term rather than a
  // damping plus a separate current push, which is the shape sim_fluid.wgsl
  // uses: a node there has no damping of its own to fold into. Two drags would
  // also be wrong on their own terms — one aimed at zero and one aimed at `u`
  // settle a chip somewhere between the two, and a leaf on a river would trail
  // the water forever.
  //
  // The coefficient is the material's own `fluid.drag` and there is no second
  // knob. `fluid.drag` already answers "how hard does a liquid grab this
  // material"; "how hard does a MOVING liquid grab it" is the same number.
  //
  // Integer division truncates toward zero, which is symmetric — an arithmetic
  // shift would bias every negative component and read as a permanent downward
  // drift (the same trap the wind drag note below describes).
  if (wet) {
    let k = i32(matFluidDrag(materials[myMat]));
    // Gated on the mode, not on the returned zero, and the reason is the one
    // sim_fluid.wgsl states: this is a drag, so reading a zero field is not
    // free of consequence in general. Here it happens to be — the identity
    // above — and the gate is kept anyway, because it also skips the primitive
    // loop entirely in the world that has no current in it, which is every
    // world today.
    var u = vec3<i32>(0, 0, 0);
    if (T.currentMode != 0u) { u = currentAtQ(startCell, &T) / PART_FIELD_SCALE; }
    p.vx -= (p.vx - u.x) * k / 16;
    p.vz -= (p.vz - u.z) * k / 16;
    // VERTICALLY only when the particle is actually inside the liquid. A
    // floater's vertical motion is the bob — gravity against the buoyancy it
    // was given on the tick it was last under — and damping that from the air
    // limb would be the water acting on a voxel it is not touching.
    if (submerged) { p.vy -= (p.vy - u.y) * k / 16; }
  }

  // ---- wind (research doc §4.6) -------------------------------------------
  // The one force site in this kernel besides gravity, which is the point: a
  // second place that touched velocity would be a second place to keep in step
  // with the substep sampling below.
  //
  // A DRAG law, not a push: the particle accelerates toward the air it is in
  // and stops when it gets there. That is what §4.6 means by "accelerates over
  // time" — an ember lofted by a gust keeps gaining speed for as long as the
  // gust outruns it — and it is also the bound (rule 2), because no amount of
  // wind or knob can make debris travel faster than the air is moving. A force
  // term with no velocity feedback has no such ceiling.
  //
  // The gate is tested HERE and not left to windAtQ's zero return, because a
  // drag term reading zero wind is not a no-op: it would drag every particle in
  // the world toward a standstill and quietly change the pinned hash. This is
  // the difference between "the field is off" and "the field is calm".
  //
  // ...and the RATE is ramped by the wind for the same reason one step in: a
  // fixed rate says the still air of a calm day resists a falling ember as hard
  // as a gale does. It does not, and modelling it that way is what made
  // gravity look broken the day wind shipped (windDragRampQ, common.wgsl, has
  // the numbers). Gravity is untouched above and stays untouched; what changes
  // is how much air there is to fall through.
  //
  // ...and NOT for a particle that is in or on water. A voxel touching a liquid
  // answers to the liquid, and only to the liquid: it has ~800x the density of
  // the air over it to be dragged by, and a raft of exploded tree skating across
  // a pond on a breeze is what modelling both at once produced. This is an
  // exclusion rather than a blend on purpose — a blend would need a submersion
  // fraction, a grid voxel has no sub-voxel position to derive one from (the
  // structural fact the buoyancy note at the top of this file is built on), and
  // the fraction would be invented rather than measured. A chip that is thrown
  // clear of the water is dry on the tick it clears it and the wind has it back.
  if (T.windMode != WIND_MODE_OFF && !wet && (p.flags & PFLAG_CALM) == 0u) {
    let resp = i32(matWindResponse(materials[p.payload & 0xFFFu]));
    // Almost every material is 0 (stone chips do not blow around), so the
    // common case is one comparison and no field evaluation at all.
    if (resp > 0) {
      let w = windAtScaledQ(startCell, &T, T.windPartScaleQ);
      // 0 in calm air, and 0 at a 0x dev multiplier — the same statement, which
      // is the point of ramping on the SCALED field. Tested before the gaps are
      // formed so a becalmed world pays a max and a divide, not six.
      let ramp = windDragRampQ(w, &T);
      if (ramp > 0) {
        // The authored per-tick rate, thinned to the air actually present. Q16
        // times Q16 over 65536 stays Q16; PART_WIND_DRAG is at most 65536 and
        // ramp at most 65536, so the product is 2^32 >> 16 = 2^16. In range.
        let rate = (PART_WIND_DRAG * ramp) / 65536;
        // Micro spray gets the same law as a whole voxel. It is the same air,
        // and droplets drifting downwind off a splash is most of what phase 3
        // buys.
        let gx = w.x / PART_FIELD_SCALE - p.vx;
        let gy = w.y / PART_FIELD_SCALE - p.vy;
        let gz = w.z / PART_FIELD_SCALE - p.vz;
        // Two steps rather than one product: gap * resp * drag would leave i32
        // at the top of both knobs' ranges. Integer division truncates toward
        // zero, which is symmetric — the asymmetry an arithmetic shift would
        // introduce reads as a permanent drift down-axis (the mq() lesson).
        p.vx += ((gx * resp) / 15) * rate / 65536;
        p.vy += ((gy * resp) / 15) * rate / 65536;
        p.vz += ((gz * resp) / 15) * rate / 65536;
      }
    }
  }

  p.vx = clamp(p.vx, -PART_MAX_VEL, PART_MAX_VEL);
  p.vy = clamp(p.vy, -PART_MAX_VEL, PART_MAX_VEL);
  p.vz = clamp(p.vz, -PART_MAX_VEL, PART_MAX_VEL);

  // ---- how patient is this floater? ---------------------------------------
  // Counted in ticks SPENT WET, not in ticks alive: a chip that bobs about
  // looking for free waterline is the case this bounds, and a chip still in
  // mid-air on its way there has not started spending anything. At expiry the
  // particle stops being fussy — it takes the first cell it can have, liquid or
  // not, supported or not — which is what guarantees the ring drains even when
  // a pond is far too small for the debris thrown into it (rule 2). The
  // overflow then piles up, which is what a real jam of driftwood does.
  var patient = false;
  if (inFluid) {
    let ft = floatTicksOf(p.flags);
    patient = ft >= PART_FLOAT_PATIENCE;
    if (submerged && !patient) { p.flags = withFloatTicks(p.flags, ft + 1u); }
  }
  // Slow enough to be looking for somewhere to rest rather than still flying.
  let slow = max(max(abs(p.vx), abs(p.vy)), abs(p.vz)) <= PART_SETTLE_SPEED;

  // sample the flight path every <= half voxel
  let maxc = max(max(abs(p.vx), abs(p.vy)), abs(p.vz));
  let n = max(1, (maxc + 127) / 128);
  var lastAir = vec3<i32>(p.px, p.py, p.pz);
  for (var k = 1; k <= n; k++) {
    let sx = p.px + p.vx * k / n;
    let sy = p.py + p.vy * k / n;
    let sz = p.pz + p.vz * k / n;
    let cell = vec3<i32>(sx >> 8u, sy >> 8u, sz >> 8u);
    if (!inBounds(cell)) { return; }  // left the world: particle dies
    if (blocksParticle(cell, inFluid)) {
      // ---- micro: land ON the surface, stain it, and stop existing ----
      // The droplet is parked at the CONTACT point (first blocked sample), not
      // backed off to the last air cell the way a reinserting particle is. The
      // stain target must be recoverable in `resolve` from particle state
      // alone, and re-deriving it there from a backed-off position would mean
      // re-tracing the step — with the velocity, which resolve must not touch
      // because microStainPriority hashes it. Parking on contact makes the
      // target simply "the cell I am in", and the half-voxel overlap is
      // invisible: the renderer shrinks a micro cube to a fraction of a cell.
      if (isMicro(p)) {
        p.px = sx; p.py = sy; p.pz = sz;
        p.flags |= PFLAG_PENDING;
        atomicMax(&claim[claimSlot(cellIndexW(cell))], microStainPriority(p));
        append(p);
        return;
      }
      // propose reinsertion at the last position it could legally be in
      p.px = lastAir.x; p.py = lastAir.y; p.pz = lastAir.z;
      let tgt = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
      // ---- A FLOATER THAT BUMPED INTO A BERTH ALREADY TAKEN ---------------
      // Reinserting here is what used to build the tower: a chip blocked by the
      // chip that landed a tick earlier proposed the cell ABOVE it, and the
      // next one proposed the cell above that. So a slow floater that cannot
      // have the cell it backed off into does not propose at all — it loses its
      // vertical motion and drifts, and the surface of a pond is very wide.
      // Anything still moving fast is genuinely being thrown at the raft and
      // lands on it, which is what a thrown log does.
      if (inFluid && slow && !canOccupy(voxWordAt(tgt), myDensity, patient)) {
        p.vy = 0;
        fluidWander(&p, materials[myMat]);
        append(p);
        return;
      }
      p.flags |= PFLAG_PENDING;
      atomicMax(&claim[claimSlot(cellIndexW(tgt))], particlePriority(p));
      append(p);
      return;
    }
    lastAir = vec3<i32>(sx, sy, sz);
  }
  p.px += p.vx;
  p.py += p.vy;
  p.pz += p.vz;

  // ---- a floater looks for a berth ----------------------------------------
  // Nothing blocked the flight, so a buoyant particle is either still rising
  // through the liquid or already bobbing at the top of it. This is the ONLY
  // exit it has: a blocked flight is what reinserts everything else, and a chip
  // floating on open water is never blocked by anything.
  //
  // Both halves of "a berth" are load-bearing. The cell must be one it can HAVE
  // (canOccupy) and it must have something under it worth resting ON
  // (settleSupported) — the waterline for something that floats, the bed for
  // something that sank. Drop the support half and neutrally buoyant matter
  // converts to a voxel hanging in mid-water; drop the occupancy half and a
  // plank settles by overwriting the water it floats on.
  if (inFluid && slow) {
    let here = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
    if (inBounds(here)) {
      if (canOccupy(voxWordAt(here), myDensity, patient) &&
          (patient || settleSupported(here, myDensity))) {
        p.flags |= PFLAG_PENDING;
        atomicMax(&claim[claimSlot(cellIndexW(here))], particlePriority(p));
        append(p);
        return;
      }
      // No berth: mill about. This is both the look (debris drifting on a pond
      // rather than frozen to it) and the mechanism that spreads a raft out
      // into one layer instead of stacking it — a particle with nowhere to go
      // keeps moving until it finds somewhere that does.
      if (submerged || settleSupported(here, myDensity)) {
        fluidWander(&p, materials[myMat]);
      }
    }
  }
  append(p);
}

@compute @workgroup_size(64)
fn resolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  if (gid.x >= liveCount(1u - T.page)) { return; }
  var p = pWrite[gid.x];
  if ((p.flags & (PFLAG_ALIVE | PFLAG_PENDING)) != (PFLAG_ALIVE | PFLAG_PENDING)) {
    return;
  }

  let cell = vec3<i32>(p.px >> 8u, p.py >> 8u, p.pz >> 8u);
  // TWO BASES (§4.1, the same rule as the hash key): `tgtSlot` is the SLOT
  // cell index and is what the reinsertion CLAIM hashes on — the claim lattice
  // must be a property of the world cell, not of which page currently holds
  // it, or two particles targeting the same cell could hash to different
  // claim slots after a reallocation and both win. `tgt` is the physical word
  // index and is only ever a memory address.
  let tgtSlot = cellIndexW(cell);
  let tgt = voxWordIndex(cell);

  // ---- micro particles: deposit a stain, never a voxel ----
  // Whether it won the claim or not, the droplet is spent — it is sub-voxel
  // matter with nowhere to go. Losing only means another droplet stained this
  // cell on this tick, which is visually identical. Retrying (the ordinary
  // particle's behaviour) would leave spray hovering against a wall until a
  // slot freed up.
  if (isMicro(p)) {
    let drip = (p.flags & PFLAG_DRIP) != 0u;
    p.flags = 0u;  // dead either way
    pWrite[gid.x] = p;
    if (drip) { return; }  // a drop off a wet body: gone, no mark
    if (atomicLoad(&claim[claimSlot(tgtSlot)]) != microStainPriority(p)) { return; }

    let w = voxWordAt(cell);
    let hit = voxMat(w);
    // The cell may have been emptied by the CA between integrate and resolve;
    // staining air would paint a stain onto nothing and it would render as a
    // floating smear.
    if (hit == MAT_AIR) { return; }
    let hk = materials[hit].klass;
    // Same surface rule the CA's own staining uses (doStaining in
    // sim_step.wgsl): a stain soaks into a SURFACE. Blood spray landing in
    // water or drifting through smoke leaves nothing behind.
    if (hk != CLASS_SOLID && hk != CLASS_POWDER) { return; }

    // The droplet stains with ITS OWN material's authored stain, so this is
    // driven by materials.json and works for anything authored to stain — not
    // just blood (conventions: no hardcoded material IDs).
    let sm = materials[p.payload & 0xFFFu];
    let stainType = matStainType(sm);
    if (stainType == 0u) { return; }  // this material does not stain: vanish
    let addAmt = matStainAmount(sm);
    let cur = voxStainAmt(w);
    let curType = voxStainType(w);
    var amt = addAmt;
    if (curType == stainType) {
      if (cur >= STAIN_AMT_MAX) { return; }  // saturated: nothing to write
      amt = min(cur + addAmt, STAIN_AMT_MAX);
    }
    voxStore(tgt, (w & ~STAIN_BITS) | packStain(stainType, amt));
    markDirtyNext(cell);
    return;
  }

  let won = atomicLoad(&claim[claimSlot(tgtSlot)]) == particlePriority(p);

  // The cell must still be one this particle may have — re-read, because the CA
  // ran between integrate and resolve. `canOccupy` is where "or a liquid I am
  // denser than" enters: a rock that sank to the bed of a pond comes to rest in
  // a cell full of water, and the water it displaces is dropped exactly as the
  // brush drops it when it paints into a pond (sim_mutate.wgsl). Conserving it
  // is not locally possible — displacement raises the level of the whole body
  // of water, which is a write this pass cannot reach — and the alternative is
  // that nothing may ever sink.
  let hereWord = voxWordAt(cell);
  let myDensity = materials[p.payload & 0xFFFu].density;
  let patient = floatTicksOf(p.flags) >= PART_FLOAT_PATIENCE &&
                matFluidLift(materials[p.payload & 0xFFFu]) > 0u;

  if (won && canOccupy(hereWord, myDensity, patient)) {
    // rejoin the grid; stamp 0xFF = "hasn't acted", falls next tick
    let mat = p.payload & 0xFFFu;
    var state = (p.payload >> 12u) & 0xFu;
    // A LIQUID is born full, exactly as sim_mutate.wgsl does for a painted one
    // — its state nibble is FULLNESS, not a variant index.
    //
    // Without this, a spawn that leaves the nibble at 0 lands at 1/8 fullness,
    // and the renderer's smooth density field (liquidDensityAt) then sees a
    // scatter of near-empty cells with no coherent isosurface between them.
    // The result reads as separately shaded translucent cubes — the exact
    // "gelatin" failure shadeViscous is written to avoid. Fullness is what
    // makes flung blood shade like the blood a brush paints.
    if (materials[mat].klass == CLASS_LIQUID) { state = LIQ_FULL_STATE; }
    // STAMP_NEVER: a reinserted particle has not acted as a grid voxel yet, so
    // it is free to move on the tick it lands.
    voxStore(tgt, packVox(mat, state, STAMP_NEVER));
    markDirtyNext(cell);
    flagLandedUnsupported(cell, mat);
    p.flags = 0u;  // dead
  } else {
    // Lost the claim (or the cell got taken): rest, retry next tick. The float
    // ticks survive — patience is a budget for finding a berth, and having a
    // berth taken from you is precisely the thing it is counting.
    p.flags = PFLAG_ALIVE | (p.flags & ((PMICRO_LIFE_MASK << PMICRO_LIFE_SHIFT) |
                                         PFLAG_CALM));
    p.vx = 0; p.vy = 0; p.vz = 0;
  }
  pWrite[gid.x] = p;
}
