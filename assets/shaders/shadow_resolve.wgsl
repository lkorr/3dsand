// shadow_resolve.wgsl — the voxel-keyed shadow cache's resolve pass
// (src/sim/world.h, the kShadowCacheBuckets block; plan: shadow cache).
//
// WHAT THIS REPLACES. sunShadowAt used to cast one full trace() per lit pixel.
// At kVoxelMeters = 0.10 and 1080p/70deg a voxel face spans ~157/d pixels, so
// the same shadow answer was recomputed 40-1000x per frame. This pass computes
// it ONCE per surface patch, and the fragment shader only reads.
//
// WHY A SEPARATE PASS AND NOT AN IN-SHADER CACHE. Two reasons, and the second
// is the one that actually decided it:
//
//   1. A fragment shader cannot elect one pixel per patch to do the work, so
//      an in-shader cache degenerates into hit/miss on alternating frames.
//   2. --render-budget (RTX 3060 Ti, overlook cam, 1080p, 2026-09-01) says the
//      shadow call site costs 2.29 ms of TRAVERSAL and 3.59 ms of REGISTER
//      FOOTPRINT. Deduplicating rays while leaving an inline fallback in
//      raymarch.wgsl would chase the smaller half and forfeit the larger one.
//      The trace() call site has to be ABSENT from the compiled fragment
//      shader, which means the ray must be cast somewhere else. Here.
//
// THE RAY CAST HERE IS NOT trace(). trace() lives in raymarch.wgsl, carries 26
// Hit fields and is not reachable from a compute shader anyway. This pass casts
// traceOpaque() (common.wgsl) — a purpose-built MEDIA-BLIND DDA with no media
// accumulation, no water surface, no micro bricks and no reflection, which is
// also why it is cheaper per ray than trace(..., false) was: it carries none of
// those registers.
//
// IT USED TO LIVE HERE, as a private `shadowMarch` returning {hit, t}. W2-A
// (docs/PLAN_lin_followups.md) moved it into common.wgsl and widened the return
// to {hit, t, cell, axis, sgn, word}, because the same march answers every
// SECONDARY ray in the engine — this pass, sunShadowAt's cache-off fallback,
// traceReflection, traceRefraction and the god-ray occlusion test. TWO DDAs
// THAT MUST AGREE IS THE CLASSIC SILENT BUG and one function is the only
// structural cure; `--selftest --gate shadow-cache` still casts the
// compute-stage ray and the fragment-stage ray at sampled surface points and
// asserts they agree, which is now a statement about the two CALL SITES rather
// than about two copies of a DDA.
//
// ---- WHAT THIS ACTUALLY BOUGHT, AND THE COST NOBODY BUDGETED FOR ----------
// Measured with --render-budget (RTX 3060 Ti, overlook cam, 1080p, 2026-09-01),
// `nocache` against baseline in one process on one world:
//
//   nocache (the old inline per-pixel ray)   14.61 ms
//   baseline (this)                          12.84 ms      -1.77, ~12%
//
// Real, and it is the largest single renderer win in the file that declared
// "there is no renderer item left in this plan worth doing". But it is a third
// of what the arithmetic predicted, and the reason is worth more than the win:
//
//   * The dedup WORKS. The same frame reports 177,703 patches requested for
//     2,073,600 pixels — 8.57 per 100 px, 11.7x fewer rays — and 0 refused, so
//     the request cap is nowhere near binding.
//   * The rays got 15x MORE EXPENSIVE EACH. 1.35M rays cost 2.29 ms in the
//     fragment shader (1.7 ns each); 177k cost ~3.9 ms here (22 ns each).
//
// THE RAYS ARE INCOHERENT, AND THAT IS THIS PASS'S DOING. In the fragment
// shader adjacent pixels cast adjacent rays, so a wavefront marched one
// neighbourhood and every voxel fetch was a hit in a line some other lane had
// already pulled. Here the work list is in APPEND ORDER — pixels race to
// atomicAdd as they shade — so 64 consecutive threads march 64 unrelated parts
// of the world and share nothing. Cutting the ray count further does not help
// (subdiv 4 -> 1 is a large count reduction for 0.55 ms), which is the
// signature of a pass bound by memory locality rather than by work.
//
// So the next win here is not fewer rays, it is SORTED rays: bin the request
// list by a Morton code of the patch cell before dispatching, so a workgroup
// marches one neighbourhood again. That is a real piece of work with its own
// cost, and it is deliberately not smuggled in here — it is written down
// because the number that motivates it took one instrumented run to get and
// would take a dozen elimination runs to guess.

