#pragma once
// THE ALCHEMY BENCH SIM: a 2D cross-section of one or two vessels, with
// liquids as particles and powders as pixels. A UI device, not world state.
//
// Liquids: Clavet et al. 2005 "Particle-based Viscoelastic Fluid Simulation"
// (double-density relaxation), the method behind grantkot.com/ll. Each
// substance has a particle MASS derived from its 3D-sim density, and the
// relaxation displacement of a pair is split by inverse mass, so heavy
// liquids sink through light ones with no buoyancy term. Two particles of
// DIFFERENT substances relax toward a rest density of zero — they only ever
// push — which is what makes a sharp interface instead of a blend.
//
// Powders: a falling-sand pixel CA on the same grid, one grain per pixel. A
// grain reads the liquid under it (the mean particle mass in that pixel) to
// decide whether it sinks, and how fast; moving into a pixel shoves that
// pixel's particles into the one it left. A RESTING grain is a wall to the
// liquid. A grain the stirring stick hits is FLUNG: it carries a velocity and
// moves ballistically until it slows, then rejoins the CA. A grain IN LIQUID
// with nothing holding it (StepGrains, "WET") is flung too, and carried by
// its liquid: dragged toward the liquid's velocity, settling through it at
// the CA's pace only inside a vessel (in flight the liquid is weightless and
// nothing settles), the drag paid back to the liquid equal and opposite; one
// moving faster than half a pixel a step is no wall -- so a falling blob of
// slime and dirt falls as one instead of standing on its own dirt.
//
// A MOVING VESSEL CARRIES ITS CONTENTS in its frame (SimConfig::vesselFeel):
// grains and liquid take the same whole-pixel shift a step, the liquid takes
// the vessel's acceleration less the part it feels as slosh.
//
// Units. The grid is `gridW x gridH` pixels, y UP (row 0 = bottom). One grain
// is one pixel of area and one UNIT; one eighth of a cell is
// `unitsPerEighth` units; a particle weighs `unitsPerParticle` units and is
// seeded on a hex lattice whose cell area is exactly that many pixels, and
// the rest density is measured from that lattice — so a settled liquid fills
// exactly the area its units say, same as a powder, and a vessel's interior
// area in pixels IS its capacity in units. Tally() counts units and is exact.
//
// Not the 3D sim: floats, no hashing, no MutationQueue. The panel turns what
// Tally() reports into an intent the game validates (conservation), which is
// the only way contents change. See DESIGN.md "Alchemy bench".
//
// CHEMISTRY (2026-09-27, docs/PLAN_alchemy_chemistry.md package C). The bench
// runs THE WORLD'S OWN RULES on its particles, grains and gas pixels
// (benchchem.h; flaskchem.cpp): a GAS phase (a pixel CA that rises, spreads,
// fades by the world's decay rules and leaves through the mouth into the
// world), STOPPERS, a BURNER (a virtual tag:hot neighbour through the glass),
// an ELECTRIFY discharge (a virtual spark neighbour), DISSOLVING by
// solutes.json (per-particle solute mass), a units LEDGER of every conversion
// and EVENTS for rules with effects (explode, and the pressure pop/burst).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "game/benchchem.h"
#include "game/composition.h"

#include <functional>

