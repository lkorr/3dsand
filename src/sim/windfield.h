#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/wind.h"
#include "sim/windprim.h"
#include "sim/world.h"

// windfield.h — the WIND FIELD BLOCK: everything the ambient field needs per
// tick that is neither a compile-time TUNE_* constant nor one of the three
// weather words (windDirQ / windSpeedQ / windGustQ).
//
// docs/RESEARCH_wind.md §13 is the model; DESIGN.md §9b the invariants.
// world.h (kWindTerrN's note) is the GPU layout: a flat `wf*` member list
// that TickParams and RenderParams both carry, filled here and nowhere else.
//
// WHAT LIVES HERE, and why each piece is a pure function:
//
//   * THE GUST-FRONT ADVECTION CLOCK. A gust band is sin(K * (s - X(t))),
//     s the distance along the wind, X(t) how far the air has travelled. For
//     the fronts to cross a meadow at the wind's speed X has to be the
//     INTEGRAL of the speed, not speed x time — U changes, and U(t) * t would
//     make every change in the weather fling the whole pattern by U' * t,
//     which after an hour is kilometres. An integral is state, and this engine
//     keeps none; so it is a SUM over fixed 16-tick blocks of the reference
//     speed WindWeatherQ already produces, memoised per (seed, tuning). The
//     memo is a cache, not state: recomputing it from tick 0 gives the same
//     integers, which is what makes it replay-safe.
//
//   * THE TERRAIN TABLE (§13.2): ground height, exposure and water fraction
//     per 32-voxel cell, 204.8 m around the window, from World::TerrainColumn
//     -- worldgen height, a pure function of (seed, map, cell). Cached by
//     world cell; a window shift queries one new row or column.
//   * THE HEIGHT PROFILE: the log-law as a 16-knot table built with exact
//     integer log2 (imath::Log2Q16), so the sim's lookup is the same
//     integers on every machine.

