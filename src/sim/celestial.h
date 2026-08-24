// celestial.h — the CLOCK behind the sky.
//
// Everything the renderer knows about where the sun, the two moons and the
// star sphere are comes from here, and it is a real orbital simulation rather
// than a phase ramp: the planet orbits a star on a Keplerian ellipse, spins on
// a tilted axis, and carries two moons on their own inclined ellipses. Seasons,
// lunar phase, the 72-day beat between the two moons and eclipses are all
// CONSEQUENCES of that geometry, not authored curves laid on top of it.
//
// Three properties are load-bearing, and each one is why an obvious
// alternative was rejected:
//
//   * PURE FUNCTION OF THE TICK. `ComputeSky(t, celestialTick)` recomputes
//     every element from epoch every call — no integrator, no accumulated
//     state. Kepler's equation from a mean anomaly is O(1), so there is no
//     performance reason to integrate, and an integrator would drift: the sky
//     an hour into a session would depend on the frame rate that got there,
//     and a replay would not reproduce it. See DESIGN.md §4's day/night note
//     for why that matters beyond aesthetics.
//   * THE SIM DOES NOT READ THIS. The CA is gated on the INTEGER day phase in
//     TickParams (world.h DayPhaseForTick / common.wgsl daylightStrength),
//     which is integer-only per CLAUDE.md rule 1. This file is float
//     throughout and touches no voxel state. The two agree because both are
//     driven by the same celestial tick, not because they share code.
//   * EVERY ELEMENT IS DATA. Orbital elements are Tuning::DayNight rows, so a
//     different sky is a JSON edit (DESIGN.md §6's "no closed-ended systems").
//
// Coordinate conventions (repo-standard, CLAUDE.md): Y is up, +Z is heading 0,
// so the local horizon frame is X = east, Y = zenith, Z = north. The ecliptic
// frame the orbits are solved in is right-handed with Z as the orbit normal;
// `EclipticToHorizon` below is the one place the two meet.

#pragma once

#include <cstdint>


// tuning.h has no namespace — everything in it is at global scope — so this
// forward declaration must be too. Wrapping it in `namespace sandvox` declares
// a DIFFERENT type and every call site becomes ambiguous.
struct Tuning;

// ---- celestial body state ---------------------------------------------------
// One body as the renderer needs it. `dir` is a unit vector in WORLD space
// (the local horizon frame) pointing AT the body; `angRadius` is its apparent
// angular radius in radians, which is what makes eclipse tests geometry rather
// than a lookup table.
struct BodyState {
  float dir[3] = {0.0f, 1.0f, 0.0f};
  float angRadius = 0.0f;
  // 0 = new (unlit, between us and the sun), 0.5 = full (opposite the sun),
  // 1 = new again. Derived from the elongation angle, so it is the real
  // phase of the real geometry — a moon near the sun in the sky IS new.
  float phase = 0.5f;
  // Signed: +1 when the lit limb faces the +x axis of the disc's screen frame,
  // -1 the other way. Without this a waxing and a waning crescent are
  // indistinguishable, and the moon appears to jump when it passes full.
  float phaseSign = 1.0f;
  // Distance from the planet centre, in the same units as the semi-major axis
  // (arbitrary — only ratios matter). Drives apparent size: an eccentric moon
  // is visibly bigger at perigee.
  float dist = 1.0f;
};

// ---- the sky at one instant -------------------------------------------------
// Kept as a flat POD of floats because it is copied straight into RenderParams
// (world.h) and the two must not drift apart.
struct SkyState {
  float sunDir[3];    // unit, toward the sun
  float moonDir[3];   // unit, toward moon A
  float dayT;         // 0..1, 0 = local midnight (mean solar time)
  float sunUp;        // smoothed 0..1 daylight weight (drives all crossfades)
  float moonPhase;    // moon A: 0 = new, 0.5 = full
  float starRot;      // radians the star sphere has wheeled (sidereal)

  // ---- moon B (added with the Keplerian overhaul) ----
  float moon2Dir[3];
  float moon2Phase;

  // Apparent angular radii, radians. The tuner authors a BASE radius and the
  // orbit modulates it by distance, so perigee is genuinely larger.
  float moonAngRadius;
  float moon2AngRadius;

  // Signed lit-limb orientation for each moon (see BodyState::phaseSign).
  float moonPhaseSign;
  float moon2PhaseSign;

  // ---- eclipses ----
  // 0 = nothing in front of the sun, 1 = the sun's disc is fully covered.
  // This is the FRACTION OF THE SUN'S AREA occluded by whichever moon covers
  // more of it, computed from the circle-circle lens area — so a partial
  // eclipse dims the world by the right amount instead of switching.
  float solarEclipse;
  // Which moon is doing it (0 = none, 1 = moon A, 2 = moon B) and how far
  // moon-on-moon overlap has got (0..1 of the smaller disc's area). The
  // shader uses the latter to draw one moon over the other.
  uint32_t eclipseBody;
  float lunarEclipse;

  // The full per-body state, for anything that needs more than the flattened
  // fields above (the eclipse math, the tuner readout, future gameplay).
  BodyState sun, moonA, moonB;

  // Whole elapsed orbits since epoch, as a fraction: 0.0 = the epoch's
  // vernal-equivalent, 0.25 = a quarter year on. Drives nothing in the shader
  // yet; it is what a season readout or a seasonal reaction gate would key on.
  float yearT;
};

// ---- orbital elements -------------------------------------------------------
// A classical element set. Angles in RADIANS here (the tuning rows are in
// degrees and are converted once, at the top of ComputeSky).
struct OrbitElements {
  double a = 1.0;         // semi-major axis (arbitrary units; ratios only)
  double e = 0.0;         // eccentricity, 0 = circle, must stay < 1
  double inc = 0.0;       // inclination to the reference plane
  double argPeri = 0.0;   // argument of periapsis
  double lonNode = 0.0;   // longitude of the ascending node
  double m0 = 0.0;        // mean anomaly at epoch
  double period = 1.0;    // orbital period, in TICKS
};

// Solve Kepler's equation M = E - e sin E for the eccentric anomaly E.
// Newton-Raphson from E = M (or from M + e for high eccentricity, where that
// start point diverges). Iteration count is FIXED, never a convergence loop:
// a loop whose trip count depends on the value is exactly the kind of thing
// that makes two machines disagree, and 6 Newton steps is already past double
// precision for every eccentricity this engine allows.
double SolveKepler(double meanAnomaly, double e);

// Position of a body on its orbit at `t` ticks after epoch, in the REFERENCE
// (ecliptic) frame. Returns the radius in `rOut`.
void OrbitPosition(const OrbitElements& el, double t, double outXyz[3],
                   double* rOut);

// The whole sky at a celestial tick. `celestialTick` is a DOUBLE because the
// dev time-scale slider can run it fast, slow or backwards and a u32 would
// quantise a 0.1x day into steps; the SIM's clock stays the integer tick.
// Negative values are fine — the orbits run backwards, exactly.
SkyState ComputeSky(const Tuning& t, double celestialTick);

// Back-compat entry point for callers that only have an integer day phase
// (the --shot path pins a time of day rather than a tick). Maps the phase onto
// the celestial tick that produces it and calls ComputeSky.
SkyState ComputeSkyState(const Tuning& t, uint32_t phase, uint32_t dayNumber);