namespace alchemy {

// THE BENCH'S WORKERS. Runs fn(begin, end) over [0, n) in a FIXED split --
// the same chunks every call, whichever thread takes one -- on a few worker
// threads and the caller's. For loops whose iterations write disjoint data
// and read nothing another iteration writes (a row of the picture, a face of
// the flow), so the result is bit for bit the serial one. Serial when n is
// under `grain` per chunk, when the pool is already running a loop, and
// with SANDVOX_BENCH_THREADS=0 (the A/B arm).
void BenchParallelFor(int n, int grain, const std::function<void(int, int)>& fn);

struct V2 {
  float x = 0, y = 0;
};

// What the sim needs to know about one material. Built from a MaterialDef by
// the caller (SubstanceFromMaterial in the panel), so this header stays free
// of the engine.
struct Substance {
  uint16_t mat = 0;
  bool powder = false;     // else liquid
  int32_t density = 1000;  // the 3D sim's integer density: the ORDER is truth
  uint32_t moveEvery = 1;  // the 3D sim's viscosity
  uint32_t color[3]{};     // 0xAABBGGRR, the material's three palette entries
  // HOW IT LOOKS, from the same authored fields the world's renderer reads
  // (materials.h MaterialGpu): glow, media opacity, and the opaque flag that
  // makes lava a surface rather than a medium. See Render.
  uint8_t emission = 0;    // 0..255
  uint8_t opacity = 255;   // 0..255 media absorbance (liquids)
  bool opaque = false;     // kMatFlagOpaque
  // CHEMISTRY: what a rule's neighbour predicate reads (materials.h
  // MaterialGpu klass / tagMask), and the phase the bench keeps it in. A
  // GAS is a pixel-CA cloud, never a particle or a grain; a SOLID (a rule's
  // product -- lava and water make stone) is kept as grains (`powder` set).
  uint8_t klass = 2;       // 0 solid, 1 powder, 2 liquid, 3 gas
  uint32_t tagMask = 0;
  bool gas = false;
  bool heavy = false;      // a HEAVY gas (materials.h kMatFlagHeavyGas): it sinks and pools
};

// A vessel's inside, as an authored profile: half-widths at heights, both in
// 0..1 of the vessel's box, bottom to top, open at the top. Symmetric about
// the vertical axis. The last row is the lip.
struct VesselShape {
  std::vector<V2> profile;  // (halfWidth, height) pairs, height ascending
  float width = 80, height = 110;  // interior box in pixels
  float wall = 1.5f;               // glass half-thickness, pixels
  // A glass body is drawn as a round bottle, not a slice: its inside wears a
  // faint glass tint shaded across its width, over whatever it holds
  // (Render, "the glass body"). A sack (PouchShape) is not glass.
  bool glass = true;
};

// The shapes the game authors today. Data later (items.json `profile`).
VesselShape FlaskShape(float width, float height);
// A drawstring sack: wide belly, pinched neck, a short open ruff.
VesselShape PouchShape(float width, float height);
// The inside area of a shape, pixels (its profile at its box, closed across
// the mouth).
float ShapeArea(const VesselShape& s);
// Rescale a shape (keeping its proportions) so its USABLE inside is `area`
// pixels -- a vessel's CAPACITY in units -- so a full flask is drawn full.
// Usable: the polygon is the glass's centre line, and the inner half of the
// glass (plus half a pixel) holds nothing; sized to the polygon alone, a
// "full" pouch had no free pixel left and jammed on every tilt.
VesselShape ShapeWithArea(VesselShape s, float area);

struct Xform {
  V2 pos;         // world position of the vessel's bottom-centre
  float angle = 0;  // radians, CCW
};

struct SimConfig {
  int gridW = 224, gridH = 288;
  int unitsPerEighth = 6;
  int unitsPerParticle = 12;
  // px / step^2. Scaled with the particle spacing: the deeper a column is
  // in particles, the more a single relaxation pass must hold up, and past
  // this the bottom of a full flask oscillates for ever instead of resting.
  float gravity = 0.025f;
  float stiffness = 0.1f;      // Clavet k, per h
  float stiffnessNear = 0.4f;  // Clavet k_near, per h
  float kernelScale = 2.1f;    // h / lattice spacing
  // Relaxation passes a step, alternating direction (a symmetric
  // Gauss-Seidel). One pass in index order -- seeded bottom-up, so always
  // bottom first -- left the bottom 40 px of a deep flask boiling for ever
  // (RMS 0.1, peaks 0.9 px a step); two alternating passes hold it to a
  // sub-pixel shimmer. The second pass only PUSHES (see StepLiquid).
  int relaxIters = 2;
  bool relaxAlternate = true;
  // mass = exp(ceil * atan(gain/ceil * ln(density/1000))): near water it is
  // (density/1000)^gain, and it never exceeds exp(ceil*pi/2).
  float massGain = 5.0f, massCeil = 1.6f;
  float viscosity = 0.02f;     // sigma for moveEvery == 1; scales with it
  // Rest density a particle wants among OTHER substances, as a fraction of
  // its own-substance rest density. < 1 = immiscible (the interface pushes).
  float crossRest = 0.5f;
  // ...but a particle with almost none of its own kind round it (a stray
  // drop, same-kind share under this) is not held apart: its rest density
  // goes back toward its own as the share falls to zero. The repulsion
  // cleared a cavity twice a lone drop's size, and water plus its cavity
  // is lighter than oil -- so a shaken drop of water FLOATED in oil, and sat
  // on top of it, for ever. A body of liquid keeps its sharp edge.
  float crossLone = 0.25f;
  // Strength of the explicit buoyancy term (see StepLiquid).
  float buoyancy = 2.0f;
  // THE SORT DRIVE (x gravity): two particles of different liquids with the
  // heavier ABOVE the lighter are pushed apart vertically -- the heavy one
  // down, the light one up. Buoyancy against the neighbourhood's mean mass
  // moves only a pocket's rim (its inside sees its own kind), and a shaken
  // mix left water jammed between oil for ever; this acts at every inverted
  // interface, so a trapped pocket always works its way to its layer.
  // 5: a shaken flask is still jumbled when the hand stops and in its
  // layers within about ten seconds (lab, 2026-09-27: 1 left ~200 pairs out
  // of order after 35 s; 8 sorted it while it was still being shaken).
  float sortDrive = 5.0f;
  // A vessel whose liquids are still out of order does not sleep (more than
  // sortAwakePairs inverted pairs, or 1 in 200 of its particles) WHILE THE
  // COUNT IS STILL FALLING: once it has not improved for sortStallSteps (240
  // substeps a second: two seconds) what is left is pinned -- a film on the
  // glass, a ragged edge -- and the vessel may sleep.
  int sortAwakePairs = 3;
  int sortStallSteps = 480;
  // A powder lighter than the liquid under it SPREADS over the surface into
  // a skin instead of keeping its pile (StepGrains, "floating powder").
  bool floatSpread = true;
  // POWDER FLOWS (StepGrains). A grain that slides keeps going (Grain::slide):
  // down a 1:2 slope and, with way on, a pixel or two across the flat, so a
  // tipped vessel's surface runs off as an avalanche rather than one grain
  // at a time down a 45-degree face. A grain with air under it FALLS --
  // ballistic from grainFallStart px/step, accelerating to grainMaxFall,
  // carrying its slide sideways as grainSlideSpeed px/step each unit of it
  // (up to 3), plus up to grainFallJitter either way -- so what runs off a
  // lip arcs out and spreads instead of dropping as a CA column, a one-pixel
  // hourglass thread (every grain 1 px behind the last, none ever free); landing
  // turns grainSplash of its fall into sideways run; one that runs into a
  // grain still falling follows it instead. A still grain steps down a 1:2
  // slope with grainSlumpChance a step, so a heap creeps to ~27 degrees
  // instead of standing at the CA's 45 -- and a neck tipped past that runs.
  bool grainFlow = true;
  float grainSlideSpeed = 0.25f;
  float grainFallJitter = 0.04f;
  float grainFallStart = 1.0f;
  float grainSplash = 0.15f;
  float grainMaxFall = 1.5f;   // px/step: a falling grain's terminal speed
  float grainSlumpChance = 0.25f;
  float maxSpeedFrac = 0.4f;
  float listSlack = 1.15f;
  float maxVesselStep = 2.0f;  // px a vessel's outline may move per substep (< the glass)
  // A vessel is a HELD THING, not a cursor: it reaches its target pose with
  // at most this acceleration (x gravity) sideways or up, and this much
  // downward (more would leave its liquid weightless). The liquid feels the
  // vessel's acceleration as a tilt of gravity, tan = a/g, so these bound
  // how far a shake can throw it up the glass.
  float vesselAccel = 1.1f;
  float vesselDropAccel = 0.6f;
  // THE CONTENTS RIDE THEIR VESSEL'S FRAME (StepLiquid, CarryGrains). A held
  // vessel's liquid moves by the same whole-pixel shift as its grains (the
  // translation of its interior's centre), takes the vessel's acceleration
  // on every particle, and FEELS only `vesselFeel` of that acceleration
  // sideways as a fictitious force -- the slosh. Pushed by the glass alone
  // (1.0), the glass shoved the whole body through two relaxation passes:
  // a lift squeezed the liquid and drove it into the sand bed the carry had
  // lifted into it (lab, 2026-09-27: occupied area -26%, 170 particles in
  // grain pixels), and a shake threw it about as a spring (density +/-40%).
  // 0.5 keeps a shaken flask sloshing and mixing (alchemy-shake,
  // alchemy-resort: a hard shake still mixes water into oil) at +/-11%;
  // below ~0.45 a hard shake no longer mixes two liquids.
  // Turning is not carried: the glass turns round a level liquid, as round a
  // level sand bed.
  float vesselFeel = 0.5f;
  // ...and of its UP-AND-DOWN acceleration, which makes no slosh -- only
  // weight: felt in full, a lift loaded the liquid to 2g and squeezed it
  // 8% denser, and a drop let it swell.
  float vesselFeelLift = 0.1f;
  // ---- where the energy goes (without these a stirred mix churns forever) --
  // Velocity kept per step, 1 - drag, RELATIVE TO THE VESSEL the particle is
  // in (its rigid motion at that point). A world-frame drag acted on a moving
  // flask's liquid as a sideways gravity and ran it up the trailing wall and
  // out; relative to the glass it only calms the slosh.
  float damping = 0.004f;
  float airDamping = 0.002f;   // in flight, outside every vessel
  // XSPH smoothing: each particle's velocity pulled this far toward its
  // neighbours' weighted mean. Eddies die; bulk flow is untouched.
  // 0.2: at 0.12 a pour left the lip in globs that splashed off the rim of
  // the vessel under it (alchemy-pour).
  float xsph = 0.2f;
  // Glass contact: the part of a particle's motion ALONG the glass (relative
  // to the glass) lost per step; the part INTO or OFF it is set to the
  // glass's own -- no bounce, and a moving wall never gives a particle more
  // speed than it has itself.
  float wallFriction = 0.3f;
  // Steps a freshly seeded particle is heavily damped for, so a vessel opens
  // calm instead of relaxing its lattice with a bang.
  int calmSteps = 90;
  // A vessel sleeps once, over a window of sleepSteps undisturbed steps,
  // its liquid has drifted less than sleepDrift px RMS (at most 1% more
  // than 3x that) and no grain in it has moved: once the picture has stopped
  // changing, so the freeze is invisible (UpdateSleep).
  float sleepDrift = 0.6f;
  int sleepSteps = 64;
  // AddVessel runs the new vessel's contents to rest (in a scratch sim)
  // before it appears: it arrives settled, and asleep.
  bool settleOnAdd = true;
  uint32_t seed = 0x5eed;
  // ---- chemistry (flaskchem.cpp) --------------------------------------------
  // One chemistry step every `chemEvery` substeps. The bench steps 240
  // substeps a second, so 4 is 60 Hz = half a WORLD tick (30 Hz): a rule's
  // world chance is scaled by chemEvery / 8 x chemRate per step.
  int chemEvery = 4;
  float chemRate = 1.0f;
  // Rule firings per chemistry step (bounded). 600 starved a lit cloud: a
  // few thousand pixels of fire want ~700 decays a step on their own, and
  // the pixels at the tail of the list waited -- flames outlived the world's,
  // threw more fire, and the cloud grew (the ether fire). A firing is cheap
  // next to the per-pixel walk that finds it.
  int chemMaxFires = 4000;
  int gasEvery = 2;              // one gas step every this many substeps
  // Gas steps a cloud lingers outside every vessel before it starts to thin
  // into the room (gasFadeLight / gasFadeHeavy of it a step, stochastic):
  // what spills over a lip is SEEN to pour and creep, then goes into the
  // world gradually -- never a pixel blinking out.
  int gasVentSteps = 70;
  float gasFadeLight = 0.03f;
  float gasFadeHeavy = 0.015f;
  // GAS IS VOLUMINOUS (owner, 2026-09-27: "the volume of liquid to gas
  // conversion should generally always make more gas"). The gas grid counts
  // in GAS UNITS, `gasExpand` to one unit of matter: one liquid unit (one
  // pixel of liquid) becomes gasExpand gas units, and a cloud at its NATURAL
  // VOLUME (pure vapour at one atmosphere) holds `gasRest` units a pixel --
  // so vapour takes gasExpand / gasRest times the room its liquid did (8).
  // gasRest is the resolution of a concentration: a haze of vapour mixed
  // with air is a pixel holding fewer. Every boundary where gas meets matter
  // converts exactly: into the grid x gasExpand; out of it (a reaction, the
  // vent, a vessel taken off) through a per-(vessel, gas) BANK that pays
  // whole matter units and keeps the remainder (under one matter unit) as
  // live gas -- nothing is rounded away (AuditUnits).
  int gasExpand = 128;
  int gasRest = 16;
  int gasPixelCap = 51200;       // GAS units one pixel holds (3200 natural volumes)
  // ---- THE GAS FLOW (flaskchem.cpp StepGas) --------------------------------
  // A grid fluid (Stam 1999 "Stable Fluids"; Bridson, "Fluid Simulation for
  // Computer Graphics"; the method of Sebastian Lague's "Simulating Smoke"):
  // a MAC velocity grid of 2x2-pixel cells over the air round the gas, one
  // air -- the gas is carried in it. Per gas step: buoyancy (a heavy vapour's
  // concentration pulls its air down, a light gas's lifts it; hot glass
  // lifts), vorticity confinement, semi-Lagrangian self-advection, and a
  // pressure projection (red-black SOR) with glass, grains and liquid as
  // walls. Gas BORN (evaporation, a reaction) is a volume source: fresh
  // vapour pushes the air out of the mouth and itself follows. The gas units ride the velocity by upwind face fluxes
  // (integer, stochastically rounded: exact), so a flask fills from its
  // liquid up, brims, and a heavy vapour pours over the lip and down the glass.
  // Velocities are px per gas step.
  float gasBuoyancy = 0.3f;      // px/step^2 at one natural volume (x the gas's own weight)
  float gasHeatLift = 0.02f;     // px/step^2 by hot glass (x heat), near the glass
  float gasVorticity = 0.25f;    // vorticity confinement strength
  float gasDamping = 0.005f;     // velocity lost a step
  float gasExpandRate = 1.0f;    // share of a new gas's volume that pushes the air (1: all of it)
  float gasExcessDiffuse = 0.2f; // share of a pixel's excess over its natural volume passed on a step
  float gasMaxSpeed = 0.9f;      // px per gas step, any face (the transport's CFL)
  float gasDiffuse = 0.04f;      // molecular diffusion (a share of a difference a step)
  // EVAPORATION SATURATES out of glass: a liquid surface sees AIR only where
  // the vapour over it is thinner than this fraction of its natural volume
  // (GatherParticleNbrs; in a vessel the vessel-wide count decides).
  float gasSaturate = 0.9f;
  int gasPressureIters = 30;
  float gasSor = 1.7f;
  float gasJitter = 0.02f;       // random push a step where gas is, px/step (turbulence)
  // THE ROOM'S DRAUGHT (owner, 2026-09-27: "a small ambient wind ... pushes
  // gas around outside vessels, and near the top/mouth of an open flask
  // lightly draws gas out, like a breeze across a bottle mouth"). A slowly
  // varying, divergence-free breeze (a drifting uniform wind plus a lattice
  // of slow eddies, a pure function of the step count) that the AIR OUTSIDE
  // EVERY VESSEL is pulled toward, gasWindGrip of the difference a gas step.
  // Air inside glass never feels it; at an open mouth it adds to the mouth's
  // draw (below). gasWind: peak speed, px per gas step (0 = still room).
  // AlchemyBench sets it every frame from tuning.json `tools.alchemyWind`.
  float gasWind = 0.12f;
  float gasWindGrip = 0.06f;
  // THE MOUTH'S EXCHANGE, which a 2-px grid cannot resolve in a 12-px neck
  // (StepGas): in the top gasWindMouthDepth neck-widths of an open vessel,
  // gas drifts out through the mouth at gasMouthExchange (px per gas step,
  // the counter-flow of gas out and room air in) + gasWindMouth x the
  // breeze's speed there (a breeze across a bottle ventilates it), fading
  // with depth -- where the mouth faces the way the gas goes: up for a light
  // gas, down for a heavy vapour (upright, it lies in the bottle; held
  // mouth-down, it pours).
  float gasMouthExchange = 0.15f;
  float gasWindMouth = 3.0f;
  float gasWindMouthDepth = 4.0f;
  // THE BURNER. Heat 0..1 rises while it is on and the vessel stands on the
  // table (its base within burnerReach px of tableY), and falls off after.
  float heatRiseSec = 2.5f;
  float heatFallSec = 8.0f;
  float tableY = 0.0f;
  float burnerReach = 18.0f;
  int shockSteps = 36;           // chemistry steps one Electrify lasts (0.6 s)
  // PRESSURE, in ATMOSPHERES: gas units per free inside pixel of a stoppered
  // vessel, x (1 + 3 heat). A cloud at its natural volume holds one gas unit
  // a pixel (gasExpand above), so a headspace exactly full of vapour at room
  // temperature is 1 -- a stoppered flask of ether that has filled its
  // headspace with vapour holds (~1.1). Past popAt the stopper POPS; over a
  // lit burner or hot glass (heat > 0.25), or past burstAt, the vessel
  // BURSTS. Heat multiplies it by up to 4, so a full headspace heated goes.
  float popAt = 3.0f;
  float burstAt = 24.0f;
};

// Something the chemistry did that the game must answer (alchemy_bench.h
// BenchEvent): a rule with an effect fired (`kind` = the effect's kind), or a
// stoppered vessel's pressure popped the stopper ("pop") or broke the glass
// ("burst"). Several firings of one kind in one vessel in one step are one
// event with `count` firings.
struct SimEvent {
  std::string kind;
  int vessel = -1;           // AddVessel index, -1 = none
  V2 at;                     // sim pixels
  int32_t radius = 0, power = 0;
  float amount = 0;
  std::string what;
  uint16_t selfMat = 0, nbrMat = 0;   // the reacting materials (0 = none / virtual)
  std::vector<uint16_t> products;     // what the rule makes (material ids)
  int count = 1;
};

// WHERE EVERYTHING IN A VESSEL WAS when it left the bench (owner,
// 2026-09-29: "if it can remember the orientation and how exactly materials
// were laid out in a flask that would be ideal"). Positions are in the
// vessel's OWN frame (x across its axis, y up from its bottom), so the layers
// come back against the glass however the vessel stood; one put away tilted
// comes back upright and slumps in AddVessel's settle, as if time had passed.
// By MATERIAL ID, not substance slot: slots are per session.
//
// Only the picture of the grains and the liquid is kept. Dissolved matter and
// gas have no place of their own: SeedExtras spreads them from the
// Composition as it always has. `layered` -- the non-gas, undissolved
// portions it was taken with -- is the KEY: AddVessel uses the layout only if
// the contents' layered portions still equal it, and seeds by portion order
// otherwise. A few hundred to a few thousand entries a vessel.
struct VesselLayout {
  Composition layered;
  struct Grain { float x, y; uint16_t mat; uint8_t variant; };
  struct Drop { float x, y; uint16_t mat; uint16_t units; uint8_t var, heat; };
  std::vector<Grain> grains;
  std::vector<Drop> drops;
};

struct Tally {
  std::vector<Composition> vessel;  // one per AddVessel, in order
  Composition spilled;              // left the panel, or in flight at close
  uint32_t totalUnits = 0;
};

class FlaskSim {
 public:
  explicit FlaskSim(const SimConfig& cfg = SimConfig{});