namespace windfield {

// Ticks per advection block: the reference speed is sampled once per block
// and held across it. 16 ticks = 0.53 s — far below any weather change, and
// the sum stays exact because a step function integrates exactly.
constexpr uint32_t kAdvBlock = 16;

// The advection clock at `tick` + `frac` (frac in [0, 1) of a tick, 0 for the
// sim). BAM16 of the BASE gust band (coefficient 1.0), reduced mod 10 turns
// (655360): every band coefficient is a multiple of 0.1, so band c's clock is
// (phase * c10 / 10) and wraps at a whole number of turns exactly when this
// does. The spatial frequency it is scaled by is GustKBase, the CPU twin of
// the shader's WINDQ_K_BASE.
uint32_t AdvPhase(const Tuning& t, uint32_t seed, uint32_t tick, float frac);

// The air's displacement since tick 0, METRES, as the weather's reference
// wind carried it: the same sum AdvPhase takes, as a vector. The clouds drift
// by this (times their altitude factor), so the sky and the grass are moved
// by one wind.
void AirDrift(const Tuning& t, uint32_t seed, uint32_t tick, float frac,
              double& dxM, double& dzM);

// WINDQ_K_BASE, on the CPU: round(65536 / wavelength-in-cells), capped at
// 27000, in f32 operation for operation like the shader const (WINDQ_K_BASE).
// A one-unit disagreement would only scale the front speed by ~1/1365.
int32_t GustKBase(const Tuning& t);

// Fill the wf* block of a TickParams / RenderParams. `tick` + `frac` is the
// instant the clocks are evaluated at (frac = 0 for the sim).
void FillWindField(TickParams& tp, const Tuning& t, uint32_t seed, uint32_t tick,
                   uint32_t dayPhase);
// The render copy takes the window origin explicitly: WriteRenderParams fills
// its own origin later than its wind block.
void FillWindField(RenderParams& rp, const Tuning& t, uint32_t seed, uint32_t tick,
                   float frac, uint32_t dayPhase, const int32_t origin[3]);

// Column queries the terrain table's last (re)build paid -- the dev readout.
uint32_t TerrainQueries();

// ---- the regime library (assets/wind/regimes.json) ------------------------
// Named presets of the three regime inputs. Sky presets name one in their
// "wind" field; wind.regime pins one. File order is the picker's order.
struct Regime {
  std::string name, label, about;
  float intensity = 0.32f, gale = 0.0f, convective = 0.0f;
};
const std::vector<Regime>& Regimes();
const Regime* FindRegime(const std::string& name);
void ReloadRegimes();
uint64_t RegimeFingerprint();

// ---- the pieces of WindWeatherQ that stage 4 owns --------------------------
// The convective storm timeline at `tick`, weighted by `convective` (Q16):
// fills o.storm* and returns the speed envelope (Q16), the extra gust
// fraction (Q16) and the heading jump (BAM32).
void StormTimeline(const Tuning& t, uint32_t seed, uint32_t tick, int32_t convective,
                   WindStateQ& o, int64_t& envelope, int64_t& stormGust,
                   uint32_t& jumpBam);
// Slope and sea/lake breezes from the stability S (Q16, [-1, 1]) and the
// mean speed (they fade out as the synoptic wind rises).
void LocalWinds(const Tuning& t, WindStateQ& o, int64_t speedQ, int64_t S);

// THE STORM'S PRIMITIVES at `tick`: the gust-front jet and the downbursts,
// resolved, written to out[0 .. return), `cap` at most, and folded into the
// union box lo/hi (which the caller seeds, empty or with its own list). A
// pure function of (tuning, seed, tick, window origin) — SubmitTick appends
// them to the tick's list and WriteRenderParams to the render copy, so the
// grass and the smoke feel the same downburst. Air only: no entrainment
// licence, so no footprint wake.
uint32_t WeatherPrims(const Tuning& t, uint32_t seed, uint32_t tick,
                      const int32_t origin[3], WindPrimGpu* out, uint32_t cap,
                      int32_t lo[3], int32_t hi[3]);

// ---- the C++ mirror of windAtQ: readouts and the wind-field gate ------------
// The ambient INTEGER field (primitives excluded) at world cell (x, y, z) for
// `tick`, in its component parts, metres per second. Resolved from the same
// wf* block the tick ships (windfield.cpp says how it is kept in step).
struct FieldProbe {
  bool inTable = false;
  float groundY = 0.0f;        // table ground under the cell, world Y
  float haglM = 0.0f;          // height above that ground, metres
  float profile = 0.0f;        // log-law profile x coupling
  float exposure = 0.0f;       // s in [-1, 1]: ridge > 0, hollow < 0
  float expMul = 1.0f;         // the exposure factor it produced
  float ramp = 1.0f;           // profile x exposure x altitude term
  float leeMean = 1.0f, leeGust = 1.0f;
  float mean[3] = {}, bands[3] = {}, extra[3] = {}, total[3] = {};
  float band1 = 0.0f;          // the base band's along-wind component, unscaled
  float refSpeed = 0.0f;       // the weather's mean at the reference height
  float gustAmp = 0.0f;        // the weather's gust amplitude there
  float weatherHeadingDeg = 0.0f, localHeadingDeg = 0.0f;
  float gustExcess = 0.0f;     // gust bands along the local mean, m/s
  WindStateQ q;                // the weather words behind all of it
};
FieldProbe Probe(const Tuning& t, uint32_t seed, uint32_t tick, uint32_t dayPhase,
                 const int32_t origin[3], int32_t x, int32_t y, int32_t z);
// The same for n cells (xyz packed), resolving the weather once.
void ProbeMany(const Tuning& t, uint32_t seed, uint32_t tick, uint32_t dayPhase,
               const int32_t origin[3], const int32_t* xyz, int n, FieldProbe* out);

}  // namespace windfield