@group(0) @binding(0) var<storage, read> voxels    : array<u32>;
@group(0) @binding(1) var<storage, read> occupancy : array<u32>;
@group(0) @binding(2) var<storage, read> materials : array<Material>;
@group(0) @binding(3) var<uniform> R : RenderParams;
@group(0) @binding(4) var<storage, read> pageTable : array<u32>;
@group(0) @binding(5) var<storage, read_write> shadowCache : array<atomic<u32>>;
@group(0) @binding(6) var<storage, read_write> shadowReq : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> shadowArgs : array<u32>;
// P1 direct injection (docs/PLAN_gi.md §3; common.wgsl IRRADIANCE GRID): every
// patch this pass publishes also deposits its lit radiance into its block-face.
// `opennessGen` is the per-slot stamp shared with the openness grid, read so
// a slot the window has reused starts its blend from zero.
@group(0) @binding(8) var<storage, read_write> irradiance  : array<u32>;
@group(0) @binding(9) var<storage, read>       opennessGen : array<u32>;
// The openness byte grid itself, read so the P1 deposit can cap the shadow
// LIFT by sky visibility (shadowLiftCap, common.wgsl): without it a cave
// floor under a tall roof deposited 45% sun into the grid every frame and the
// gather lit the cave with it. The published cache value is NOT capped here;
// the fragment shader caps at its read, so the cache stays the pure ray
// answer --gate shadow-cache compares against the fragment-stage ray.
@group(0) @binding(10) var<storage, read>      openness    : array<u32>;
// The penumbra window (src/sim/world.h kShadowHistBytes) — one word per bucket,
// read-modify-written below. Bound here and NOWHERE else: the fragment shader
// reads the published byte out of shadowCache and never this.
@group(0) @binding(11) var<storage, read_write> shadowHist : array<u32>;

// ======================= THE SUN IS NOT A POINT ============================
//
// WHAT THIS FIXES. One ray per patch answers a yes/no question, so the cache
// could only publish "lit" or "shadowed at this blocker's lift" — a hard step
// one patch wide however far away the blocker was. Walking the sun across the
// sky then advanced that step one patch at a time, which is what a shadow
// expanding in visible jumps IS (owner report 2026-09-11), and no amount of
// subdivision fixes it: a finer grid makes the step smaller, not softer.
//
// THE RAY IS JITTERED INSIDE THE SOLAR CONE and the last SHADOW_SAMPLES
// verdicts are averaged, so a patch publishes the FRACTION of the disc it can
// see. That is a real penumbra: it costs ZERO extra rays, and its width falls
// out of the geometry — a blocker `d` away spreads the cone over 2*d*tan(angle),
// so a kerb stays crisp and a canopy 10 m up softens over most of a metre.
//
// A WINDOW, NOT AN EXPONENTIAL BLEND. An EMA over a cycling sample set never
// settles: it oscillates with the sequence's period forever, which is a
// shadow that pulses. A sliding window over a FIXED sequence is exact after
// SHADOW_SAMPLES frames and then stops moving entirely while the scene and the
// sun hold still, because the same slot is rewritten with the same bit.
// --gate shadow-cache's flicker arm asserts exactly that, which is why its
// warm-up is longer than the window.
//
// THE PATTERN IS ROTATED PER PATCH, so the 1/16 quantisation of the estimate
// dithers across neighbouring patches instead of terracing into 17 visible
// bands. That trades a contour for sub-voxel grain, and the grain is the better
// artifact here: it is STATIC (the rotation is a hash of the patch, not of the
// frame), it is finer than a voxel at subdiv 4, and the reader averages four
// patches bilinearly on top of it.
const SHADOW_SAMPLES : u32 = 16u;   // world.h kShadowSamples
const SHADOW_HIST_MASK : u32 = 0xFFFFu;
const SHADOW_FILL_SHIFT : u32 = 16u;
const SHADOW_FILL_MASK : u32 = 31u;
const SHADOW_LIFT_SHIFT : u32 = 21u;
// tan of the cone half-angle. A const-eval of a tuning float, exactly like the
// fluid/wind rows: the kernel that reads it is the only consumer.
const SHADOW_CONE_TAN : f32 = tan(TUNE_SHADOW_SUN_ANGLE * 0.017453292);

