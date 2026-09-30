#pragma once
#include <cstdint>

#include "sim/wind.h"
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
// The later stages of docs/RESEARCH_wind.md §13 add the terrain table, the
// regime's field parameters and the storm primitives to this file.

namespace windfield {

// Ticks per advection block: the reference speed is sampled once per block
// and held across it. 16 ticks = 0.53 s — far below any weather change, and
// the sum stays exact because a step function integrates exactly.
constexpr uint32_t kAdvBlock = 16;

// The advection clock at `tick` + `frac` (frac in [0, 1) of a tick, 0 for the
// sim). BAM16 of the BASE gust band (coefficient 1.0), reduced mod 10 turns
// (655360): every band coefficient is a multiple of 0.1, so band c's clock is
// (phase * c10 / 10) and wraps at a whole number of turns exactly when this
// does. `kBam` is the base spatial frequency, BAM16 per cell — the same
// integer WINDQ_K_BASE the shader const-folds (see GustKBase).
uint32_t AdvPhase(const Tuning& t, uint32_t seed, uint32_t tick, float frac);

// The air's displacement since tick 0, METRES, as the weather's reference
// wind carried it: the same sum AdvPhase takes, as a vector. The clouds drift
// by this (times their altitude factor), so the sky and the grass are moved
// by one wind.
void AirDrift(const Tuning& t, uint32_t seed, uint32_t tick, float frac,
              double& dxM, double& dzM);

// WINDQ_K_BASE, on the CPU: round(65536 / wavelength-in-cells), capped at
// 27000. Must match the shader's const, which is computed from the same knob
// with the same formula — check_invariants holds the formula text.
int32_t GustKBase(const Tuning& t);

// Fill the wf* block of a TickParams / RenderParams. `tick` + `frac` is the
// instant the clocks are evaluated at (frac = 0 for the sim).
void FillWindField(TickParams& tp, const Tuning& t, uint32_t seed, uint32_t tick,
                   uint32_t dayPhase);
void FillWindField(RenderParams& rp, const Tuning& t, uint32_t seed, uint32_t tick,
                   float frac, uint32_t dayPhase);

}  // namespace windfield
