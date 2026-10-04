// demon_seals.h — SEALS, CHANNELS, CIRCLE STRENGTH, RELEASE, GAZE, TELLS
// (docs/PLAN_demons.md D3; DESIGN.md §17 "Demons -- seals ...").
//
// THE SALT RING CONTAINS; THE PILES CUT. D1's circle (demon_circle.h) decides
// whether a demon is CONTAINED at all. Inside a closed circle, what the demon
// can still DO is a set of CHANNELS, each cut ("severed") by a pile of one
// seal material lying in a BAND round the ring:
//
//   move      salt      walking out (the ring's own salt, counted by mass)
//   cast_out  sulfur    its spells leaving the circle      (D4 calls AllowCastOut)
//   blink     quicksilver  teleporting                     (D4 calls AllowBlink)
//   touch     iron      a blow across the ring             (MobFence::Blow)
//
// POTENCY of a channel = sum over the band's seal cells of perCell x the
// cell's MASS in eighths / 8 (powder mass, liquid fullness; a solid is a whole
// cell), cells lighter than `minEighths` not counted (a scattered film is not
// a pile). A channel is SEVERED iff potency >= the demon's resistance for it
// (assets/demons/<name>.json `resist`; a channel the file does not list has
// resistance 0, i.e. the demon does not have it). An UNSEVERED channel is a
// LOOPHOLE: the demon can use it from inside the circle -- claws across the
// ring, a bolt out of it, a blink out of it (which frees it), a ring too thin
// to hold its legs (which frees it).
//
// CIRCLE STRENGTH (an integer, points) = sum over seal entries of the band's
// mass-cells / cellsPerPoint (salt included: width and integrity of the ring)
// + lit candles x perCandle (capped) - the gaze strain loss. Weighed against
// the demon's power on RELEASE (TB_DEMON_RELEASE through TickInput): held iff
// strength >= power + contract weight (0 until D5). Held -> RELEASED (the
// def's `released` behaviour profile, not aimed at you); not held -> UNBOUND
// and hostile.
//
// THE BAND is an annulus round the circle's centroid, from `bandInM` inside
// the inside's furthest column to `bandOutM` beyond it, over a slab from one
// row below the demon's feet to `slabAbove` above (seals) / `candleSlabAbove`
// (candles). Re-read on the SAME triggers as the circle (D1: a chunk it
// overlaps awake in the snapshot, or the recheck cadence) -- so a pile the
// wind scatters out of the band flips its channel at the next re-read. A band
// cell no store holds -> the LAST READING is held (as the circle's verdict is).
//
// GAZE (per demon: `hold` | `avert`): per tick while contained, a cone test of
// the summoner's camera forward (TickInput::lookFwd, at the eye) against the
// demon's head, within `rangeM`. Breaking the rule raises a STRAIN meter;
// keeping it lowers it. A full meter costs the circle `strainPenalty` points
// (accumulating to `strainLossMax`) and empties. No instant fail. The camera
// lock and the look-away key are D5's.
//
// TELLS (assets/demons/tells.json): lines by MARGIN (strength - power -
// weight), per demon or default. Data now; D5's dialogue speaks them.
//
// DETERMINISM: CPU gameplay state in the 30 Hz tick; voxels only through the
// probe D1 binds (T-4 snapshot mirror, then the fetch cache); integer
// potencies and strength; line picks by rng::Hash3. NOT SAVED (as D1): a
// binding is the live demon's, and D5 is where bound demons persist.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game/demon_circle.h"
#include "math3d.h"

struct TickAuthorityCtx;
struct SessionTick;
struct UIState;
struct MaterialDef;
struct LiveDemon;