// ============ THE WINDOW HAS SEVENTEEN LEVELS; THE SUN HAS NONE =============
//
// WHAT THIS FIXES (owner report 2026-09-12, the follow-up to the one above).
// The cone made the shadow a gradient in SPACE. It is still a staircase in
// TIME, because SHADOW_SAMPLES binary verdicts can only estimate the disc's
// coverage at 1/16, and a patch therefore holds a value for a while and then
// jumps 6.25% of full contrast when the blocker's edge crosses one of the
// sixteen sample directions. Do the arithmetic at the tuning this ships with:
// dayNight.cycleMinutes is 6, so the sun crosses a degree of sky per SECOND
// and sweeps the whole 2 deg cone in two — sixteen levels in two seconds is a
// visible step every 7 or 8 frames, for as long as the shadow is moving. That
// is "the pixels still discretely jump in a bunch of small steps".
//
// MORE SAMPLES IS NOT THE ANSWER and there is no room for them anyway: the
// window, its fill count and the lift byte already use 29 of a word's 32 bits,
// and doubling the sample count halves the step while doubling the warm-up.
// Sixty-four levels would still step.
//
// SO THE PUBLISHED BYTE GLIDES. The window output is low-passed on its way
// into the slot, which turns each 1/16 jump into a ramp a few frames long; the
// ramps of a moving shadow run into each other and what comes out is a
// continuous slide at 1/255, which is the byte's own resolution and the end of
// the road.
//
// THIS IS NOT THE EMA THE BLOCK ABOVE REFUSES, and the distinction is the
// whole reason it is safe. That one would have averaged the RAW SAMPLES — a
// cycling sequence, so its output oscillates with the sequence's period
// forever and never settles. This one averages the WINDOW'S OUTPUT, which in a
// static scene is not a sequence at all: it is one number, bit-identical every
// frame, because the same slot is rewritten with the same bit. A low-pass fed
// a constant converges to that constant and stops. The flicker arm of
// --gate shadow-cache is what holds that claim honest.
//
// EXACT IN INTEGERS, for the same reason. The glide is computed on the stored
// BYTE, not on a float that is re-quantised at the write: a float blend that
// lands 0.4 units short of its target rounds back and forth between two
// adjacent bytes forever, which is a 1/255 flicker — small, but "0 pixels
// moved between two warmed frames" is a claim about zero. Integer steps with a
// minimum magnitude of one unit reach the target exactly and then produce a
// delta of exactly zero.
//
// A SNAP THRESHOLD keeps it from costing responsiveness. The sun creeps; a
// blocker that appears or vanishes (a block mined, a door opened) moves the
// answer by a large fraction at once, and there is no reason to smear that
// over a third of a second. Past SHADOW_GLIDE_SNAP the new answer is taken
// whole. The sun's own 1/16 steps are nowhere near it, and neither is the
// lift byte's 16-frame refresh, so both glide.
//
// THE LAG IS BOUNDED AND SMALL. Below |d*rate| = 1 the step floors at one unit
// per frame, so a steadily-moving shadow settles into a constant-velocity
// slide with a standing error of about 1/rate units — ~17 of 255 at the rate
// below, which is one window step, which is a couple of centimetres of shadow
// position. A constant-velocity slide is the smoothest thing this can be.
//
// OFF AT A ZERO CONE ANGLE, so `render.shadowSunAngle = 0` still reproduces
// the pre-2026-09-11 hard shadow exactly, value for value, and stays the
// differential oracle it was.
const SHADOW_GLIDE : f32 = 0.06;        // per frame, on the 0..255 byte
const SHADOW_GLIDE_SNAP : i32 = 64;     // >= this many units: take it whole

