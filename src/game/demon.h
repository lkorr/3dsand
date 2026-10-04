// demon.h — DEMONS: the summoning, the circle's hold, the goof
// (docs/PLAN_demons.md D1; DESIGN.md "Demons").
//
// THE SHAPE OF IT. A demon's NAME is a glyph (verb `summon`, glyphs.json
// `summon_<name>`); casting it reports a SpellSummon (the VM never spawns a
// body). The owner queues it here, asks the fetch cache for every chunk the
// circle scan may read, and LEAD ticks later (the demon taking shape) reads
// the floor and the salt circle at the arrival point through the T-4
// snapshot stores (demon_circle.h) and spawns the demon there:
//
//   * inside an intact salt circle -> CONTAINED: it keeps its hostile profile
//     and goes on wanting you, but MobSystem's fence (mobfence.h) refuses
//     every walk that would take its footprint centre out of the circle and
//     every blow aimed across it. Not a wall: the player and items cross.
//   * anywhere else (no circle, a gap, cast onto the ring, a scan that could
//     not see) -> UNBOUND, at once, its brain pointed at the summoner. The
//     goof is deliberate.
//
// While contained, the circle is RE-READ when a chunk it overlaps is awake in
// the snapshot, or every `recheckTicks` otherwise (bounded: one small fill per
// demon per check). A re-read that finds the loop open -- wind scattered the
// salt, water dissolved it, a boot scuffed it -- unbinds the demon that tick.
// A re-read that cannot see (the circle out of every store) HOLDS the last
// answer, so a demon left contained in a cellar stays contained while you are
// away; the chunks are requested again for the next check.
//
// CONTENT IS DATA (assets/demons/): one `<name>.json` per demon (the mob def,
// its behaviour profile, power, tier, gaze, per-channel resistances, the
// schemes it knows -- the last four read from D3/D6 on) and `circle.json`
// (salt by name, the escape radius, the slab, the cadences). Read again at
// every summoning, so an edit is live on the next cast.
//
// DETERMINISM: CPU gameplay state in the 30 Hz tick, reading voxels only
// through the snapshot stores (both pure functions of the tick), spawning
// through MobSystem::Spawn in a fixed order. No random draw yet.
//
// NOT SAVED (D1): the live list and the pending queue. A contained demon in a
// save loads back as the creature it is -- its hostile profile, no circle --
// i.e. loose. D5 (contracts) is where bound demons start persisting.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game/demon_circle.h"
#include "math3d.h"

struct TickAuthorityCtx;
struct SessionTick;
struct SpellSummon;
struct GlyphLibrary;
class MobSystem;

// ---- content -----------------------------------------------------------------

struct DemonDef {
  std::string id;          // the file stem: what a glyph's `summon.demon` names
  std::string name;        // "Skerrick"
  std::string mob;         // the mob def stem (assets/mobs/demon/<mob>)
  std::string behavior;    // assets/mobs/behaviors.json profile
  int32_t tier = 1;        // 1 imp .. 3 greater (D6's free act at >= 2)
  int32_t power = 10;      // against circle strength (D3)
  std::string gaze = "avert";   // "hold" | "avert" (D3)
  std::vector<std::pair<std::string, int32_t>> resist;  // channel -> resistance (D3)
  std::vector<std::string> schemes;                     // scheme names (D6)
};

struct DemonCircleCfg {
  std::string material = "salt";
  float radiusMaxM = 4.0f;      // the fill escaping this far = no circle
  int32_t slabBelow = 1;        // slab rows below / above the feet row
  int32_t slabAbove = 1;
  int32_t recheckTicks = 15;    // re-read cadence while contained
  int32_t leadTicks = 8;        // cast -> arrival (the fetches land)
  int32_t floorSearch = 12;     // cells below the impact to look for a floor
  int32_t minEighths = 4;       // a salt cell walls with this much of a cell in it
};