namespace demon {

enum class Channel : uint8_t { Move = 0, CastOut, Blink, Touch };
constexpr int kChannels = 4;
const char* ChannelName(Channel c);            // "move" | "cast_out" | "blink" | "touch"
bool ChannelByName(const std::string& n, Channel& out);

// ---- content (assets/demons/seals.json, tells.json) ------------------------------

struct SealDef {
  std::string material;    // by name, resolved per scan against the live table
  Channel severs = Channel::Move;
  int32_t perCell = 1;     // potency per full cell
  int32_t cellsPerPoint = 4;   // strength: one point per this many full cells (0 = none)
};

struct TellBand {
  int32_t minMargin = 0;   // this band speaks while margin >= minMargin
  std::vector<std::string> lines;
};

struct SealLib {
  std::vector<SealDef> seals;
  // the band
  float bandInM = 0.6f, bandOutM = 0.9f;
  int32_t slabAbove = 2;
  int32_t minEighths = 2;
  // candles
  std::string candle = "candle_flame";
  int32_t candleSlabAbove = 6;
  int32_t perCandle = 3;
  int32_t candlesMax = 8;
  // gaze
  float gazeConeDeg = 18.0f;
  float gazeRangeM = 8.0f;
  int32_t strainRise = 3, strainFall = 1, strainMax = 300;
  int32_t strainPenalty = 8, strainLossMax = 32;
  // tells: default bands, and per-demon overrides (demon id -> bands), each
  // sorted by minMargin, highest first.
  std::vector<TellBand> tells;
  std::vector<std::pair<std::string, std::vector<TellBand>>> demonTells;
};

// Called by LoadDemons for the two stems it does not treat as demon defs. A
// bad file is reported into `log` and leaves the defaults; never fatal.
void LoadSealFile(const std::string& path, const std::string& stem, SealLib& out,
                  std::string& log);
bool IsSealFile(const std::string& stem);   // "seals" | "tells"

// ---- one reading of the band ------------------------------------------------------

struct SealReading {
  bool valid = false;
  std::array<int32_t, kChannels> potency{};   // per channel
  std::array<int32_t, kChannels> eighths{};   // seal mass per channel, eighths (attribution)
  int32_t saltEighths = 0;                    // the `move` seal's mass, eighths
  int32_t sealPoints = 0;                     // strength from seal entries (salt included)
  int32_t candlesLit = 0;
  int32_t cellsRead = 0;                      // cost
  IVec3 unknownAt{};                          // !valid: the first unseen cell
};

// The band pass. `feetY` is the circle's feet row. Pure (reads only `probe`).
SealReading ScanSeals(const CircleProbe& probe, const SealLib& lib,
                      const std::vector<MaterialDef>& mats, const CircleShape& circle);

// ---- a live demon's binding (LiveDemon::bind) ------------------------------------

enum class ReleaseResult : uint8_t { None = 0, Held, Overpowered };

struct Binding {
  SealReading reading;
  uint32_t scanTick = 0;
  int32_t strength = 0;          // reading points + candles - strain loss
  int32_t power = 0;             // the def's, at the last evaluation
  uint8_t severed = 0;           // bit per Channel
  // gaze
  int32_t strain = 0;            // 0..strainMax
  int32_t strainMax = 1;         // the library's, for the HUD
  int32_t strainLoss = 0;        // points the circle has lost to the gaze
  uint32_t lapses = 0;           // times the meter filled
  bool gazeHold = false;         // the def's rule
  bool gazeBroken = false;       // this tick, in range
  // the hooks' counters
  uint32_t blowsLoophole = 0;    // blows across the ring allowed (touch unsevered)
  uint32_t castOutRefused = 0, castOutAllowed = 0;
  uint32_t blinkRefused = 0, blinkAllowed = 0;
  ReleaseResult release = ReleaseResult::None;
  int32_t releaseMargin = 0;
  bool Severed(Channel c) const { return (severed >> (int)c) & 1u; }
};

// ---- the query D4 calls (and the fence) ------------------------------------------

// True iff `demonId` (a mob id) is a demon held CONTAINED in an intact circle
// AND that channel's seal potency >= its resistance. Not a demon, not
// contained, or a channel left open -> false (the demon may use it).
bool ChannelSevered(const TickAuthorityCtx& w, uint64_t demonId, Channel ch);

// D4's cast hook: may this demon's spell, cast from `from`, land its footprint
// at `to` (world voxels)? Refused iff contained, cast_out severed, and `to` is
// outside the circle. Counted on the demon's binding.
bool AllowCastOut(TickAuthorityCtx& w, uint64_t demonId, Vec3 from, Vec3 to);

// D4's blink hook: may this demon teleport to `to`? Contained: refused iff
// blink is severed. Allowed and `to` outside the circle = the loophole: the
// demon is UNBOUND at once (it has left the circle). Not contained: allowed.
bool AllowBlink(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t demonId, Vec3 to,
                uint32_t tick);

// The overpower check, for the release key and (D5) the dialogue's `release`:
// held iff strength >= power + weight. Held -> Released; else -> Unbound and
// aimed at the summoner. Returns None if `demonId` is not contained.
ReleaseResult Release(TickAuthorityCtx& w, std::span<SessionTick> players, uint64_t demonId,
                      int32_t contractWeight, uint32_t tick);

// The tell for a margin: a line from the demon's own table (else the default),
// picked by rng::Hash3(salt, band, demon). Empty if no table.
const std::string& TellFor(const SealLib& lib, const std::string& demonId, int32_t margin,
                           uint32_t salt);

// ---- the tick (called from DemonTick) --------------------------------------------

// Release presses, band re-reads (when D1 re-read the circle this tick, or the
// demon has never been read), the move loophole, gaze strain, strength.
void SealsTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
               const CircleProbe& probe);

// The HUD tab's numbers for one demon (DemonTick's HUD loop).
void FillHud(UIState& ui, const LiveDemon& ld);

}  // namespace demon