  // Every substance the panel may see this session. Index = substance slot.
  void SetSubstances(const std::vector<Substance>& subs);

  // The world's rules and solutes in this sim's substance slots (benchchem.h,
  // built by flasksim_mats.h BuildBenchChemistry). Empty = no chemistry.
  void SetChemistry(const Chemistry& c);
  const Chemistry& Chem() const { return chem_; }
  // Pauses the rules (gas still moves and vents). AddVessel's settling runs
  // paused: a vessel arrives as it was put away, not half-reacted.
  void PauseChemistry(bool p) { chemPaused_ = p; }

  // Adds a vessel at `x` holding `c`, seeded ALREADY SETTLED: substances
  // layered by density, heaviest at the bottom, liquids on a rest lattice,
  // powders packed, gas in the headspace, dissolved portions spread through
  // their solvent. `stoppered` closes the mouth. Returns the vessel index.
  // With a `layout` whose key matches `c` (VesselLayout), the grains and
  // liquid go back where they were instead, then settle the same way.
  int AddVessel(const VesselShape& shape, const Xform& x, const Composition& c,
                bool stoppered = false, const VesselLayout* layout = nullptr);
  // THE PICTURE OF VESSEL `v` NOW (its grains and liquid, in its own frame).
  // Take it BEFORE RemoveVessel, then FinishLayout it with what came out.
  VesselLayout SnapshotVessel(int v) const;
  // Keys `L` to `c` (what the vessel was taken off with) and puts `c`'s
  // portions in bottom-up order by where they were in `L`: layered portions
  // by mean height, then dissolved and gas portions as they were.
  void FinishLayout(VesselLayout& L, Composition& c) const;
  // Would AddVessel use `L` for `c`?
  bool LayoutFits(const VesselLayout& L, const Composition& c) const;