struct DemonLibrary {
  std::vector<DemonDef> defs;
  DemonCircleCfg circle;
  const DemonDef* Find(const std::string& id) const {
    for (const DemonDef& d : defs)
      if (d.id == id) return &d;
    return nullptr;
  }
};
// assets/demons/*.json. A bad file is skipped LOUDLY into `log`, never fatal.
bool LoadDemons(const std::string& dir, DemonLibrary& out, std::string& log);

// SANDVOX_ALL_NAMES=1: every demon NAME glyph is granted (caster.h
// GrantAllAndBind). Without it a name is knowledge you have to find (D2's
// book). Read once.
bool DebugAllDemonNames();

// ---- the world's demons ---------------------------------------------------------

enum class DemonState : uint8_t { Contained = 0, Unbound };
const char* DemonStateName(DemonState s);

struct LiveDemon {
  uint64_t mobId = 0;
  std::string demon;        // DemonDef::id
  std::string name;         // DemonDef::name
  int session = 0;          // the summoner (PlayerSession::index)
  DemonState state = DemonState::Unbound;
  CircleShape circle;       // the inside, while contained
  uint32_t spawnTick = 0, stateTick = 0, lastCheck = 0;
  uint32_t movesRefused = 0, blowsRefused = 0;
  std::string why;          // why this state (the HUD / the gate)
};

struct PendingSummon {
  IVec3 at{};
  int glyph = -1;
  std::string demon;
  int session = 0;
  int32_t tariff = 0;
  uint32_t castTick = 0, dueTick = 0;
};

struct DemonWorld {
  static constexpr size_t kMaxPending = 8;    // casts in flight (rule 2)
  static constexpr size_t kMaxLive = 16;      // demons this world tracks
  // A 4 m escape radius reads up to ~100 chunks round the arrival; the
  // nearest first. The fetch queue drains World::kFetchPerTick a tick, so
  // this is two ticks of it, inside the lead.
  static constexpr int32_t kMaxFetchPerSummon = 128;
  DemonLibrary lib;
  bool loaded = false;
  std::string loadLog;
  std::vector<PendingSummon> pending;
  std::vector<LiveDemon> live;
  // The MobSystem whose fence points at this object (cleared on destruction:
  // a harness's MobSystem outlives the rig that owns this).
  MobSystem* fenced = nullptr;
  struct Stats {
    uint64_t summons = 0;     // summonings that arrived (spawned)
    uint64_t contained = 0;   // ...inside an intact circle
    uint64_t loose = 0;       // ...anywhere else
    uint64_t refused = 0;     // refused (unknown demon / mob, no room, spawn failed)
    uint64_t broken = 0;      // circles that opened under a contained demon
    uint64_t rechecks = 0;    // circle re-reads
    uint64_t held = 0;        // re-reads that could not see, answer held
    uint64_t movesRefused = 0, blowsRefused = 0;   // the fence's refusals
  } stats;
  DemonWorld() = default;
  DemonWorld(const DemonWorld&) = delete;
  DemonWorld& operator=(const DemonWorld&) = delete;
  ~DemonWorld();
  const LiveDemon* Find(uint64_t mobId) const {
    for (const LiveDemon& d : live)
      if (d.mobId == mobId) return &d;
    return nullptr;
  }
};

// The world's demons, created on first use.
DemonWorld& Demons(TickAuthorityCtx& w);

// Phase I: the VM reported a summoning by `session`'s cast. Queued (and its
// chunks requested); refused -- tariff refunded to that session's caster -- if
// the glyph names no demon or the queue is full.
void DemonQueueSummon(TickAuthorityCtx& w, std::span<SessionTick> players, int session,
                      const SpellSummon& su, const GlyphLibrary& glyphs, uint32_t tick);

// Once per tick, before phase H's mobs.PreTick: due arrivals spawn, contained
// demons have their circle re-read, the fence is (re)installed, the HUD line
// is written for each summoner.
void DemonTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick);

// The summoning itself, with the stores already holding what it reads: the
// floor, the circle, the spawn, the verdict. What DemonTick runs for a due
// arrival; public so a gate can drive one arrival through the real path.
uint64_t DemonArrive(TickAuthorityCtx& w, std::span<SessionTick> players,
                     const PendingSummon& p, uint32_t tick);
