// support.cpp — shared sim/render plumbing. Moved verbatim out of main.cpp's
// anonymous namespace so the selftest could leave main.cpp without cloning it.
// See support.h for why one definition matters here.

#include "test/support.h"

#include "sim/pagetable.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "gpu/resources.h"
#include "net/authority.h"
#include "measure/perfscope.h"
#include "sim/biomes.h"
#include "sim/farfield.h"
#include "sim/farplumes.h"
#include "sim/oprecord.h"
#include "sim/treeatlas.h"
#include "sim/worldmap.h"
#include "sim/wind.h"
#include "sim/weather.h"
#include "sim/renderspec.h"
#include "sim/worldedit.h"
#include "sim/waterbody.h"
#include "sim/windprim.h"
#include "sim/windfield.h"
#include "sim/currentprim.h"
#include "sim/trample.h"
#include "sim/solutes.h"
#include "sim/stream.h"   // ApplyStructureChanges: Stream::RegenerateChunks

#include <map>
#include <set>
#include <tuple>

namespace sandvox {

// Must track tuning.json player.model — the avatar gates exist to test the
// character the GAME plays, and one pinned to the old name goes on passing
// against a character nobody plays (see the note at the avatar gate itself).
const char* kAvatarDefName = "human";

// Read-and-cleared once per frame by the telemetry path. See TakeSnapshotStalls
// in support.h for why this exists and what a nonzero value means.
namespace {
uint32_t g_snapshotStalls = 0;
uint32_t g_readbackDeclines = 0;
// P2-D attribution — see SnapshotStallStats in support.h.
SnapshotStallStats g_stallStats{};
// P3-F: the PassTimer SubmitTick hangs its three untabled GPU spans on. NULL
// everywhere except inside a --perf recording, so the game and the selftest
// encode exactly the command buffer they always did.
::PassTimer* g_tickTimer = nullptr;

// One (begin, end) timestamp pair around a run of raw GPU commands, named for
// kPerfRenderSpans. Scoped rather than paired calls for the reason ScopeTimer
// gives: an early return past the closing write leaves a query unwritten, and
// Absorb reads an unwritten pair as "pass disabled" and silently drops it — a
// span that fails to zero rather than to a visible number is worse than none.
struct TickGpuSpan {
  const rhi::CommandEncoder& enc;
  ::PassTimer* t = nullptr;
  uint32_t b = 0, e = 0;
  TickGpuSpan(const rhi::CommandEncoder& encoder, const char* name)
      : enc(encoder) {
    ::PassTimer* timer = g_tickTimer;
    if (timer && timer->Valid() && timer->AllocPassPair(name, b, e)) {
      t = timer;
      enc.WriteTimestamp(t->NativeQuerySet(), b, false);
    }
  }
  ~TickGpuSpan() {
    if (t) enc.WriteTimestamp(t->NativeQuerySet(), e, true);
  }
  TickGpuSpan(const TickGpuSpan&) = delete;
  TickGpuSpan& operator=(const TickGpuSpan&) = delete;
};
}


void SetSubmitTickPassTimer(::PassTimer* t) { g_tickTimer = t; }
::PassTimer* SubmitTickPassTimer() { return g_tickTimer; }


// ---- SHORT-RANGE MODE, the headless handle -------------------------------
// The mode is normally driven by the dev panel checkbox, which no headless
// path can press. Rather than thread a bool through --shot, the perf harness,
// the lab and the mob portrait — five call sites that would each have to
// remember — it is latched HERE, in the one function every drawing path
// already goes through to write RenderParams.
//
// Set it with `--short-range` or SANDVOX_SHORT_RANGE=1; the windowed game
// calls SetShortRange() from the frame loop so the checkbox stays the live
// authority there. Render-only: it touches no sim input and cannot reach the
// world hash.
namespace {
bool g_shortRange = false;
bool g_shortRangeEnvRead = false;
}

bool ShortRangeMode() {
  if (!g_shortRangeEnvRead) {
    g_shortRangeEnvRead = true;
    const char* e = std::getenv("SANDVOX_SHORT_RANGE");
    if (e && e[0] && e[0] != '0') g_shortRange = true;
  }
  return g_shortRange;
}

void SetShortRange(bool on) {
  ShortRangeMode();   // consume the env default first, then override it
  g_shortRange = on;
}

// Which ceiling the mode uses (support.h ShortRangeNear). Same latch shape as
// above so `--short-range-near` / SANDVOX_SHORT_RANGE_NEAR reach every headless
// drawing path, and the panel's radio re-asserts it every frame in the game.
namespace {
bool g_shortRangeNear = false;
bool g_shortRangeNearEnvRead = false;
}

bool ShortRangeNear() {
  if (!g_shortRangeNearEnvRead) {
    g_shortRangeNearEnvRead = true;
    const char* e = std::getenv("SANDVOX_SHORT_RANGE_NEAR");
    if (e && e[0] && e[0] != '0') g_shortRangeNear = true;
  }
  return g_shortRangeNear;
}

void SetShortRangeNear(bool on) {
  ShortRangeNear();   // consume the env default first, then override it
  g_shortRangeNear = on;
}

// Time of day used by --shot, as a 0..1 fraction of the cycle (0 = midnight,
// 0.5 = noon). Set by `--time`; see RunShots.
float g_shotTimeOfDay = 0.34f;

// The ONE place the asset tree is located. Every loader (materials, tuning,
// shaders via LoadShader, prefabs, mobs, sounds, and tests/baseline.json next
// to it) builds its path from this string, so overriding it here overrides all
// of them at once.
//
// SANDVOX_ASSET_DIR in the ENVIRONMENT wins over the compiled-in path. That is
// what lets a WGSL-only or tuning-only worktree run the MAIN checkout's
// sandvox.exe against its own assets/ without a build of its own: the SPIR-V
// cache keys on shader CONTENT, so an edited shader is recompiled on the first
// run regardless of which binary loads it. Resolved once, printed once, so a
// run that used the wrong tree says so in its first line rather than in a hash
// that quietly matches the wrong baseline.
std::string AssetDir() {
  static const std::string resolved = [] {
    std::string dir;
    const char* from = "compiled-in";
    if (const char* env = std::getenv("SANDVOX_ASSET_DIR"); env && *env) {
      dir = env;
      from = "SANDVOX_ASSET_DIR";
    } else {
#ifdef SANDVOX_ASSET_DIR
      dir = SANDVOX_ASSET_DIR;
#else
      dir = "assets";
#endif
    }
    while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\'))
      dir.pop_back();
    std::printf("assets: %s (%s)\n", dir.c_str(), from);
    std::fflush(stdout);
    return dir;
  }();
  return resolved;
}

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Ticks per in-game SOLAR day, from the tuning cycle length. Sim runs at 30 Hz.
// The definition itself lives in tuning.h so sim/celestial.cpp can use it
// without linking the render plumbing; this is the name the frame loop and the
// gates already call.
uint32_t TicksPerDay(const Tuning& t) { return TicksPerDayFromTuning(t); }

uint32_t DayPhaseNow(uint32_t tick) {
  const Tuning& t = CurrentTuning();
  return DayPhaseForTick(Celestial().SimTick(tick), TicksPerDay(t),
                         t.dayNight.freeze != 0,
                         (uint32_t)t.dayNight.freezePhase);
}

// The sky for a given sim tick — now a full Keplerian solve (sim/celestial.*)
// rather than a phase ramp. `tick` is routed through the celestial clock, which
// is DISENGAGED unless the dev overlay's time-speed slider has been moved: on
// every headless path this is the identity map and the celestial tick IS the
// sim tick, so the pinned world hash cannot move.
SkyState SkyForTick(const Tuning& t, uint32_t tick) {
  return ComputeSky(t, Celestial().RenderTick(tick));
}

// ---- the TAA camera jitter -------------------------------------------------
// See support.h for why this is here and not inlined in the frame loop.
void ApplyTaaJitter(const Camera& cam, const Vec3& eye, float aspect,
                    uint32_t renderW, uint32_t renderH, uint32_t frameIdx,
                    float amp, Camera& outJittered,
                    Simulation::TaaCamera& outTaa) {
  outJittered = cam;
  outTaa = Simulation::TaaCamera{};
  // The fov WriteRenderParams will use, read from where it reads it. Taking
  // cam.fovY instead would be a second opinion about the projection, and the
  // whole point of measuring the shift below is that there is only one.
  const float thf = std::tan(CurrentTuning().camera.fovY * 0.5f);
  if (renderW == 0 || renderH == 0) return;

  float rx = 0.0f, ry = 0.0f;
  Simulation::TaaJitter(frameIdx, &rx, &ry);
  rx *= amp;
  ry *= amp;
  // A pixel is 2*tanHalfFov/renderPx of angle. Yaw turns about WORLD Y, so its
  // angular effect on the view direction scales with cos(pitch) — divided back
  // out, floored so a straight-up look does not ask for an infinite nudge.
  const float cp = std::max(0.2f, std::cos(cam.pitch));
  outJittered.yaw += rx * (2.0f * thf * aspect) / ((float)renderW * cp);
  outJittered.pitch += ry * (2.0f * thf) / (float)renderH;

  // ---- and what that nudge actually did, in pixels -------------------------
  // MEASURED, not derived. The resolve has to know where the image moved to
  // within a fraction of a pixel or its reconstruction filter is centred on the
  // wrong place, and a sign or a cos(pitch) term recovered from the rotation
  // algebra is exactly the kind of thing that is wrong once and then wrong
  // quietly. So: take the UNJITTERED forward direction — the one that landed on
  // the exact centre of the screen before the nudge — and project it through
  // the JITTERED basis. Where it lands, relative to the centre, IS the shift,
  // sign and perspective term included. The small-angle algebra above only has
  // to get the amplitude roughly right; this is the truth.
  const Vec3 jr = outJittered.Right(), ju = outJittered.Up(),
             jf = outJittered.Forward();
  const Vec3 f0 = cam.Forward();
  const float vz = std::max(1e-4f, f0.dot(jf));
  const float nx = f0.dot(jr) / (vz * thf * aspect);
  const float ny = f0.dot(ju) / (vz * thf);
  outTaa.right[0] = jr.x; outTaa.right[1] = jr.y; outTaa.right[2] = jr.z;
  outTaa.up[0] = ju.x;    outTaa.up[1] = ju.y;    outTaa.up[2] = ju.z;
  outTaa.fwd[0] = jf.x;   outTaa.fwd[1] = jf.y;   outTaa.fwd[2] = jf.z;
  // `eye` is ABSOLUTE world voxels — the residency window is a view into the
  // infinite world at absolute chunk coordinates (world.h: "the resident cube
  // covers world chunks [origin, origin+kNChunk)"), so a window shift does not
  // move it and the frame-to-frame delta is real camera motion and nothing
  // else. Widened to double so the subtraction in WriteTaaParams keeps the
  // sub-voxel part at the tens of thousands of voxels a 20 km map reaches.
  outTaa.eye[0] = (double)eye.x;
  outTaa.eye[1] = (double)eye.y;
  outTaa.eye[2] = (double)eye.z;
  outTaa.tanHalfFov = thf;
  outTaa.aspect = aspect;
  outTaa.jitterX = (nx * 0.5f) * (float)renderW;
  outTaa.jitterY = (-ny * 0.5f) * (float)renderH;
}

// The SPEC_* record (sim/renderspec.h). Written only by WriteRenderParams
// below, read only by Simulation::DrawWorld.
namespace {
RenderSpec gRenderSpec;
// The sim's gas latch, published by Simulation::EncodeTick and read by
// WriteRenderParams below (renderspec.h). A file-local flag for the reason
// gRenderSpec above is one: the value crosses from simulation.cpp to the one
// author of the flag word, and neither TU may include the other's header.
bool gGasRenderActive = false;
// ...and the same latch for the LONG-RANGE box (world.h kGasFarOuterN). Its
// own variable rather than a second bit of the one above, because the two gate
// different work and are true in different worlds.
bool gGasFarRenderActive = false;
}
const RenderSpec& LastRenderSpec() { return gRenderSpec; }
void SetGasRenderActive(bool active) { gGasRenderActive = active; }
bool GasRenderActive() { return gGasRenderActive; }
void SetGasFarRenderActive(bool active) { gGasFarRenderActive = active; }
bool GasFarRenderActive() { return gGasFarRenderActive; }


// ============================================================================
// THE CLOUDS' half of the frame uniforms (cloud.wgsl, src/sim/weather.h)
// ============================================================================
namespace {
CloudFrame gCloudFrame;

// Cloud DRIFT in metres, a pure function of the sim clock.
//
// The drift is the integral of the wind over time, and an integral is exactly
// the kind of state this engine refuses to keep. It used to be summed here in
// closed form over WindWeather's epoch blend; since 2026-09-30 the SAME sum
// moves the gust fronts (windfield.h AirDrift / AdvPhase — a prefix over
// 16-tick blocks of the reference wind, memoised per tuning), so the sky and
// the grass are carried by one wind rather than two integrals of it that
// agree only while the weather is a smoothstep between epochs (storm
// envelopes and the sky-driven regime are not).
void CloudDrift(const Tuning& tun, uint32_t seed, double tSec, double& dx, double& dz) {
  if (tSec < 0.0) tSec = 0.0;
  const double tt = tSec * 30.0;
  const double fl = std::floor(tt);
  const uint32_t tick = (uint32_t)std::min(fl, 4.0e9);
  windfield::AirDrift(tun, seed, tick, (float)(tt - fl), dx, dz);
}

double Wrap(double v, double period) {
  const double r = std::fmod(v, period);
  return r < 0.0 ? r + period : r;
}

// Per-call state the temporal resolve needs: the previous frame's camera and
// target, and the ping-pong parity. Render-only, so it may live here.
struct CloudPrev {
  bool valid = false;
  double eye[3] = {0, 0, 0};
  float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, fwd[3] = {0, 0, 1};
  float tanHalfFov = 1.0f, aspect = 1.0f;
  uint32_t lowW = 0, lowH = 0;
  uint32_t parity = 0;
  uint32_t frame = 0;
  uint32_t age = 0;
  double wall = -1.0;
};
// One per VIEW: the game's (and every harness's) main view, and the
// auxiliary one a portrait draws inside the main view's frame. Sharing one
// was the bug: the portrait's different target size failed the main view's
// size test every frame (no accumulation ever), and its parity flip made the
// main view resolve into the half it had just read.
CloudPrev gCloudPrev;
CloudPrev gCloudPrevAux;
unsigned gVeilPitch = 0;

// ---- WEATHER-MAP REUSE (cloud.wgsl `weather`) -----------------------------
// The weather map is 512^2 texels of six 5-octave gradient-noise fBms — about
// 31M hashes and 16M sincos — and it used to be rebuilt in full every frame
// although nothing in it moves at frame rate. Everything it depends on is one
// of three kinds:
//   * TRANSLATIONS of the noise domain: the drift (weatherOff) and the camera
//     re-centring (weatherOrigin). A translated field is the same field read
//     at a shifted position, so the map is REUSED and the lookup (weatherAt)
//     adds the metres the drift has moved since it was built (spare.yz). No
//     approximation at all while the reused map still covers the view.
//   * SLOW CHANGES of shape: weatherEvolve (1 unit per 25 min, on a lattice
//     of cloudWeatherScaleM / 0.7 ~ 17 km) and the preset's coverage / type /
//     precip (weather transitions over minutes). Rebuilt when they move past
//     a step small enough that the step is below what the eye can see — the
//     evolve step is ~7 m of front movement, under a 125 m texel.
//   * a changed map scale or a first frame: rebuilt outright.
// The camera probe (cloudMaps CLOUD_PROBE_BASE) is written by the regen, so
// between regens it is the column the camera was over at the last one — at
// most a few hundred metres off, for a rain-here flag.
struct WeatherGen {
  bool valid = false;
  float off[2] = {0, 0};
  float origin[2] = {0, 0};
  float evolve = 0, coverage = 0, precip = 0, cloudType = 0, scaleM = 0;
};
WeatherGen gWeatherGen;      // the map on the GPU (committed by the recorder)
WeatherGen gWeatherPending;  // what this frame's uniform asked to build
bool gWeatherRegenAsked = false;

}  // namespace

const CloudFrame& LastCloudFrame() { return gCloudFrame; }
unsigned LastVeilPitch() { return gVeilPitch; }
CloudHistoryProbe MainCloudHistory() {
  CloudHistoryProbe p;
  p.valid = gCloudPrev.valid;
  p.age = gCloudPrev.age;
  p.lowW = gCloudPrev.lowW;
  p.lowH = gCloudPrev.lowH;
  return p;
}
void CommitCloudWeather() {
  if (!gWeatherRegenAsked) return;
  gWeatherGen = gWeatherPending;
  gWeatherRegenAsked = false;
}

// Fills the weather fields of `rp` and uploads CloudParams. Called from
// WriteRenderParams, which is the single author of every frame uniform, so
// every drawing path — the game, --shot, the lab, the perf harness, the
// portrait — gets the same sky with nothing to remember.
static void WriteCloudParams(const rhi::Queue& queue, const World& world,
                             RenderParams& rp, const Vec3& eyeV, float aspect,
                             float viewPx, uint32_t tick, float frameFrac,
                             const SkyState& sky, uint32_t targetW,
                             uint32_t targetH, bool auxView) {
  const float eyeA[3] = {eyeV.x, eyeV.y, eyeV.z};
  struct { float x, y, z; } eye{eyeV.x, eyeV.y, eyeV.z};
  const Tuning& tun = CurrentTuning();
  const Tuning::Render& rr = tun.render;
  const double tSec = ((double)tick + (double)frameFrac) / 30.0;
  CloudPrev& prev = auxView ? gCloudPrevAux : gCloudPrev;
  const double wall = NowSeconds();
  float dt = 0.0f;
  if (prev.wall >= 0.0) dt = (float)std::clamp(wall - prev.wall, 0.0, 0.25);
  prev.wall = wall;

  const float camXM = eye.x * kVoxelMeters, camZM = eye.z * kVoxelMeters;
  // An aux view PEEKS at the weather (commit = false): the main view's eased
  // sky, with neither the ease nor Last() touched.
  const weather::State st =
      weather::Resolve(tun, rp.seed, tSec, dt, !auxView);
  const weather::Preset& w = st.mix;
  const bool on = st.enabled &&
                  (w.coverage > 0.001f || w.cirrus > 0.001f || w.precip > 0.001f);

  CloudParams cp{};
  // The TARGET's size, which is what the raymarch's fragXY counts in. viewPx
  // is not always it: under TAA + render.taaSharpLod viewPx is the native
  // height (an LOD bias) while the target is renderW x renderH.
  const uint32_t fullH = targetH ? targetH
                                 : (uint32_t)std::max(1.0f, std::round(viewPx));
  const uint32_t fullW =
      targetW ? targetW : (uint32_t)std::max(1.0f, std::round(viewPx * aspect));
  const uint32_t div = (uint32_t)std::clamp(rr.cloudResDiv, 1, 8);
  const uint32_t lowW = (fullW + div - 1) / div, lowH = (fullH + div - 1) / div;
  cp.fullW = fullW;
  cp.fullH = fullH;
  cp.lowW = lowW;
  cp.lowH = lowH;
  cp.resDiv = div;

  // ---- the temporal history ----
  // Valid only if last frame also marched, into a target of the same size.
  // Anything else — the first frame, a resize, a portrait drawn between two
  // game frames at a different size — starts the history over.
  const bool histValid = on && prev.valid && prev.lowW == lowW &&
                         prev.lowH == lowH;
  prev.parity ^= 1u;
  const uint32_t half = lowW * lowH * kCloudHistWords;
  cp.histCur = prev.parity * half;
  cp.histPrev = (prev.parity ^ 1u) * half;
  cp.flags = (on ? kClfOn : 0u) | (histValid ? kClfHistValid : 0u);
  // Frames the history has accumulated since it was last reset. The resolve
  // blends at max(render.cloudTemporal, 1 / (age + 1)): an exact running MEAN
  // for the first frames after a reset, so a four-frame --shot is the average
  // of four jittered marches rather than one march with three ghosts on it.
  prev.age = histValid ? prev.age + 1 : 0;
  cp.spare[0] = (float)prev.age;
  cp.frame = ++prev.frame;
  for (int i = 0; i < 3; i++) {
    cp.prevRight[i] = prev.right[i];
    cp.prevUp[i] = prev.up[i];
    cp.prevFwd[i] = prev.fwd[i];
    cp.eyeDeltaM[i] = (float)(((double)eyeA[i] - prev.eye[i]) * (double)kVoxelMeters);
  }
  cp.prevTanHalfFov = prev.tanHalfFov;
  cp.prevAspect = prev.aspect;
  // R2 low-discrepancy jitter of the LOW-RES grid: over a handful of frames
  // the accumulated history has sampled every sub-texel position, which is
  // the quarter-resolution march paying for a half-resolution image.
  {
    const double g = 1.32471795724474602596;
    const double a1 = 1.0 / g, a2 = 1.0 / (g * g);
    cp.jitter[0] = (float)(std::fmod(0.5 + a1 * cp.frame, 1.0) - 0.5);
    cp.jitter[1] = (float)(std::fmod(0.5 + a2 * cp.frame, 1.0) - 0.5);
  }
  for (int i = 0; i < 3; i++) {
    prev.right[i] = rp.camRight[i];
    prev.up[i] = rp.camUp[i];
    prev.fwd[i] = rp.camFwd[i];
    prev.eye[i] = (double)eyeA[i];
  }
  prev.tanHalfFov = rp.tanHalfFov;
  prev.aspect = rp.aspect;
  prev.lowW = lowW;
  prev.lowH = lowH;
  prev.valid = on;

  // ---- the weather ----
  cp.coverage = w.coverage;
  cp.cloudType = w.cloudType;
  cp.density = w.density;
  cp.precip = w.precip;
  cp.baseM = w.baseM;
  cp.thicknessM = w.thicknessM;
  cp.darkness = w.darkness;
  cp.cirrus = w.cirrus;
  cp.cirrusAltM = w.cirrusAltM;
  cp.precipType = w.precipType;
  cp.overcast = st.overcast;
  cp.wetness = st.wetness;
  cp.mist = w.mist;
  cp.camM[0] = eye.x * kVoxelMeters;
  cp.camM[1] = eye.y * kVoxelMeters;
  cp.camM[2] = eye.z * kVoxelMeters;

  // ---- the drift: every unbounded quantity folded here, in double ----
  double dx = 0.0, dz = 0.0;
  CloudDrift(tun, rp.seed, tSec, dx, dz);
  const double ws = (double)rr.cloudWindScale;
  dx *= ws;
  dz *= ws;
  {
    const double wn = std::sqrt(dx * dx + dz * dz);
    // Downwind unit for the towers' lean and the cirrus streaks: the surface
    // wind's own heading this frame.
    cp.windX = rp.windDir[0];
    cp.windZ = rp.windDir[1];
    (void)wn;
  }
  const double shapeS = rr.cloudShapeScaleM, detailS = rr.cloudDetailScaleM;
  const double weatherS = rr.cloudWeatherScaleM, cirrusS = rr.cloudCirrusScaleM;
  // Pattern(p - drift): the offset is MINUS the drift, per tile. A slow rise
  // on the shape's y axis makes the towers boil rather than slide.
  cp.shapeOff[0] = (float)Wrap(-dx / shapeS, 1.0);
  cp.shapeOff[1] = (float)Wrap(-tSec * 0.9 / shapeS, 1.0);
  cp.shapeOff[2] = (float)Wrap(-dz / shapeS, 1.0);
  cp.detailOff[0] = (float)Wrap(-dx * 1.15 / detailS, 1.0);
  cp.detailOff[1] = (float)Wrap(-tSec * 2.5 / detailS, 1.0);
  cp.detailOff[2] = (float)Wrap(-dz * 1.15 / detailS, 1.0);
  // The weather fields move a little slower than the clouds in them (the
  // "offset the coverage and texture movement" trick, plan §1c), and morph in
  // their own time.
  cp.weatherOff[0] = (float)Wrap(-dx * 0.8 / weatherS, 4096.0);
  cp.weatherOff[1] = (float)Wrap(-dz * 0.8 / weatherS, 4096.0);
  cp.weatherEvolve = (float)Wrap(tSec / 1500.0, 4096.0);
  cp.cirrusOff[0] = (float)Wrap(-dx * 1.8 / cirrusS, 1.0);
  cp.cirrusOff[1] = (float)Wrap(-dz * 1.8 / cirrusS, 1.0);
  cp.cirrusEvolve = (float)Wrap(tSec / 4000.0, 1.0);

  // ---- the maps' footprints, snapped to their texel grids ----
  cp.weatherTexelM = 125.0f;
  {
    const double t = cp.weatherTexelM;
    const double h = (double)kCloudWeatherN * 0.5 * t;
    cp.weatherOrigin[0] = (float)(std::floor(((double)camXM - h) / t) * t);
    cp.weatherOrigin[1] = (float)(std::floor(((double)camZM - h) / t) * t);
  }
  // ---- reuse the weather map, or rebuild it (WeatherGen above) ----
  {
    const WeatherGen& g = gWeatherGen;
    const float S = (float)weatherS;
    const float shiftX = (cp.weatherOff[0] - g.off[0]) * S;
    const float shiftZ = (cp.weatherOff[1] - g.off[1]) * S;
    // Slack: 16 texels (2 km) of combined drift + camera travel from the
    // centre the map was built round. The map reaches 256 texels (32 km) each
    // way, so the view keeps >= 30 km of it in every direction.
    const float slackM = 16.0f * cp.weatherTexelM;
    const float lagX = std::fabs(cp.weatherOrigin[0] - g.origin[0]);
    const float lagZ = std::fabs(cp.weatherOrigin[1] - g.origin[1]);
    const bool regen =
        !g.valid || g.scaleM != S ||
        std::fabs(shiftX) + lagX > slackM || std::fabs(shiftZ) + lagZ > slackM ||
        std::fabs(cp.weatherEvolve - g.evolve) > 4e-4f ||
        std::fabs(cp.coverage - g.coverage) > 2e-3f ||
        std::fabs(cp.precip - g.precip) > 2e-3f ||
        std::fabs(cp.cloudType - g.cloudType) > 2e-3f;
    // An aux view records no cloud passes, so it must not ask for (or stage)
    // a regen the main view's recorder would then commit as its own.
    if (!auxView) gWeatherRegenAsked = on && regen;
    if (regen) {
      static WeatherGen auxScratch;
      WeatherGen& p = auxView ? auxScratch : gWeatherPending;
      p.valid = true;
      p.off[0] = cp.weatherOff[0];
      p.off[1] = cp.weatherOff[1];
      p.origin[0] = cp.weatherOrigin[0];
      p.origin[1] = cp.weatherOrigin[1];
      p.evolve = cp.weatherEvolve;
      p.coverage = cp.coverage;
      p.precip = cp.precip;
      p.cloudType = cp.cloudType;
      p.scaleM = S;
      cp.flags |= kClfWeather;
      cp.spare[1] = 0.0f;
      cp.spare[2] = 0.0f;
    } else {
      // The map on the GPU was built at g.origin with g.off: read it there,
      // shifted by the drift since. The regen-only fields (off, origin) are
      // left as the map's own so nothing can read a mixed pair.
      cp.weatherOrigin[0] = g.origin[0];
      cp.weatherOrigin[1] = g.origin[1];
      cp.weatherOff[0] = g.off[0];
      cp.weatherOff[1] = g.off[1];
      cp.spare[1] = shiftX;
      cp.spare[2] = shiftZ;
    }
  }
  cp.shadowTexelM = 40.0f;
  // Just under the lowest local base: the jitter cap mirrors common.wgsl's
  // cloudBaseJitterM (10% of thickness, at most CLOUD_BASE_JITTER_MAX_M
  // 150 m). 0.12 x thickness put a storm's plane 200 m below sea level.
  cp.shadowPlaneM =
      w.baseM - std::min(0.1f * w.thicknessM, 150.0f) - 0.02f * w.thicknessM;
  {
    // Centre the shadow map where the CAMERA's light ray crosses the plane,
    // so the ground around the player is always inside it. The key light is
    // the sun by day and moon A by night — keyLightDirP's rule to within the
    // two-moon tie-break, which only moves the map's centre, never a value.
    const float* L = sky.sunUp >= 0.5f ? sky.sunDir : sky.moonDir;
    const double ly = std::max((double)L[1], 0.05);
    const double up = (double)cp.shadowPlaneM - (double)cp.camM[1];
    const double qx = (double)camXM + (double)L[0] / ly * up;
    const double qz = (double)camZM + (double)L[2] / ly * up;
    const double t = cp.shadowTexelM;
    const double h = (double)kCloudShadowN * 0.5 * t;
    cp.shadowOrigin[0] = (float)(std::floor((qx - h) / t) * t);
    cp.shadowOrigin[1] = (float)(std::floor((qz - h) / t) * t);
  }

  // ---- lightning ----
  cp.flash[0] = camXM + st.flashX;
  cp.flash[1] = w.baseM + w.thicknessM * 0.35f;
  cp.flash[2] = camZM + st.flashZ;
  cp.flashAmp = on ? st.flash : 0.0f;

  queue.WriteBuffer(world.cloudUBO, 0, &cp, sizeof(cp));

  // ---- the RenderParams half ----
  rp.weatherFlags = (on ? kRwfClouds : 0u) |
                    (on && w.precip > 0.01f && w.coverage > 0.2f ? kRwfRain : 0u);
  // An aux view (the character screen's portrait) raymarches the world too,
  // but the cloud deck it would composite is the MAIN view's screen-space
  // resolve -- another camera, another size -- and the rain streaks ride the
  // same flag pair. Its sky is the plain one; overcast and wetness still
  // colour the ground below.
  if (auxView) rp.weatherFlags = 0u;
  rp.overcast = on ? st.overcast : 0.0f;
  rp.wetness = on ? st.wetness : 0.0f;
  // The flash reaches the ground only as the part of it not lost in the deck;
  // a distant stroke is a glow on the clouds, a close one lights the field.
  rp.lightning = on ? st.flash * 0.9f : 0.0f;
  // Mist: rain and fog thicken the air. A multiplier on the far-field fog
  // density the caller computed, so the horizon still dissolves where the
  // cascades end — mist only brings it closer.
  if (on) rp.fogDensity *= 1.0f + w.mist;

  // An aux view records no cloud passes of its own (and composites none, see
  // above), and the main view writing its params next frame must find the
  // recorder's answer still its own.
  if (!auxView) {
    gCloudFrame.on = on;
    gCloudFrame.lowW = lowW;
    gCloudFrame.lowH = lowH;
  }
}

// ---- THE FRAME'S LIGHT: one author for the key light and the ambient ------
// Fills RenderParams' dayWeight / moonLit / keyDir / keyCol / ambGround /
// ambSky / ambOvercast from the sky state already in `rp` and the TUNE_*
// values, exactly as the per-pixel WGSL did (common.wgsl keyLightColorP,
// keyLightDirP, ambientAtP, moonContribP, sunTransmittance, airMass — the
// shader now reads the results instead of re-deriving them). Everything here
// is frame-constant, so it was 21 + 18 call sites of per-pixel smoothsteps,
// pows and an acos computing the same numbers, in two copies that had
// drifted apart (the terrain's missed the overcast and lightning).
//
// Render-only float math: nothing here reaches the sim or the world hash.
// Called after WriteCloudParams, which is what writes overcast / lightning.
static void ResolveFrameLight(RenderParams& rp, const Tuning::Render& r) {
  auto clampf = [](float x, float lo, float hi) { return std::min(std::max(x, lo), hi); };
  auto smooth = [&](float e0, float e1, float x) {
    const float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  };
  // moonContribP: up-ness x intensity x (illuminated fraction)^2 shaping.
  auto moonContrib = [&](const float* dir, float phase, float intensity) {
    return smooth(-0.10f, 0.18f, dir[1]) * intensity *
           (0.15f + 1.70f * phase * phase);
  };
  // airMass (Kasten-Young) and sunTransmittance, common.wgsl.
  const float c = clampf(rp.sunDir[1], -0.02f, 1.0f);
  const float zdeg = std::acos(clampf(c, -1.0f, 1.0f)) * (180.0f / 3.14159265358979f);
  const float mass = 1.0f / (c + 0.50572f * std::pow(std::max(96.07995f - zdeg, 1e-3f), -1.6364f));
  static const float kRayleigh[3] = {0.1440f, 0.3125f, 0.7940f};
  const float kSunTransmitK = 0.09f;

  const float a = moonContrib(rp.moonDir, rp.moonPhase, r.moonLightIntensity);
  const float b = moonContrib(rp.moon2Dir, rp.moon2Phase, r.moon2LightIntensity);
  const float f = clampf(rp.solarEclipse, 0.0f, 1.0f);
  const float dayW = rp.sunUp * (1.0f - std::pow(f, r.eclipseCurve) * r.eclipseDarkness);
  const float inv = 1.0f / std::max(r.moonLightIntensity, 1e-4f);
  const float moonLit = (a + b) * inv;
  rp.dayWeight = dayW;
  rp.moonLit = moonLit;

  // Key colour: the brighter moon by night, the reddened sun by day.
  for (int k = 0; k < 3; k++) {
    const float sunCol = std::exp(-kRayleigh[k] * mass * kSunTransmitK * r.sunReddening) *
                         r.sunColor[k] * r.sunIntensity;
    const float keyMoon = a >= b ? r.moonLightColor[k] * a : r.moon2LightColor[k] * b;
    rp.keyCol[k] = keyMoon + (sunCol - keyMoon) * dayW;
  }
  // Key direction: a hard switch at RAW sunUp = 0.5 (keyLightDirP's argument
  // for not blending, and for not using the eclipse-dimmed weight).
  {
    const float* d = rp.sunUp >= 0.5f ? rp.sunDir : (a >= b ? rp.moonDir : rp.moon2Dir);
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float il = len > 0.0f ? 1.0f / len : 0.0f;
    for (int k = 0; k < 3; k++) rp.keyDir[k] = d[k] * il;
    if (len <= 0.0f) rp.keyDir[1] = 1.0f;
  }
  // Hemisphere ambient at its two ends. Linear in n.y, so the shader's
  // mix(ambGround, ambSky, n.y * 0.5 + 0.5) is the old expression exactly.
  const float moonAmt = 0.30f * (moonLit >= 0.001f ? 1.0f : 0.0f) + 1.40f * moonLit * 0.5f;
  for (int k = 0; k < 3; k++) {
    const float nightG = r.nightAmbGround[k] * (0.45f + moonAmt);
    const float nightS = r.nightAmbSky[k] * (0.45f + moonAmt);
    rp.ambGround[k] = nightG + (r.ambGround[k] - nightG) * dayW;
    rp.ambSky[k] = nightS + (r.ambSky[k] - nightS) * dayW;
  }
  rp.ambOvercast = clampf(rp.overcast, 0.0f, 1.0f) * 0.8f;
}

void WriteRenderParams(const rhi::Queue& queue, const World& world,
                       const Vec3& eye, const Camera& cam, float aspect,
                       bool shadows, float time,
                       float fogDensity, float viewPx, uint32_t tick,
                       uint32_t fluidCount, float frameFrac,
                       uint32_t extraFlags, uint32_t targetW,
                       uint32_t targetH, bool auxView) {
  RenderParams rp{};
  rp.fluidCount = fluidCount;  // 0 skips the MPM fluid surface march entirely
  Vec3 f = cam.Forward(), r = cam.Right(), u = cam.Up();
  rp.camPos[0] = eye.x; rp.camPos[1] = eye.y; rp.camPos[2] = eye.z;
  rp.camRight[0] = r.x; rp.camRight[1] = r.y; rp.camRight[2] = r.z;
  rp.camUp[0] = u.x; rp.camUp[1] = u.y; rp.camUp[2] = u.z;
  rp.camFwd[0] = f.x; rp.camFwd[1] = f.y; rp.camFwd[2] = f.z;
  rp.tanHalfFov = std::tan(CurrentTuning().camera.fovY * 0.5f);
  rp.aspect = aspect;
  rp.time = time;
  // bit 0 = sun shadows, bit 1 = active-voxel debug highlight (extraFlags),
  // bit 2 = short-range mode, bit 3 = gas may be present (the crossfade;
  // docs/PLAN_gas_particles.md stage 1b), bit 4 = short-range NEAR arm (the
  // 50 m ceiling instead of the 100 m one), bit 5 = the LONG-RANGE gas box has
  // something in it (world.h kGasFarOuterN — a frozen fire past 51.2 m). Bits
  // 2, 3, 4 and 5 are OR'd in here rather than passed by the caller so that
  // every drawing path gets them — see ShortRangeMode above and
  // SetGasRenderActive / SetGasFarRenderActive in renderspec.h.
  //
  // Bit 4 is deliberately NOT reflected into RenderSpec: it picks a distance
  // inside a branch bit 2 already guards, so it changes no shader's shape and
  // must not double the pipeline variants.
  rp.flags = (shadows ? 1u : 0u) | extraFlags | (ShortRangeMode() ? 4u : 0u) |
             (GasRenderActive() ? 8u : 0u) | (ShortRangeNear() ? 16u : 0u) |
             (GasFarRenderActive() ? 32u : 0u);
  // Publish the SPEC_* predicates for this frame (support.h RenderSpec). Read
  // off `rp` rather than off the arguments, so the record is the WORD THAT WAS
  // UPLOADED and not a second derivation of it.
  gRenderSpec.fluid = rp.fluidCount > 0u;
  gRenderSpec.debugViz = (rp.flags & 2u) != 0u;
  gRenderSpec.shortRange = (rp.flags & 4u) != 0u;
  // ---- the shadow cache's clock (world.h kShadowCacheBuckets) ----
  // ONE CALL HERE IS ONE RENDERED FRAME, which is exactly the clock the cache
  // needs and the reason the counter lives in this function rather than in the
  // frame loop: every path that draws (the game loop, --shot, the lab, the perf
  // harness, the character portrait) writes render params first, so none of
  // them can forget to advance it. Wraps at 2^32 and only the low 4 bits are
  // compared, so the wrap is a non-event.
  static uint32_t renderFrame = 0;
  rp.frameIdx = ++renderFrame;
  rp.shadowSubdiv = (uint32_t)CurrentTuning().render.shadowCacheSubdiv;
  // ~41 deg elevation: low enough that terrain and canopy cast readable
  // shadows (near field AND the far-field cascade shadow march), high enough
  // that valleys aren't pits. The old 0.78 y put the sun ~52 deg up and
  // flattened the world — shadows were 1-2 cells long and the far field read
  // as unlit wallpaper.
  // Sun/moon now come from the tick-driven cycle rather than a fixed tuning
  // vector: render.sunDir survives only as the fallback when the cycle is
  // disabled (cycleMinutes clamped, freeze pinned) and as the tuner's manual
  // handle. See ComputeSkyState.
  const Tuning& tun = CurrentTuning();
  SkyState sky = ComputeSky(tun, Celestial().RenderTickInterp(tick, (double)frameFrac));
  rp.sunDir[0] = sky.sunDir[0];
  rp.sunDir[1] = sky.sunDir[1];
  rp.sunDir[2] = sky.sunDir[2];
  rp.moonDir[0] = sky.moonDir[0];
  rp.moonDir[1] = sky.moonDir[1];
  rp.moonDir[2] = sky.moonDir[2];
  rp.dayT = sky.dayT;
  rp.sunUp = sky.sunUp;
  rp.moonPhase = sky.moonPhase;
  rp.starRot = sky.starRot;
  // Moon B + eclipse geometry. All of it falls out of the orbital solve — the
  // renderer is told where the bodies ARE and how big they look, and draws
  // them; it decides nothing about the sky's state.
  rp.moon2Dir[0] = sky.moon2Dir[0];
  rp.moon2Dir[1] = sky.moon2Dir[1];
  rp.moon2Dir[2] = sky.moon2Dir[2];
  rp.moon2Phase = sky.moon2Phase;
  rp.moonAngRadius = sky.moonAngRadius;
  rp.moon2AngRadius = sky.moon2AngRadius;
  rp.moonPhaseSign = sky.moonPhaseSign;
  rp.moon2PhaseSign = sky.moon2PhaseSign;
  rp.solarEclipse = sky.solarEclipse;
  rp.lunarEclipse = sky.lunarEclipse;
  rp.eclipseBody = sky.eclipseBody;
  // The axis the starfield wheels about — derived from latitude, so the stars
  // turn about the same pole the sun arcs around. raymarch.wgsl reads it in
  // BOTH starField() and nightGlow(); leaving it unwritten does not fail, it
  // renders the night sky as flat black (see the poleDir note in world.h).
  rp.poleDir[0] = sky.poleDir[0];
  rp.poleDir[1] = sky.poleDir[1];
  rp.poleDir[2] = sky.poleDir[2];
  // MPM fluid render bounds (plan §7 item 5). The AABB is what actually makes
  // the fluid march sleep: `fluidCount` is a MONOTONE estimate that never
  // decays, so without this a world that once held water pays a screen-wide
  // block-map march forever. An empty box (lo > hi) is the "no fluid" signal
  // the shader tests, and it costs one slab test per ray.
  {
    IVec3 flo{0, 0, 0}, fhi{-1, -1, -1};
    world.FluidRenderBounds(tick, flo, fhi);
    rp.fluidLo[0] = flo.x; rp.fluidLo[1] = flo.y; rp.fluidLo[2] = flo.z;
    rp.fluidHi[0] = fhi.x; rp.fluidHi[1] = fhi.y; rp.fluidHi[2] = fhi.z;
  }
  rp.fogDensity = fogDensity;  // horizon fades at the trusted far-field extent
  rp.viewPx = viewPx;          // water ripple LOD footprint (see world.h)
  // Micro-detail animation clock + per-cell variation key (see world.h). Both
  // are render-only inputs; the tick is passed rather than `time` so a flipbook
  // advances at the sim's rate on every machine and reproduces in a replay.
  rp.tick = tick;
  rp.seed = kDefaultSeed;
  // Wind. The evolving half of the field (docs/RESEARCH_wind.md §4.2) — the
  // rest of it is TUNE_* constants folded into the shader. Derived from the
  // TICK, not from `time`, for the same reason the flipbook clock is: weather
  // that advanced with wall time would run at a different rate per machine and
  // would not reproduce in a replay. WindWeather is the only author of these,
  // here and (phase 4) in TickParams, so the renderer and the CA cannot end up
  // in different weather.
  {
    const WindState wind = WindWeather(tun, rp.seed, tick, DayPhaseNow(tick));
    rp.windDir[0] = wind.dirX;
    rp.windDir[1] = wind.dirZ;
    rp.windSpeed = wind.speed;
    rp.windGust = wind.gust;
    // The field block, from the same resolution the sim's copy comes from
    // (SubmitTick), with the three clocks at the frame's sub-tick instant so
    // the grass animates between ticks instead of stepping at 30 Hz.
    {
      const IVec3 wo = world.WindowOrigin();
      const int32_t o3[3] = {wo.x, wo.y, wo.z};
      windfield::FillWindField(rp, tun, rp.seed, tick, frameFrac, DayPhaseNow(tick), o3);
    }
  }
  // WIND PRIMITIVES (§4.3). The SAME resolved list SubmitTick shipped to the
  // sim this tick — WindPrims() is advanced there and read here, which is what
  // makes the grass lean in a fan's blast and the debug arrows agree with the
  // smoke. Copied rather than shared because the sim and render bind groups
  // deliberately have no buffer in common.
  {
    const WindPrimSystem& wp = WindPrims();
    const uint32_t n = std::min(wp.Count(), kWindPrimCap);
    rp.windPrimCount = n;
    const IVec3 lo = wp.BoundsLo(), hi = wp.BoundsHi();
    rp.windPrimLo[0] = lo.x; rp.windPrimLo[1] = lo.y; rp.windPrimLo[2] = lo.z;
    rp.windPrimHi[0] = hi.x; rp.windPrimHi[1] = hi.y; rp.windPrimHi[2] = hi.z;
    for (uint32_t i = 0; i < n; i++)
      std::memcpy(&rp.windPrims[i * kWindPrimWords], wp.Resolved()[i].w,
                  kWindPrimWords * sizeof(int32_t));
  }
  // THE CURRENT FIELD, the render copy (plan component 8). The SAME resolved
  // list SubmitTick shipped to the sim this tick — CurrentPrims() is advanced
  // there and read here, exactly as WindPrims() is, which is what makes the
  // surface waves drift the way the arrow overlay says they should.
  //
  // `currentRenderOn` is NOT sim.currentMode. The render arm is always live
  // because a renderer cannot write a voxel; the sim arm is the gated one. That
  // asymmetry is the whole shape of M4 — see DESIGN.md §9c.
  {
    const CurrentPrimSystem& cp = CurrentPrims();
    const uint32_t n = std::min(cp.Count(), kCurrentPrimCap);
    rp.currentRenderOn = 1u;
    rp.currentPrimCount = n;
    const IVec3 lo = cp.BoundsLo(), hi = cp.BoundsHi();
    rp.currentPrimLo[0] = lo.x; rp.currentPrimLo[1] = lo.y;
    rp.currentPrimLo[2] = lo.z;
    rp.currentPrimHi[0] = hi.x; rp.currentPrimHi[1] = hi.y;
    rp.currentPrimHi[2] = hi.z;
    for (uint32_t i = 0; i < n; i++)
      std::memcpy(&rp.currentPrims[i * kCurrentPrimWords],
                  cp.Resolved()[i].w, kCurrentPrimWords * sizeof(int32_t));
  }
  // Component 9's impact ring. Bounded by construction and render-only: no sim
  // kernel reads it, it is not hashed and it is not saved.
  {
    const WaveImpactRing& wi = WaveImpacts();
    const uint32_t n = std::min(wi.Count(), kWaveImpactCap);
    rp.waveImpactCount = n;
    for (uint32_t i = 0; i < n; i++) {
      rp.waveImpacts[i * 4 + 0] = wi.Data()[i].x;
      rp.waveImpacts[i * 4 + 1] = wi.Data()[i].z;
      rp.waveImpacts[i * 4 + 2] = wi.Data()[i].t0;
      rp.waveImpacts[i * 4 + 3] = wi.Data()[i].amp;
    }
  }
  // The trample ring (sim/trample.h): footprints the plants flatten under.
  // Render-only like the impact ring above; the frame loop presses it, this
  // is its only reader. Empty in every headless path unless a gate presses.
  {
    const TrampleRing& tr = Tramples();
    const uint32_t n = std::min(tr.Count(), kTrampleCap);
    rp.trampleCount = n;
    float lo[3], hi[3];
    if (n > 0 && tr.Bounds(lo, hi)) {
      for (int k = 0; k < 3; k++) { rp.trampleLo[k] = lo[k]; rp.trampleHi[k] = hi[k]; }
    } else {
      for (int k = 0; k < 3; k++) { rp.trampleLo[k] = 1.0f; rp.trampleHi[k] = 0.0f; }
    }
    for (uint32_t i = 0; i < n; i++) {
      const TrampleStamp& s = tr.Data()[i];
      float* w = &rp.tramples[i * 8];
      w[0] = s.x; w[1] = s.z; w[2] = s.y; w[3] = s.radius;
      w[4] = s.t0; w[5] = s.tEnd; w[6] = s.strength; w[7] = 0.0f;
    }
  }
  IVec3 o = world.WindowOrigin();
  rp.origin[0] = o.x; rp.origin[1] = o.y; rp.origin[2] = o.z;
  // The clouds and the weather (cloud.wgsl): CloudParams, and the four
  // RenderParams fields they own. Last, because it scales rp.fogDensity.
  WriteCloudParams(queue, world, rp, eye, aspect, viewPx, tick, frameFrac, sky,
                   targetW, targetH, auxView);
  // veilBase's pitch, computed the way the shader computes it (f32 product,
  // rounded); EnsureVeil adds a column of slack for the rounding-mode tie.
  gVeilPitch = (unsigned)std::max(1.0f, std::round(rp.viewPx * rp.aspect));
  // After the clouds: the ambient's overcast and lightning come from there.
  ResolveFrameLight(rp, tun.render);
  queue.WriteBuffer(world.renderUBO, 0, &rp, sizeof(rp));
}

// Encode + submit one sim tick (uniform writes must precede the submit and
// happen once per tick, hence submit-per-tick). particlesActive must be
// derived only from tick-deterministic inputs (explosion history + a settled
// particle count), never from frame timing — see DESIGN.md §2/§4.
void SubmitTick(GpuContext& ctx, World& world, Simulation& sim, uint32_t tick,
                uint32_t seed, const std::vector<BrushOp>& opsIn,
                const std::vector<ExplosionOp>& expsIn,
                const std::vector<CellOp>& cellsIn, bool hashEnable,
                IVec3 playerChunk, bool wantReadback, bool particlesActive,
                const std::vector<ParticleSpawn>& spawns,
                uint32_t farCount,
                const std::vector<FluidSpawnOp>& fluidSpawns,
                uint32_t fluidLive,
                bool vizActive) {
  // ---- THE CHOKE POINT, AND WHAT IT NOW OWES THE STREAM -------------------
  //
  // Three of the six op vectors were clamped here and three were not: `cells`,
  // `spawns` and `fluid` were min()'d against their caps and SILENTLY
  // truncated, while `ops` and `exps` were uploaded at whatever size the
  // producers had built — into buffers sized kMaxOpsPerTick * 32 and
  // kMaxExplosionsPerTick * 32 (world.cpp). Two producers push BrushOps in a
  // loop without consulting the cap at all (mob.cpp's bleed drip,
  // avatar.cpp's sever stain), so the overrun was one busy tick away.
  //
  // So every stream is clamped HERE, at the one place all of them pass
  // through, and every refusal is COUNTED (sim/oprecord.h). A truncation that
  // is only a missing voxel is the bare-count failure CLAUDE.md rule 6 is
  // about; a truncation with a number beside it in build/last_run.json is a
  // measurement. The producer-side checks stay as the belt to this brace.
  //
  // The parameters are renamed rather than the body rewritten: rebinding the
  // three names here means the four hundred lines below keep reading `ops`,
  // `exps` and `cells` and go on describing what actually reached the GPU.
  std::vector<BrushOp> opsClamp;
  std::vector<ExplosionOp> expsClamp;
  std::vector<CellOp> cellsCanon;
  uint32_t brushTrunc = 0, expTrunc = 0;
  if (opsIn.size() > kMaxOpsPerTick) {
    brushTrunc = (uint32_t)opsIn.size() - kMaxOpsPerTick;
    opsClamp.assign(opsIn.begin(), opsIn.begin() + kMaxOpsPerTick);
  }
  if (expsIn.size() > kMaxExplosionsPerTick) {
    expTrunc = (uint32_t)expsIn.size() - kMaxExplosionsPerTick;
    expsClamp.assign(expsIn.begin(), expsIn.begin() + kMaxExplosionsPerTick);
  }
  // Cell ops: DEDUPE before the cap, so a stream that only overflows because
  // it repeats itself is not truncated for it. Keep-first in push order; a
  // tick with no duplicates leaves `cellsIn` untouched and allocates nothing.
  {
    uint32_t dupeCell = 0xFFFFFFFFu;
    const uint32_t dropped =
        opstream::CanonicalizeCells(cellsIn, cellsCanon, &dupeCell);
    if (dropped) opstream::NoteCellDupes(tick, dropped, dupeCell);
  }
  // SOLUTE POURS TO THE TAIL (world.h CellOpSolute). sim_mutate's solPour is
  // ONE invocation that walks the stream backwards from its end while the ops
  // are pours, so they must be contiguous there. A stable partition: voxel ops
  // keep their push order among themselves (the dedupe above already ran, and
  // the `cells` dispatch has no order), pours keep theirs (solPour applies them
  // in it). A tick with no pour -- nearly every tick -- is untouched.
  {
    const std::vector<CellOp>& src = cellsCanon.empty() ? cellsIn : cellsCanon;
    bool seenPour = false, needMove = false;
    for (const CellOp& op : src) {
      const bool pour = IsSoluteCellOp(op.word);
      if (!pour && seenPour) { needMove = true; break; }
      seenPour |= pour;
    }
    // THE POUR CAPS, counted (world.h kMaxSolutePourOpsPerTick): at most
    // that many pours, keep-first; and the stream's own cap
    // (kMaxCellOpsPerTick) cuts VOXEL ops before pours, because the plain
    // clamp below cuts the tail -- which is where the pours are -- and a
    // pour's mass was already taken out of a vessel.
    uint32_t pours = 0;
    for (const CellOp& op : src) pours += IsSoluteCellOp(op.word) ? 1u : 0u;
    const uint32_t keepPours = std::min(pours, kMaxSolutePourOpsPerTick);
    const uint32_t voxelOps = (uint32_t)src.size() - pours;
    const uint32_t keepVoxel = std::min(voxelOps, kMaxCellOpsPerTick - keepPours);
    if (needMove || keepPours < pours || keepVoxel < voxelOps) {
      std::vector<CellOp> part;
      part.reserve(keepVoxel + keepPours);
      uint32_t nv = 0, np = 0, lostUnits = 0;
      for (const CellOp& op : src)
        if (!IsSoluteCellOp(op.word) && nv < keepVoxel) { part.push_back(op); nv++; }
      for (const CellOp& op : src) {
        if (!IsSoluteCellOp(op.word)) continue;
        if (np < keepPours) { part.push_back(op); np++; }
        else lostUnits += (op.word >> 12) & 0xFFFu;
      }
      if (pours > keepPours) {
        opstream::NoteSolutePourTrunc(pours - keepPours, lostUnits);
        std::fprintf(stderr, "solute pours: %u op(s) past the %u-a-tick cap refused at tick %u "
                     "(%u units lost)\n", pours - keepPours, kMaxSolutePourOpsPerTick, tick,
                     lostUnits);
      }
      // Voxel ops cut here are counted as cellTrunc exactly as the clamp
      // below would have counted them.
      if (voxelOps > keepVoxel) opstream::NoteTruncation(0, 0, voxelOps - keepVoxel, 0, 0, 0);
      cellsCanon.swap(part);
    }
  }
  const std::vector<BrushOp>& ops = brushTrunc ? opsClamp : opsIn;
  const std::vector<ExplosionOp>& exps = expTrunc ? expsClamp : expsIn;
  const std::vector<CellOp>& cells = cellsCanon.empty() ? cellsIn : cellsCanon;
  // ---- HOW THIS FUNCTION IS BILLED ---------------------------------------
  //
  // SubmitTick used to be ONE bar on the Performance tab, called "submit", with
  // the tooltip "command-buffer submit and the generated barriers". It is not:
  // it is TickParams assembly, the wind and water registries, six op-stream
  // uploads, the entire CPU half of the page table (including a second
  // vkQueueSubmit inside the free probe), the pass-table walk, and only then a
  // submit. A user watching that bar jump from 0.2 ms to 30 ms on an idle frame
  // had no way to tell which of those seven things moved.
  //
  // So the function bills itself, in five non-overlapping spans, and they are
  // deliberately NOT nested — PerfSpan has no child-subtraction and a nested
  // pair would double-count. Where a span has to yield to an inner one it is
  // Close()d and a fresh one opened after.
  //
  // ---- N1: PUBLISH THE SNAPSHOT THIS TICK IS OWED ------------------------
  //
  // From here until the head of the next SubmitTick, `World::Snap()` is the
  // snapshot of tick - World::kSnapshotLatency, EXACTLY -- not the freshest one
  // that happened to land. See the block comment on that constant for why the
  // difference is the whole of finding L1: every consumer below (the wind wake,
  // the page table's tightening and occupancy fold, the fluid render bounds)
  // and every gameplay decision the caller made for this tick used to read a
  // world whose AGE depended on how fast this machine's GPU came back.
  //
  // WHERE THE +1 IS. The frame loop assembles a tick's ops BEFORE calling this,
  // so a decision made for tick T reads the snapshot published at the head of
  // SubmitTick(T-1), i.e. tick T - kSnapshotLatency - 1. Still a constant, and
  // still a pure function of (inputs, tick) -- which is the property that
  // matters. Closing the one-tick gap means publishing between `tick++` and the
  // op assembly, which is a line in main.cpp's frame loop and not this file's
  // to move.
  //
  // The wait is the price of the constant and it is COUNTED, never skipped:
  // World::SnapshotPipe().waits is package N1's kill criterion.
  if (tick >= World::kSnapshotLatency) {
    const uint32_t target = tick - World::kSnapshotLatency;
    // Free first: a fence that retired during the frame's render costs a poll.
    ctx.ProcessEvents();
    // Then block, on the ONE submit that owes the oldest snapshot -- not
    // WaitIdle, which would additionally wait on the render of the frame in
    // front and every tick queued behind (measured ~93 ms when the old
    // staleness fallback did that). Bounded by the ring depth: each iteration
    // retires at least one slot and nothing re-arms one from in here.
    for (int i = 0; i < World::kReadbackSlots &&
                    world.ReadbackPendingAtOrBefore(target);
         i++) {
      const auto w0 = std::chrono::steady_clock::now();
      sandvox::PerfSpan spanWait(PerfScope::ReadbackStall);
      if (!ctx.WaitOldestPendingMap()) break;
      world.NoteSnapshotWait();
      g_snapshotStalls++;
      g_stallStats.stalls++;
      g_stallStats.issuedArm++;  // a copy WAS encoded; it has not landed yet
      g_stallStats.mapWaits++;
      g_stallStats.gapHist[std::min(World::kSnapshotLatency, 8u)]++;
      g_stallStats.gapMax =
          std::max(g_stallStats.gapMax, World::kSnapshotLatency);
      g_stallStats.mapArmMs +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - w0)
              .count();
    }
    if (world.PublishSnapshotsUpTo(target)) g_stallStats.mapArm++;
  }
  // Every span is a branch on a bool unless --telemetry or --perf is live.
  sandvox::PerfSpan spanHead(PerfScope::Upload);
  particlesActive = particlesActive || !exps.empty() || !spawns.empty();
  uint32_t cellCount = std::min((uint32_t)cells.size(), kMaxCellOpsPerTick);
  uint32_t spawnCount = std::min((uint32_t)spawns.size(), kMaxParticleSpawnsPerTick);
  // MLS-MPM fluid: `fluidLive` is the caller's CONSERVATIVE live estimate
  // (snapshot count + spawns since — the GPU owns the real number). The spawn
  // budget is charged by the CALLER before emitting (rule 2); this clamp is
  // the belt to that brace, and the GPU excite scan enforces the cap exactly.
  uint32_t fluidSpawnCount = std::min((uint32_t)fluidSpawns.size(),
                                      kMaxFluidSpawnsPerTick);
  fluidLive = std::min(fluidLive, kFluidCap);
  if (fluidSpawnCount > kFluidCap - fluidLive)
    fluidSpawnCount = kFluidCap - fluidLive;
  // Every refusal in one ledger. `cells` is charged against the CANONICAL
  // stream, so a duplicate that was already dropped is not counted twice --
  // it is reported as a dupe, which is a different fault from an overflow.
  opstream::NoteTruncation(
      brushTrunc, expTrunc, (uint32_t)cells.size() - cellCount,
      (uint32_t)spawns.size() - spawnCount,
      (uint32_t)fluidSpawns.size() - fluidSpawnCount, 0);
  // ---- THE SINGLE-PRODUCER CHECK (PLAN_multiplayer_m9 §4 finding 4) -------
  //
  // Under M9 every world-authored system (mobs, debris, worldgen patch-ups)
  // must run over a chunk on EXACTLY ONE machine, or the chunk gets the op
  // twice and the two worlds diverge in a way no hash comparison can
  // attribute. That rule is enforced by construction in the systems
  // themselves (M9.4-B/C); this is the AUDIT that says whether they did it,
  // placed at the one choke point every op passes through.
  //
  // It OBSERVES. It never refuses an op, because dropping one would turn a
  // plumbing bug into a missing voxel — CLAUDE.md rule 6's exact failure
  // mode. The counts and the first offender's attribution go to
  // build/last_run.json's `opstream` block; only SANDVOX_NET_STRICT=1 aborts.
  //
  // COST WHEN SINGLE-PLAYER: one null pointer test. `net::Hook()` is null
  // unless a connected game installs a hook (M9.4-D), so no gate, no smoke
  // and no single-player frame executes a line of this, and the bytes that
  // reach the GPU are unchanged. That is why this package may not move the
  // determinismHash.
  if (const net::AuthorityHook* hook = net::Hook()) {
    const std::vector<opstream::OpMeta> opMeta =
        opstream::ResolveAuthors(opstream::Stream::Brush, (uint32_t)ops.size());
    const std::vector<opstream::OpMeta> expMeta = opstream::ResolveAuthors(
        opstream::Stream::Explosion, (uint32_t)exps.size());
    const net::AuthorityMemory kNoMemory;
    const net::AuthorityMemory& mem =
        hook->chunkMemory ? *hook->chunkMemory : kNoMemory;
    uint32_t viol = 0;
    auto check = [&](uint32_t producer, IVec3 wc) {
      if (producer == (uint32_t)opstream::Producer::Unknown) {
        opstream::NoteUnknownProducer();
        return;
      }
      // Only the chunk-gated producers can trespass. A brush stroke is
      // author-gated and legal anywhere the player's arm reaches, so asking
      // ChunkAuthority about it would report a violation every time a player
      // painted near the boundary — which is the behaviour the game is FOR.
      if (producer != (uint32_t)opstream::Producer::Count &&
          !net::ProducerNeedsChunkAuthority((uint8_t)producer))
        return;
      const uint32_t owner = net::ChunkAuthority(wc, hook->peers, mem);
      if (owner == hook->me) return;
      opstream::NoteAuthorityViolation(tick, producer, wc.x, wc.y, wc.z, owner);
      viol++;
    };
    for (size_t i = 0; i < ops.size(); i++)
      check(opMeta[i].producer,
            net::ChunkOfVoxel({ops[i].x, ops[i].y, ops[i].z}));
    for (size_t i = 0; i < exps.size(); i++)
      check(expMeta[i].producer,
            net::ChunkOfVoxel({exps[i].x, exps[i].y, exps[i].z}));
    // CELL OPS carry a SLOT index, not world coords (sim_mutate.wgsl:184 and
    // §4 finding 3), so the chunk they name is only knowable under MY window
    // origin — an aliased slot (wc + 32k) reads as the local chunk and that is
    // precisely why a remote CellOp may never be applied. Decoded here through
    // the same World helper the sim uses, and skipped for a slot the window's
    // arithmetic cannot produce (a ticket slot; kTicketSlots is 0 at P0).
    // Producer::Count marks "a CellOp" in the attribution: a CellOp has no
    // OpMeta, so there is no producer name to give.
    for (uint32_t i = 0; i < cellCount; i++) {
      const uint32_t slot = cells[i].cellIdx / kChunkVol;
      if (!World::IsWindowSlot(slot)) continue;
      check((uint32_t)opstream::Producer::Count, world.SlotToWorldChunk(slot));
    }
    if (viol && hook->strict) {
      const opstream::StreamCounts& c = opstream::Counts();
      std::fprintf(stderr,
                   "FATAL SANDVOX_NET_STRICT: tick %u emitted %u op(s) into "
                   "chunks player %u does not own; first was producer %s at "
                   "chunk (%d,%d,%d), owned by %d\n",
                   tick, viol, hook->me,
                   c.firstViolProducer == (uint32_t)opstream::Producer::Count
                       ? "cellop"
                       : opstream::ProducerName((uint8_t)c.firstViolProducer),
                   c.firstViolChunk[0], c.firstViolChunk[1],
                   c.firstViolChunk[2], (int32_t)c.firstViolOwner);
      std::abort();
    }
  }
  TickParams tp{tick, seed, (uint32_t)ops.size(), hashEnable ? 1u : 0u,
                (uint32_t)exps.size(), sim.Page(), cellCount, 0};
  tp.spawnCount = spawnCount;
  tp.farCount = farCount;  // far-field fills ride the tick submit (render-only)
  // The disturbance-excite switch rides the tick input stream (the dayPhase
  // precedent): tuning is read CPU-side, HERE, so replays and the twice-run
  // determinism gates capture it and a per-gate SetCurrentTuning overrides it
  // with no pipeline rebuild.
  tp.fluidExciteEnable =
      CurrentTuning().sim.fluidExciteMode != 0 ? 1u : 0u;
  tp.fluidSpawnCount = fluidSpawnCount;
  // Wind, the sim's copy (docs/RESEARCH_wind.md §4.2). Same tick, same seed and
  // the same WindWeather call the renderer makes in WriteRenderParams above —
  // one author, so the CA and the grass are in one weather. Quantised to
  // Q16.16 here because everything downstream is integer (rule 1); the gate
  // rides along beside it, read CPU-side per tick the way fluidExciteMode is,
  // so a per-gate SetCurrentTuning moves it without a pipeline rebuild.
  {
    const Tuning& wtun = CurrentTuning();
    const WindStateQ wq = WindQuantize(WindWeather(wtun, seed, tick, DayPhaseNow(tick)));
    tp.windDirQ[0] = wq.dirX;
    tp.windDirQ[1] = wq.dirZ;
    tp.windSpeedQ = wq.speed;
    tp.windGustQ = wq.gust;
    tp.windMode = (uint32_t)wtun.sim.windMode;
    // The weather, the sim's copy: rain + damp + wetness in one word, from
    // the same seed and tick the renderer's sky is resolved on (weather.h
    // SimRainWord). What "rain" / "rainDamped" reactions read.
    // ONE value per tick, shared with the CPU rain readers the session fed
    // from the same latch (weather::LatchTickRain); a replay takes the
    // recorded word, because the pin inside it is a human input.
    tp.weatherRain = weather::TakeTickRain(wtun, seed, tick);
    opstream::RecordedWeatherRain(tick, tp.weatherRain);
    // The gas edge (docs/PLAN_gas_particles.md). Read here, from the same
    // tuning snapshot windMode comes from, so the value the kernel branches on
    // and the value Simulation gates Cond::Gas on are one read.
    tp.gasMode = (uint32_t)wtun.sim.gasMode;
    // The two dev force multipliers, Q8. Rounded half-away-from-zero by hand
    // for the WindQuantize reason — the rounding mode is part of what the sim
    // sees, so it is written here rather than left to a compiler flag. At the
    // 1.0x default this is exactly kWindScaleOne and every shader consumer
    // takes its identity path, which is what keeps the pinned hash pinned.
    auto scaleQ = [](float v) {
      float c = v < 0.0f ? 0.0f : v;
      int32_t q = (int32_t)(c * (float)kWindScaleOne + 0.5f);
      return q > kWindScaleMax ? kWindScaleMax : q;
    };
    tp.windGasScaleQ = scaleQ(wtun.sim.windGasScale);
    tp.windPartScaleQ = scaleQ(wtun.sim.windPartScale);
    // The drag ramp reference, m/s -> Q16.16 world cells/s. Converted HERE and
    // not in the shader for the WindQuantize reason above: metres are a knob
    // unit, the sim only ever sees cells, and one boundary between them is one
    // that cannot disagree with itself. LoadTuning floors the knob at 1 m/s,
    // and the max() is the second belt — the kernel divides by this.
    {
      double cells = (double)wtun.sim.windDragRef / (double)kVoxelMeters;
      double r = cells * 65536.0 + 0.5;
      if (r > 2147483000.0) r = 2147483000.0;
      tp.windDragRefQ = r < 65536.0 ? 65536 : (int32_t)r;
    }
  }
  // ---- WIND PRIMITIVES (docs/RESEARCH_wind.md §4.3) ------------------------
  //
  // Advanced HERE, in the one function the game loop, --shot and every gate go
  // through, for the reason this file exists at all: a second call site that
  // resolved the list at a different tick would ship the sim a fan that is
  // somewhere the renderer does not draw it, and that reads as a shader bug.
  // Everything downstream — the render copy in WriteRenderParams, the wake
  // list below, the page-table footprint declaration after BeginTick — reads
  // what this call produced.
  //
  // An EMPTY list writes count 0 and the empty AABB, and every shader consumer
  // takes an exact-identity early-out on that, so a world with no fans in it is
  // bit-identical to one built before wind primitives existed.
  std::vector<uint32_t> windWake;
  {
    WindPrimSystem& wp = WindPrims();
    wp.Tick(tick);
    const uint32_t n = std::min(wp.Count(), kWindPrimCap);
    tp.windPrimCount = n;
    const IVec3 lo = wp.BoundsLo(), hi = wp.BoundsHi();
    tp.windPrimLo[0] = lo.x; tp.windPrimLo[1] = lo.y; tp.windPrimLo[2] = lo.z;
    tp.windPrimHi[0] = hi.x; tp.windPrimHi[1] = hi.y; tp.windPrimHi[2] = hi.z;
    for (uint32_t i = 0; i < n; i++)
      std::memcpy(&tp.windPrims[i * kWindPrimWords], wp.Resolved()[i].w,
                  kWindPrimWords * sizeof(int32_t));

    // THE FOOTPRINT WAKE (§10). Only primitives holding the entrainment
    // licence produce one, and the snapshot's occupancy filters out the sky —
    // so a decorative gust costs nothing and a fan aimed at a dune wakes the
    // dune. The budget is charged here, before emission, and the refusals are
    // counted rather than hidden.
    const WorldSnapshot& sn = world.Snap();
    static const std::vector<uint32_t> kNoOcc;
    wp.BuildWake(world, sn.valid ? sn.occupancy : kNoOcc,
                 (uint32_t)std::max(0, CurrentTuning().sim.windWakeChunks),
                 windWake);
    tp.windWakeCount = (uint32_t)windWake.size();
    for (size_t i = 0; i < windWake.size(); i++) tp.windWake[i] = windWake[i];
  }
  // WATER BODIES (docs/PLAN_water_master.md M1; sim/waterbody.h). Advanced
  // HERE, from the one place the game and every harness go through, for exactly
  // the reason WindPrims() is: a path that forgot to advance it would describe a
  // world with no lakes in it while every other path had them.
  //
  // From M2 it WRITES INTO `tp`: geometry, thresholds and a chunk list, all of
  // them pure functions of (seed, window, tuning). That is the whole shape of
  // the fix M1 flagged — the quiescence term used to read the async snapshot,
  // and a shave gated on "when the CPU got around to noticing" is rule 1 broken
  // through the back door. Quiescence and adoption are GPU-side now
  // (sim_waterbody.wgsl); what rides this stream cannot see a fence.
  //
  // `sim.waterBodyMode` 0 is an immediate early-out that leaves both counts
  // at zero, so every pass row's condition is false and nothing is recorded.
  // The SHIPPED tuning.json sets mode 1 (label lakes, write nothing — see the
  // settled-tick note further down), so this is not a default-off system.
  const WaterBodyGpu* waterGpu = nullptr;
  uint32_t drainBodies = 0;
  {
    const Tuning& wt = CurrentTuning();
    WaterBodySystem& wb = WaterBodies();
    // `worldEdited` is the mutation latch component 6 needs (waterbody.h's
    // drainHotUntil_): holes appear when someone digs, and every dig arrives
    // through the mutation queue, so this is the CPU-visible, tick-stream
    // signal that a hole MIGHT now exist.
    //
    // NARROWED TO A LABELLED CHUNK, and the narrowing is worth 1.9 ms. Arming on
    // ANY mutation anywhere means a lab scene that builds itself out of cell ops
    // arms every lake in the window for 30 s, which keeps the discharge's
    // spawn-op block reserved, which keeps the whole fluid pipeline recorded on
    // ticks nothing is happening: measured 5.10 -> 7.02 ms p50 on `pond68` and
    // "0 idle ticks first" where the scene otherwise reports 68. So a mutation
    // only arms a body whose OWN chunks it touched.
    //
    // `ChunkBody()` here is LAST tick's labelling — deliberately, and it is
    // still tick-deterministic: the labelling is a pure function of (seed,
    // window) and only changes when one of those moves.
    bool worldEdited = false;
    // WHERE the edit was, for component 8's drain seeder (the hole hint). The
    // FIRST qualifying mutation wins rather than the last, which is arbitrary
    // but has to be one of the two and has to be stated: whichever it is, it
    // must be a pure function of the tick's op list, because a swirl seeded
    // from a scheduling-dependent choice would be rule 1 through the back door.
    IVec3 editCell{0, 0, 0};
    {
      const std::vector<uint32_t>& lbl = wb.ChunkBody();
      auto touch = [&](IVec3 wc, IVec3 cell) {
        if (worldEdited || !world.ChunkInWindow(wc)) return;
        if (lbl.size() == kNumSlots && lbl[World::SlotChunkIndex(wc)] != 0) {
          worldEdited = true;
          editCell = cell;
        }
      };
      for (uint32_t i = 0; i < cellCount && !worldEdited; i++) {
        const uint32_t slot = cells[i].cellIdx / kChunkVol;
        if (slot < kNumSlots && lbl.size() == kNumSlots && lbl[slot] != 0) {
          worldEdited = true;
          // The op carries a SLOT-linear index, so the world position comes
          // back through the window (SlotToWorldChunk) plus the in-chunk
          // offset. A slot index is a memory address, not an identity — see
          // the page-table rule in CLAUDE.md — and reading it as one is what
          // would put the swirl in another chunk's lake.
          const uint32_t loc = cells[i].cellIdx % kChunkVol;
          const IVec3 wc = world.SlotToWorldChunk(slot);
          editCell = {wc.x * (int)kChunk + (int)(loc % kChunk),
                      wc.y * (int)kChunk + (int)((loc / kChunk) % kChunk),
                      wc.z * (int)kChunk + (int)(loc / (kChunk * kChunk))};
        }
      }
      for (const BrushOp& o : ops)
        touch({o.x >> 4, o.y >> 4, o.z >> 4}, {o.x, o.y, o.z});
      for (const ExplosionOp& e : exps)
        touch({e.x >> 4, e.y >> 4, e.z >> 4}, {e.x, e.y, e.z});
    }
    spanHead.Close();
    {
      sandvox::PerfSpan spanWb(PerfScope::WaterBody);
      // ---- W-D: EVIDENCE, BEFORE THE TICK (PLAN_water_relevel.md §8.2) ----
      //
      // THE CHOKEPOINT, and it is the reason this call is here and not in the
      // brush or in a gate. Everything that places a voxel in this engine
      // arrives at SubmitTick as one of these two lists: the player's brush (and
      // the spell streams that share it) as `ops`, and every system that writes
      // exact cells — mobs, debris, prefabs, tree felling, the world edit layer
      // — plus every gate's hand-built op list as `cells`. There is no other
      // door into the MutationQueue, so accounting here sees the player and the
      // harness by the same path, which is what makes gate pass N's positive arm
      // a statement about the thing a player would do.
      //
      // BEFORE wb.Tick, so a threshold crossed by THIS tick's ops raises its
      // probe on THIS tick rather than one later — the promotion scan runs
      // inside Tick and reads what this deposited. Nothing it reads is anything
      // but the op lists and the tick number: no snapshot, no readback, no
      // clock. Both knobs at their off values make it an immediate return.
      wb.NoteMutations(world, tick, wt.sim.waterBodyMode,
                       wt.sim.waterDiscoverMinEighths, cells.data(), cellCount,
                       ops.data(), (uint32_t)ops.size());
      // ---- W3: THE BLAST IMPULSE (PLAN_water_relevel.md §5) --------------
      //
      // HERE, and not in sim_explode.wgsl, because rule 3 keeps the explosion's
      // writes in the mutation path and `waterFlux`'s pipes have exactly one
      // writer (`wbFlux`, §4.1). What crosses is the EVENT — (x, z, radius,
      // strength) — on the tick input stream, which is the same door the hole
      // hint above uses and for the same reason: a replay reproduces this list
      // and the twice-run determinism gate compares it.
      //
      // THE SAME CHOKEPOINT NoteMutations JUST USED. `exps` is every explosion
      // in the engine — the crosshair detonate, grenade fuses, spell blasts and
      // every gate's hand-built list — because they all arrive at SubmitTick as
      // this one vector. There is no second door.
      //
      // BEFORE wb.Tick, which consumes the queue into the GPU payload, so a
      // blast on THIS tick disturbs the water on THIS tick.
      //
      // At sim.waveBlastImpulse 0 the emitter queues nothing at all.
      for (const ExplosionOp& e : exps)
        WaterBodyNoteBlast(wb, e, wt.sim.waveBlastImpulse);
      wb.Tick(world, seed, tick, wt.sim.waterBodyMode, wt.sim.waterBodyTestDrain,
              wt.sim.drainMaxEighthsPerTick, wt.sim.waterRelevelMax,
              worldEdited, editCell);
    }
    const WaterBodyGpu& g = wb.Gpu();
    waterGpu = &g;
    tp.waterBodyMode = (uint32_t)wt.sim.waterBodyMode;
    tp.waterBodyCount = g.bodyCount;
    tp.waterChunkCount = (uint32_t)g.chunks.size();
    tp.waterTestDrain = wt.sim.waterBodyTestDrain;
    tp.waterQuietTicks = wt.sim.waterBodyQuietTicks;
    tp.waterMinVolume = wt.sim.waterBodyMinVolume;
    // W-D: discovery's size gate. Read by the ledger's adoption branch and ONLY
    // for a body carrying WBF_DISCOVER, so with no probe in the registry this
    // word reaches no branch at all.
    tp.waterAdoptMinArea = wt.sim.waterAdoptMinArea;
    // M5: the scheduled container re-derive (components 2 case 2 + 10). Both
    // fields are pure functions of the tick — see WaterBodySystem::BuildGpu —
    // and `kWaterBodyCap` here means "nothing sweeps", which is the state of
    // every basin nobody has dug into and is what leaves both sweep pass rows
    // unrecorded.
    tp.waterSweepSlot = g.sweepSlot;
    tp.waterSweepLevel = g.sweepLevel;
    // ---- M3: RESERVE THE DISCHARGE'S OP BLOCK (component 6) --------------
    //
    // `spawnAppend` reads a CPU-sized op stream, and the discharge cannot size
    // itself: the head `h` comes from a level the GPU owns. So the CPU does the
    // one thing it can do deterministically — CHARGE THE BUDGET BEFORE EMISSION
    // (rule 2) — by reserving a fixed block per proposed body immediately after
    // this tick's real pours, and sim_waterbody.wgsl's wbDrain fills every slot
    // in it. The ledger refuses the discharge outright to any body that did not
    // get a block (`b < T.waterDrainBodies`), so a granted eighth always has a
    // particle behind it and plan §3.2 holds by construction.
    //
    // Zero at sim.waterBodyMode 0, zero at sim.drainMaxEighthsPerTick 0, and
    // zero whenever nothing is proposed — so the shipped world reserves nothing,
    // the discharge row is not recorded, and the pinned hash cannot see it.
    const uint32_t drainRoom =
        fluidSpawnCount < kMaxFluidSpawnsPerTick
            ? (kMaxFluidSpawnsPerTick - fluidSpawnCount) / kWaterDrainOpsPerBody
            : 0u;
    // GATED ON THE MUTATION LATCH, not merely on the knob. The block is filled
    // every tick it exists, so a standing reservation keeps fluidSpawnCount
    // non-zero forever, keeps the whole fluid seam recorded, and costs a lake
    // nobody has touched real milliseconds — see WaterBodyGpu::drainArmed.
    if (g.drainArmed) drainBodies = std::min(g.bodyCount, drainRoom);
    tp.waterDrainSpawnBase = fluidSpawnCount;
    tp.waterDrainBodies = drainBodies;
    tp.waterDrainMax = wt.sim.drainMaxEighthsPerTick;
    tp.waterExciteRadius = wt.sim.drainExciteRadius;
    // ---- W1: THE RELEVEL RATE, AND ITS ARM, IN ONE WORD ------------------
    //
    // ZEROED unless the footprint is declared this tick. `wbRelevel` is the
    // second voxel WRITER in this subsystem and `writesThisTick` is the CPU's
    // own answer to "were its chunks handed to the page table"; it does not
    // otherwise reach the GPU, and a kernel that read TUNE_WATER_RELEVEL_MAX
    // directly would go on relevelling into JITTER sentinels the first tick the
    // hot window closed. One number, one owner — the same shape as the drain's
    // `b < T.waterDrainBodies` refusal.
    //
    // At sim.waterRelevelMax 0 this is 0 on every tick, both kernels return on
    // their first comparison, and W1 is an exact identity.
    tp.waterRelevelMax = g.writesThisTick ? wt.sim.waterRelevelMax : 0;
    // ---- W2: THE SURFACE-MOMENTUM ARM (PLAN_water_relevel.md §4.2) -------
    //
    // Same word, same owner, same reason. The wave APPLY is folded into
    // `wbRelevel` — one pass writes a column once per tick (§4.3) — so it is
    // the same voxel writer and inherits the same permission: it may only run
    // on a tick whose chunks were handed to the page table. Zeroed with the
    // relevel's arm rather than separately, because a wave with no apply to
    // fold into would spend momentum into a column nobody writes.
    //
    // At sim.waveMode 0 this is 0 on every tick AND the `waterFlux` row is not
    // recorded at all (Cond::WaterWave), so W2 is an exact identity.
    tp.waveMode =
        (g.writesThisTick && wt.sim.waterRelevelMax > 0) ? wt.sim.waveMode : 0;
    // The total the seam dispatches over: real pours, then the reserved block.
    // Written AFTER tp.fluidSpawnCount's own assignment above on purpose — the
    // WriteBuffer below still uploads only the CPU half, because the GPU owns
    // the rest of the range.
    tp.fluidSpawnCount = fluidSpawnCount + drainBodies * kWaterDrainOpsPerBody;
    for (size_t i = 0; i < g.bodies.size() && i < kWaterBodyScalars; i++)
      tp.waterBodies[i] = g.bodies[i];
    for (size_t i = 0; i < g.chunks.size() && i < kWaterChunkCap; i++)
      tp.waterChunks[i] = g.chunks[i];
    // ---- W3: the impulse block (§5) -------------------------------------
    //
    // NOT gated on `writesThisTick` the way the relevel rate above is, and the
    // asymmetry is the point: an impulse writes no voxel. It is a term in the
    // head `wbFlux` integrates, and `wbFlux` itself is already refused on any
    // tick the relevel's arm is down (`T.waveMode` is zeroed with it), so the
    // permission is inherited rather than restated. Restating it here would
    // silently drop a blast on the one tick the footprint happened not to be
    // declared, which is exactly the tick somebody just threw a grenade.
    //
    // Empty is the shipping state and an exact identity.
    tp.waterImpulseCount = g.impulseCount;
    for (size_t i = 0; i < g.impulses.size() && i < kWaterImpulseScalars; i++)
      tp.waterImpulses[i] = g.impulses[i];
  }