  // ---- the vessel's devices (flaskchem.cpp) ----------------------------------
  // A stopper closes the mouth: nothing -- gas, liquid, powder -- leaves.
  void SetStopper(int v, bool on);
  bool Stoppered(int v) const { return VesselAlive(v) && vessels_[v].stoppered; }
  // The burner under a vessel, and how hot its glass is now (0..1).
  void SetBurner(int v, bool on);
  bool Burner(int v) const { return VesselAlive(v) && vessels_[v].burner; }
  float Heat(int v) const { return VesselAlive(v) ? vessels_[v].heat : 0.0f; }
  bool Burning(int v) const;   // the flame is lit under it right now
  // Electrify: everything it holds (liquid, grains, gas) sees a spark
  // neighbour for cfg.shockSteps steps.
  void Shock(int v);
  bool Shocked(int v) const { return VesselAlive(v) && vessels_[v].shock > 0; }
  // Gas inside a vessel, in GAS units (SimConfig::gasExpand to a unit of
  // matter); its pressure (gas units per free inside pixel, x heat: 1 = a
  // headspace full at the gas's natural volume) and that pressure as a
  // fraction of where the stopper pops.
  int GasUnits(int v) const;
  float Pressure(int v) const;
  float PressureFraction(int v) const { return Pressure(v) / std::max(1e-6f, cfg_.popAt); }
  int GasExpand() const { return GasE(); }
  // Gas units a pixel holds at the gas's natural volume (SimConfig::gasRest).
  int GasRest() const { return GasR(); }
  // For gates: inside pixels of vessel v holding gas, and inside pixels that
  // are AIR (no glass, grain, gas or liquid) -- what a surface can evaporate into.
  int GasPixelsIn(int v) const;
  int AirPixelsIn(int v) const;
  // For gates: evaporations (a particle decaying into a gas by a rule scaled
  // by its AIR neighbours) and how many of them had liquid right above them.
  int Evaporations() const { return evapFires_; }
  int BuriedEvaporations() const { return evapBuried_; }
  // A vessel whose glass broke (a burst): gone, its contents loose.
  bool Broken(int v) const { return v >= 0 && v < (int)vessels_.size() && vessels_[v].broken; }
  // Breaks a vessel's glass now (a rule-authored "burst" effect).
  void Burst(int v) { ShatterVessel(v); }
  // The chemistry's events since the last call.
  std::vector<SimEvent> TakeEvents();
  // THE LEDGER, in units per substance slot: what reactions made and
  // unmade. Matter never appears from nothing on the bench: every unit in it
  // was seeded, emitted, or is in `produced`, and AuditUnits proves it.
  const std::vector<int64_t>& Produced() const { return produced_; }
  const std::vector<int64_t>& Consumed() const { return consumed_; }
  // Live units per slot (every form: particle, grain, gas, dissolved, a
  // pending pool, the spill, what was taken off with a vessel or drained)
  // against seeded + produced - consumed. False (with `why`) on any gap.
  bool AuditUnits(std::string* why) const;
  int ReactionsFired() const { return firedTotal_; }
  // Units of `slot` dissolved anywhere (gates).
  int64_t DissolvedUnits(int slot) const;
  int GasPixelCount() const { return (int)gasList_.size(); }

  // Kinematic: the UI owns a vessel's TARGET pose; the vessel moves toward
  // it at most maxVesselStep px per substep. Its contents ride its frame's
  // translation (SimConfig::vesselFeel); its TURNING is the glass's alone --
  // the glass pushes liquid round and sweeps grains.
  void SetVesselXform(int v, const Xform& x);
  const Xform& VesselXform(int v) const { return vessels_[v].x; }
  // Moves a vessel's glass (and pose target) straight to `x` with no motion
  // profile: for placing, not carrying.
  void TeleportVessel(int v, const Xform& x);
  // Takes `turn` (a whole number of turns, radians) off the vessel's angle,
  // its previous angle and its target alike: the same pose, the same motion,
  // just counted from a nearer zero, so a vessel turned round and round never
  // has to unwind the turns to stand upright again.
  void UnwindAngle(int v, float turn);
  // Would vessel `v` at `x` keep its glass clear of every other vessel's?
  // Step() refuses moves that fail this; the panel asks it to steer.
  bool PoseClear(int v, const Xform& x) const;
  // Would a vessel of this shape, not yet on the bench, fit at `x`?
  bool ShapeClear(const VesselShape& shape, const Xform& x) const;

  // The room's draught (SimConfig::gasWind), px per gas step; 0 = still.
  void SetGasWind(float w) { cfg_.gasWind = std::max(0.0f, w); }

  // The stirring stick: a capsule a..b of radius r, or off.
  void SetStick(bool on, V2 a = {}, V2 b = {}, float r = 3);

  // Pours `units` grains of powder substance slot `sub` at `at`, moving `vel`.
  // Returns how many were placed (a full pixel refuses).
  int EmitGrains(int sub, V2 at, int units, V2 vel);
  // The substance slot of a material id, -1 if the sim has none.
  int SlotOf(uint16_t mat) const { return mat < slotOf_.size() ? slotOf_[mat] : -1; }
  const Substance& Sub(int slot) const { return subs_[slot]; }

  void Step(int substeps = 1);

  // Runs without input until nothing is in flight (or `maxSteps`), so a pour
  // that is mid-air at close lands where it was going.
  void Settle(int maxSteps);

  // Takes a vessel off the bench: what is INSIDE it goes with it (returned,
  // exact, in eighths by largest remainder over its own units); anything in
  // flight stays in the sim and lands wherever it lands. The index stays
  // valid and the vessel is inert from here on (no glass, no contents).
  Composition RemoveVessel(int v);
  bool VesselAlive(int v) const { return v >= 0 && v < (int)vessels_.size() && !vessels_[v].outline.empty(); }
  // The vessel's outline in world pixels (for the panel's hit tests).
  std::vector<V2> VesselOutline(int v) const;
  const VesselShape& Shape(int v) const { return vessels_[v].shape; }

