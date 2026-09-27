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
#include <cstdint>
#include <vector>

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
  // ---- where the energy goes (without these a stirred mix churns forever) --
  // Velocity kept per step, 1 - drag: a plain linear drag on every particle.
  float damping = 0.02f;
  // XSPH smoothing: each particle's velocity pulled this far toward its
  // neighbours' weighted mean. Eddies die; bulk flow is untouched.
  float xsph = 0.12f;
  // Glass contact: the part of a particle's motion ALONG the glass (relative
  // to the glass) lost per step; the part INTO or OFF it is set to the
  // glass's own -- no bounce, and a moving wall never gives a particle more
  // speed than it has itself.
  float wallFriction = 0.3f;
  // Steps a freshly seeded particle is heavily damped for, so a vessel opens
  // calm instead of relaxing its lattice with a bang.
  int calmSteps = 90;
  // A vessel sleeps once none of its liquid has moved faster than this (px
  // per step) for this many steps in a row.
  float sleepSpeed = 0.12f;
  int sleepSteps = 45;     // neighbour-list radius / h   // velocity cap as a fraction of h per step
  uint32_t seed = 0x5eed;
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

  // Adds a vessel at `x` holding `c`, seeded ALREADY SETTLED: substances
  // layered by density, heaviest at the bottom, liquids on a rest lattice,
  // powders packed. Returns the vessel index.
  int AddVessel(const VesselShape& shape, const Xform& x, const Composition& c);

  // Kinematic: the UI owns a vessel's TARGET pose; the vessel moves toward
  // it at most maxVesselStep px per substep. The glass pushes liquid and
  // sweeps grains; contents are never carried rigidly.
  void SetVesselXform(int v, const Xform& x);
  const Xform& VesselXform(int v) const { return vessels_[v].x; }
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
  // Read-only views for gates and the lab.
  const std::vector<V2>& Positions() const { return px_; }
  const std::vector<V2>& Velocities() const { return pv_; }

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
    std::vector<V2> outline;  // local, left lip -> bottom -> right lip
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
  };

  void BuildOutline(Vessel& v) const;
  bool InsideLocal(const Vessel& v, V2 local) const;
  bool InsideVessel(const Vessel& v, V2 world) const;
  V2 ToLocal(const Xform& x, V2 w) const;
  V2 ToWorld(const Xform& x, V2 l) const;

  void SeedVessel(int vi, const Composition& c);
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
  template <class F>
  void ForNeighbours(int i, F&& f) const;
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
  // Particles [0, nAct_) are awake; [nAct_, size) belong to sleeping vessels.
  int nAct_ = 0;
  std::vector<int> nbrStart_, nbr_;
  std::vector<V2> xs_;        // XSPH scratch

  // hash grid over h-sized cells
  int cellsW_ = 0, cellsH_ = 0;
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
  int highlight_ = -1;
  bool active_ = true;
  int grainMoves_ = 0;   // grains moved in the current step
  uint32_t rng_;
  uint32_t step_ = 0;
};

}  // namespace alchemy