  // ---- THE CURRENT FIELD (plan component 8) --------------------------------
  //
  // Advanced HERE, after the water bodies, for two reasons that are both about
  // one instant: the drain seeder reads the descriptors this tick's
  // WaterBodySystem::Tick just produced, and the render copy in
  // WriteRenderParams reads what THIS call resolved. A second call site that
  // resolved at a different tick would ship the sim a whirlpool the renderer
  // draws somewhere else, which reads as a shader bug.
  //
  // An EMPTY list writes count 0 and the empty AABB, and every consumer takes
  // an exact-identity early-out on that — so a world with no drains and no
  // streams is bit-identical to one built before this system existed.
  {
    const Tuning& ct = CurrentTuning();
    CurrentPrimSystem& cp = CurrentPrims();
    // Seeds first, resolve second. Both seeders only ever assert primitives
    // whose parameters are a pure function of the tick input stream — see the
    // authority note at the top of sim/currentprim.h.
    cp.SeedStreams(world, seed, tick);
    cp.SeedDrains(WaterBodies(), seed, tick);
    cp.Tick(tick);
    const uint32_t n = std::min(cp.Count(), kCurrentPrimCap);
    tp.currentMode = (uint32_t)ct.sim.currentMode;
    tp.currentPrimCount = n;
    const IVec3 lo = cp.BoundsLo(), hi = cp.BoundsHi();
    tp.currentPrimLo[0] = lo.x; tp.currentPrimLo[1] = lo.y;
    tp.currentPrimLo[2] = lo.z;
    tp.currentPrimHi[0] = hi.x; tp.currentPrimHi[1] = hi.y;
    tp.currentPrimHi[2] = hi.z;
    for (uint32_t i = 0; i < n; i++)
      std::memcpy(&tp.currentPrims[i * kCurrentPrimWords],
                  cp.Resolved()[i].w, kCurrentPrimWords * sizeof(int32_t));
  }