  Tally Count() const;

  // What has left the bench since the last call, in WHOLE eighths, taken out
  // of the spill (the fraction of an eighth stays until more joins it, so
  // Count() keeps adding up). The bench streams it into the world as it
  // falls. `exitX` = the mean x (sim pixels) it left at, -1 if unknown.
  // `by` (optional) splits the SAME eighths by the vessel each left: the
  // vessel the matter was last inside (a particle's psrc_, a grain's src),
  // so two flasks spilling in one step each pour from their own lip. The
  // split is a partition of the return value: sum(by->vessel) + by->unknown
  // == it, eighth for eighth.
  struct SpillBy {
    std::vector<Composition> vessel;  // per FlaskSim vessel index
    Composition unknown;              // left no vessel we know of (loose gas, ...)
  };
  Composition DrainSpilled(float* exitX = nullptr, SpillBy* by = nullptr);

  // RGBA8 (0xAABBGGRR), gridW x gridH, row 0 = TOP (image order). Alpha 0
  // where there is nothing, so the panel's own backdrop shows through.
  void Render(std::vector<uint32_t>& out) const;

  int GridW() const { return cfg_.gridW; }
  int GridH() const { return cfg_.gridH; }
  int ParticleCount() const { return (int)px_.size(); }
  int AwakeParticleCount() const { return nAct_; }
  bool VesselAsleep(int v) const { return v >= 0 && v < (int)vessels_.size() && vessels_[v].asleep; }
  int GrainCount() const { return (int)grains_.size(); }
  float KernelRadius() const { return h_; }

  // For gates: mean height of each substance slot (px), -1 if absent.
  std::vector<float> MeanHeights() const;
  // For gates: how many particles/grains are moving faster than `speed`.
  int MovingCount(float speed) const;
  // For gates: a vessel's out-of-order liquid pairs at the last liquid step
  // (a heavier liquid over a lighter one), what keeps it awake to re-sort.
  int InvertedPairs(int v) const { return vessels_[v].inverted; }
  // For gates: the box the grains of slot `sub` occupy, {x0, x1, y0, y1}
  // in grid pixels ({-1,-1,-1,-1} for none): a floating heap is narrow and
  // tall, a skin wide and flat.
  std::array<int, 4> GrainBox(int sub) const {
    std::array<int, 4> r{-1, -1, -1, -1};
    for (const Grain& g : grains_)
      if (g.sub == sub) {
        const bool first = r[0] < 0;
        r[0] = first ? g.x : std::min(r[0], (int)g.x);
        r[1] = std::max(r[1], (int)g.x);
        r[2] = first ? g.y : std::min(r[2], (int)g.y);
        r[3] = std::max(r[3], (int)g.y);
      }
    return r;
  }
  // The vessel under a point: inside its outline or within `slack` px of its
  // glass; the most recently added wins (it is drawn on top). -1 for none.
  int HitVessel(V2 p, float slack = 6.0f) const;
  // Draw this vessel's glass lit (the one under the hand), -1 for none.
  void SetHighlight(int v) { highlight_ = v; }
  // Did the last Step change anything visible? (Awake liquid, a grain that
  // moved, a vessel that moved, the stick.) A quiet table need not be redrawn.
  bool Active() const { return active_; }
  // Does the picture move on its own even when nothing is simulated (glow,
  // fizz, the light on water)? The panel keeps redrawing it, slower.
  bool Animated() const { return !px_.empty() || !gasList_.empty() || anyDevice_ || !shards_.empty(); }
  // WHERE THE BENCH'S TIME GOES: ms summed per phase since ResetProfile, and
  // the peak of what drives each (for gates and SANDVOX_BENCH_PROF). A clock
  // read per phase a substep -- nothing next to the phases themselves.
  struct Profile {
    double move = 0, liquid = 0, grains = 0, gasFlow = 0, gasMove = 0;
    double chemPart = 0, chemGrain = 0, chemGas = 0, chemTail = 0, render = 0;
    // Inside the above: the gas flow's pressure solve and self-advection, the
    // gas's share of the picture.
    double flowSolve = 0, flowAdvect = 0, renderGas = 0;
    int steps = 0, gasSteps = 0, chemSteps = 0, renders = 0;
    int peakGasPx = 0, peakParticles = 0, peakGrains = 0, peakFires = 0;
    double Total() const {
      return move + liquid + grains + gasFlow + gasMove + chemPart + chemGrain + chemGas + chemTail;
    }
  };
  const Profile& Prof() const { return prof_; }
  void ResetProfile() { prof_ = Profile{}; }
  // For gates: live gas pixels per substance slot, and their units.
  void GasBySlot(std::vector<int>& pixels, std::vector<int64_t>& units) const {
    pixels.assign(subs_.size(), 0);
    units.assign(subs_.size(), 0);
    for (int k : gasList_)
      if (gasAmt_[k] && gasSub_[k] < subs_.size()) { pixels[gasSub_[k]]++; units[gasSub_[k]] += gasAmt_[k]; }
  }
  // Read-only views for gates and the lab.
  const std::vector<V2>& Positions() const { return px_; }
  const std::vector<V2>& Velocities() const { return pv_; }
  // Grain pixels in grain order (stable across a step unless a grain left
  // the table), for gates.
  void GrainPositions(std::vector<V2>& out) const {
    out.resize(grains_.size());
    for (size_t i = 0; i < grains_.size(); i++) out[i] = {(float)grains_[i].x, (float)grains_[i].y};
  }

 private:
  struct Vessel {
    VesselShape shape;
    Xform x, prevX, target;
    // SLEEP: a vessel whose liquid has come to rest stops being simulated
    // (its particles are kept after the awake ones and skipped) until
    // something disturbs it: it moves, the stick comes near, or anything not
    // its own enters its box.
    bool asleep = false;
    int quiet = 0;
    int inverted = 0;         // out-of-order liquid pairs, last liquid step (StepLiquid)
    int invBest = INT32_MAX;  // fewest seen since it was last disturbed (UpdateSleep)
    int sortStall = 0;        // steps since `inverted` last beat invBest
    bool grainBusy = false;   // a grain inside it moved this step
    // ---- chemistry devices (flaskchem.cpp) ----
    bool stoppered = false;
    bool burner = false;
    float heat = 0;           // 0..1, the glass
    int shock = 0;            // chemistry steps of discharge left
    bool broken = false;      // burst: gone, its contents loose
    V2 vel;                   // px / step (the motion profile, Step)
    // THE CONTENTS' FRAME (Step, FrameMotion): the velocity of the interior's
    // centre this step (px / step) -- what carries grains AND liquid -- and
    // its change since the last step.
    V2 frameVel, frameAcc;
    // ...and the WHOLE-PIXEL SHIFT it carries its contents by this step, the
    // sub-pixel remainder kept for the next: grains and liquid take the same
    // shift, so a carried pile and the liquid over it keep their places.
    V2 carryRem;
    int shiftX = 0, shiftY = 0;
    float angVel = 0;         // rad / step
    float reach = 1;          // farthest outline point from the pose origin
    std::vector<V2> outline;  // local, left lip -> bottom -> right lip
    // Near-glass raster in the vessel's frame (BuildOutline).
    enum : uint8_t { kNear = 0, kFarIn = 1, kFarOut = 2 };
    std::vector<uint8_t> near;
    float nx0 = 0, ny0 = 0;
    int nw = 0, nh = 0;
    uint8_t Near(V2 l) const {
      const int x = (int)std::floor(l.x - nx0), y = (int)std::floor(l.y - ny0);
      if (x < 0 || y < 0 || x >= nw || y >= nh) return kFarOut;
      return near[(size_t)y * nw + x];
    }
  };
  struct Grain {
    int16_t x, y;
    uint8_t sub, variant;
    float fx = 0, fy = 0;  // sub-pixel position while flung
    float vx = 0, vy = 0;  // non-zero = flung
    uint8_t moved = 0;     // step parity it last moved on
    // The vessel it was last seen clearly INSIDE (not in glass), -1 for
    // none. The glass sweep pushes a grain back to this side: judged from
    // where a grain already in the glass is, it can read as outside.
    int8_t home = -1;
    uint8_t chem = 0;      // chemistry step it last changed on (low byte)
    // Way on a resting grain (SimConfig::grainFlow): the sign is the way it
    // is sliding, the size (1..3) how many steps it has kept sliding. A step
    // it does not move clears it.
    int8_t slide = 0;
    // The vessel it was last inside, kept after it leaves (home goes to -1):
    // what a spill is attributed to (DrainSpilled's SpillBy).
    int8_t src = -1;
  };

