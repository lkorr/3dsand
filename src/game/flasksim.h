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
// pixel's particles into the one it left. Grains are walls to the liquid.
// A grain the stirring stick hits is FLUNG: it carries a velocity and moves
// ballistically (dragged by the liquid) until it slows, then rejoins the CA.
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
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "game/benchchem.h"
#include "game/composition.h"

namespace alchemy {

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
  // Strength of the explicit buoyancy term (see StepLiquid).
  float buoyancy = 2.0f;
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
  int chemMaxFires = 600;        // rule firings per chemistry step (bounded)
  int gasEvery = 2;              // one gas CA step every this many substeps
  int gasVentSteps = 70;         // gas steps a cloud lingers outside every vessel before it is in the world
  int gasPixelCap = 400;         // units one gas pixel holds
  // THE BURNER. Heat 0..1 rises while it is on and the vessel stands on the
  // table (its base within burnerReach px of tableY), and falls off after.
  float heatRiseSec = 2.5f;
  float heatFallSec = 8.0f;
  float tableY = 0.0f;
  float burnerReach = 18.0f;
  int shockSteps = 36;           // chemistry steps one Electrify lasts (0.6 s)
  // PRESSURE: mean gas units per free inside pixel of a stoppered vessel at
  // which the stopper pops -- or, over a lit burner or hot glass (heat > 0.25) or past
  // passes burstAt, the vessel BURSTS. A gas unit is the matter of a liquid
  // unit (the ledger counts them alike), so a flask of water boiled wholly to
  // steam holds ~1 unit per free pixel; real steam would be 1600x the volume.
  // Hence a pop well under 1: a stoppered flask on the burner goes.
  float popAt = 0.6f;
  float burstAt = 3.0f;
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
  int AddVessel(const VesselShape& shape, const Xform& x, const Composition& c,
                bool stoppered = false);

  // ---- the vessel's devices (flaskchem.cpp) ----------------------------------
  // A stopper closes the mouth: nothing -- gas, liquid, powder -- leaves.
  void SetStopper(int v, bool on);
  bool Stoppered(int v) const { return VesselAlive(v) && vessels_[v].stoppered; }
  // The burner under a vessel, and how hot its glass is now (0..1).
  void SetBurner(int v, bool on);
  bool Burner(int v) const { return VesselAlive(v) && vessels_[v].burner; }
  float Heat(int v) const { return VesselAlive(v) ? vessels_[v].heat : 0.0f; }
  bool Burning(int v) const;   // the flame is lit under it right now
  // Electrify: its liquid sees a spark neighbour for cfg.shockSteps steps.
  void Shock(int v);
  bool Shocked(int v) const { return VesselAlive(v) && vessels_[v].shock > 0; }
  // Gas inside a vessel, units; its pressure (units per free inside pixel).
  int GasUnits(int v) const;
  float Pressure(int v) const;
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
  // it at most maxVesselStep px per substep. The glass pushes liquid and
  // sweeps grains; contents are never carried rigidly.
  void SetVesselXform(int v, const Xform& x);
  const Xform& VesselXform(int v) const { return vessels_[v].x; }
  // Moves a vessel's glass (and pose target) straight to `x` with no motion
  // profile: for placing, not carrying.
  void TeleportVessel(int v, const Xform& x);
  // Would vessel `v` at `x` keep its glass clear of every other vessel's?
  // Step() refuses moves that fail this; the panel asks it to steer.
  bool PoseClear(int v, const Xform& x) const;
  // Would a vessel of this shape, not yet on the bench, fit at `x`?
  bool ShapeClear(const VesselShape& shape, const Xform& x) const;

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
  Composition DrainSpilled(float* exitX = nullptr);

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
    bool grainBusy = false;   // a grain inside it moved this step
    // ---- chemistry devices (flaskchem.cpp) ----
    bool stoppered = false;
    bool burner = false;
    float heat = 0;           // 0..1, the glass
    int shock = 0;            // chemistry steps of discharge left
    bool broken = false;      // burst: gone, its contents loose
    V2 vel;                   // px / step (the motion profile, Step)
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
  };

  void BuildOutline(Vessel& v, bool raster = true) const;
  bool InsideLocal(const Vessel& v, V2 local) const;
  bool InsideVessel(const Vessel& v, V2 world) const;
  V2 ToLocal(const Xform& x, V2 w) const;
  V2 ToWorld(const Xform& x, V2 l) const;

  void SeedVessel(int vi, const Composition& c);
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
  void GatherParticleNbrs(int i, int hv, std::vector<ChemNb>& out);
  void GatherPixelNbrs(int x, int y, int hv, bool isGas, std::vector<ChemNb>& out);
  int NearestParticle(float x, float y, float r) const;
  int UnitsOf(uint8_t type, int idx) const;
  void ConvertEnt(uint8_t type, int idx, int q, int to, int hv);
  void TakeFromEnt(uint8_t type, int idx, int q, int hv);
  void Deposit(int sub, uint32_t q, V2 at, int hv);
  bool AddGasAt(int sub, uint32_t& q, int x, int y);
  void ReleaseSolute(int i, int hv);
  void AddPool(int vessel, int sub, uint32_t q, V2 at);
  void FlushPools();
  void CompactDead();
  int SpawnParticle(V2 p, int sub, int units, int home);
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
  bool SweepChain(int gi, const Vessel& v, bool wantIn);
  bool PoseClear(const Vessel& v, const Xform& x) const;
  bool GrainFree(int x, int y) const;
  bool PlaceGrain(Grain g, int nearX, int nearY);
  void BuildCells();
  void BucketPixels();
  float LiquidMassAt(int x, int y, int* count, V2* vel) const;
  void ShoveLiquid(int fromX, int fromY, int toX, int toY);
  void MoveGrain(int gi, int x, int y);
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
  std::vector<Pool> pools_;             // units waiting to become a particle / grain / gas
  std::vector<int> chemHead_, chemNext_, chemTouched_;   // particles by pixel
  std::vector<uint8_t> present_, activeSlot_;
  std::vector<uint8_t> grainDead_;      // grains a chemistry step removed (compacted at its end)
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
  void BucketChem();
  std::vector<ChemNb> nbScratch_;
  uint32_t chemStep_ = 0;
  int firedThisStep_ = 0, firedTotal_ = 0;
  bool needPartition_ = false, anyDevice_ = false;
  void AddGasPixel(int k) {
    if (!gasListed_[k]) { gasListed_[k] = 1; gasList_.push_back(k); }
  }
  bool active_ = true;
  int grainMoves_ = 0;   // grains moved in the current step
  uint32_t rng_;
  uint32_t step_ = 0;
};

}  // namespace alchemy