  // Fluid-lab flat-slab worldgen (world.h kLabSlabY): 0 everywhere except
  // --lab/--fluid-bench. Set on EVERY TickParams write so streamed genList
  // refills and far-cascade fills that ride this tick see the same world.
  tp.labMode = World::LabWorld() ? 1u : 0u;
  // Day phase for THIS tick. Derived from an INTEGER tick counter — the
  // daylight-gated reactions read it, so anything frame-timed here would break
  // determinism (CLAUDE.md rule 1).
  //
  // That counter is the celestial clock's, not the raw sim tick, and the
  // difference is deliberate: the dev time-speed slider is meant to make the
  // WORLD respond to accelerated time (water freezing, snow melting), not just
  // to race the sun across a world that ignores it. The clock is an exact
  // rational counter, so this stays integer end to end, and it is DISENGAGED
  // unless the slider has been moved — on every headless path it returns
  // `tick` unchanged and the pinned hash cannot move.
  tp.dayPhase = DayPhaseNow(tick);
  IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  // THE WIND FIELD BLOCK (windfield.h): the gust-front advection clock, the
  // regime's field parameters, the height profile and the terrain table. After
  // the origin, because the table is centred on the window. A pure function
  // of (tuning, seed, tick, window), like everything else on this stream.
  windfield::FillWindField(tp, CurrentTuning(), seed, tick, tp.dayPhase);
  // The mirror corner for the seam's fluid-occupancy fold: the SAME clamp
  // EncodeReadbacks applies to the same input below, so the fold and the
  // voxel mirror describe one cube.
  IVec3 mb = world.MirrorBaseFor(
      {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1});
  tp.mirrorBase[0] = mb.x; tp.mirrorBase[1] = mb.y; tp.mirrorBase[2] = mb.z;
  tp.vizActive = vizActive ? 1u : 0u;
  // ---- UPLOAD: the MutationQueue op stream reaching the GPU ---------------
  // The second half of the Upload span (the first ran from function entry to
  // the water registry). Ends before the day/night wake below, which records a
  // dispatch and is therefore an ENCODE, not an upload.
  sandvox::PerfSpan spanUpload(PerfScope::Upload);
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  if (!ops.empty())
    ctx.queue.WriteBuffer(world.opsBuf, 0, ops.data(), ops.size() * sizeof(BrushOp));
  if (!exps.empty())
    ctx.queue.WriteBuffer(world.expOps, 0, exps.data(), exps.size() * sizeof(ExplosionOp));
  if (cellCount > 0)
    ctx.queue.WriteBuffer(world.cellOps, 0, cells.data(), cellCount * sizeof(CellOp));
  if (spawnCount > 0)
    ctx.queue.WriteBuffer(world.spawnOps, 0, spawns.data(),
                          spawnCount * sizeof(ParticleSpawn));
  if (fluidSpawnCount > 0) {
    ctx.queue.WriteBuffer(world.fluidSpawnOps, 0, fluidSpawns.data(),
                          fluidSpawnCount * sizeof(FluidSpawnOp));
    // Render-only: a fresh pour must be visible on the frame it lands, and the
    // block list that normally bounds the fluid march is a few ticks behind.
    world.NoteFluidSpawnBounds(fluidSpawns.data(), fluidSpawnCount, tick);
  }
  if (particlesActive) {
    // the write page starts each tick empty; survivors + emissions repopulate
    uint32_t zero = 0;
    ctx.queue.WriteBuffer(world.particleCounts, (1 - sim.Page()) * 4, &zero, 4);
  }

