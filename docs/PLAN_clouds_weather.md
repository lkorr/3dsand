# PLAN — clouds and weather

Status: packages A, B, C, E and F LANDED 2026-09-23 (worktree
`clouds-weather`), plus tier-1 rain/snow and lightning from D/G. DESIGN.md §9.w
is what shipped and supersedes the sketches below where they differ. Still open
(2026-10-02 audit: rain DOES touch the sim now — `RCOND_RAIN`/`RCOND_RAINDAMP`
in `common.wgsl`, `TickParams.weatherRain`, `"rain"` douse and `"rainDamped"`
ignition rules in `reactions.json` (main `9a0a702`, 2026-09-23), tier 2's
per-voxel wet stain on exposed ground (`sim_mutate.wgsl` `rainFall`, drying,
gate `rain-stain`, `f039607`, 2026-09-25) and the exposure map below; only
tier 3 water placement is still missing from that item):
~~rain touching the SIM (§2.5 tier 2 wet stain + `RCOND_RAIN`, tier 3 water)~~, a
tuner PAGE for authoring presets (they are plain JSON; the Clouds and Sky
weather tabs cover the knobs), thunder audio, cloud god-rays, biome-driven
weather, and recovering the raymarch fs spill (16 -> 96 B/thread).

2026-09-30: rain LEANS and lands where its fall line lands -- a render-only
rain shadow map along the lean (drops + wet ground), per-preset `windShare` /
`maxLeanDeg`, and on the sim side an integer rain slope on TickParams, a
slanted ground sampler and a rain exposure map the douse/damp rules read (tier
2's exposure, done properly). DESIGN.md §9.w "Where the rain lands"; gate
`rain-lean`.

Deviations from the plan, and why:
- §2.1 did NOT extend WindWeather/TickParams: nothing in the sim reads weather
  yet, so the resolver is its own render-only module (`src/sim/weather.*`) that
  READS the wind (cloud drift, streak lean). The integer TickParams copy is the
  first step of tier 2, not a prerequisite for clouds.
- §2.3's "one texture fetch" is one bilinear upsample of a 3-word history, and
  it keeps TWO transmittances (hazed and direct) — a single T let the sun disc
  shine through a hazed thick cloud.
- §2.4 is a per-frame cloud SHADOW MAP (compute), not taps along each shadow
  ray: one bilinear lookup at every shading site, bodies included.
- The weather presets are sorted onto a moisture LADDER walked by smooth noise
  (DESIGN.md §9.w), which is what makes "no cuts" a gate claim.

Original status: RESEARCH + PLAN, nothing landed. 2026-09-11.

Sources: the VoxelGameDev scrape at `F:\discord scrape\txt` (channels *graphics*,
*voxels*, *raytracing*, *lighting*, *procedural-generation*, *fluids*,
*data-storage*), read against what this engine already has.

---

## 0. What the sky already is, and what is actually missing

`raymarch.wgsl`'s sky is not a weak system. It has Rayleigh + Mie in-scatter with
a correctly-split view/sun air mass, a limb-darkened solar disc with real
eclipse coverage, two moons on Keplerian orbits with phase sign and mutual
occultation, a starfield sized in PIXELS wheeling about the true celestial pole,
a Milky Way with dust lanes, two nebula masses, and aurora curtains. `SkyState`
comes from `ComputeSky(tuning, celestialTick)` — a pure function of the tick, no
integrator (`src/sim/celestial.h`).

One thing is missing, and it is the only structural one:

> **There is nothing between the camera and the atmosphere.** Every pixel of sky
> is a pure function of *direction*. Nothing in it varies with where you stand,
> nothing in it moves except the bodies, and nothing in it tells you anything.

Everything else on a "more interesting sky" list is polish on a good system.
Clouds are the missing system, and weather is the reason to want them.

A second consequence worth stating up front: **sky pixels are currently the
cheapest pixels in the frame** (`RS_PX_SKY` — they hit nothing, near or far).
Clouds spend exactly the budget that is currently free, and the worst case is a
full overcast filling the screen. Measure there, not at the default.

---

## 1. What the corpus actually says

### 1a. Lin tried volumetric clouds and abandoned them — twice

Dec 2019 he shipped them ("The only downside to having volumetric clouds is that
standing on the ground and waving some cloth around doesn't seem all that cool in
comparison"). By Jan 2020: *"inside the eye of a storm does not look appealing
after a while, trust me lol. i hate volumetric clouds and many clouds in general
after being inside them getting them looking right."* By Sept 2020, asked again:
*"tried that back in december" / "they didn't look very good"* — and no paper, no
HZD, just experiments.

jwnjwn's diagnosis is the useful part: *"I feel they need to be way lower res
than your actual voxel res. Because distance will make them just seem
non-blocky. And they gotta look blocky yo."* Lin's own landing point, in the same
exchange: *"maybe ray marched clouds, but on a very low res texture / then
upscale with nearest neighbor."*

**Taken as:** photoreal multi-scatter volumetrics are the wrong target for this
engine. The winning form is a low-resolution march whose resolution is a
deliberate art choice, not a perf concession. That is the *same* conclusion the
perf literature reaches from the other end (1f), which is a good sign.

### 1b. Putting clouds in the DDA does not work

apothic0n, Mar 2025, rendering clouds inside his voxel DDA: at the **chunk**
stage they were satisfyingly blocky but *"if i try doing movement at that scale
it just instantly jumps chunk to chunk"*; at the **voxel** stage motion was
smooth but *"they arent as blocky"* and *"really bad performance"*. His
workaround was to make the clouds move fast enough that per-block stepping was
invisible.

**Taken as:** blockiness and smooth drift are in direct tension when the cloud
*is* the world grid. Decoupling the cloud's lattice from the world's is what
buys both. (And see §2.3 — for this engine the DDA is off the table for a second,
harder reason.)

### 1c. The three-field weather model — this is the core of the plan

extra_witchy, Dec 2024, in *procedural-generation*:

> - cloud texture
> - cloud coverage
> - raininess
>
> Clouds are produced by cloud coverage and cloud texture combined, so that with
> greater cloud coverage in a position the lower the threshold for cloud is.
>
> The sampling offset can move over time based on the wind, and offsetting the
> coverage and texture sample movement (or moving the texture sample in the 4th
> dimension) creates patterns where clouds don't just move with the wind, but
> also change shape.
>
> The raininess can combine with the cloud coverage to determine when rain
> happens; at high raininess even small amounts of cloud produce strong rain,
> while at medium raininess scattered clouds don't rain and overcast weather
> rains slightly; at low raininess there is no rain no matter the cloud coverage.

Three scalars, and they generate the whole legible taxonomy — clear, fair,
scattered, overcast, drizzle, downpour — with no state machine. apothic0n in the
same thread arrives at the localisation from the other side: two noise layers,
one ordinary cloud and one rain cloud, *"areas under the rain clouds (aka
anywhere the noise is within a certain threshold) will have rain. boom localized
rain."*

### 1d. Cloud shadow is one lookup, not a march

57a, Dec 2023, in *raytracing*: *"I just do normal raytracing towards the sun
rotation, and at the end I check whether the ray intersects with a cloud."*

**Taken as:** the whole world dimming under an overcast costs one call appended
to the shadow ray this engine already casts. Cheapest big win on the list, and
the one that turns weather from something you look at into something you feel.

### 1e. Rain must be gated per-sample on sky visibility, not culled per-player

*data-storage*, on culling raindrops by the sun value at the drop's position:
*"I think typically you would only have rain drops in a region around the player,
right?" / "in which case that might not be a good idea, because you'd be culling
all the rain drops if the player is just a few meters away from the door" / "so
there'd be seemingly no rain outside"* — and *"Better than rain falling through
the ceiling"*, plus *"the rain volume can be like 40x40 and still be
performant"*.

**Taken as:** a player-centred cull is the failure mode. This engine has a
per-cell openness (sky-visibility) grid, so it can gate per sample and get both
halves right.

### 1f. Rain that places water floods worlds — the corpus is unanimous

The *fluids* channel, at length: *"would be pretty cool if rain would place
water, but not stack or it'd break the game"*; a scheme to remove all 1-level
water when `weather != rain`, abandoned as *"too error prone to place water with
rain"*; and the summary line, *"it's kinda a pain that to add stuff like rain,
you need to have a ton of other systems in place as well."* Elsewhere: *"i'll
just make rain clouds super rare"* as the mitigation.

**Taken as:** in a falling-sand engine with a real fluid solver this is not a
cosmetic worry, it is CLAUDE.md rule 2. World-wide rain that emits voxels is an
unbounded emergent process. §2.5 puts it behind a budget and a default-off
switch, and makes the *default* way rain touches the world be staining.

### 1g. Low-res + temporal reconstruction is the standard, and TAA is the ally

gpgpu: *"volumetric... just means you do it at a low res then temporally
reconstruct it"*, and the fallback for weak hardware is *"even lower res
clouds"*. The Frostbite sky/cloud paper discussion: *"you can render volumetrics
at impressively low resolution without noticeable change in quality."* And
directly: *"some stochastic rendering techniques lean on TAA a bit to resolve
them, particularly for expensive stuff like hair and clouds."*

The counter-warning, from Lin in *graphics* while trying exactly this: *"I'm
still thinking of ways to do lower resolution volumetrics, since doing a 'dumb
blur' on that will cause leakage I'm afraid"* — and separately, whether an
edge-aware à-trous filter even works when the input is lower-res than the output.

**Taken as:** half or quarter res plus reprojection, composited *before* TAA so
TAA finishes the job. Leakage is a real risk but clouds are the easy case: the
cloud layer is far away and has no silhouette against near geometry to leak
across.

### 1h. Froxels are the wrong tool here, and godrays are the frame killer

gpgpu on a froxel volumetric-fog implementation: *"it's pretty good for local
fog, but has some issues when you make the frustum huge and try clouds. for that
you need another technique."* Worth knowing, because this engine's per-pixel
godray march is the same family and it would be tempting to just point it upward.

Godrays: half a dozen independent reports of them halving framerate
(*"fuckin god rays cut the framerate in half no matter what"*, *"can't wait to
add back god rays and it drops to like 20fps"*, *"grass and god rays are still
the biggest bottlenecks"*). This engine already has `TUNE_GODRAY_STEPS` and an
`RS_GODRAY` span. **Cloud-shaped godrays go last and behind a switch.**

### 1i. Smaller items, taken as written

- Render the cloudscape to a **cubemap** once and sample it cheaply in GI and
  reflections (cosmo1337, after getting procedural drifting clouds working).
- Clouds want **transparent borders**, and sorting transparent clusters is the
  usual pain — irrelevant for a raymarched compositing pass, which is a reason
  to prefer it.
- A skybox with clouds looks *wrong* when terrain fades into it (*"it made it
  look really bizarre when the terrain blended into clouds instead of sky"*) —
  which is an argument for the cloud layer having real altitude and real
  geometry rather than being painted on the dome.
- Orographic precipitation exists as an idea (*"wind, precipitation from clouds
  that are pushed up mountain fronts"*) — cheap once coverage is a spatial field
  and the engine has a terrain height mirror. Filed under later.
- A caution on weather-from-one-noise-field: *"if you use one noise, it
  guarantees both spatially and temporally, that one weather pattern can only
  follow or be followed by a pair of two others"* — so coverage and raininess
  must be independent draws, not two slices of one field. This engine already
  states the same rule for RNG salts in `wind.h`.

---

## 2. The plan

### 2.1 The weather regime: extend `WindWeather`, do not add a second author

`src/sim/wind.h` is **already a deterministic epoch weather system** and it
already has a `storm` flag. It draws per-epoch targets from
`Hash3(seed ^ kWindWeatherSalt, epoch, component)`, smoothsteps across a
2048-tick epoch, interpolates direction as a vector, and has a `weatherAuto`
switch for pinning it. Its own header states the invariant this plan must not
break:

> `WindWeather` is the ONE author of these values for both UBOs... If the
> renderer and the sim each derived their own weather, grass and smoke would blow
> different ways in the same frame and the bug would look like a shader problem.

So: **`WindState` becomes `WeatherState`**, gaining `coverage01`, `raininess01`,
`cloudBase`, `cloudThickness`, and a named preset id. Same epoch, same
smoothstep, same salt, **new component indices (4, 5, 6…) rather than bit-slices
of existing draws** — the rule that file already argues for.

The payoff is that this is free and it is what makes clouds *informative*: a
storm epoch is high coverage AND high raininess AND low cloud base AND hard wind,
because they come from one draw. An overcast that is not also the gusty tick is
a lie, and this construction makes that lie unrepresentable.

- `weatherAuto = false` pins everything — the existing switch already covers
  "I need two comparable screenshots", which is why the `wind` group has it.
- New `weather.*` tuning rows for manual coverage / raininess / base / thickness,
  so the tuner can hold a sky while you author it.
- Ships in `RenderParams` (floats, render side) **and** in `TickParams` as
  Q16.16 integers (`weatherRainQ`, `weatherCoverQ`) on the tick input stream, the
  `dayPhase` / `windDirQ` precedent. That is what lets the sim see rain
  deterministically without the renderer's floats ever reaching the CA — exactly
  the split `celestial.h` documents.

### 2.2 The cloud field: a pure function of position and time, like `windAt`

A new `assets/shaders/cloud.wgsl`, **not** `common.wgsl`. That is not style:
a `common.wgsl` edit misses the SPIR-V cache for every shader in the engine and
cost **536 s** of pipeline compile on 2026-09-08, against under 1 s for the same
constants placed next to their single consumer. Every cloud constant lives with
the cloud code.

`cloudDensity(worldXZ, altitude, t)`, built exactly as §1c specifies:

- **coverage** — low-frequency 2D fbm, multiplied by the regime's `coverage01`.
- **detail** — higher-frequency fbm, advected at a *different* rate from
  coverage, plus a slow drift on a third axis so shape evolves rather than
  merely translating.
- **density** = `smoothstep(1 - coverage, 1 - coverage + w, detail)` — coverage
  lowers the threshold, which is the whole trick.
- **altitude** — a vertical profile from `cloudBase` / `cloudThickness`: soft
  base, erosion toward the top, so the deck has a bottom you can fly under.

Advection direction and speed come from the regime wind, so **clouds drift the
way the grass leans**. That is one more thing that is free because of §2.1.

Two decks, not one (cheap, and the single biggest "the sky has depth" win): a
low cumulus deck carrying the weather, and a thin high cirrus deck at a
different altitude, scale and advection rate. One extra sample of the same
function with different parameters.

### 2.3 The render form: a separate half-res pass, NOT inside `fs()`

This constraint is from this repo, not the corpus, and it is the one that decides
the architecture.

`raymarch.wgsl`'s fragment entry has **no register headroom**. Adding per-ray
state inside `giGatherRays` took it from 128 to 168 registers, and the measured
"plant tax" turned out to be occupancy, not spill — `Local Memory Size` never
moved. A cloud march inside `trace()` is per-ray state on the hottest shader in
the engine, and §1b's *"really bad performance"* is the same wall approached
naively.

So:

- A new **`Table::CloudCache`** frame phase, following the `Table::ShadowCache`
  precedent already in `pass_table.h` (per-FRAME, not per-tick, recorded
  immediately before the render pass that consumes it).
- Target: half or quarter resolution RGBA — **cloud colour + transmittance**.
  Marched with a dithered start offset, the pattern `godRays()` already uses
  (screen-space and time-free, so it does not crawl).
- Upsample nearest-ish. **This is a feature**, not a concession — §1a is the
  entire argument, and it is Lin's own conclusion.
- Composite in `fs()` as **one texture fetch**, folded into the sky tiers.
  Because `skyAirglow()` is the fog / reflection / ambient target and
  `skyColorNoBodies()` is what `reflectionSky()` calls, an overcast automatically
  reaches distant fog, water reflections and the ambient lookup. The three-tier
  split already in the file pays for itself here; nothing else has to know.
- Temporal reprojection + blend in the cloud pass, then let TAA finish (§1g).
  Composite *before* TAA.
- **Run `--shader-stats` before and after.** The register cliff is the thing
  that kills this, and the one number that tells you is in
  `build/shader_stats.json`.

### 2.4 Cloud shadow and the overcast world

§1d, applied: `sunShadowAt()` already returns a scalar. Multiply it by
`cloudTransmittanceToSun(hitPos)` — one slab intersection against the cloud deck
plus a handful of density taps along the sun direction. No march, no new pass.

Two halves, and the second is the one people forget:

1. **The key light attenuates** — patchy sun under scattered cloud, gone under
   overcast. That is the shadow term above.
2. **The ambient and the shadow's softness change too.** §2.3 gives the darker,
   greyer `skyAirglow` for free. The remaining piece is that shadows go *diffuse*
   under cloud — a `TUNE_CLOUD_SHADOW_SOFTEN` widening the existing penumbra,
   not a new system. Without it an overcast reads as "someone turned the sun
   down" rather than as an overcast, which is the same failure mode
   `eclipseDim()`'s perceptual curve exists to avoid.

### 2.5 Rain: three tiers, and only the cheapest one is world-wide

**Tier 1 — cosmetic, on whenever it rains.** Analytic streaks in the composite
pass, each sample gated on the openness grid so rain stops under a roof and
keeps falling two metres away outside. Per-sample, not player-centred — §1e names
the failure mode precisely. No sim, no voxels, zero cost off-camera.

**Tier 2 — the world gets wet. This is the default way rain touches the sim.**
The staining and absorption systems already exist, already have authored data
(`water`'s `stain: {type: "wet", amount: 15, chance: 260, washes: true}`, and
substrate `absorb: {capacity: 12}`), and — critically — **already have sleep
discipline**: every step is monotone toward a bounded fixed point, so the chunks
settle and sleep. A rain tick applies wet-stain to sky-exposed surface cells at
a budgeted rate. No voxels created, bounded by construction, hash-visible, and
it makes rain *matter* — dark wet ground, and reactions that can key on it.

**Tier 3 — rain places water. OPT-IN, BUDGETED, default off.** §1f is
unanimous that this floods worlds. If it ships: a `MutationQueue` emitter with a
per-tick op budget **charged before emission** (the existing rule), only into
cells whose substrate absorption is already saturated, with the existing
water-body / evaporation path as the sink. Never the mechanism by which rain is
visible.

**The reaction gate.** Rain needs a *live* condition, so it is a new
`RCOND_RAIN` bit reading the `TickParams` integer from §2.1. Bits 0..7 of
`ReactionGpu.cond` hold 3 of 8 (`SKY`, `DAY`, `NIGHT`) — there is room. It is
explicitly **not** the existing `requires:` weather switch: that prunes the rule
out of the GPU table at *compile* time (`WeatherFlagEnabled`), which is exactly
right for `waterFreezes` and exactly wrong for a condition that changes every
few minutes.

### 2.6 What the cloud tells you — an authored taxonomy, not code

The "robust system we can iterate on" requirement lands here. Weather types are
**data**, following `assets/biomes/` and `assets/water/`:
`assets/weather/<name>.json` — clear, fair, scattered, overcast, drizzle, storm —
each naming its coverage range, raininess, base altitude, thickness, cloud
albedo/absorption, and a gameplay block. The regime draw in §2.1 selects a
*named* preset rather than raw scalars.

Three things that buys:

- **Legible.** Low dark base + high thickness + high coverage = rain is coming,
  and it *is* coming, because the same epoch draw that made the sky look that way
  set the raininess.
- **Iterable.** A new weather type is a JSON file plus a row in the tuner,
  validated by a data gate — the `scripts/test_environment.mjs` precedent, no
  rebuild.
- **Open-ended** (design guideline 4). Game systems hook weather **by name**:
  a reaction, a biome, a mob behaviour or a spell asks for `"storm"`, it does not
  switch on an enum in C++.

---

## 3. The rest of the "more interesting sky" list, ranked by look-per-cost

**Nearly free, and large:**

- **Cloud-reddened sunsets.** Falls out of §2.3 with no extra work — the cloud
  shading takes `keyLightColor()`, which already reddens through
  `sunTransmittance`. Cloud bottoms catching a low sun is the single most
  recognisable sky image there is and it costs nothing once B lands.
- **Rainbow.** An angular ring at 42° from the *anti*-solar point, gated on
  "raining AND sun up AND sun low". One dot product, one `smoothstep`. Highest
  look-per-instruction item on this entire document.
- **22° halo and sun dogs** through the high cirrus deck. Purely angular, gated
  on the high deck's presence. This file already draws a limb-darkened disc; a
  halo is the same kind of math and no march.

**Cheap:**

- **Crepuscular rays from cloud edges** — *not* a new march. Modulate the
  existing `godRays()` source term by cloud transmittance along the sun
  direction, which §2.4 has already computed. Shafts appear through gaps in the
  deck for the price of a multiply.
- **Clouds in water reflections and on the far cascades** — free via
  `reflectionSky()` / `skyAirglow()` (§2.3).

**Moderate, later:**

- **Cloudscape → cubemap** for GI and reflection sampling (§1i). Real win, but
  it is a new derived producer with an ownership question (design guideline 3),
  so it wants its own package.
- **Lightning as a real light.** `keyLightColor()` is already the single
  chokepoint every shading path agrees on, so a flash is a one-frame override
  there plus a thunder cue with a distance delay — not a new light system. The
  corpus wanted this repeatedly (*"when the thunder strikes it lights up the
  room"*).

**Deliberately not doing:** photoreal HZD / Frostbite multi-scatter volumetrics.
§1a is the whole argument, and it comes from the person whose look this engine
already references.

---

## 4. Packages, in order, each cheap to verify

Each one is independently shippable and each is verifiable in ONE `--verify`
launch — the "authoring cheap-to-verify work" rule.

**A — the weather regime.** `wind.h` → `weather.h` extension, `weather.*` tuning
rows, `TickParams` integers, tuner readout. No render change at all.
*Verify:* `--sweep weather.coverage=0,1` proves reachability with no file edited;
a new `weather` gate asserts purity (same tick → same state, forwards and
backwards) and that a storm epoch's four fields co-move. Cannot move the hash
until something reads the new `TickParams` words.

**B — cloud field + half-res pass + composite.** No shadow, no rain.
*Verify:* one `--verify` with `--shot-frames screenshot_sky,screenshot,screenshot_far`
plus `--budget-arms`, and `--shader-stats` compared against the current
`build/shader_stats.json`. **If the raymarch fs register count moved, stop and
fix that before anything else.**

**C — cloud shadow + overcast ambient + soften.**
*Verify:* one gate asserting the key light and the ambient both drop under a
pinned overcast tick and recover under a pinned clear one; one shot.

**D — rain tiers 1 and 2 + `RCOND_RAIN`.**
*Verify:* a `rain-wet` gate — sky-exposed grass gains wet stain under a rain
tick, roofed grass does not, and the awake-chunk count returns to rest
afterwards (rule 2, and the `terrain` gate's pass D already knows how to report
*which field* moved). The hash moves here, on purpose; `--selftest --rebaseline`
once, at the end, and move on.

**E — weather presets as JSON + a tuner page + the data gate.**

**F — the cheap angular extras:** rainbow, halo, second deck polish.

**G — optional/later:** tier-3 rain, lightning, cloud godrays, cubemap GI.

---

## 5. Risks, named

1. **The register cliff (§2.3).** The single most likely way this goes wrong.
   Measured with `--shader-stats`, never by eye.
2. **The `common.wgsl` compile cliff.** 536 s per iteration if cloud constants
   land there. They go in `cloud.wgsl`.
3. **Rule 2.** Rain is an unbounded emergent process unless budgeted, and the
   corpus has watched several people discover this (§1f). Tier 2 is bounded by
   construction because staining is monotone toward a fixed point; tier 3 is
   bounded only by an explicit op budget.
4. **Determinism split.** The regime rides `TickParams` as integers; the cloud
   *field* is float and render-only and must never be what the sim reads. This is
   exactly the split `celestial.h` documents, and copying it is the cheap way to
   get it right.
5. **The worst case is not the default.** Sky pixels are free today
   (`RS_PX_SKY`). Every perf number for this work must be taken at **full
   coverage**, looking up, in a `--budget-arms` arm that pins the weather.
6. **File contention, right now.** `bash scripts/board.sh active` shows
   `agent-f3eadd` holding `raymarch.wgsl`, `common.wgsl`, `tuning_params.def`,
   `tuning.json`, `tuner_schema.js` and `main.cpp` for a 50 m short-range fog
   option plus night-sky compositing (stars behind aurora/nebulae and the moon
   discs). **Package B collides with that head-on**, and their fog change touches
   the same `skyAirglow` fog target §2.3 relies on. Sequence after them, or split
   the claim explicitly.