// A stable orthonormal pair spanning the plane normal to `n`. Frisvad's branch
// is avoided in favour of picking the axis `n` leans on least, which costs two
// compares and cannot degenerate.
fn shadowConeBasis(n : vec3f) -> mat2x3<f32> {
  let a = abs(n);
  var up = vec3f(0.0, 0.0, 1.0);
  if (a.z >= a.x && a.z >= a.y) { up = vec3f(1.0, 0.0, 0.0); }
  let t = normalize(cross(up, n));
  return mat2x3<f32>(t, cross(n, t));
}

// Sample `i` of a SHADOW_SAMPLES-point sunflower disc, rotated by `rot` turns.
// Equal-area radii (sqrt of the stratum) and the golden angle between points:
// every prefix of the sequence is already well spread, which matters because
// a patch's first frames publish a partial window.
fn shadowConeDir(L : vec3f, i : u32, rot : f32) -> vec3f {
  if (SHADOW_CONE_TAN <= 0.0) { return L; }
  let b = shadowConeBasis(L);
  let r = sqrt((f32(i) + 0.5) / f32(SHADOW_SAMPLES));
  let a = f32(i) * 2.39996323 + rot * 6.28318531;
  let off = (b[0] * cos(a) + b[1] * sin(a)) * (r * SHADOW_CONE_TAN);
  return normalize(L + off);
}

// --------------------------------------------------------------- passes ----

// prepare: turn last frame's request COUNT into a dispatch size, and reopen the
// list for this frame's fragment shader to append into.
//
// The count is moved to a SAVED slot rather than read in place, because the
// same word has to go back to zero before the render pass runs. Ordering is
// safe by construction: this pass, then resolve, then the draw — all in one
// command buffer with the pass table's barriers between them.
@compute @workgroup_size(1)
fn prepare() {
  let raw = atomicLoad(&shadowReq[0]);
  let n = min(raw, SHADOW_REQ_CAP);
  atomicStore(&shadowReq[1], n);
  // Overflow is graceful and counted, not fatal: past the cap a patch is simply
  // not registered, so it misses next frame and shades from the miss default.
  // The failure is one slightly wrong patch, not a lost voxel — the opposite of
  // the page pool, where exhaustion aborts.
  atomicStore(&shadowReq[2], raw);
  atomicStore(&shadowReq[3], select(0u, raw - SHADOW_REQ_CAP, raw > SHADOW_REQ_CAP));
  atomicStore(&shadowReq[0], 0u);
  shadowArgs[0] = (n + 63u) / 64u;
  shadowArgs[1] = 1u;
  shadowArgs[2] = 1u;
}