  // ---- CPU-authored gas spawns (docs/PLAN_gas_particles.md) --------------
  // Uploaded EVERY tick, header included, even when the list is empty: the
  // count lives in word 0 of the buffer itself, so a tick that skipped the
  // write would re-spawn the previous tick's ops. 32 bytes when idle.
  //
  // Simulation is told the count BEFORE EncodeTick because the C_GAS latch
  // decides there whether the pass that drains this list is recorded at all.
  std::vector<GasSpawnOp> gas;
  {
    world.TakeGasSpawns(gas);
    // REPLAY takes this list from the record instead. Gas spawns are the one
    // op stream SubmitTick does not receive as an argument -- it drains a CPU
    // queue that the fire/reaction systems filled earlier in the tick -- so a
    // replay that only re-fed the arguments would silently drop them. No-op
    // unless a Log is armed.
    opstream::ReplaceGasIfReplaying(tick, gas);
    std::vector<uint32_t> hdr(kGasSpHdr + gas.size() * kGasSpStride, 0u);
    hdr[kGasSpCount] = (uint32_t)gas.size();
    if (!gas.empty())
      std::memcpy(hdr.data() + kGasSpHdr, gas.data(), gas.size() * sizeof(GasSpawnOp));
    ctx.queue.WriteBuffer(world.gasSpawnOps, 0, hdr.data(), hdr.size() * 4);
    sim.NoteGasSpawns((uint32_t)gas.size());
  }