  void BuildOutline(Vessel& v, bool raster = true) const;
  bool InsideLocal(const Vessel& v, V2 local) const;
  bool InsideVessel(const Vessel& v, V2 world) const;
  V2 ToLocal(const Xform& x, V2 w) const;
  V2 ToWorld(const Xform& x, V2 l) const;

  void SeedVessel(int vi, const Composition& c, const VesselLayout* layout = nullptr);
  void SeedFromLayout(int vi, const Composition& c, const VesselLayout& L);
  // `c`'s layered portions (no gas, nothing dissolved, nothing without a slot).
  Composition LayeredOf(const Composition& c) const;
  // ---- chemistry (flaskchem.cpp) ----
  struct ChemNb {
    uint8_t type;    // NbParticle / NbGrain / NbGas / NbAir / NbHeat / NbSpark
    int idx;         // particle, grain, pixel (gas, air)
    int slot;        // substance slot, -1 for air and virtual
    uint8_t dir;     // kChemDown / kChemUp / kChemSide
    V2 at;
  };
  enum : uint8_t { NbParticle = 0, NbGrain = 1, NbGas = 2, NbAir = 3, NbHeat = 4, NbSpark = 5 };
  struct Pool { int vessel; int sub; uint32_t units; double sx, sy, w; };
  void StepChemistry();
  void StepGas();
  void CarryGas();
  void UpdateDevices();
  void CheckPressure();
  bool TryRules(uint8_t selfType, int selfIdx, int slot, const std::vector<ChemNb>& nb, int hv, V2 at,
                double scale);
  // LAZY NEIGHBOURS (a gas pixel's): TryRules gathers them only once a rule's
  // roll has passed -- most of a cloud's pixels roll nothing in a step, and
  // gathering was most of what a chemistry step cost (the ether fire).
  struct PendingNb { bool on = false; int x = 0, y = 0, hv = -1; bool isGas = false; };
  PendingNb nbPending_;
  // Per slot, per rule: can its neighbour predicate match anything on the
  // bench this step (a present slot, the air, a live virtual neighbour)? A
  // rule that cannot is skipped without gathering (StepChemistry).
  std::vector<std::vector<uint8_t>> ruleLive_;
  // A FLAME (a slot carrying the burner's hot tags: fire, burning ether): its
  // decay to air is it BURNING OUT, not a vapour dispersing into the room.
  bool Flame(int slot) const { return chem_.heat.on && (subs_[slot].tagMask & chem_.heat.tags) != 0; }
  // A slow decay to air of a non-flame: dispersal (TryRules keeps it off in a
  // sealed vessel, for a heavy gas in any vessel, and for a gas in the room).
  bool Disperses(const ChemRule& r, int slot) const {
    return r.kind == kChemDecay && r.prodSelf == kChemAir && r.chance * 10u < chem_.chanceDen && !Flame(slot);
  }
  // Pixels within reach of a liquid particle (NearestParticle's widest query
  // from a pixel, 0.8 spacing), rebuilt with the buckets: a pixel off it has
  // no particle to find, and most of a cloud is off it.
  std::vector<uint8_t> nearLiq_;
  std::vector<int> nearLiqTouched_;
  void GatherParticleNbrs(int i, int hv, std::vector<ChemNb>& out);
  void GatherPixelNbrs(int x, int y, int hv, bool isGas, std::vector<ChemNb>& out);
  int NearestParticle(float x, float y, float r) const;
  int UnitsOf(uint8_t type, int idx) const;
  void ConvertEnt(uint8_t type, int idx, int q, int to, int hv);
  void TakeFromEnt(uint8_t type, int idx, int q, int hv);
  void Deposit(int sub, uint32_t q, V2 at, int hv);
  // `within`: -2 = anywhere; else the vessel whose inside the gas must stay
  // in (-1 = outside every vessel). A product never lands across the glass.
  // `reach`: how far (Chebyshev px) it looks for room round (x, y).
  bool AddGasAt(int sub, uint32_t& q, int x, int y, int within = -2, int reach = 3);
  // THE GAS BANK (SimConfig::gasExpand). Gas units leaving the grid for a
  // form counted in matter units go in here, per (bin, gas slot) -- bin 0 =
  // outside every vessel, v + 1 = vessel v -- and come out as the WHOLE
  // matter units they make; the remainder (< gasExpand) stays banked, live
  // gas of that slot in that bin (Count, AuditUnits, RemoveVessel read it).
  uint32_t BankGas(int bin, int slot, uint32_t gasUnits);
  int GasBin(int hv) const { return hv >= 0 && hv < (int)vessels_.size() ? hv + 1 : 0; }
  int GasE() const { return std::max(1, cfg_.gasExpand); }
  int GasR() const { return std::max(1, cfg_.gasRest); }
  // An entity's matter in GAS units (a particle or grain x gasExpand).
  int64_t FineOf(uint8_t type, int idx) const;
  void ReleaseSolute(int i, int hv);
  void AddPool(int vessel, int sub, uint32_t q, V2 at);
  void FlushPools();
  void CompactDead();
  int SpawnParticle(V2 p, int sub, int units, int home);
  // Every effect of a fired rule (ChemRule::fx .. fx + fxCount).
  void RaiseEvents(int hv, V2 at, uint16_t selfMat, uint16_t nbrMat, const ChemRule& r);
  void RaiseEvent(const ChemEffect& fx, int hv, V2 at, uint16_t selfMat, uint16_t nbrMat,
                  const ChemRule& r);
  void RaisePressureEvent(const char* kind, int v, float pressure);
  void ShatterVessel(int v);
  void SeedExtras(int vi, const Composition& c);
  void RenderChem(std::vector<uint32_t>& out) const;
  double Rand01() { return (Rand() & 0xFFFFFF) / 16777216.0; }
  static uint8_t SeedHeat(V2 p);
  void AdoptSettled(int vi, const FlaskSim& from);
  void MoveVessels();
  void RebuildWalls();
  void StepLiquid();
  void CollideLiquid();
  void StepGrains();
  void CarryGrains();
  void UpdateGrainHomes();
  void UpdateSleep();
  void Partition();
  // `glassReach`: how far (Manhattan px from the grain) the search may run
  // through glass; the wider retry is for a grain deep in the band.
  bool SweepChain(int gi, const Vessel& v, bool wantIn, int glassReach = 2);
  bool PoseClear(const Vessel& v, const Xform& x) const;
  bool GrainFree(int x, int y) const;
  // `within` as AddGasAt's: a reaction's product is placed only on its own
  // side of the glass (the ring search reaches 4 px, the wall is ~3.5).
  bool PlaceGrain(Grain g, int nearX, int nearY, int within = -2);
  // PlaceGrain, then -- for a DEPOSIT (a reaction's powder, a pool
  // flushing), which lands where its source was, often buried in a pile --
  // the nearest free pixel on the same side of the glass out to
  // kDepositReach.
  bool PlaceDeposit(Grain g, int nearX, int nearY, int within);
  void BuildCells();
  void BucketPixels();
  float LiquidMassAt(int x, int y, int* count, V2* vel) const;
  void ShoveLiquid(int fromX, int fromY, int toX, int toY);
  // `shove`: the liquid in (x, y) takes the pixel it left (ShoveLiquid).
  void MoveGrain(int gi, int x, int y, bool shove = true);
  // A grain's home vessel's frame velocity (Vessel::frameVel; none = 0).
  V2 frameVelOf(const Grain& g) const {
    return g.home >= 0 && g.home < (int)vessels_.size() && !vessels_[g.home].outline.empty()
               ? vessels_[g.home].frameVel : V2{};
  }
  // How fast a grain settles through the liquid at pixel k (px / step, the
  // CA's sinking rate): heavier and thinner, faster.
  float WetSettle(const Grain& g, float ml, size_t k) const;
  // The liquid round a grain takes back the change `dv` of the grain's
  // velocity its drag made (equal and opposite momentum, in units).
  void PayDrag(const Grain& g, V2 dv, float mg);
  uint32_t Rand();
  float Mass(int sub) const { return mass_[sub]; }

