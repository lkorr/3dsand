#pragma once
// weather.h — the SKY'S weather: which clouds are up, how much of the sky
// they cover, whether it is raining, and how that eases from one to the next.
//
// docs/PLAN_clouds_weather.md is the plan of record; DESIGN.md §9.w states
// the invariants. The short version:
//
//   * A weather TYPE is data. assets/weather/<name>.json names a preset —
//     coverage, cloud type, deck altitude and thickness, cirrus, rain, mist,
//     lightning — and nothing in C++ switches on a name. A new kind of sky is
//     a JSON file (design guideline 4, "no closed-ended systems").
//
//   * The resolved weather is a PURE FUNCTION of (tuning, presets, seed,
//     tick). Like `WindWeather` and `ComputeSky` it integrates nothing and
//     holds no state that a frame boundary could move, so a replay renders the
//     sky the player saw and `--shot` is reproducible.
//
//   * It is RENDER-ONLY except for ONE integer: SimRainWord, below, which
//     rides TickParams and is how rain douses fires and damps ignition (the
//     "rain" / "rainDamped" reaction flags). Floats are allowed everywhere
//     else for exactly that reason. Do not hand the sim one of these floats —
//     anything else the weather should do to the world goes through that word
//     (or a sibling of it), quantised on the tick stream.
//
// THE AUTOMATIC CYCLE. The three-field model from the corpus (plan §1c):
// weather is not a state machine, it is a position on a MOISTURE ladder.
// Presets are sorted by their authored `moisture`; a smooth 1-D value-noise
// signal over weather epochs walks up and down that ladder, so the sky drifts
// clear -> fair -> scattered -> overcast -> rain -> storm and back through its
// NEIGHBOURS rather than jumping from a storm to a cloudless noon. Each
// preset owns a share of the ladder proportional to its `weight`, which is
// how often the cycle lingers in it (weight 0 = manual only, e.g. snow).
// Coverage, rain and raininess come from ONE draw, so a dark low deck is
// always the deck that rains (plan §2.1: "an overcast that is not also the
// rainy tick is a lie, and this construction makes the lie unrepresentable").

#include <cstdint>
#include <string>
#include <vector>

struct Tuning;

namespace weather {

// One authored sky. Every field is a scalar the cloud pass or the composite
// consumes; `Lerp` below blends two of them field by field, which is the
// whole of a weather transition. Lengths are METRES, fractions are 0..1.
struct Preset {
  std::string name;       // file stem — the one identity
  std::string label;      // for the tuner / dev panel
  float moisture = 0.5f;  // ladder position for the automatic cycle
  float weight = 1.0f;    // share of the ladder (0 = never chosen by auto)

  // ---- the cumulus deck ----
  float coverage = 0.4f;    // fraction of the sky the deck covers
  float cloudType = 0.5f;   // 0 stratus sheet, 0.5 cumulus, 1 cumulonimbus
  float density = 1.0f;     // extinction multiplier
  float baseM = 1400.0f;    // deck base altitude above sea level
  float thicknessM = 1600.0f;
  float darkness = 0.0f;    // extra base absorption: the slate underside of rain

  // ---- the high deck ----
  float cirrus = 0.2f;      // opacity of the thin ice-cloud layer
  float cirrusAltM = 8000.0f;

  // ---- precipitation ----
  float precip = 0.0f;      // raininess: how readily cloud turns into rain
  float precipType = 0.0f;  // 0 rain, 1 snow (blends: sleet)