  // ---- far fire plumes (world.h kGasFarEmitMax) --------------------------
  // The OPPOSITE upload discipline to the gas spawn list above, and for the
  // opposite reason. A spawn op is consumed on the tick it arrives, so its
  // buffer must be rewritten every tick or the previous tick's ops respawn.
  // The emitter list is a STANDING description of where the frozen fires are:
  // its membership changes only when a chunk is evicted, re-loaded or the
  // window moves, and its crossfade weights when the eye has moved
  // kGasFarEyeStepVox (FarPlumes::SetEye; the eye-only rebuild reuses the
  // cached membership). It is uploaded only when Build() produced a different
  // image — no ticks at all in a world nobody has set alight, and otherwise a
  // window shift or a weight byte that actually moved.
  //
  // Simulation is told the count BEFORE EncodeTick for NoteGasSpawns' reason:
  // the count is the splat row's dispatch extent AND half the condition on the
  // density box's clear, both of which the recorder resolves there.
  //
  // Null before Stream::Init, and null is simply "no emitters" — a harness that
  // never streams behaves exactly as it did before the feature existed.
  if (world.farPlumes) {
    FarPlumes& plumes = *world.farPlumes;
    // render.farPlumeRange is metres; the index wants voxels, and the clamp in
    // LoadTuning has already held it inside the long-range box, so this is a
    // unit conversion and not a second bound.
    const int32_t rangeVox =
        (int32_t)(CurrentTuning().render.farPlumeRange / kVoxelMeters);
    plumes.Build(world.WindowOrigin(), rangeVox);
    const uint32_t* pw = nullptr;
    uint32_t pn = 0;
    if (plumes.TakeUpload(&pw, &pn))
      ctx.queue.WriteBuffer(world.gasFarEmit, 0, pw, (size_t)pn * 4);
    sim.NoteFarPlumes(plumes.Count(), plumes.CountWide());
  }

  // ---- THE TICK, AS A RECORD (docs/PLAN_multiplayer_now.md N3) ------------
  //
  // HERE and not at function entry, because what a replay has to reproduce is
  // what REACHED THE GPU: the clamped, canonical op vectors and the completed
  // TickParams, not what the producers hoped to send. TickParams specifically:
  // world.h says many sim.* words ride it per tick "so a replay reproduces the
  // stream", and a record that carried only the op payloads would replay a
  // different wind, a different day phase and a different fluid gate.
  //
  // Under replay this COMPARES instead of writing -- see RecordFrame. One bool
  // test when neither is armed.
  {
    opstream::TickInputs in;
    in.tick = tick;
    in.seed = seed;
    in.hashEnable = hashEnable ? 1u : 0u;
    in.wantReadback = wantReadback ? 1u : 0u;
    in.particlesActive = particlesActive ? 1u : 0u;
    in.vizActive = vizActive ? 1u : 0u;
    in.playerChunk[0] = playerChunk.x;
    in.playerChunk[1] = playerChunk.y;
    in.playerChunk[2] = playerChunk.z;
    in.farCount = farCount;
    in.fluidLive = fluidLive;
    opstream::RecordFrame(in, tp, ops, exps, cells.data(), cellCount,
                          spawns.data(), spawnCount, fluidSpawns.data(),
                          fluidSpawnCount, gas);
  }

  // Day/night sleep handshake. The daylight-gated reactions deliberately do
  // NOT hold a chunk awake while their condition is unmet, so a pond that went
  // to sleep at dusk would never notice sunrise. Wake the world on the ticks
  // where daylight actually switches on or off — a handful per in-game day.
  //
  // Derived from the tick, not from frame timing, so every machine wakes on
  // the same tick and the hash stays identical (CLAUDE.md rule 1).
  //
  // Compared on the PREVIOUS CELESTIAL tick, not the previous sim tick. Under
  // the time-speed slider the clock can advance many day-phase ticks per sim
  // tick — comparing against `tick - 1` would test a phase the world never
  // saw, and at 100x the world would sail through several dawns without ever
  // waking. `prevCel` is the clock's own previous value, so the switch is
  // detected however far the clock jumped (a jump that skips a whole day still
  // wakes exactly once, which is right: one wake re-dirties everything).
  if (tick > 0) {
    const Tuning& dtun = CurrentTuning();
    const uint32_t prevCel = Celestial().PrevSimTick(tick);
    uint32_t prevPhase = DayPhaseForTick(prevCel, TicksPerDay(dtun),
                                         dtun.dayNight.freeze != 0,
                                         (uint32_t)dtun.dayNight.freezePhase);
    bool wasDay = DaylightStrengthCpu(prevPhase) > 0;
    bool isDay = DaylightStrengthCpu(tp.dayPhase) > 0;
    if (wasDay != isDay) sim.EncodeWakeAll(ctx.queue);
  }
  spanUpload.Close();

