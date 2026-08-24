// celestial.cpp — Keplerian orbits driving the sky. See celestial.h for the
// three properties this file exists to preserve (pure function of the tick,
// invisible to the sim, elements are data).

#include "sim/celestial.h"

#include <cmath>

#include "sim/tuning.h"

namespace {

// Mirrors kDayPhaseMax / kDayPhaseMask in world.h. Duplicated deliberately
// rather than including world.h: that header drags in the whole GPU RHI, and
// this file is also linked into the CPU-only tool targets. The static_assert
// against the real constants lives in world.h, so the two cannot drift.
constexpr uint32_t kDayPhaseMax = 65536u;
constexpr uint32_t kDayPhaseMask = kDayPhaseMax - 1u;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
constexpr double kDeg = kPi / 180.0;

inline double Wrap(double x, double period) {
  double r = std::fmod(x, period);
  return r < 0.0 ? r + period : r;
}

// A 3-vector of doubles, kept local: the engine's Vec3 is float, and orbital
// angle accumulation over a long session genuinely needs the mantissa. Only
// the final unit vectors are narrowed to float.
struct D3 {
  double x = 0, y = 0, z = 0;
};

inline D3 Norm(D3 v) {
  double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (l < 1e-12) return {0.0, 1.0, 0.0};
  return {v.x / l, v.y / l, v.z / l};
}
inline double Dot(const D3& a, const D3& b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline D3 Cross(const D3& a, const D3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline void Store(const D3& v, float out[3]) {
  out[0] = (float)v.x;
  out[1] = (float)v.y;
  out[2] = (float)v.z;
}

// Rotate about the X axis (used for the axial tilt: the obliquity is the angle
// between the ecliptic normal and the planet's spin axis).
inline D3 RotX(const D3& v, double c, double s) {
  return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
}

// Angular separation between two unit directions, in radians. acos of a dot
// product loses precision at small angles — which is EXACTLY the regime an
// eclipse test lives in — so use the atan2 form, which stays accurate to the
// last bit near zero.
inline double Separation(const D3& a, const D3& b) {
  D3 c = Cross(a, b);
  double s = std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
  return std::atan2(s, Dot(a, b));
}

// Area of the lens where two discs of angular radii r1, r2 overlap, as a
// FRACTION of the first disc's area. Flat-disc approximation (the angles here
// are under a degree, so spherical correction is far below a pixel).
//
// Returned as a fraction of disc 1 rather than of the lens, because every
// consumer asks "how much of the SUN is gone" — a total eclipse by a small
// moon must read as 1.0 even though the lens is the moon's whole area.
double OverlapFraction(double sep, double r1, double r2) {
  if (r1 <= 1e-9) return 0.0;
  if (sep >= r1 + r2) return 0.0;          // no contact
  if (sep <= std::fabs(r1 - r2)) {
    // One disc is entirely inside the other. If the occulter is the bigger
    // one, disc 1 is completely hidden; otherwise it hides its own area.
    return r2 >= r1 ? 1.0 : (r2 * r2) / (r1 * r1);
  }
  const double d = sep;
  const double a1 = (d * d + r1 * r1 - r2 * r2) / (2.0 * d * r1);
  const double a2 = (d * d + r2 * r2 - r1 * r1) / (2.0 * d * r2);
  const double c1 = std::acos(a1 < -1.0 ? -1.0 : (a1 > 1.0 ? 1.0 : a1));
  const double c2 = std::acos(a2 < -1.0 ? -1.0 : (a2 > 1.0 ? 1.0 : a2));
  const double tri = 0.5 * std::sqrt(
      std::fmax(0.0, (-d + r1 + r2) * (d + r1 - r2) * (d - r1 + r2) * (d + r1 + r2)));
  const double lens = r1 * r1 * c1 + r2 * r2 * c2 - tri;
  const double frac = lens / (kPi * r1 * r1);
  return frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
}

// Phase of a body lit by the sun, seen from the planet. `toBody` and `toSun`
// are unit vectors from the planet. Elongation 180 deg (body opposite the sun)
// = full = 0.5; elongation 0 (body at the sun) = new = 0.0/1.0.
//
// The SIGN is what distinguishes waxing from waning: it is which side of the
// body-to-observer plane the sun is on, measured against the disc frame the
// shader will build. Getting it wrong makes the terminator flip discontinuously
// as the moon passes full, which reads as the disc rolling over.
void PhaseOf(const D3& toBody, const D3& toSun, float* phaseOut,
             float* signOut) {
  const double elong = Separation(toBody, toSun);
  // Illuminated fraction of the visible disc = (1 + cos(phaseAngle)) / 2 with
  // phaseAngle = pi - elongation for a distant sun. Map onto 0=new..0.5=full
  // so the existing shader's `(moonPhase - 0.5) * 2pi` lighting angle is
  // unchanged — this function replaces WHERE the number comes from, not what
  // it means.
  double p = elong / kTau;              // 0 at conjunction, 0.5 at opposition
  if (p < 0.0) p = 0.0;
  if (p > 0.5) p = 0.5;
  // Which limb is lit: project the sun onto the disc's local +x axis, built
  // the same way moonLayer() builds it (world up crossed with the body dir).
  D3 upv{0.0, 1.0, 0.0};
  if (std::fabs(toBody.y) > 0.95) upv = D3{1.0, 0.0, 0.0};
  const D3 mx = Norm(Cross(upv, toBody));
  const double side = Dot(toSun, mx);
  *phaseOut = (float)p;
  *signOut = side >= 0.0 ? 1.0f : -1.0f;
}

}  // namespace

// ---- Kepler ------------------------------------------------------------------

double SolveKepler(double meanAnomaly, double e) {
  const double M = Wrap(meanAnomaly, kTau);
  // Start point. E = M is the classic choice and converges for e up to ~0.6;
  // past that it can walk the wrong way on the first step, so bias toward the
  // periapsis side. Tuning clamps e well below that, but the solver is the
  // wrong place to assume its caller behaved.
  double E = e < 0.6 ? M : (M + (M < kPi ? e : -e));
  // FIXED iteration count — never `while (|dE| > eps)`. A convergence loop's
  // trip count is a function of the value, and two builds that fold the
  // arithmetic differently would take different numbers of steps and land on
  // different bits. 6 Newton steps is quadratic convergence from a start point
  // already within e of the answer: past double precision for any e < 0.9.
  for (int i = 0; i < 6; i++) {
    const double f = E - e * std::sin(E) - M;
    const double fp = 1.0 - e * std::cos(E);
    E -= f / (fp > 1e-9 ? fp : 1e-9);
  }
  return E;
}

void OrbitPosition(const OrbitElements& el, double t, double outXyz[3],
                   double* rOut) {
  const double period = el.period > 1e-6 ? el.period : 1e-6;
  const double M = el.m0 + kTau * (t / period);
  const double e = el.e < 0.0 ? 0.0 : (el.e > 0.9 ? 0.9 : el.e);
  const double E = SolveKepler(M, e);
  // True anomaly from the eccentric anomaly. The half-angle atan2 form is
  // used rather than acos((cosE - e)/(1 - e cosE)) because it is single-valued
  // across the whole orbit — the acos form needs a branch on sin(E) that gets
  // the quadrant wrong at the apsides.
  const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(E * 0.5),
                                     std::sqrt(1.0 - e) * std::cos(E * 0.5));
  const double r = el.a * (1.0 - e * std::cos(E));
  if (rOut) *rOut = r;

  // In-plane position, then the standard 3-rotation into the reference frame:
  // Rz(lonNode) * Rx(inc) * Rz(argPeri). Written out rather than composed from
  // matrices because it is three lines and the composed version obscures which
  // angle is which.
  const double u = el.argPeri + nu;      // argument of latitude
  const double cu = std::cos(u), su = std::sin(u);
  const double co = std::cos(el.lonNode), so = std::sin(el.lonNode);
  const double ci = std::cos(el.inc), si = std::sin(el.inc);
  outXyz[0] = r * (co * cu - so * su * ci);
  outXyz[1] = r * (so * cu + co * su * ci);
  outXyz[2] = r * (su * si);
}

// ---- the sky -----------------------------------------------------------------

SkyState ComputeSky(const Tuning& tun, double celestialTick) {
  const Tuning::DayNight& d = tun.dayNight;
  SkyState s{};

  // ---- clocks -------------------------------------------------------------
  // One SIDEREAL day is the planet's rotation period. The SOLAR day (sunrise
  // to sunrise) is longer, because the planet has also moved along its orbit
  // and has to turn a little further to face the star again. `cycleMinutes`
  // is authored as the day the PLAYER experiences, so it is the solar day and
  // the sidereal rotation is derived from it — the other way round would make
  // lengthening the year quietly shorten the day.
  const double solarDayTicks = (double)TicksPerDayFromTuning(tun);
  const double yearDays = d.yearLengthDays > 0.5f ? (double)d.yearLengthDays : 0.5;
  const double yearTicks = solarDayTicks * yearDays;
  // The SIDEREAL rotation rate, in turns per tick.
  //
  //   n_sidereal = n_solar - n_orbit
  //
  // and the SIGN here is the single most bug-prone line in this file, so it is
  // worth stating why. The sun's hour angle is (spin angle) minus (the sun's
  // ecliptic longitude). Both this frame's spin and the planet's orbital
  // motion are counter-clockwise about +z — the SAME sense — so the two rates
  // ADD in the hour angle and must therefore SUBTRACT here to leave one solar
  // turn per solar day. Writing `+` instead gives a year that overshoots by
  // exactly two full turns of azimuth, which looks locally plausible (the sun
  // still rises and sets) and is only visible as accumulated drift.
  //
  // Guarded because a year exactly one solar day long makes the two rates
  // cancel: the planet would be tidally locked and there would be no rotation
  // at all. That is a degenerate JSON value, not a sky, so fall back to no
  // correction rather than dividing by zero.
  const double invSid = 1.0 / solarDayTicks - 1.0 / yearTicks;
  const double siderealDayTicks =
      std::fabs(invSid) > 1e-12 ? 1.0 / invSid : solarDayTicks;

  // FREEZE. The freeze controls exist so a shot or a selftest can pin the
  // light; they pin the celestial tick to the one that produces the requested
  // phase, so the moons and the season freeze coherently with the sun rather
  // than the sun stopping while the moons keep moving.
  double t = celestialTick;
  if (d.freeze != 0) {
    const double frac = (double)(uint32_t)(d.freezePhase & (int)kDayPhaseMask) /
                        (double)kDayPhaseMax;
    t = frac * solarDayTicks;
  }

  // ---- the planet's orbit -------------------------------------------------
  // Solved heliocentrically; the direction TO the star from the planet is the
  // negation of the planet's position vector.
  OrbitElements planet;
  planet.a = 1.0;
  planet.e = (double)d.orbitEccentricity;
  planet.inc = 0.0;                 // the planet's own orbit DEFINES the ecliptic
  planet.argPeri = (double)d.orbitArgPeriapsis * kDeg;
  planet.lonNode = 0.0;
  planet.m0 = (double)d.orbitMeanAnomaly0 * kDeg;
  planet.period = yearTicks;
  double pp[3], pr = 1.0;
  OrbitPosition(planet, t, pp, &pr);
  const D3 sunEcl = Norm(D3{-pp[0], -pp[1], -pp[2]});
  s.yearT = (float)Wrap(t / yearTicks, 1.0);
  // The sun's direction at the EPOCH (t = 0), used below to phase the planet's
  // rotation so that tick 0 is mean solar midnight. Recomputed rather than
  // cached because ComputeSky must stay a pure function of the tick — a static
  // here would make the sky depend on which Tuning was loaded first.
  double pp0[3];
  OrbitPosition(planet, 0.0, pp0, nullptr);
  const D3 sunEcl0 = Norm(D3{-pp0[0], -pp0[1], -pp0[2]});

  // ---- ecliptic -> horizon ------------------------------------------------
  // Two rotations, in this order:
  //   1. OBLIQUITY. Tip the ecliptic frame by the axial tilt about the x axis
  //      (the equinox line), giving the planet's EQUATORIAL frame. This is the
  //      entire mechanism behind seasons: the same orbit seen through a tipped
  //      equator puts the sun higher in one half of the year than the other,
  //      and the amplitude of that swing IS the tilt.
  //   2. ROTATION. Spin about the (now vertical) polar axis by the sidereal
  //      rotation angle, and tip the polar axis down from the zenith by the
  //      colatitude so the observer sits at a latitude rather than at the pole.
  //
  // The old system had `sunPeakElevation` as a direct clamp on how high the
  // sun got. That is now an OUTPUT: peak elevation = 90 - |latitude - tilt|.
  // `latitudeDeg` is the knob that replaced it, and it is the honest one —
  // it also fixes how long the day is, which a peak-elevation clamp could not.
  const double tilt = (double)d.axialTilt * kDeg;
  const double ct = std::cos(tilt), st = std::sin(tilt);
  // Colatitude: 0 at the north pole, 90 deg at the equator.
  const double colat = (90.0 - (double)d.latitudeDeg) * kDeg;
  const double cc = std::cos(colat), sc = std::sin(colat);
  // The spin angle, phased so that MEAN SOLAR MIDNIGHT lands at t = 0.
  //
  // The bare sidereal angle is not enough: the frame it spins is the ecliptic
  // one, in which the sun sits at longitude lambda0 at the epoch, so a spin of
  // zero puts the sun at an arbitrary hour. Subtracting lambda0 anchors the
  // epoch to the sun, and the remaining +pi/2 is the ecliptic-frame convention
  // (the sun's direction is measured from +x while the horizon frame's north
  // comes out of +y).
  //
  // Anchoring it this way rather than fitting a constant is what makes dayT
  // and the sun agree: `dayT = t / solarDayTicks` is MEAN solar time, and mean
  // solar noon is now t = 0.5 by construction for every orbit. What is left
  // over is the equation of time — apparent noon oscillating about mean noon
  // by up to ~15 minutes across the year — which is real, is what an eccentric
  // tilted orbit does, and is bounded rather than accumulating.
  //
  // NOTE `sunAzimuth` is deliberately NOT part of this angle. It reads as
  // "which compass direction is east", i.e. a rotation of the OBSERVER about
  // their own vertical, and folding it into the spin instead makes it a
  // rotation in TIME: at the default 24 degrees it moved noon by 24/360 of a
  // day (~1.6 hours of game time) away from dayT 0.5, silently desynchronising
  // the visible sun from the integer day phase the reactions are gated on. It
  // is applied as a yaw of the finished horizon vector below.
  const double sunLon0 = std::atan2(sunEcl0.y, sunEcl0.x);
  const double theta =
      kTau * (t / siderealDayTicks) + kPi - sunLon0 + kPi * 0.5;
  const double cth = std::cos(theta), sth = std::sin(theta);
  // Observer yaw: spins the finished horizon frame about +y (up), so the sun,
  // both moons and the stars all turn together and none of them changes when
  // it rises.
  const double yaw = (double)d.sunAzimuth * kDeg;
  const double cy = std::cos(yaw), sy = std::sin(yaw);

  // Ecliptic (x, y, z=orbit normal) -> horizon (x=east, y=up, z=north).
  auto eclipticToHorizon = [&](const D3& v) -> D3 {
    // 1. obliquity about x: equatorial frame, z still the spin axis.
    const D3 eq = RotX(v, ct, st);
    // 2. spin about the z (polar) axis.
    const D3 sp{eq.x * cth - eq.y * sth, eq.x * sth + eq.y * cth, eq.z};
    // 3. relabel to Y-up and tip the pole by the colatitude. In the horizon
    //    frame the pole sits at elevation = latitude in the north.
    //    east = sp.x, and (north, up) is (sp.y, sp.z) rotated by the colat.
    const D3 hz{sp.x, sp.y * sc + sp.z * cc, -(sp.y * cc - sp.z * sc)};
    // 4. observer yaw about up (see `yaw` above): a purely spatial rotation,
    //    applied last so it cannot move the time of noon.
    return D3{hz.x * cy + hz.z * sy, hz.y, -hz.x * sy + hz.z * cy};
  };

  const D3 sunH = Norm(eclipticToHorizon(sunEcl));
  Store(sunH, s.sunDir);
  s.sun.dir[0] = s.sunDir[0];
  s.sun.dir[1] = s.sunDir[1];
  s.sun.dir[2] = s.sunDir[2];
  s.sun.dist = (float)pr;
  // Apparent solar radius scales with 1/distance, so an eccentric year has a
  // visibly bigger sun at perihelion. Authored radius is at a = 1.
  s.sun.angRadius = (float)((double)d.sunAngularRadius * kDeg / (pr > 1e-6 ? pr : 1e-6));
  s.sun.phase = 0.5f;

  // dayT: the SOLAR time of day, 0 = midnight. This is what the shader's
  // blends key on, and it must agree with the integer sim phase, so it is
  // derived from the solar day rather than from the sun's actual hour angle
  // (the two differ by the equation of time — up to ~15 minutes on an
  // eccentric orbit, which is real physics but must not desync the two clocks).
  s.dayT = (float)Wrap(t / solarDayTicks, 1.0);

  // Daylight weight: smoothstep across the twilight band around the horizon,
  // so sky, ambient and key light crossfade together instead of snapping at
  // the geometric horizon. Unchanged from the old system on purpose — this is
  // the render-side daylight measure and the look is tuned against it.
  {
    const float w = d.twilightWidth > 0.0f ? d.twilightWidth : 0.22f;
    float x = (s.sunDir[1] + w * 0.35f) / w;
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    s.sunUp = x * x * (3.0f - 2.0f * x);
  }

  // ---- the moons ----------------------------------------------------------
  // Each moon orbits the PLANET, so its ecliptic-frame direction is its own
  // position vector (no negation). Periods are authored as SYNODIC (new moon
  // to new moon, the cycle a player actually watches); the sidereal period the
  // orbit is integrated with is derived, because the planet's motion around
  // the star stretches the phase cycle exactly as it does on Earth:
  //   1/T_syn = 1/T_sid - 1/T_year
  // Authoring the sidereal period instead would make "an 8-day moon" mean a
  // phase cycle of 8.6 days, which is not what the number says.
  auto moonElements = [&](float periodDays, float incDeg, float ecc,
                          float argPeri, float node, float m0,
                          float semiMajor) {
    OrbitElements el;
    const double syn = solarDayTicks * (double)(periodDays > 0.1f ? periodDays : 0.1f);
    // Guard the degenerate case where the synodic period equals the year (the
    // moon would be tidally locked to the sun direction and 1/T_sid = 0).
    double invSid = 1.0 / syn + 1.0 / yearTicks;
    el.period = (std::fabs(invSid) > 1e-12) ? 1.0 / invSid : syn;
    el.a = (double)semiMajor;
    el.e = (double)ecc;
    el.inc = (double)incDeg * kDeg;
    el.argPeri = (double)argPeri * kDeg;
    el.lonNode = (double)node * kDeg;
    el.m0 = (double)m0 * kDeg;
    return el;
  };

  // `baseRadiusDeg` is in DEGREES, like every other angle in Tuning::DayNight,
  // and is converted here. Taking it in radians instead was a real bug: 1.7
  // read as radians is a 97-degree moon, which covers the sun a third of the
  // time — the `celestial` gate's "eclipses are not rare" assertion is what
  // caught it, and the parameter is named for its unit now so it cannot recur.
  auto solveMoon = [&](const OrbitElements& el, float baseRadiusDeg,
                       BodyState* out) {
    double mp[3], mr = 1.0;
    OrbitPosition(el, t, mp, &mr);
    const D3 ecl = Norm(D3{mp[0], mp[1], mp[2]});
    const D3 h = Norm(eclipticToHorizon(ecl));
    Store(h, out->dir);
    out->dist = (float)mr;
    // Apparent size ~ 1/distance: perigee is measurably larger, which is the
    // "supermoon" and falls out of the eccentricity for free.
    const double scale = el.a > 1e-9 ? el.a / (mr > 1e-9 ? mr : 1e-9) : 1.0;
    out->angRadius = (float)((double)baseRadiusDeg * kDeg * scale);
    PhaseOf(h, sunH, &out->phase, &out->phaseSign);
    return h;
  };

  const OrbitElements elA =
      moonElements((float)d.lunarPeriodDays, d.moonInclination,
                   d.moonEccentricity, d.moonArgPeriapsis, d.moonNode,
                   d.moonMeanAnomaly0, 1.0f);
  const OrbitElements elB =
      moonElements((float)d.moon2PeriodDays, d.moon2Inclination,
                   d.moon2Eccentricity, d.moon2ArgPeriapsis, d.moon2Node,
                   d.moon2MeanAnomaly0, 1.35f);
  const D3 moonAH = solveMoon(elA, d.moonAngularRadius, &s.moonA);
  const D3 moonBH = solveMoon(elB, d.moon2AngularRadius, &s.moonB);

  s.moonDir[0] = s.moonA.dir[0];
  s.moonDir[1] = s.moonA.dir[1];
  s.moonDir[2] = s.moonA.dir[2];
  s.moonPhase = s.moonA.phase;
  s.moonPhaseSign = s.moonA.phaseSign;
  s.moonAngRadius = s.moonA.angRadius;
  s.moon2Dir[0] = s.moonB.dir[0];
  s.moon2Dir[1] = s.moonB.dir[1];
  s.moon2Dir[2] = s.moonB.dir[2];
  s.moon2Phase = s.moonB.phase;
  s.moon2PhaseSign = s.moonB.phaseSign;
  s.moon2AngRadius = s.moonB.angRadius;

  // ---- eclipses -----------------------------------------------------------
  // Pure geometry: a solar eclipse is a moon's disc overlapping the sun's on
  // the sky. It is RARE without any special-casing, because it needs the moon
  // to be new (the ~30 deg of orbit where it is anywhere near the sun) AND
  // within its own inclination of the ecliptic at that moment — the same two
  // conditions that make real eclipses rare. Raising a moon's inclination
  // makes its eclipses rarer; setting it to 0 makes one happen every synodic
  // month, which is a good way to check this code works.
  {
    const double sepA = Separation(moonAH, sunH);
    const double sepB = Separation(moonBH, sunH);
    const double fA = OverlapFraction(sepA, (double)s.sun.angRadius,
                                      (double)s.moonA.angRadius);
    const double fB = OverlapFraction(sepB, (double)s.sun.angRadius,
                                      (double)s.moonB.angRadius);
    if (fA >= fB) {
      s.solarEclipse = (float)fA;
      s.eclipseBody = fA > 0.0 ? 1u : 0u;
    } else {
      s.solarEclipse = (float)fB;
      s.eclipseBody = 2u;
    }
    // Below the horizon nothing is eclipsed as far as the world is concerned —
    // and, more importantly, the sun's disc is not drawn there, so letting the
    // term survive would dim a night for no visible cause.
    if (s.sunDir[1] < -0.05f) {
      s.solarEclipse = 0.0f;
      s.eclipseBody = 0u;
    }

    // Moon-on-moon: measured as a fraction of the FARTHER moon's disc, since
    // that is the one being hidden. Moon B is given the larger semi-major axis
    // above, so it is always the occulted one.
    const double sepM = Separation(moonAH, moonBH);
    s.lunarEclipse = (float)OverlapFraction(sepM, (double)s.moonB.angRadius,
                                            (double)s.moonA.angRadius);
  }

  // ---- the star sphere ----------------------------------------------------
  // Stars wheel with the SIDEREAL rotation, not the solar day — that is the
  // whole reason the constellations drift a few minutes earlier each night,
  // and with a short game year the drift is fast enough to notice.
  s.starRot = (float)(kTau * Wrap(t / siderealDayTicks, 1.0) * (double)d.starRotSpeed);
  return s;
}

SkyState ComputeSkyState(const Tuning& t, uint32_t phase, uint32_t dayNumber) {
  // Map an integer day phase + day count back onto a celestial tick. Used by
  // the --shot path, which pins a time of day rather than running a clock.
  const double solarDayTicks = (double)TicksPerDayFromTuning(t);
  const double frac = (double)(phase & kDayPhaseMask) / (double)kDayPhaseMax;
  return ComputeSky(t, ((double)dayNumber + frac) * solarDayTicks);
}