  SimConfig cfg_;
  std::vector<Substance> subs_;
  std::vector<float> mass_, visc_;
  std::vector<Vessel> vessels_;
  float spacing_ = 2.6f, h_ = 5.5f, rho0_ = 1, restArea_ = 6;

  // liquid particles (SoA)
  std::vector<V2> px_, pv_, pprev_;
  std::vector<uint8_t> psub_;
  std::vector<uint16_t> pw_;  // units this particle carries
  std::vector<float> mbar_;   // neighbourhood mean mass, last step
  std::vector<uint8_t> calm_; // steps of calm-in left
  std::vector<int8_t> phome_; // vessel a particle was last inside, -1 none
  std::vector<int8_t> psrc_;  // ...and the last one it WAS inside, kept once it leaves (spill attribution)
  std::vector<uint8_t> pvar_; // look: which palette entry + a phase, fixed per particle
  std::vector<V2> panc_;      // where it was when its vessel's quiet window began
  std::vector<uint8_t> pheat_; // look: molten matter's heat phase (SeedHeat)
  std::vector<uint8_t> psol_;  // solute species dissolved in it (0 = none)
  std::vector<uint16_t> pmass_; // units of that species' powder dissolved in it
  // Particles [0, nAct_) are awake; [nAct_, size) belong to sleeping vessels.
  int nAct_ = 0;
  std::vector<int> nbrStart_, nbr_, nbrFill_;
  std::vector<std::pair<int, int>> pairs_;   // each neighbour pair once
  std::vector<int> rcJ_;      // relaxation scratch: i's pairs inside h
  std::vector<float> rcQ_;
  std::vector<V2> rcN_;
  // StepLiquid scratch: the vessel whose FRAME each awake particle rode this
  // step (-1 none): its speed cap, its whole-pixel carry and its contacts.
  std::vector<int8_t> pframe_;
  std::vector<V2> xs_;        // XSPH scratch
  std::vector<float> xw_;
  std::vector<int> orderScratch_;

  // hash grid over h-sized cells
  int cellsW_ = 0, cellsH_ = 0;
  float cellSize_ = 6;
  std::vector<int> cellStart_, cellIdx_, cellOf_;

  // per-pixel particle buckets (coupling)
  // Particles by pixel: a head per pixel and a next per particle.
  std::vector<int> pixHead_, pixNext_, pixOf_;
  std::vector<float> fieldW_, fieldM_, fieldVisc_;
  std::vector<V2> fieldV_;

  // powder grid: grain index + 1, or 0
  std::vector<int> grid_;
  std::vector<Grain> grains_;
  std::vector<uint8_t> wall_;  // glass: the vessel's index + 1, 0 = none
  // INSIDE which vessel each pixel's centre is (index + 1, first vessel wins),
  // rasterized with the glass. The one answer to "whose is this grain": the
  // point-in-polygon test it replaced ran per grain per substep.
  std::vector<uint8_t> inside_;
  // SWEEP MEMO: pixels a failed sweep search already explored this pass
  // (stamped epoch*2 + side). Within one pass the free pixels only get
  // fewer, so a second search through them would fail the same way; without
  // this a tipped full pouch re-explored its whole pile per wedged grain.
  std::vector<uint32_t> bfsDead_;
  uint32_t deadEpoch_ = 0;
  // SAND SLEEPS in 16x16 tiles. A resting grain in a tile nothing touched
  // last step is skipped: it failed every move then and nothing within a
  // pixel of it has changed. Every writer of grid_ wakes the tiles round
  // what it wrote (Wake), a moving vessel wakes everything.
  std::vector<uint8_t> awake_, awakeNext_;
  std::vector<uint8_t> sandTile_;   // tiles holding any grain, this step
  int tilesW_ = 0, tilesH_ = 0;
  bool wakeAll_ = true;
  void Wake(int x, int y) {
    for (int dy = -1; dy <= 1; dy += 2)
      for (int dx = -1; dx <= 1; dx += 2) {
        const int tx = (x + dx) >> 4, ty = (y + dy) >> 4;
        if (tx < 0 || ty < 0 || tx >= tilesW_ || ty >= tilesH_) continue;
        awake_[ty * tilesW_ + tx] = awakeNext_[ty * tilesW_ + tx] = 1;
      }
  }
  // The liquid field's written box (cleared next step instead of the grid).
  int fbx0_ = 0, fby0_ = 0, fbx1_ = -1, fby1_ = -1;
  std::vector<int> wedged_;    // grains the last sweep could not place
  std::vector<int> bfsParent_, bfsQueue_;
  std::vector<uint32_t> bfsSeen_;
  uint32_t bfsStamp_ = 0;

  bool stickOn_ = false;
  V2 stickA_, stickB_, stickPrevA_, stickPrevB_;
  float stickR_ = 3;