  // ---- the page table: MATERIALIZE BEFORE THE ENCODER (§3) ----------------
  //
  // A GPU kernel cannot allocate, so every page a kernel might write must
  // exist before the command buffer is submitted. This is the whole reason
  // the phase has a CPU-side conservative dirty mirror: the question
  // "at encode time, what is the set of chunks this tick could write?" is
  // CPU-derivable, and the GPU has no monopoly on it.
  //
  // The order below is the §3.2 normative definitions, in their stated order:
  //   BeginTick        -> clears this tick's C(N) contributors
  //   AddOp*           -> contributor (a), opTargets(N)
  //   UpdateParticles  -> contributor (b), particleChunks(N)
  //   TightenFromSnapshot -> step (2), INTERSECTION only, never assignment
  //   WakeAll/Refilled/ParticleShell -> step (3), strictly AFTER the tightening
  //   Materialize      -> step (1) propagate, then step (4) allocate + fill
  //
  // EncodeWakeAll above has already unioned all-ones into the mirror (§3.2a
  // fix 1: the wake IS a dirty-set mutation and the two must be ONE operation,
  // not two that must agree).
  PageTable& pt = *world.pages;
  // ---- PAGE TABLE, CPU HALF -----------------------------------------------
  // Everything from here to RetirePages: BeginTick, the AddOp* contributors,
  // UpdateSpawnRing/UpdateFluidChunks, TightenFromSnapshot, Materialize (which
  // is Classify + the allocator), ConsumeOccupancy and RetirePages. This runs
  // EVERY tick whether or not anything moved, so unlike the CA it does not
  // sleep — which makes it the one block in SubmitTick that can be expensive on
  // a frame where the player is doing nothing at all.
  //
  // It also contains the free-confirmation probe, which issues its OWN
  // vkQueueSubmit and memcpys up to kMaxFreeProbesPerTick x 16 KiB out of a
  // mapped staging buffer. That is a second submit hiding inside what the page
  // called "queue submit", and it was invisible.
  sandvox::PerfSpan spanPt(PerfScope::PageTableCpu);
  // Install the free-confirmation probe once (see SetChunkProbe): a page is
  // only released when the chunk's WORDS say empty, because occupancy does not
  // see the stain layer and the hash does.
  static bool probeInstalled = false;
  if (!probeInstalled) {
    probeInstalled = true;
    GpuContext* pctx = &ctx;
    World* pw = &world;
    struct ProbeState {
      rhi::Buffer staging;
      rhi::MapTicket map;
      size_t stagingSlots = 0;
      size_t lastBatchSize = 0;
    };
    auto state = std::make_shared<ProbeState>();

    pt.SetChunkProbe(
        // SUBMIT: encode copies, kick deferred map, return per-slot validity.
        [pctx, pw, state](const std::vector<uint32_t>& slots)
            -> std::vector<bool> {
          std::vector<bool> ok(slots.size(), false);
          if (slots.empty()) return ok;
          const size_t stride = (size_t)kChunkVol * 4;
          if (state->stagingSlots < slots.size()) {
            state->stagingSlots = slots.size();
            state->staging = CreateBuffer(
                pctx->device, (uint64_t)state->stagingSlots * stride,
                rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                "freeProbe");
          }
          // ---- DECIDE FIRST, THEN CREATE THE ENCODER (P3-E + P3-F) -------
          //
          // Two independent reasons arrived at the same shape, and both are
          // about the same fact: a command buffer that is created and then
          // dropped is not free.
          //
          // P3-E, correctness. `CreateCommandEncoder` calls
          // Backend::BeginCommands, which drains the whole pending-upload queue
          // into that command buffer's head. This block used to create the
          // encoder up front and then `return ok` without submitting whenever
          // every candidate had lost its page since selection - which happens
          // constantly once the free path is allowed to run - and the writes it
          // had swallowed, including PageTable::Materialize's page-table flush
          // for the whole tick, died with it. The GPU kept the pre-allocation
          // JITTER sentinel and the tick's `pagefill` plus the next shift's
          // `genChunk` each wrote 4,096 words through it: 21,733,376 lost
          // voxels on --gate streaming, with a perfectly correct CPU-side page
          // table the whole time. (Backend::AbandonCommands now hands an
          // abandoned encoder's uploads back, so this is belt to that braces.)
          //
          // P3-F, liveness. A timestamp pair must be WRITTEN by a command
          // buffer that is actually submitted: PassTimer::EncodeResolve resolves
          // [0, used_) with VK_QUERY_RESULT_WAIT_BIT, so a pair allocated into a
          // dropped buffer would make the tick's resolve wait forever on a query
          // nothing ever wrote - a hung device, not a wrong number.
          //
          // So: build the plan, and only then open a command buffer at all.
          std::vector<std::pair<uint64_t, size_t>> plan;
          plan.reserve(slots.size());
          for (size_t i = 0; i < slots.size(); i++) {
            const uint64_t off = pw->PageOffsetOfSlot(slots[i]);
            if (off == World::kNoPage) continue;
            plan.emplace_back(off, i);
            ok[i] = true;
          }
          const size_t copied = plan.size();
          if (copied == 0) return ok;
          rhi::CommandEncoder enc = pctx->device.CreateCommandEncoder();
          {
            // TIMED (P3-F). Up to kPageFreeProbesPerTick x 16 KiB of copies on
            // their OWN command buffer and their own vkQueueSubmit, inside what
            // the page called "page table CPU". The queries resolve with the
            // tick's buffer, which is submitted after this one on the same
            // queue - so the pair is closed by the time the resolve executes.
            TickGpuSpan spanProbe(enc, "freeProbeCopy");
            for (const auto& pr : plan)
              enc.CopyTracked(pass::Buf::Voxels, pw->voxels, pr.first,
                              state->staging, pr.second * stride, stride);
          }
          pctx->queue.Submit(enc.Finish());
          state->map = rhi::MapReadDeferred(pctx->device, state->staging, 0,
                                            (uint64_t)slots.size() * stride);
          state->lastBatchSize = slots.size();
          return ok;
        },
        // HARVEST: non-blocking Ready() check; copy data out if complete.
        [state](uint32_t* out) -> bool {
          if (!state->map.Ready()) return false;
          state->map.Wait();
          if (!state->map.Succeeded() || !state->map.Data()) {
            state->map.Unmap();
            return false;
          }
          const size_t stride = (size_t)kChunkVol * 4;
          std::memcpy(out, state->map.Data(),
                      state->lastBatchSize * stride);
          state->map.Unmap();
          return true;
        });
  }
  pt.BeginTick(tick);
  for (const BrushOp& o : ops)
    pt.AddOpSphere({o.x, o.y, o.z}, o.radius, world);
  for (const ExplosionOp& e : exps)
    pt.AddOpBox({e.x, e.y, e.z}, kMaxExplosionRadius, world);  // EXP_BOX
  for (uint32_t i = 0; i < cellCount; i++) {
    pt.AddOpTarget(cells[i].cellIdx / kChunkVol);  // already a slot chunk index
    // A SOLUTE POUR (world.h CellOpSolute) may precipitate its powder up to
    // 16 cells BELOW its cell or a few above it (sim_mutate.wgsl solPour
    // walks to the surface), so the chunks over and under it are written too.
    if (IsSoluteCellOp(cells[i].word)) {
      const uint32_t slot = cells[i].cellIdx / kChunkVol;
      if (slot < kNumSlots) {
        const IVec3 wc = world.SlotToWorldChunk(slot);
        for (int dy : {-1, 1})
          if (world.ChunkInWindow({wc.x, wc.y + dy, wc.z}))
            pt.AddOpTarget(World::SlotChunkIndex({wc.x, wc.y + dy, wc.z}));
      }
    }
  }
  // THE WHOLE POINT OF THE FOOTPRINT WAKE (docs/RESEARCH_wind.md §10). The
  // chunks a wind primitive is about to dirty-mark are declared as OP TARGETS,
  // in the same breath and from the same list, so they are materialized with
  // their 26-ring before the command buffer exists.
  //
  // This is what repairs the page table's soundness argument. `cpuDirty` is
  // tightened against a lagging snapshot, and that tightening is only sound
  // because settled matter writes nothing — entrainment is the first rule that
  // makes resting voxels move, and switching it on without this lost 62 voxels
  // to page faults across two 160-tick runs. A grain that hops into a
  // neighbouring chunk now hops into one the CPU already said could be written.
  for (uint32_t slot : windWake) pt.AddOpTarget(slot);
  // THE SAME REPAIR, FOR THE SURFACE SHAVE (docs/PLAN_water_master.md M2). The
  // shave is the second rule in this engine to make RESTING voxels move, and
  // the paragraph above is exactly why that matters: cpuDirty's tightening
  // against a lagging snapshot is only sound because settled matter writes
  // nothing. Entrainment broke that and lost 62 voxels; a shave into a JITTER
  // sentinel would lose a lake one eighth at a time and report it as page
  // faults rather than as a leak.
  //
  // Declared ONLY when a shave can actually fire this tick (`writesThisTick`),
  // so a still lake materializes no pages at all — the residency cost of the
  // feature at rest is zero, not small.
  if (waterGpu && waterGpu->writesThisTick) {
    for (uint32_t e : waterGpu->chunks) pt.AddOpTarget(e & 0xFFFFu);
  }
  {
    std::vector<IVec3> spawnCells, expCenters;
    spawnCells.reserve(spawnCount);
    for (uint32_t i = 0; i < spawnCount; i++)
      spawnCells.push_back({spawns[i].px >> 8, spawns[i].py >> 8,
                            spawns[i].pz >> 8});
    for (const ExplosionOp& e : exps) expCenters.push_back({e.x, e.y, e.z});
    // particleSpawnChunks(N): THIS tick's spawn sites, one ring, recomputed
    // from scratch. Not carried — see the adjacency argument in
    // PageTable::UpdateSpawnRing.
    pt.UpdateSpawnRing(spawnCells, expCenters, world);
  }
  {
    // fluidChunks(N): every chunk the MLS-MPM seam may write a voxel into —
    // the active block slots from the latest DELIVERED snapshot readback plus
    // this tick's CPU-known fluid spawn cells, dilated one ring inside
    // UpdateFluidChunks. The settle converter's >= 8 calm-tick floor is what
    // makes the readback latency safe (world.h fluid block).
    //
    // LatestDelivered(), not Snap(): this materializes pages, which is page
    // TABLE work — derived data that wants the freshest thing the GPU has
    // handed back, not the fixed-age view gameplay decisions read. See the
    // block comment on World::LatestDelivered.
    const WorldSnapshot& sn = world.LatestDelivered();
    std::vector<uint32_t> blockSlots;
    if (sn.valid && sn.fluidBlockCount > 0) {
      blockSlots.assign(sn.fluidBlocks.begin(),
                        sn.fluidBlocks.begin() + sn.fluidBlockCount);
    }
    std::vector<IVec3> fluidCells;
    fluidCells.reserve(fluidSpawnCount);
    for (uint32_t i = 0; i < fluidSpawnCount; i++)
      fluidCells.push_back({fluidSpawns[i].px >> 16, fluidSpawns[i].py >> 16,
                            fluidSpawns[i].pz >> 16});
    pt.UpdateFluidChunks(blockSlots, fluidCells, world);
  }
  {
    // LatestDelivered(), not Snap(), and the freshness is load-bearing rather
    // than nice: the tightening is the ONLY thing that shrinks cpuDirty, and
    // it rolls a stale snapshot forward one N26 ring per tick of gap. Pointing
    // it at the fixed T-K view is what produced page faults and a mass leak in
    // N1's first cut. Derived data, so this may be timing-dependent.
    const WorldSnapshot& sn = world.LatestDelivered();
    if (sn.valid) pt.TightenFromSnapshot(sn.dirtyFlags, sn.tick, tick);
    // Contributor (e), the particle flight shell — strictly AFTER the
    // tightening, like (c)/(d): a union applied after an intersection cannot
    // be undone by it. Covers the GPU-decided landing writes an airborne
    // particle will make (a mid-flight snapshot legitimately tightens the
    // mirror to empty — a flying particle dirties nothing — and the
    // intersection can never ADD the landing back). See §3.4.
    pt.ApplyParticleShell(sn, particlesActive);
  }
  pt.Materialize(ctx.queue);
  // ---- deallocation (§3.6), AFTER materialization -------------------------
  // Order matters: cpuDirty is the materialization set, and the free
  // condition's second conjunct tests against it. Running the free decision
  // before Materialize would test a mirror that had not yet absorbed this
  // tick's ops, and could free a chunk this very tick is about to write.
  //
  // Both steps read data the CPU already has: the occupancy the snapshot
  // already carries, and a tick counter. No new readback, no new scan.
  // LatestDelivered() for the same reason the tightening uses it: the free
  // path is page-table bookkeeping, and freeing against a K-tick-old occupancy
  // reading is how a chunk that just gained matter gets demoted under it.
  if (world.LatestDelivered().valid)
    pt.ConsumeOccupancy(world.LatestDelivered().occupancy,
                        world.LatestDelivered().occStain,
                        world.LatestDelivered().tick, tick);
  pt.RetirePages(tick);

  // ---- the §3.4 settled-skip latch ----------------------------------------
  // Fed here because this is the one function BOTH the game loop and every
  // harness tick go through, so the latch cannot see a different world than
  // the encoder does. Declared BEFORE EncodeTick, which reads it.
  //
  // `dirtiedNow` deliberately over-declares: farCount is render-only derived
  // data that cannot dirty a sim chunk, but it costs nothing to be wrong in
  // the safe direction and the list stays a plain "did anything arrive".
  //
  // particlesActive is NOT in the list (§3.2d). It says the particle PASSES are
  // recorded, not that anything was written this tick; the ticks on which a
  // particle can be created are exps/spawns/fluid, all of which are here, and
  // the population already in flight is proven empty (or not) by the snapshot
  // conjunct below. It used to be here, and it held the latch off for the whole
  // 400-tick post-explosion window main.cpp keeps the pipeline alive for.
  //
  // windWake is in the list because it IS a chunk-dirtying input: a settled
  // world with a fan pointed at a dune would otherwise prove itself idle and
  // skip the CA rows the wake had just made necessary, and the fan would mark
  // chunks nothing then simulated.
  //
  // The water shave is in the list for the windWake reason and only when it can
  // fire: it dirty-marks chunks, so a settled world with a draining lake would
  // otherwise prove itself idle and skip the CA rows the shave had just made
  // necessary. A merely LABELLED lake is not an input — it writes nothing —
  // which is what keeps the settled-tick skip alive at sim.waterBodyMode 1.
  sim.NoteTickInputs(tick, !ops.empty() || !exps.empty() || cellCount > 0 ||
                               spawnCount > 0 || !windWake.empty() ||
                               (waterGpu && waterGpu->writesThisTick) ||
                               drainBodies > 0 ||
                               fluidLive + fluidSpawnCount > 0 ||
                               // Rain is an input for the windWake reason:
                               // sim_mutate.wgsl rainFall dirty-marks a chunk
                               // whose surface it newly wets.
                               (tp.weatherRain & 0xFFu) != 0);  // materials.h kRainAmountMask
  {
    // A snapshot can only license a skip if it is BOTH valid and fresh enough
    // (Simulation::NoteSnapshot enforces the freshness against lastDirtyTick_),
    // and shows nothing in flight — `resolve` is a dirty-writer whose target
    // the CPU never chose, so activeChunks alone does not mean settled.
    const WorldSnapshot& sn = world.Snap();
    if (sn.valid) {
      // SOLUTE POOL EXHAUSTION IS FATAL (world.h kSolutePoolPages, the
      // kPoolPages policy): the GPU allocator refused a page, so which slots
      // lost is scheduling-dependent and the writes they would have taken are
      // gone. Aborting at detection names the moment; continuing would be a
      // world that differs between machines with nothing to say why.
      if (sn.solExhausted != 0) {
        std::fprintf(stderr,
                     "FATAL: solute page pool exhausted at tick %u (%u refused "
                     "pops): pool %u pages, high water %u, free %u. Mass in "
                     "flight: dissolved %u, discarded %u, precipitated %u. The "
                     "pool is sized in world.h (kSolutePoolPages = kNumSlots / "
                     "kSolPoolDivisor); this is genuine demand past it.\n",
                     sn.tick, sn.solExhausted, kSolutePoolPages, sn.solHighWater,
                     sn.solFree, sn.solDissolved, sn.solDiscarded, sn.solPrecip);
        std::fflush(stderr);
        std::abort();
      }
      // ...AND SO IS A SOLUTE STORE FAULT (world.h kSolMFaults; the check
      // every solute shader's solFault comment names). The allocator promises
      // a page to every chunk a tick can write, so a store into a sentinel
      // is a lost (or, half a swap, duplicated) write of authoritative,
      // hashed mass: a bug, named at the first tick it happened.
      if (sn.solFaults != 0) {
        const uint32_t slot = sn.solFaultSlot ? sn.solFaultSlot - 1u : 0u;
        const IVec3 wc = sn.solFaultSlot ? world.SlotToWorldChunk(slot) : IVec3{0, 0, 0};
        std::fprintf(stderr,
                     "FATAL: %u solute store(s) refused by a chunk with no page; "
                     "the first at tick %u in slot %u (world chunk %d,%d,%d). The "
                     "writer reached past the chunks solWant paged for it "
                     "(sim_solute.wgsl solWant / solFault).\n",
                     sn.solFaults, sn.solFaultTick, slot, wc.x, wc.y, wc.z);
        std::fflush(stderr);
        std::abort();
      }
      sim.NoteSnapshot(sn.tick, sn.activeChunks, sn.particleCount);
      // The C_GAS latch's disarming input. Latent by design — see the block in
      // Simulation::EncodeTick for why a stale zero cannot turn gas off.
      sim.NoteGasLive(sn.gasCount);
      // The RENDER flag's arming input (RenderParams bit 3). Parcels OR gas
      // voxels: a plume that never leaves the window has no parcels and still
      // has to crossfade at the faces. See Simulation::NoteGasSeen.
      sim.NoteGasSeen(sn.gasCount > 0 ||
                      (sn.dirtyReasonOr & kDirtyGasMask) != 0);
    }
  }

  // THE genList UPLOAD MUST HAPPEN BEFORE THE ENCODER EXISTS, and this is a
  // trap worth stating: rhi::Queue::WriteBuffer is a DEFERRED host write
  // (Backend::QueueWrite) that drains "at the head of the NEXT command buffer".
  // Called after CreateCommandEncoder, the write drains into the command buffer
  // AFTER this one, so the pageFill dispatch below read the PREVIOUS tick's
  // genList — every JITTER page materialized as zeros and 2,114 chunks of stone
  // silently became air.
  const uint32_t jitterFills = pt.UploadJitterFills(ctx.queue);
  spanPt.Close();