  // ---- the world under it ----
  float mist = 0.0f;        // extra ground fog, x the base fog density (+1)
  float lightning = 0.0f;   // mean flashes per minute (0 = never)
  float windShear = 1.0f;   // how fast the deck drifts vs the wind (x)
};

// The weather for one frame, resolved and blended. What WriteRenderParams and
// the cloud UBO are filled from.
struct State {
  Preset mix;              // the blended fields (name = the dominant preset)
  std::string fromName;    // the pair being blended, for the readout
  std::string toName;
  float blend = 0.0f;      // 0 = all `from`, 1 = all `to`
  float wetness = 0.0f;    // 0..1, how soaked the exposed ground is
  float overcast = 0.0f;   // 0..1, how much of the dome is under cloud
  float flash = 0.0f;      // lightning brightness this frame (0 = none)
  float flashX = 0.0f, flashZ = 0.0f;  // flash position, metres from camera
  bool enabled = true;     // render.clouds master switch
};

// Field-by-field blend of two presets. `t` is not clamped by the callers'
// contract; it is here, because a blend outside 0..1 extrapolates coverage
// past 1 and the cloud pass has no defence against that.
Preset Lerp(const Preset& a, const Preset& b, float t);

// The loaded preset set. Loaded lazily from AssetDir()/weather on first use
// and again on Reload() (the R hot-reload and F5 both call it). A missing or
// empty directory is NOT fatal: the built-in table below is used and one
// warning is printed, so a checkout without the folder still has a sky.
// The four fields SimRainWord reads, as Q16 fixed point (65536 = 1.0),
// quantised ONCE per load from the authored floats. The sim word is computed
// from these in integers only — see SimRainWord.
struct PresetQ {
  int64_t weight = 0, coverage = 0, precip = 0, precipType = 0;
};

class Library {
 public:
  const std::vector<Preset>& Presets();   // sorted by moisture
  // Parallel to Presets(): entry i is Presets()[i] quantised.
  const std::vector<PresetQ>& PresetsQ();
  const Preset* Find(const std::string& name);
  void Reload();
  const std::vector<std::string>& Warnings() const { return warnings_; }

 private:
  void EnsureLoaded();
  bool loaded_ = false;
  std::vector<Preset> presets_;
  std::vector<PresetQ> presetsQ_;
  std::vector<std::string> warnings_;
};
Library& Presets();

// ---- the runtime override (dev panel / tuner "Weather" row) ----------------
// A knob the game can set without editing tuning.json: pin a named preset, or
// "" to hand the sky back to tuning (weather.autoCycle / weather.preset). The
// SKY eases over weather.transitionSeconds of frame time, so flipping presets
// in the dev panel reads as the weather turning, not a cut. The SIM does not
// ease: SimRainWord reads the pin directly, because a wall-clock ease on the
// tick stream would make the world hash depend on frame pacing. The pin is an
// input, recorded on the tick stream like any other.
void SetOverride(const std::string& name);
const std::string& Override();

// Resolve the weather for this frame. `timeSeconds` is the sim clock
// (tick / 30 + frameFrac / 30) — NOT wall time, so replays and --shot agree.
// `frameDtSeconds` only drives the manual-override ease and the lightning
// envelope; pass 0 on a headless path and both snap.
State Resolve(const Tuning& t, uint32_t seed, double timeSeconds,
              float frameDtSeconds);

// THE SIM'S VIEW OF THE WEATHER: TickParams::weatherRain for `tick`, the one
// integer by which the sky touches the world (reaction flags "rain" and
// "rainDamped" — materials.h kCondRain / kCondRainDamp; layout kRain*).
//   bits 0..7   rain reaching the ground now, 0..255
//   bits 8..15  weather.rainIgniteDamp, 0..255
//   bits 16..23 ground wetness (the leaky integral), 0..255
// A pure function of (tuning, presets, seed, tick, the pinned preset): it
// reads the un-eased TARGET, never the frame-time ease, so frame pacing cannot
// reach the world hash. INTEGER MATH END TO END (2026-09-24): every float input
// — the presets (PresetQ) and the weather.* tuning — is quantised to Q16 by one
// exact conversion, and the schedule, ladder, blend and wetness sum run on
// int64. It used to run the renderer's float path (std::exp, float smoothstep
// and lerp, then lround), and a libm `exp` is exactly the kind of thing two
// machines may disagree on in the last bit — which a quantiser then turns into
// a different word and a multiplayer desync. The renderer keeps the float path;
// the two agree up to fixed-point rounding, not bit-for-bit.
// 0 when weather.clouds or weather.rainTouchesWorld is off.
uint32_t SimRainWord(const Tuning& t, uint32_t seed, uint32_t tick);
// The word the last SimRainWord call returned — a readout for the dev panel,
// never an input to anything.
uint32_t LastSimRainWord();

// The last State Resolve returned, for the dev panel's readout and the gate.
const State& Last();

// Make the next Resolve SNAP to its target instead of easing toward it: for a
// harness that changes the pin and wants the very next frame to show it.
void Snap();

}  // namespace weather