  std::vector<uint32_t> spilledUnits_;  // per substance slot
  // Where spilled matter left the table since the last DrainSpilled, as a
  // unit-weighted sum of x.
  double exitSum_ = 0, exitW_ = 0;
  void NoteExit(float x, uint32_t units) { exitSum_ += (double)x * units; exitW_ += units; }
  // WHICH VESSEL it left, per substance slot, in units: [vessel][slot]. Only
  // proportions -- the spill's truth is spilledUnits_; DrainSpilled splits
  // the eighths it drains by this and clamps it to what is still pending.
  std::vector<std::vector<uint32_t>> exitBy_;
  void NoteFrom(int vessel, int sub, uint32_t units) {
    if (vessel < 0 || sub < 0 || !units) return;
    if ((size_t)vessel >= exitBy_.size()) exitBy_.resize((size_t)vessel + 1);
    std::vector<uint32_t>& row = exitBy_[(size_t)vessel];
    if ((size_t)sub >= row.size()) row.resize(spilledUnits_.size() > (size_t)sub ? spilledUnits_.size() : (size_t)sub + 1, 0u);
    row[(size_t)sub] += units;
  }
  int highlight_ = -1;
  // ---- the look (Render): derived per substance at SetSubstances ----------
  struct Look {
    uint32_t col[3];   // display colours (the palette is linear light)
    uint8_t kind;      // LookKind
    float film, absorb;  // alpha = 1 - (1 - film) e^(-absorb * depth)
    float glow;        // 0..1
    uint8_t alpha[256];  // by depth below the surface
  };
  std::vector<Look> look_;
  // x, y: world pixels, re-derived each picture from lx, ly in the frame of
  // the vessel it rose in (-1: none), so a carried flask carries its bubbles.
  struct Bubble { float x, y, vy; uint8_t sub; uint8_t age; int8_t vessel; float lx, ly; };
  mutable std::vector<Bubble> bubbles_;
  mutable uint32_t lookRng_ = 0x9e3779b9u;
  mutable uint32_t lastRenderStep_ = 0;
  mutable std::vector<float> rTotal_, rBest_, rGlow_;
  mutable std::vector<int> rOwner_;
  mutable std::vector<uint8_t> rDepth_;
  mutable std::vector<float> rHeat_;
  mutable std::vector<uint8_t> rGlowOn_;
  // ---- chemistry state (flaskchem.cpp) ----
  Chemistry chem_;
  bool chemOn_ = false, chemPaused_ = false;
  std::vector<int> slotOf_;             // material id -> substance slot
  std::vector<int64_t> produced_, consumed_, seeded_, removed_, drained_;
  // THE GAS PHASE: per pixel, which gas (slot, 0xFF none), how many units,
  // and how many gas steps it has spent outside every vessel.
  std::vector<uint8_t> gasSub_;
  std::vector<uint16_t> gasAmt_;
  std::vector<uint8_t> gasAge_;
  std::vector<int> gasList_;            // pixels holding gas (may hold stale zeros)
  std::vector<uint8_t> gasListed_;
  std::vector<Pool> pools_;             // units waiting to become a particle / grain (matter units) / gas (GAS units)
  std::vector<uint32_t> gasBank_;       // (vessels + 1) x slots, gas units (BankGas)
  std::vector<uint32_t> emitCredit_;    // (vessels + 1) x product slots: gas units a gas's emits owe, under a unit
  int evapFires_ = 0, evapBuried_ = 0;
  std::vector<int> chemHead_, chemNext_, chemTouched_;   // particles by pixel
  std::vector<uint8_t> present_, activeSlot_;
  // Per vessel, its gas in per-mille of an atmosphere as of this chemistry
  // step's start: from 970 (stoppered; 1100 open) it evaporates nothing (GatherParticleNbrs), from
  // 900 a heavy vapour spills over its lip (StepGas).
  std::vector<int> vesselAir_;
  std::vector<uint8_t> grainDead_;      // grains a chemistry step removed (compacted at its end)
  // THE GAS FLOW (StepGas): a MAC grid of kGasCell-pixel cells, velocities
  // in px per gas step. Only the cells on ACTIVE TILES (below) are stepped;
  // everything else is still air at zero pressure.
  // 2 px: the pressure solve is a quarter of the cells a 1-px grid had (the
  // units still move pixel by pixel; the velocity is sampled bilinearly).
  static constexpr int kGasCell = 2;
  // THE ACTIVE TILES (kGasTile cells square): a tile is stepped when it
  // holds gas, touches a tile that does, or lies on a vessel holding some.
  // Everything else is still air at zero pressure -- two flasks at the ends
  // of the bench are two small solves, not one bench-wide box.
  static constexpr int kGasTile = 8;
  int gtlW_ = 0, gtlH_ = 0;
  std::vector<uint8_t> gTileOn_, gTileWas_, gTileSeed_;
  std::vector<uint8_t> gasHolds_;
  int gcW_ = 0, gcH_ = 0;
  std::vector<float> gu_, gv_;          // (gcW+1) x gcH, gcW x (gcH+1)
  std::vector<float> gu2_, gv2_;        // advection scratch
  std::vector<float> gp_;               // pressure, warm-started
  std::vector<float> gRhs_;             // over the padded box
  std::vector<V2> gcSolidV_;            // a solid cell's velocity (world frame)
  std::vector<uint8_t> gcFrame_;        // the vessel (index + 1) a cell's air is inside, 0 none
  std::vector<float> gcDen_;            // natural volumes of gas per free pixel
  std::vector<float> gcBirth_;          // gas units born in the cell since the last gas step (Deposit)
  std::vector<float> gcBuoy_;           // buoyant acceleration, px/step^2 (+ up)
  std::vector<int> gcComp_, gcQueue_;   // over the padded box
  std::vector<uint8_t> gLf_;            // the padded box: 0 air, 1 solid, 2 open
  std::vector<float> gLp_, gInv_;       // padded-box pressure; 1 / neighbour count per air cell
  std::vector<int> gRed_;               // air cells, red then black
  std::vector<V2> gConf_;               // vorticity confinement force per box cell
  std::vector<float> gcCurl_;
  // THE LOOK's advected texture (Neyret 2003, "Advected Textures"): two
  // layers of texture coordinates per cell (x, y each), carried by the
  // velocity and reset to the cell's own position in turn, half a period
  // apart -- the wisps move WITH the gas and stand still when it does.
  static constexpr int kTexCell = 4;
  int gtW_ = 0, gtH_ = 0;
  std::vector<float> gtc_;              // 5 per texel: layer A (x, y), layer B (x, y), phase
  std::vector<float> gtcOld_;
  uint32_t gtcStep_ = 0;
  std::vector<uint8_t> liqPx_;          // pixels liquid holds (the gas's walls)
  std::vector<V2> liqCellV_;            // mean liquid velocity per cell (world)
  std::vector<float> liqCellN_;
  std::vector<V2> gasPrevVel_;          // per vessel: its velocity at the last gas step
  std::vector<Xform> gasPrevPose_;      // per vessel: its pose at the last gas step
  int gbx0_ = 0, gby0_ = 0, gbx1_ = -1, gby1_ = -1;   // the active box, cells
  struct GasMove { int from, to; uint16_t n; uint8_t sub, age; };
  std::vector<GasMove> gasMoves_;
  std::vector<int> flowK_, flowTo_;     // StepGas's flowing pixels, and per face where to (-1 none, -2 the world)
  std::vector<float> flowF_;            // ... and per face the flux, units
  std::vector<uint16_t> flowN_;         // ... and per face the whole units it moves
  void EnsureGasFlow();
  void StepGasFlow(int bx0, int by0, int bx1, int by1);
  // The room's draught: this gas step's phases, from the step count (StepGas
  // sets them), and its sines per column / row of faces (StepGasFlow).
  void SetWindPhase();
  V2 WindAt(V2 p) const;   // the breeze at a point, px per gas step
  float windU0_ = 0, windPhX_ = 0, windPhY_ = 0;
  // Per open vessel holding gas, this gas step: its mouth's draw (StepGas).
  struct MouthDraw { Xform x; V2 lm; float hw, depth, speed; V2 up; float x0, x1, y0, y1; };
  std::vector<MouthDraw> draws_;
  std::vector<float> windCol_, windRow_;
  // A face velocity at a point (px), bilinear; `from` another field of the same shape.
  float SampleGu(float x, float y, const float* from = nullptr) const;
  float SampleGv(float x, float y, const float* from = nullptr) const;
  void RenderGas(std::vector<uint32_t>& out) const;
  std::vector<uint32_t> gasMark_;       // gas pixels a gas step already moved into (its stamp)
  uint32_t gasStamp_ = 0;
  std::vector<int> gasOrder_;
  std::vector<SimEvent> events_;
  // Glass flying from a burst (look only), advanced by the picture.
  struct Shard { float x, y, vx, vy; int life; };
  mutable std::vector<Shard> shards_;
  mutable uint32_t shardStep_ = 0;
  mutable std::vector<float> rGas_;
  mutable std::vector<uint8_t> rGasSub_;
  mutable std::vector<float> rGasTmp_;
  mutable std::vector<float> rGasAcc_;
  void BucketChem();
  std::vector<ChemNb> nbScratch_;
  uint32_t chemStep_ = 0;
  int firedThisStep_ = 0, firedTotal_ = 0;
  bool needPartition_ = false, anyDevice_ = false;
  void AddGasPixel(int k) {
    if (!gasListed_[k]) { gasListed_[k] = 1; gasList_.push_back(k); }
  }
  bool active_ = true;
  mutable Profile prof_;
  static double NowMs();   // steady clock, ms (the profile's)
  int grainMoves_ = 0;   // grains moved in the current step
  uint32_t rng_;
  uint32_t step_ = 0;
};

}  // namespace alchemy