  // ---- ENCODE: describing the tick's GPU work -----------------------------
  // The pass-table walk plus the readback copies. This is CPU time spent
  // TALKING about GPU work, so it grows with the number of recorded rows, not
  // with how much they do — which is why it is worth separating from the
  // submit it used to be bundled with.
  sandvox::PerfSpan spanEnc(PerfScope::Encode);
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  // The fills go in at the HEAD of the command buffer, before any row (§5.4):
  // FillTracked declares TransferWrite on Voxels, and the first row with
  // RW(Voxels) then gets a derived TRANSFER->COMPUTE barrier. A fill recorded
  // after a dispatch that reads the page is exactly the hazard this ordering
  // exists to prevent.
  //
  // TIMED (P3-F). One vkCmdFillBuffer per page materialized from an
  // EMPTY/UNIFORM sentinel, 16 KiB each, and under sustained flight there are
  // thousands of them in a tick. They are raw transfer commands, so the pass
  // table never sees them, so the Performance page billed them to nothing at
  // all — not to a node, and not even to `unattributed`, because an untimed
  // command produces no sample to drop.
  {
    TickGpuSpan spanFill(enc, "pageFillCmd");
    pt.DrainFills(enc);
  }
  // The JITTER half of materialization, same position and same reason: a page
  // whose words vary per cell cannot be a fill pattern, so it is a dispatch.
  // Recorded BEFORE EncodeTick so the tick's first voxel read sees the filled
  // page (the recorder derives the COMPUTE->COMPUTE barrier from the W(Voxels)
  // in the pageFill row against the tick's first RW(Voxels)).
  sim.EncodePageFill(enc, jitterFills);
  // The seam's spawn dispatch covers the CPU pours AND the reserved drain
  // block, so both counts carry the total; `waterDrainBodies` is what sizes the
  // discharge row itself and what the ledger's rule-2 refusal compares against.
  const uint32_t fluidSpawnTotal =
      fluidSpawnCount + drainBodies * kWaterDrainOpsPerBody;
  sim.EncodeTick(enc, (uint32_t)ops.size(), hashEnable, (uint32_t)exps.size(),
                 particlesActive, cellCount, spawnCount,
                 fluidLive + fluidSpawnTotal, fluidSpawnTotal,
                 (uint32_t)windWake.size(), vizActive,
                 waterGpu ? (uint32_t)waterGpu->chunks.size() : 0u,
                 drainBodies,
                 waterGpu ? waterGpu->sweepSlot : kWaterBodyCap);
  sim.EncodeFarFill(enc, farCount);
  // ---- N1: THE READBACK IS UNCONDITIONAL ---------------------------------
  //
  // `wantReadback` used to gate this and no longer does (docs/PLAN_multiplayer_now.md
  // N1). A FIXED latency needs a copy on EVERY tick: the publish at
  // T + World::kSnapshotLatency has nothing to hand over otherwise, and
  // "did this tick get a snapshot" would go back to being a property of which
  // caller ran the tick rather than of the tick number. It costs nothing new --
  // the harness arm already forced one on every tick (HarnessSnapshotDrain, and
  // P5-I made that residency-independent) and the game path passes true.
  //
  // A readback is a pure copy out plus a map; it mutates no world state.
  (void)wantReadback;
  // THE RING MAY NEVER DECLINE ON THE SIM PATH. A tick that finds every slot in
  // flight gets no copy, so the publish K ticks later has nothing to give and
  // the latency stops being a constant. `return false` at World::EncodeReadbacks
  // is now unreachable-by-construction rather than merely rare: wait for a slot
  // instead. Bounded by the ring depth -- each iteration retires at least one
  // slot and nothing re-arms one from inside the loop.
  for (int i = 0; i < World::kReadbackSlots && !world.ReadbackSlotFree(); i++) {
    sandvox::PerfSpan spanWait(PerfScope::ReadbackStall);
    if (!ctx.WaitOldestPendingMap()) break;
    world.NoteSnapshotSlotWait();
  }
  bool doCopy = false;
  {
    // TIMED (P3-F): up to ~2 MiB of copies out per tick (~0.8 MiB at rest,
    // more with chunk fetches in flight), recorded as raw
    // CopyBufferToBuffer rather than as pass rows. `readback` was a CPU-only
    // row on the Performance page; this is the GPU side of the same system.
    TickGpuSpan spanRb(enc, "readbackCopy");
    // The digest table rides hash ticks only and the swimming fold only
    // ticks that recorded the seam — the only ticks either GPU table can
    // have changed on (World::EncodeReadbacks). 236 KiB of the ~1 MiB a
    // resting tick used to copy out.
    doCopy = world.EncodeReadbacks(ctx.device, enc,
                                   {playerChunk.x - 1, playerChunk.y - 1, playerChunk.z - 1},
                                   1 - sim.Page(), tick, hashEnable,
                                   sim.FluidSeamRecorded());
    if (doCopy) {
      world.EncodeDirtyCopy(enc, sim.DirtyNext());
    } else {
      // Structurally impossible after the wait above unless the ring had
      // nothing outstanding to wait FOR. Counted at both ends: here for the
      // Performance page, and in World::SnapshotPipe().declines, which the
      // snapshot-latency gate asserts is zero.
      g_readbackDeclines++;
      g_stallStats.refusedArm++;
    }
  }
  spanEnc.Close();
  {
    // ---- SUBMIT, and now it really is only the submit ---------------------
    // Finish() runs the barrier generator over the recorded rows and
    // vkQueueSubmit hands the buffer to the driver. Both are real CPU costs
    // and both belong to the RHI; nothing else does.
    sandvox::PerfSpan spanSub(PerfScope::Submit);
    ctx.queue.Submit(enc.Finish());
    sim.FlipPage();
    if (doCopy) world.KickReadback();
  }
  // ---- THE HARNESS DRAIN IS NO LONGER A LATENCY, ONLY AN ORDERING ---------
  //
  // It used to be the thing that gave the harness a snapshot at all, and it
  // gave it a ONE-TICK-LATENT one -- a latency the game never ran at, which is
  // finding L1' of docs/RESEARCH_multiplayer_readiness.md. The publish at the
  // head of SubmitTick is now the only thing that decides what Snap() holds,
  // and it holds tick - kSnapshotLatency under the harness and under the game
  // alike. So --selftest tests the SHIPPED latency.
  //
  // What is left here is a TEST-ORDERING requirement and nothing else: gates
  // read the world through blocking hash/occupancy reads either side of this
  // and several depend on every submit having retired before they look
  // (selftest.h's ordering note). It cannot change what Snap() reports, only
  // how long the publish has to wait for it -- which is why the harness pays
  // zero snapshot waits and the game pays a measured few.
  if (HarnessSnapshotDrain()) {
    sandvox::PerfSpan spanWait(PerfScope::ReadbackStall);
    ctx.WaitIdle();
    ctx.ProcessEvents();
  }
  // ---- THE PAGE TABLE'S OWN SNAPSHOT CADENCE, AND WHY IT SURVIVED N1 ------
  //
  // This is the paged self-defence that predates N1, restored deliberately
  // after N1's first cut deleted it. The reasoning that deleted it was: "with
  // an exact latency the staleness predicate is a compile-time constant, so
  // the fallback is dead code". That was true of the predicate and false of
  // the NEED. §3.2's intersection is the only thing that shrinks cpuDirty, and
  // TightenFromSnapshot dilates one N26 ring per tick of gap — so pinning the
  // page table to a K-tick-old snapshot does not make it stale-but-fine, it
  // makes the materialization set the wrong SHAPE. Chunks the GPU is about to
  // write stop being materialized, and a store into an unmaterialized chunk is
  // a PAGE FAULT: a lost voxel. Measured on that first cut: `daylight-boundary`
  // pageFaults 1 and `ca-level-pond` leaking two eighths of water, in a suite
  // that had neither before.
  //
  // WHAT CHANGED, and it is the whole reason both customers can be served: the
  // drain now waits for DELIVERY and not for PUBLICATION. Snap() is advanced
  // by PublishSnapshotsUpTo at the head of the tick and by nothing else, so
  // draining here cannot move it a single tick — the fixed latency is
  // untouched, and the page table gets the fresh readback it has always
  // needed. Before N1 these were the same act, which is why the conflict was
  // invisible.
  //
  // Two thresholds, unchanged: gap 0 inside PageTable::InSettleWindow (a
  // freshly generated world's dirty set is at its lifetime maximum and even a
  // 2-4 tick lag measured 16,347 pages in use by tick 8 against 9,396 for the
  // same settle at gap 0), and kPagedSnapshotMaxGap after it. Dense takes none
  // of this: the identity map has no mirror to starve.
  //
  // RAISING kPagedSnapshotMaxGap TO BUY FEWER STALLS IS STILL A TRAP. The
  // tightening ends in `IntersectWith(snap); UnionWith(snap)`, which is
  // (A n S) u S == S, so this constant is the EXPONENT on the materialization
  // set and not a latency knob. Measure with SANDVOX_PT_DEBUG=1 before
  // granting it more rings; SANDVOX_SNAP_MAXGAP makes that one run per
  // candidate instead of one rebuild per candidate.
  static const uint32_t kPagedSnapshotMaxGap = [] {
    constexpr uint32_t kDefault = 4;
    // World::kSnapshotLatency's ceiling, checked where it is declared rather
    // than in two comments: the published snapshot is K ticks old, and the
    // page table's drain may not accept a delivered one older than that.
    static_assert(World::kSnapshotLatency <= kDefault,
                  "kSnapshotLatency must stay <= kPagedSnapshotMaxGap");
    if (const char* e = std::getenv("SANDVOX_SNAP_MAXGAP")) {
      const long n = std::strtol(e, nullptr, 10);
      if (n >= 1 && n <= 13) {
        std::printf("[snap] SANDVOX_SNAP_MAXGAP=%ld (default %u)\n", n,
                    kDefault);
        return (uint32_t)n;
      }
    }
    return kDefault;
  }();
  if (world.residency == World::Residency::Paged) {
    const uint32_t maxGap =
        world.pages->InSettleWindow(tick) ? 0u : kPagedSnapshotMaxGap;
    auto delivered = [&] {
      const WorldSnapshot& ld = world.LatestDelivered();
      return ld.valid && tick <= ld.tick + maxGap;
    };
    if (!delivered()) ctx.ProcessEvents();  // free: a fence that already fired
    // Bounded by the ring depth — each iteration retires at least one slot and
    // nothing re-arms one from in here. One targeted fence per iteration, never
    // a device drain: WaitIdle waits for WORK, and work is not what is missing.
    for (int i = 0; i < World::kReadbackSlots && !delivered(); i++) {
      sandvox::PerfSpan spanWait(PerfScope::ReadbackStall);
      if (!ctx.WaitOldestPendingMap()) break;
      world.NoteSnapshotSlotWait();
      g_snapshotStalls++;
      g_stallStats.stalls++;
      g_stallStats.issuedArm++;
      g_stallStats.mapWaits++;
    }
    if (!delivered()) g_stallStats.proceedStale++;
  }
}

uint32_t TakeSnapshotStalls() {
  const uint32_t n = g_snapshotStalls;
  g_snapshotStalls = 0;
  return n;
}

uint32_t TakeReadbackDeclines() {
  const uint32_t n = g_readbackDeclines;
  g_readbackDeclines = 0;
  return n;
}

SnapshotStallStats TakeSnapshotStallStats() {
  const SnapshotStallStats s = g_stallStats;
  g_stallStats = SnapshotStallStats{};
  return s;
}

namespace {
// Off by default: main.cpp's frame loop shares SubmitTick and must never pay
// this sync point. Only --selftest / --vk-smoke / --measure opt in.
bool g_harnessSnapshotDrain = false;
}  // namespace

void SetHarnessSnapshotDrain(bool on) { g_harnessSnapshotDrain = on; }
bool HarnessSnapshotDrain() { return g_harnessSnapshotDrain; }

bool ReloadEnvironment(GpuContext& ctx, Simulation& sim,
                       const std::vector<MaterialDef>& mats,
                       biomes::EnvironmentStamp& stamp, std::string& log) {
  const std::string assetDir = AssetDir();
  const std::string mapName = worldmap::ActiveMapName(CurrentTuning().world.mapLayer);
  // The same three loads, in the same order and with the same refusals, as
  // boot (main.cpp): the biome set first because it is the id space the other
  // two are laid out in.
  biomes::BiomeSet set;
  if (!biomes::LoadBiomeSet(assetDir, mats, set, log)) {
    log += "environment reload: a biome/water/species file did not parse -- kept the old tables\n";
    return false;
  }
  std::vector<std::string> problems;
  if (biomes::ValidateBiomeSet(set, problems)) {
    for (const std::string& p : problems) log += "biomes: " + p + "\n";
    log += "environment reload: biome files are invalid -- kept the old tables\n";
    return false;
  }
  worldmap::WorldMapData map;
  std::vector<uint32_t> words;
  if (!worldmap::LoadWorldMap(assetDir, mapName, set, mats.size(), kDefaultSeed, map, log) ||
      !worldmap::PackWorldMap(set, map, words, log)) {
    log += "environment reload: world map '" + mapName + "' failed to load -- kept the old tables\n";
    return false;
  }
  TreeAtlas trees;
  if (!LoadTreeAtlas(assetDir + "/trees", mats, set, trees, log)) {
    log += "environment reload: tree atlas failed to load -- kept the old tables\n";
    return false;
  }
  // Everything parsed and validated: now, and only now, replace. The CPU
  // twins (World::MapBiomeAt, TerrainHeight's site pads) read the current map
  // through worldmap::CurrentWorldMap, so the two sides flip together.
  ctx.WaitIdle();
  worldmap::SetCurrentWorldMap(std::move(map));
  sim.UploadEnvironment(ctx.device, ctx.queue, trees, words);
  // The edit layer is the map's (P7): re-read it beside the map it belongs
  // to. The caller's SubmitWorldgen queues it and re-seeds the far field.
  LoadWorldEditLayerForMap(assetDir);
  stamp = biomes::StampEnvironment(assetDir, mapName, worldmap::CurrentWorldMap().editLayer);
  return true;
}

bool ApplyStructureChanges(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
                           FarField* far, const std::vector<MaterialDef>& mats,
                           biomes::EnvironmentStamp& stamp, StructureReapply& rep) {
  (void)world;
  using Site = worldmap::WorldMapData::StampSite;
  std::map<std::string, Site> before;
  for (const Site& s : worldmap::CurrentWorldMap().sites)
    if (s.structure) before[s.id] = s;
  if (!ReloadEnvironment(ctx, sim, mats, stamp, rep.log)) return false;
  std::map<std::string, Site> after;
  for (const Site& s : worldmap::CurrentWorldMap().sites)
    if (s.structure) after[s.id] = s;
  auto same = [](const Site& a, const Site& b) {
    return a.x == b.x && a.z == b.z && a.padY == b.padY && a.rot == b.rot && a.sink == b.sink &&
           a.radius == b.radius && a.padMargin == b.padMargin && a.apron == b.apron && a.words == b.words;
  };
  // The widest tree: a trunk the OLD footprint kept out may stand in the new
  // gap, crown and all (and one the new footprint refuses must go).
  int treeReach = 0, treeAbove = 0;
  {
    std::vector<TreeSpeciesHeader> hs;
    std::string tl;
    if (ReadTreeSpeciesHeaders(AssetDir() + "/trees", hs, tl))
      for (const TreeSpeciesHeader& h : hs) {
        treeReach = std::max(treeReach, h.reach);
        treeAbove = std::max(treeAbove, h.above);
      }
  }
  // What a changed site can have changed, and so the box to regenerate:
  //  - its voxels and its pad: the footprint + apron + ramp;
  //  - everything genChunk derives from the column's ground height h under
  //    the pad: the skin and subsoil just below it, and the near-surface
  //    cavern band, which follows h down to h - 100 (caveBands: h - vlen(40)
  //    - up to vlen(60)); so the box reaches 128 below the lower of the bare
  //    ground and the pad;
  //  - trees, ONLY when the footprint itself moved or resized (the lattice
  //    keeps trunks off it, siteBlocksTrunk): a trunk the old footprint kept
  //    out may stand now and one the new footprint refuses must go, crown
  //    and all -- the per-tree building rule keeps every crown
  //    kStampTreeClear off the rect, so the widest reach + that sideways,
  //    the tallest tree upward. An
  //    asset edit in place keeps the footprint, and that is the common case
  //    (P5's save), so it does not pay for the forest.
  const int belowGround = (128 * kVoxelsPerMetre) / 10;
  auto boxOf = [&](const Site& s, bool trees) {
    const int r = s.radius + s.apron + s.padMargin +
                  (trees ? (int)worldmap::kStampTreeClear + treeReach : 0) + 1;
    IVec3 lo{s.x - r, 0, s.z - r}, hi{s.x + r, 0, s.z + r};
    // The ground the pad cuts or fills, sampled over the box (the bare
    // ground: what the terrain is without this or any pad).
    int gMin = s.padY, gMax = s.padY;
    for (int j = 0; j <= 4; j++)
      for (int i = 0; i <= 4; i++) {
        const int g = worldmap::BareGroundHeight(lo.x + (hi.x - lo.x) * i / 4,
                                                 lo.z + (hi.z - lo.z) * j / 4, kDefaultSeed);
        gMin = std::min(gMin, g);
        gMax = std::max(gMax, g);
      }
    lo.y = gMin - std::max(belowGround, (int)worldmap::kStampSinkMax + 16);
    hi.y = std::max(s.padY + 1 - s.sink + s.ny, gMax + (trees ? treeAbove : 0)) + 16;
    return std::make_pair(lo, hi);
  };
  std::set<std::string> ids;
  for (const auto& [id, s] : before) ids.insert(id);
  for (const auto& [id, s] : after) ids.insert(id);
  for (const std::string& id : ids) {
    auto b = before.find(id);
    auto a = after.find(id);
    if (b != before.end() && a != after.end() && same(b->second, a->second)) continue;
    rep.changed.push_back(id);
    const bool footprintSame = b != before.end() && a != after.end() &&
                               b->second.x == a->second.x && b->second.z == a->second.z &&
                               b->second.radius == a->second.radius &&
                               b->second.nx == a->second.nx && b->second.nz == a->second.nz &&
                               b->second.apron == a->second.apron &&
                               b->second.padMargin == a->second.padMargin;
    if (b != before.end()) rep.boxes.push_back(boxOf(b->second, !footprintSame));
    if (a != after.end()) rep.boxes.push_back(boxOf(a->second, !footprintSame));
  }
  // Every chunk of every box, once, in a fixed order (the gen list is part
  // of the op record: a pure function of the boxes).
  std::set<std::tuple<int, int, int>> seen;
  std::vector<IVec3> chunks;
  for (const auto& [lo, hi] : rep.boxes)
    for (int cz = lo.z >> 4; cz <= (hi.z >> 4); cz++)
      for (int cy = lo.y >> 4; cy <= (hi.y >> 4); cy++)
        for (int cx = lo.x >> 4; cx <= (hi.x >> 4); cx++)
          if (seen.insert({cz, cy, cx}).second) chunks.push_back({cx, cy, cz});
  if (!chunks.empty()) rep.chunks = stream.RegenerateChunks(chunks, &rep.dropped);
  if (far != nullptr)
    for (const auto& [lo, hi] : rep.boxes) rep.farEntries += far->RefillBox(lo, hi);
  std::printf("structures: re-applied %zu changed structure(s): %u chunks regenerated, %u "
              "stored edits dropped, %u far fills\n",
              rep.changed.size(), rep.chunks, rep.dropped, rep.farEntries);
  return true;
}