// resolve: one media-blind shadow ray per requested patch.
@compute @workgroup_size(64)
fn resolve(@builtin(global_invocation_id) gid : vec3<u32>) {
  let n = atomicLoad(&shadowReq[1]);
  if (gid.x >= n) { return; }

  let base = SHADOW_REQ_HEADER + gid.x * SHADOW_REQ_WORDS;
  let key = atomicLoad(&shadowReq[base]);
  let bucket = atomicLoad(&shadowReq[base + 1u]);
  let packedCell = atomicLoad(&shadowReq[base + 2u]);
  let packedSub = atomicLoad(&shadowReq[base + 3u]);

  // Reconstruct the patch's world-space centre and normal. The request stores
  // the cell TOROIDAL (9 bits per axis at WORLD_N 512), so it means the same
  // patch whether or not the window moved between the frame that queued it
  // and this one — shadowPackCell in common.wgsl says why that matters — and
  // it is unwrapped into the window that is current NOW, which is the window
  // the fragment shader reading the answer will be looking through.
  let cell = shadowUnwrapCell(packedCell, R.origin * i32(CHUNK));
  let face = (packedCell >> (WORLD_SHIFT * 3u)) & 7u;
  let hp = shadowPatchCentre(cell, face, packedSub & 7u, (packedSub >> 3u) & 7u,
                             R.shadowSubdiv);
  let n3 = shadowFaceNormal(face);

  // ---- which sample of the window is this frame's, and is the slot ours ----
  // The slot's state word is read BEFORE the ray, for one reason: its VALID bit
  // is the only signal that says whether the window in shadowHist belongs to
  // this patch. A slot is claimed by zeroing its state word (raymarch.wgsl
  // shadowSlotRead), and nothing but this pass ever sets valid, so "not valid"
  // is exactly "claimed since the last publish" — a patch that has just taken
  // the slot over, whose predecessor's 16 samples are about some other surface
  // entirely. Carrying them over would paint one patch's penumbra onto another.
  let ver = shadowPatchVerifier(packedCell, packedSub);
  let old0 = atomicLoad(&shadowCache[bucket * 2u + 1u]);
  let fresh = !shadowStateValid(old0) || shadowStateVerifier(old0) != ver;
  var hist = select(shadowHist[bucket], 0u, fresh);
  var fill = (hist >> SHADOW_FILL_SHIFT) & SHADOW_FILL_MASK;
  // Read BEFORE the increment below: the glide may only run once the window is
  // a full window. While it is filling, the divisor itself is changing, so the
  // output moves for a reason that has nothing to do with the scene and
  // smoothing it would only delay the first honest answer — and would leave
  // the value still creeping after the warm-up --gate shadow-cache pays for.
  let wasFull = fill >= SHADOW_SAMPLES;
  let slotIdx = R.frameIdx % SHADOW_SAMPLES;

  // THE CENTRE RAY IS NOT JITTERED, and which frames take it is load-bearing.
  // The LIFT — how far a shadowed patch is lifted toward TUNE_SHADOW_LIFT by
  // its blocker's distance — is a smooth, slowly-varying quantity, and there
  // are no spare bits to run a second 16-sample window for it. So it is
  // refreshed from ONE deterministic ray: the undeflected one, on the frame
  // this patch's window wraps, plus immediately on a fresh slot so that a newly
  // visible patch never spends a frame at the reset value of 0 (which is
  // contact black). A single fixed ray means the byte settles and STAYS, which
  // is what keeps a static scene's published value bit-stable frame to frame.
  let centre = fresh || slotIdx == 0u;
  let rot = f32(shadowPatchKey(packedCell, packedSub) & 0xFFFFu) * (1.0 / 65536.0);
  let dir = select(shadowConeDir(keyLightDirP(R), slotIdx, rot),
                   keyLightDirP(R), centre);

  // shadowCoarseFromT() is `coarseFromT` (W2-B): past that distance the march
  // terminates on the 4^3 blockers mask instead of the voxel. It MUST be the
  // same expression sunShadowAt passes — the gate casts this ray and that one
  // at the same surface point and asserts they agree — which is why it is a
  // function in common.wgsl and not a knob read twice. The two pointers are
  // how a function in common.wgsl reaches bindings declared after it (see
  // traceOpaque).
  let s = traceOpaque(hp + n3 * TUNE_SHADOW_BIAS, dir,
                      TUNE_SHADOW_STEPS, shadowCoarseFromT(),
                      &occupancy, &materials);
  // A BURIED PATCH HAS NO OPINION. The ray starts TUNE_SHADOW_BIAS off the
  // face; a hit within a twentieth of a voxel of that means the cell in front
  // of the patch is itself solid — the patch is not a surface anyone can see,
  // it is the top face of the block UNDER a terrace step, or the side face
  // of one INSIDE the hill. No pixel is ever on such a patch, but the reader's
  // bilinear taps roll into the neighbour cell's same face past a face edge
  // (raymarch.wgsl shadowCached), and at every step of a terraced hillside
  // that neighbour is buried. Resolving it to a contact-black value painted a
  // one-pixel dark line along every voxel edge of the terrain, sun or no sun
  // — --gate shadow-cache's walk arm counted them as ~15-21k "phantom" pixels
  // a frame, every one on a step edge. Marking the slot INVALID instead makes
  // the tap drop out of the blend (weight 0) and the pixel shade from its own
  // face's patches, which is the ordinary patch-quantised edge and nothing
  // more. The slot stays claimed and live, so it is re-requested each frame
  // (one ray that terminates in its first cell) and never reclaimed by a
  // foreign patch mid-frame.
  let buried = s.hit && s.t < 0.05;

  // ---- fold this frame's verdict into the window ----
  // `lit` is the sample's bit. RAN OUT OF STEPS counts as OCCLUDED, not as
  // daylight: leaving the window is sky, but exhausting the budget underground
  // is a blocker somewhere past it, and "no hit = lit" put full sun on the
  // floor of any cave deeper than the budget reaches along the sun (2026-09-02,
  // with block termination off: 384 fine steps = 38 m). It takes the FAR
  // blocker's lift below, which the openness cap at the reader turns into
  // nothing inside a cave and into the usual soft lift outdoors. Same rule in
  // sunShadowAt (raymarch.wgsl); the gate compares them.
  let ranOut = !s.hit && s.steps > u32(TUNE_SHADOW_STEPS);
  let sunSeen = !s.hit && !ranOut;
  let bit = 1u << slotIdx;
  hist = select(hist & ~bit, hist | bit, sunSeen);
  if (fill < SHADOW_SAMPLES) { fill = fill + 1u; }

  // The lift this ray saw, if it is the one that owns the byte. The law is
  // sunShadowAt's, verbatim, and must stay that way: the depth of a shadow is
  // taken from how far the ray travelled before being blocked, so a contact
  // shadow stays dark and a distant blocker's shadow lifts.
  var liftB = (hist >> SHADOW_LIFT_SHIFT) & 0xFFu;
  if (centre) {
    var lv = 0.0;
    if (s.hit) {
      let dM = s.t * VOXEL_METERS;
      lv = clamp(smoothstep(TUNE_SHADOW_SOFT_NEAR, TUNE_SHADOW_SOFT_FAR, dM) *
                 TUNE_SHADOW_LIFT, 0.0, 1.0);
    } else if (ranOut) {
      lv = TUNE_SHADOW_LIFT;
    } else {
      // The centre ray is clear, so nothing is blocking the middle of the disc
      // and only its rim can be clipped. Such a blocker is by construction a
      // grazing one; lifting its sliver of shadow all the way to the far value
      // would brighten the outer half of every penumbra. Contact-dark is the
      // conservative end and the one that keeps an edge reading as an edge.
      lv = 0.0;
    }
    liftB = u32(lv * 255.0 + 0.5);
  }
  hist = (hist & SHADOW_HIST_MASK) | (fill << SHADOW_FILL_SHIFT) |
         (liftB << SHADOW_LIFT_SHIFT);

  // ---- the published value ----
  // `open` is the fraction of the window that saw the sun. The rest of the disc
  // is blocked, and what survives there is the lift — so the two compose as
  // `open + (1 - open) * lift`, which degenerates EXACTLY to the pre-cone
  // behaviour at a zero cone angle: every sample agrees, `open` is 0 or 1, and
  // the value is the lift or full sun. Unfilled slots read as 0 bits, which is
  // why the divisor is `fill` and not SHADOW_SAMPLES: a patch resolved for the
  // first time publishes its one centre ray's answer, same as it always did.
  let openF = f32(countOneBits(hist & SHADOW_HIST_MASK)) / f32(max(fill, 1u));
  let liftF = f32(liftB) * (1.0 / 255.0);
  let aim = clamp(openF + (1.0 - openF) * liftF, 0.0, 1.0);
  // ---- the glide (see THE WINDOW HAS SEVENTEEN LEVELS, above) ----
  // Integer units of the stored byte throughout, so the fixed point is exact.
  var valU = i32(aim * 255.0 + 0.5);   // `aim` not `target`: reserved word
  // SHADOW_GLIDE = 0 is the differential oracle: it takes the window's answer
  // whole, which is the pre-2026-09-12 behaviour, and `--gate shadow-cache`'s
  // creep arm goes from 0.8% of moves jumping to 99.2% — and from 43,928
  // slot-frames moving at all to 7,146, because moving a LITTLE EVERY FRAME
  // rather than a lot occasionally is precisely what the fix is. One WGSL
  // const, no rebuild: that is the whole A/B.
  if (SHADOW_GLIDE > 0.0 && SHADOW_CONE_TAN > 0.0 && wasFull && !fresh) {
    let prevU = i32(old0 & 0xFFu);
    let d = valU - prevU;
    if (d != 0 && abs(d) < SHADOW_GLIDE_SNAP) {
      var s = i32(round(f32(d) * SHADOW_GLIDE));
      // At least one unit toward the target — a rate that rounds to zero would
      // park the byte a quantum short and never arrive.
      if (s == 0) { s = select(-1, 1, d > 0); }
      if (abs(s) > abs(d)) { s = d; }   // never overshoot
      valU = prevU + s;
    }
  }
  let v = f32(valU) * (1.0 / 255.0);
  // Written UNGUARDED, unlike the publish below. If the slot has changed hands
  // since the request was queued, the value write is dropped but this one is
  // not — and that is harmless by construction: the new owner's verifier will
  // not match `old0` next frame, so it reads `fresh` and starts the window from
  // zero. Guarding it would cost a second atomic load to learn nothing.
  shadowHist[bucket] = hist;

  // ---- P1 direct injection (docs/PLAN_gi.md §3) ----
  // The patch is a lit (or shadowed) piece of a real surface and this pass is
  // the one place that knows its cell, its face and its shadow term at once, so
  // it deposits albedo × sun × lambert × lit into the block-face word here —
  // one voxel-word read on top of the ray. Not guarded by the cache publish
  // below: a slot that changed hands is a cache-identity problem and the light
  // is still real. Buried patches (the cell in front is solid) deposit nothing
  // — no surface anyone sees, and a bilinear reader would never ask. Racy
  // against the other patches of the same face by design; see irrDeposit.
  if (TUNE_GI_STRENGTH > 0.0 && !buried) {
    let pw = voxWordAt(cell);
    let pm = materials[voxMat(pw)];
    // Burning foliage deposits the MEAN of its breath, which is what
    // sim_openness.wgsl's walk deposits into the SAME word -- the two writers
    // of one value have to agree, and this pass is the loud one (every frame,
    // against once per sweep). Depositing the raw palette instead put a
    // burning crown's leaf-green into the grid at full emission and lit
    // everything under the tree green (owner report 2026-09-03).
    let bt = burnTint(pm, paletteColor(pm, voxState(pw), &materials),
                      f32(pm.emission) / 255.0, burnTintMean());
    let ob = opennessByteAt(cell, face, &openness, &opennessGen);
    let lit = shadowLiftCap(v, select(-1.0, f32(ob) * (1.0 / OPEN_MAX), ob >= 0));
    let sample = irrSample(bt.albedo, n3, keyLightDirP(R), keyLightColorP(R),
                           lit, bt.emis);
    let stampOk = opennessGen[chunkIndexW(cell)] == opennessStamp(worldChunkOf(cell));
    irrDeposit(irrIndexOfCell(cell, face), sample, GI_RESOLVE_ALPHA, stampOk,
               &irradiance);
  }

  // Publish, guarded on the slot still being OURS — key AND verifier. A slot
  // can only change hands once it has gone stale (raymarch.wgsl shadowSlotRead
  // never steals a live one), but a duplicate claim or a set that turned over
  // in the frame since this request was queued would otherwise stamp our value
  // under another patch's identity — precisely the "wrong shadow" the blend's
  // zero weight exists to avoid. Losing the write instead costs this patch one
  // frame with no opinion. `requested` and the verifier ride through unchanged.
  // `ver` is computed at the top of the pass — the window above needs it to
  // tell "my slot, carry the samples" from "somebody else's, start over".
  if (atomicLoad(&shadowCache[bucket * 2u]) != key) { return; }
  let old = atomicLoad(&shadowCache[bucket * 2u + 1u]);
  if (shadowStateVerifier(old) != ver) { return; }
  // `valU` and not `u32(v * 255.0 + 0.5)`: the byte IS the state the glide
  // above iterates on, and a round trip through a float is the one place a
  // fixed point could pick up a unit of drift.
  atomicStore(&shadowCache[bucket * 2u + 1u],
              shadowPackState(u32(valU), R.frameIdx & 15u,
                              shadowStateRequested(old), !buried, ver));
}