// (Body render plumbing lives in game/bodyreg.h — see the note in support.h.)

// See support.h for why no gate may write an absolute Y.
int FixtureY(int x, int z, uint32_t seed, int above, int pad) {
  return FixtureYOver(x, z, x, z, seed, above, pad);
}

int FixtureYOver(int x0, int z0, int x1, int z1, uint32_t seed, int above,
                 int pad) {
  // A coarse 5x5 sample of the footprint. TerrainHeight is ~25 hash3 and this
  // runs once per fixture, so the cost is nothing; sampling the CORNERS ONLY
  // would miss a ridge crossing the middle of a wide slab.
  int h = INT32_MIN;
  const int nx = std::max(x1 - x0, 0), nz = std::max(z1 - z0, 0);
  for (int j = 0; j <= 4; j++)
    for (int i = 0; i <= 4; i++)
      h = std::max(h, World::TerrainHeight(x0 + nx * i / 4, z0 + nz * j / 4,
                                           seed));
  const int y = h + above;
  // Clamped against the window, not against a constant: a fixture built
  // outside residency reads as air to every kernel and the gate fails as
  // "nothing landed" rather than as "your fixture is off the map".
  return std::min(y, (int)kWorldN - pad);
}

bool WriteBmpFile(const std::string& path, const std::vector<uint8_t>& rgba,
              uint32_t w, uint32_t h) {
  uint32_t rowBytes = w * 3;
  uint32_t imgBytes = rowBytes * h;
  uint32_t fileBytes = 54 + imgBytes;
  std::vector<uint8_t> f(fileBytes, 0);
  auto put32 = [&](size_t off, uint32_t v) { std::memcpy(&f[off], &v, 4); };
  f[0] = 'B'; f[1] = 'M';
  put32(2, fileBytes); put32(10, 54); put32(14, 40);
  put32(18, w); put32(22, h);
  f[26] = 1; f[28] = 24;
  put32(34, imgBytes);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* src = &rgba[(size_t)(h - 1 - y) * w * 4];
    uint8_t* dst = &f[54 + (size_t)y * rowBytes];
    for (uint32_t x = 0; x < w; x++) {
      dst[x * 3 + 0] = src[x * 4 + 2];
      dst[x * 3 + 1] = src[x * 4 + 1];
      dst[x * 3 + 2] = src[x * 4 + 0];
    }
  }
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) return false;
  std::fwrite(f.data(), 1, f.size(), fp);
  std::fclose(fp);
  return true;
}

// Synchronously read the 4-byte world hash (selftest only).
uint32_t ReadHashSync(GpuContext& ctx, World& world) {
  uint32_t result = 0;
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.hash, 0, &result, 4, "hashRead");
  return result;
}

uint32_t HashWorldNow(GpuContext& ctx, World& world, Simulation& sim, uint32_t seed) {
  TickParams tp{0, seed, 0, 1, 0, 0, 0, 0};
  // THE ORIGIN IS LOAD-BEARING HERE, and it was not before the JITTER sentinel
  // existed. This standalone rehash builds a fresh TickParams, and `origin`
  // defaults to {0,0,0} — harmless while nothing in the hash path used it.
  // sim_occupancy's analytic sentinel branch now resolves a JITTER chunk's
  // WORLD position from (slot, origin) to synthesize its palette variants, so a
  // zero origin after a window shift hashes every jittered chunk at the wrong
  // coordinates. Symptom: --vk-smoke-loud diverged at ticks 86/88 — the first
  // two shifts — with the chunk CONTENTS provably identical (a per-chunk digest
  // diff showed zero differing slots), because only the hash was wrong.
  const IVec3 wo = world.WindowOrigin();
  tp.origin[0] = wo.x; tp.origin[1] = wo.y; tp.origin[2] = wo.z;
  ctx.queue.WriteBuffer(world.tickUBO, 0, &tp, sizeof(tp));
  rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
  sim.EncodeHashOnly(enc);
  ctx.queue.Submit(enc.Finish());
  // ho_occupancyFull rewrote the per-chunk digest table outside a hash tick,
  // so the next snapshot must copy it rather than inherit the previous one.
  world.NoteChunkHashWritten();
  return ReadHashSync(ctx, world);
}

// EVERY Y HERE IS A HEIGHT ABOVE THE GROUND, never an absolute one, and that is
// load-bearing rather than tidy. These ops are what the determinism hash and the
// smoke probes are computed over: a sand column falling onto terrain, a pour
// landing on the platform, fire reaching a surface, a seed on soil. Written as
// absolute Y they were silently anchored to a 5.4 m terrain band — the moment
// the datum moves (the terrain overhaul raises it by ~200 voxels) every one of
// them is either buried in rock or dropped from the sky into a chunk that is not
// resident, and the failure surfaces as a hash change with no cause.
//
// World::TerrainHeight is genColumn's `h` exactly (the height contract in
// DESIGN.md), so `+ kDelta` means what it reads as: that many voxels of clear
// air above the ground the brush is aimed at.
std::vector<BrushOp> SelftestOps(uint32_t tick, uint32_t seed) {
  std::vector<BrushOp> ops;
  auto ground = [&](int x, int z) { return World::TerrainHeight(x, z, seed); };
  if (tick >= 5 && tick < 150) {
    ops.push_back({100, ground(100, 100) + 110, 100, 6, kMatSand, 0, 0, 0});
    ops.push_back({176, ground(176, 176) + 90, 176, 5, kMatWater, 0, 0, 0});
  }
  if (tick >= 30 && tick < 90) {
    ops.push_back({64, ground(64, 72) + 6, 72, 4, kMatSmoke, 0, 0, 0});
  }
  // reaction-system coverage: lava boiling the pool, fire on the wood
  // platform, seeds germinating — all feed the determinism hash check
  if (tick >= 40 && tick < 100) {
    ops.push_back({176, ground(176, 150) + 60, 150, 4, kMatLava, 0, 0, 0});
  }
  if (tick >= 60 && tick < 120) {
    ops.push_back({110, ground(110, 110) + 20, 110, 3, kMatFire, 0, 0, 0});
  }
  if (tick >= 10 && tick < 16) {
    ops.push_back({150, ground(128, 128) + 30, 128, 2, kMatSeed, 0, 0, 0});
  }
  // melt-mode coverage (laser, PLAN §C1): catches the falling sand column in
  // a mode-2 brush — molten-glass conversion feeds the determinism hash. Four
  // under the sand source, so it cuts the column rather than its origin.
  if (tick >= 70 && tick < 100) {
    ops.push_back({100, ground(100, 100) + 106, 100, 3, 0, 2u, 0, 0});
  }
  // SOLUTE COVERAGE (docs/PLAN_solutes.md): salt rains into the water pour, so
  // the determinism hash covers dissolving, mass riding every liquid move,
  // diffusion, the page allocator and -- where the lava reaches the pool --
  // brine boiling off into precipitate. Resolved BY NAME through the table the
  // GPU holds (no id pinned for it); a table without salt simply skips it.
  if (tick >= 20 && tick < 70) {
    if (const SoluteDef* salt = CurrentSoluteNamed("salt"))
      ops.push_back({176, ground(176, 176) + 96, 176, 2, salt->from, 0, 0, 0});
  }
  return ops;
}

// Explosion + particle coverage for the determinism hashes: a terrain blast
// (solids/powder ejecta) and a pool blast (liquid splash).
std::vector<ExplosionOp> SelftestExps(uint32_t tick, uint32_t seed) {
  std::vector<ExplosionOp> exps;
  if (tick == 60) {
    int h = World::TerrainHeight(100, 100, seed);
    exps.push_back({100, h, 100, 14, 400, 0, 0, 0});
  }
  if (tick == 90) {
    exps.push_back({176, 50, 176, 10, 300, 0, 0, 0});
  }
  return exps;
}
constexpr uint32_t kSelftestFirstExp = 60;
// particles are possible from the first scripted explosion until well after
// the last ejecta has reinserted; must be a pure function of tick (§2/§4)
bool SelftestParticlesActive(uint32_t tick) { return tick >= kSelftestFirstExp; }

// Synchronously read both particle page counts (selftest only).
void ReadCountsSync(GpuContext& ctx, World& world, uint32_t out[2]) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.particleCounts, 0, out, 8,
                        "countsRead");
}

// The WHOLE buffer, ledger and sweep outputs alike (kWaterBodyStateTotalWords).
// One readback rather than two because M5's gate passes B and G compare a
// ledger word against a curve word in the same breath — a body's live level
// against the split elevation the sweep found for it — and two reads taken a
// tick apart would let a scheduled re-derive land between them.
void ReadWaterLedgerSync(GpuContext& ctx, World& world, int32_t* out) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.waterBodyState, 0, out,
                        (size_t)kWaterBodyStateTotalWords * 4,
                        "waterLedgerRead");
}

// `out32` must have room for kFluidArgsWords (world.h), which is what every
// caller declares. The size used to be a literal 32 here and a literal 128 in
// three places in world.cpp; they are one constant now.
void ReadFluidArgsSync(GpuContext& ctx, World& world, uint32_t* out32) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.fluidArgsStage, 0, out32,
                        kFluidArgsBytes, "fluidArgsRead");
}

// ---- gas particles: the gates' readback surface ---------------------------
// SYNCHRONOUS, and only ever called from a gate. The frame path reads gas
// through the snapshot ring (WorldSnapshot::gas*), exactly like everything
// else; nothing here is on it.

// Live gas parcels per PAGE. `GasAliveSync` picks the one the tick just wrote.
void ReadGasCountsSync(GpuContext& ctx, World& world, uint32_t out[2]) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.gasCounts, 0, out, 8,
                        "gasCountsRead");
}

// This tick's gas counters: gasSpawn's 8-word header, cleared before the CA
// runs, so every word is per-tick and not a running total. Index with the
// kGasSp* enum in world.h.
void ReadGasStatsSync(GpuContext& ctx, World& world, uint32_t* out16) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.gasSpawn, 0, out16,
                        kGasSpHdrBytes, "gasStatsRead");
}

uint32_t GasAliveSync(GpuContext& ctx, World& world, Simulation& sim) {
  uint32_t c[2] = {};
  ReadGasCountsSync(ctx, world, c);
  // Same parity as the ballistic count: after SubmitTick's FlipPage, Page() is
  // the buffer the tick just wrote.
  return std::min(c[sim.Page() & 1], kGasParticleCap);
}

// Reads the live gas page back and counts the parcels at or above `worldY`.
// The whole page, because there is no ordering to exploit — a parcel's slot
// says nothing about where it is (rule 1: behaviour is derived from state,
// never from a buffer slot, and that cuts both ways).
uint32_t GasAboveYSync(GpuContext& ctx, World& world, Simulation& sim,
                       int32_t worldY, uint32_t* outTotal) {
  const uint32_t n = GasAliveSync(ctx, world, sim);
  if (outTotal) *outTotal = n;
  if (n == 0) return 0;
  std::vector<uint32_t> p((size_t)n * 8, 0u);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.gasParticles[sim.Page() & 1], 0,
                        p.data(), (size_t)n * 32, "gasParticlesRead");
  uint32_t above = 0;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t* r = p.data() + (size_t)i * 8;
    if ((r[7] & kPFlagAlive) == 0) continue;   // flags
    const int32_t py = (int32_t)r[1] >> 8;     // 24.8 -> cell
    if (py >= worldY) above++;
  }
  return above;
}

// The outer density box, folded over the cells at or above `worldY`. `outMax`
// is the densest cell and `outSum` the total, which is what a gate asserting
// "there is a plume up there" wants — a max alone cannot tell one stray parcel
// from a column, and a sum alone cannot tell a column from a haze.
//
// The mapping is world.h's, restated nowhere: originVox = windowOrigin -
// kWorldN/2, cell = (voxel - originVox) >> kGasOuterShift.
void ReadGasOuterAboveSync(GpuContext& ctx, World& world, int32_t worldY,
                           uint32_t* outMax, uint64_t* outSum) {
  std::vector<uint32_t> g(kGasOuterWords, 0u);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.gasOuter, 0, g.data(),
                        (size_t)kGasOuterWords * 4, "gasOuterRead");
  const IVec3 wo = world.WindowOrigin();   // CHUNK units
  const int32_t oy = wo.y * (int32_t)kChunk - (int32_t)(kWorldN / 2);
  uint32_t mx = 0;
  uint64_t sum = 0;
  // Two 16-bit counts per word since stage 1b (world.h kGasOuterWords), so the
  // linear cell index indexes a u16 array and not a byte one.
  const uint16_t* b = (const uint16_t*)g.data();
  for (uint32_t cy = 0; cy < kGasOuterN; cy++) {
    if (oy + (int32_t)(cy << kGasOuterShift) < worldY) continue;
    for (uint32_t cz = 0; cz < kGasOuterN; cz++)
      for (uint32_t cx = 0; cx < kGasOuterN; cx++) {
        const uint32_t v = b[(cz * kGasOuterN + cy) * kGasOuterN + cx];
        if (v > mx) mx = v;
        sum += v;
      }
  }
  if (outMax) *outMax = mx;
  if (outSum) *outSum = sum;
}

// The outer density box, folded over ONE WORLD-VOXEL BOX. The `Above` fold
// beside this one answers "is there a plume up there anywhere", which is the
// right question for a gate whose fixture is the only gas in the world; a gate
// that has to say WHICH column the density is over needs a box, and asserting
// on the whole-box sum instead would pass on a plume that came out somewhere
// else entirely.
//
// Same mapping, same three copies of one identity: originVox = windowOrigin -
// kWorldN/2, cell = (voxel - originVox) >> kGasOuterShift. Ends are inclusive
// in CELLS, so a box smaller than a cell still reads the cell it lands in.
void ReadGasOuterBoxSync(GpuContext& ctx, World& world, IVec3 loVox,
                         IVec3 hiVox, uint32_t* outMax, uint64_t* outSum) {
  std::vector<uint32_t> g(kGasOuterWords, 0u);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.gasOuter, 0, g.data(),
                        (size_t)kGasOuterWords * 4, "gasOuterBoxRead");
  const IVec3 wo = world.WindowOrigin();
  const int32_t h = (int32_t)(kWorldN / 2);
  const int32_t o[3] = {wo.x * (int32_t)kChunk - h, wo.y * (int32_t)kChunk - h,
                        wo.z * (int32_t)kChunk - h};
  const int32_t lv[3] = {loVox.x, loVox.y, loVox.z};
  const int32_t hv[3] = {hiVox.x, hiVox.y, hiVox.z};
  int32_t lo[3], hi[3];
  for (int a = 0; a < 3; a++) {
    lo[a] = std::max((lv[a] - o[a]) >> (int32_t)kGasOuterShift, 0);
    hi[a] = std::min((hv[a] - o[a]) >> (int32_t)kGasOuterShift,
                     (int32_t)kGasOuterN - 1);
  }
  uint32_t mx = 0;
  uint64_t sum = 0;
  const uint16_t* b = (const uint16_t*)g.data();
  for (int32_t cz = lo[2]; cz <= hi[2]; cz++)
    for (int32_t cy = lo[1]; cy <= hi[1]; cy++)
      for (int32_t cx = lo[0]; cx <= hi[0]; cx++) {
        const uint32_t v = b[((uint32_t)cz * kGasOuterN + (uint32_t)cy) *
                                 kGasOuterN + (uint32_t)cx];
        if (v > mx) mx = v;
        sum += v;
      }
  if (outMax) *outMax = mx;
  if (outSum) *outSum = sum;
}

// The LONG-RANGE box (world.h kGasFarOuterN), folded over a world-voxel box.
// Same shape as the reader above; the two differ only in the buffer, the cell
// shift and the ORIGIN RULE, and the third is the one that matters — this box's
// origin is floored to the cell so the lattice is fixed in world space.
// Restating it here rather than parameterising the near reader keeps the gate's
// copy of the mapping next to the mapping's own justification.
void ReadGasFarOuterBoxSync(GpuContext& ctx, World& world, IVec3 loVox,
                            IVec3 hiVox, uint32_t* outMax, uint64_t* outSum) {
  std::vector<uint32_t> g(kGasFarOuterWords, 0u);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.gasFarOuter, 0, g.data(),
                        (size_t)kGasFarOuterWords * 4, "gasFarOuterBoxRead");
  const IVec3 wo = world.WindowOrigin();
  // Anisotropic cell (world.h kGasFarOuterShiftY): 64 voxels in x/z, 8 in y.
  const int32_t shv[3] = {(int32_t)kGasFarOuterShift, (int32_t)kGasFarOuterShiftY,
                          (int32_t)kGasFarOuterShift};
  const int32_t offv[3] = {kGasFarOuterOffsetVox, kGasFarOuterOffsetVoxY,
                           kGasFarOuterOffsetVox};
  const int32_t wov[3] = {wo.x * (int32_t)kChunk, wo.y * (int32_t)kChunk,
                          wo.z * (int32_t)kChunk};
  int32_t o[3];
  for (int a = 0; a < 3; a++)
    o[a] = ((wov[a] - offv[a]) >> shv[a]) << shv[a];
  const int32_t lv[3] = {loVox.x, loVox.y, loVox.z};
  const int32_t hv[3] = {hiVox.x, hiVox.y, hiVox.z};
  int32_t lo[3], hi[3];
  for (int a = 0; a < 3; a++) {
    lo[a] = std::max((lv[a] - o[a]) >> shv[a], 0);
    hi[a] = std::min((hv[a] - o[a]) >> shv[a], (int32_t)kGasFarOuterN - 1);
  }
  uint32_t mx = 0;
  uint64_t sum = 0;
  const uint16_t* b = (const uint16_t*)g.data();
  for (int32_t cz = lo[2]; cz <= hi[2]; cz++)
    for (int32_t cy = lo[1]; cy <= hi[1]; cy++)
      for (int32_t cx = lo[0]; cx <= hi[0]; cx++) {
        const uint32_t v = b[((uint32_t)cz * kGasFarOuterN + (uint32_t)cy) *
                                 kGasFarOuterN + (uint32_t)cx];
        if (v > mx) mx = v;
        sum += v;
      }
  if (outMax) *outMax = mx;
  if (outSum) *outSum = sum;
}

void ReadPageFaultsSync(GpuContext& ctx, World& world, uint32_t out[4]) {
  rhi::ReadbackBlocking(ctx.device, ctx.queue, world.pageFaults, 0, out, 16,
                        "pageFaultRead");
}

uint32_t ReadActiveChunksSync(GpuContext& ctx, World& world, Simulation& sim) {
  std::vector<uint32_t> flags(kNumSlots, 0);
  rhi::ReadbackBlocking(ctx.device, ctx.queue, sim.DirtyActive(), 0, flags.data(),
                        kNumSlots * 4, "activeRead");
  uint32_t n = 0;
  for (uint32_t i = 0; i < kNumSlots; i++)
    if (flags[i] != 0) n++;
  return n;
}


}  // namespace sandvox
