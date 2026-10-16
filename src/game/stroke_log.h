#pragma once
// stroke_log.h -- THE CALIBRATION LOG: every NPC stroke STARTED, with its
// outcome (docs/PLAN_distant_npcs.md §2.6 / §3 D2a).
//
// WHY EVERY STROKE AND NOT EVERY WOUND. The distant tier resolves a blow as a
// roll (game/distant_combat.h), and the roll needs a HIT RATE. A log of wounds
// cannot give one: the denominator is the swings that met nothing -- the
// target stepped off the line, the cut passed in front of the chest, a shield
// came up -- and those leave no mark anywhere (plan audit A9). So the record
// opens when a swing BEGINS (MobSystem::StartStroke, the one door every AI and
// scripted swing goes through) and closes when it ends, and in between every
// damage sweep the stroke runs is watched: did it meet the target, a shield, a
// blade, somebody else, or nothing; which limb lost how much life; how much
// went into a shell or into the guard; what blood it opened.
//
// IT MEASURES BY DIFFERENCE, NOT BY INSTRUMENTING THE WOUND MODEL. Before a
// stroke's sweep the target's per-limb hp, shell hp, held-item hp, hp-by-cause
// ledger (Mob::HpLostBy) and open bleed budget are read; after it, read again.
// Inside one sweep nothing else touches that body (StepStroke is serial, and
// bleeding and burning run in their own phases), so the difference IS what the
// blow did, after every shell rule, every repeat-strike falloff and every
// carve's volume charge -- the numbers the abstract tier has to reproduce --
// and no line of the cut, blunt or bite resolvers had to learn the log exists.
//
// ZERO COST WHEN OFF: every hook in mob.cpp is `if (Sink* s = Active())`, one
// load of a global and a branch. Observation only: the sink reads public state
// and writes nothing back (the `distant-calib-matrix` gate runs a fight with
// the log off and on and requires the per-tick creature digests and op batches
// to be bit-identical).
//
// ON: the `distant-calib-matrix` gate installs a sink for its fights (a Scope);
// `SANDVOX_DISTANT_CALIB=<out.json>` runs that gate's full matrix and writes
// the reduced tables. Nothing else turns it on.

#include <cstdint>
#include <string>
#include <vector>

#include "game/distant_combat.h"
#include "phys/damagecause.h"

class Mob;
class MobSystem;
struct EdgeSweepResult;
struct ItemLibrary;

namespace strokelog {

enum class Outcome : uint8_t {
  Open = 0,  // still swinging
  Miss,      // its sweeps met nothing of the target and were not stopped
  Blocked,   // a SHIELD arrested it (MobSystem::FindShieldBlock)
  Parried,   // a BLADE arrested it (MobSystem::FindParry)
  Hit,       // the target's body (flesh or a worn shell) lost hp to it
  HitOther,  // it met somebody or something else (a bystander, a corpse)
};
const char* OutcomeName(Outcome o);

enum class EndReason : uint8_t {
  Open = 0,
  Finished,  // the program ran out (StepStroke's Finished)
  Aborted,   // the effector went, the swinger died or the library reloaded
  Replaced,  // a new stroke began on the same creature first
  Flushed,   // still open when the log was flushed
};

struct Stroke {
  // ---- AT THE START (MobSystem::StartStroke) --------------------------------
  uint32_t startTick = 0;
  uint64_t attacker = 0, target = 0;
  std::string attackerDef, profile;
  std::string style;        // the BASE style (the table row: "horizontal_r")
  std::string form;         // the weapon form played ("short", ...), "" = base
  std::string weapon;       // item name, natural weapon name, or "?" (no edge)
  std::string weaponClass;  // ItemDef::weaponClass, or "natural"
  bool natural = false;
  float strikeCut = 0, strikeBlunt = 0, strikeBite = 0;  // what it swings with
  distant::StrikeKind kind = distant::StrikeKind::Cut;
  float distance = 0;       // centre to centre, world voxels
  int windupTicks = 0, cutTicks = 0;   // this swing's, after the tempo jitter
  distant::GuardClass guard = distant::GuardClass::None;  // the TARGET's
  bool targetArmoured = false;   // any live worn shell over its body limbs
  distant::LimbRole drawnRole = distant::LimbRole::Other;  // the style's draw
  bool targetAlive = true;
  // ---- THE OUTCOME (the sweeps) ---------------------------------------------
  Outcome outcome = Outcome::Open;
  EndReason end = EndReason::Open;
  uint32_t impactTick = 0;  // first sweep that met anything or was arrested
  uint32_t endTick = 0;
  int sweeps = 0, contacts = 0;
  bool arrested = false, arrestedByShield = false;
  // The limb that lost the most life (or whose shell took the blow, when no
  // life was lost), its role, and the hardest shell worn over it.
  int limb = -1;
  distant::LimbRole role = distant::LimbRole::Other;
  float shellHardness = 0;
  float lifeHp = 0;         // life hp the target's body limbs lost
  float shellHp = 0;        // hp its worn shells lost
  float guardHp = 0;        // hp its held items lost (a parry's wear)
  float bleedOwed = 0;      // blood opened, in hp (bleed budget x bleedHpPerVoxel)
  float hpByCause[(int)DamageCause::Count] = {};
  float power = 0;          // best contact sweep: speed ramp x edge alignment
  float tipSpeed = 0;       // world voxels/s, fastest sweep
  bool killed = false;      // the target died inside this stroke's sweeps
  // D2b: base limbs of the target that came OFF inside this stroke's sweeps
  // (a body before, none after), and the roles they had.
  int severed = 0;
  int severedByRole[distant::kRoles] = {};
  uint64_t severedMask = 0;   // bit i: base limb i came off (i < 64)
  bool metOther = false;    // a sweep touched something that was not the target
  float StrikeTotal() const { return strikeCut + strikeBlunt + strikeBite; }
};

class Sink {
 public:
  // The library a held weapon's item def is read from (null: the system's).
  explicit Sink(const ItemLibrary* items = nullptr) : items_(items) {}

  // ---- the hooks (mob.cpp; each one call behind `if (Sink* s = Active())`) --
  void OnStart(MobSystem& sys, const Mob& attacker, uint32_t tick);
  void BeforeSweep(MobSystem& sys, const Mob& attacker);
  void OnSweep(const Mob& attacker, const EdgeSweepResult& r);
  void AfterSweep(MobSystem& sys, const Mob& attacker, uint32_t tick);
  void OnEnd(const Mob& attacker, uint32_t tick, EndReason why);

  // Close everything still open (a gate's end of fight).
  void Flush(uint32_t tick);
  void Clear();
  const std::vector<Stroke>& Strokes() const { return strokes_; }
  size_t OpenCount() const { return open_.size(); }

 private:
  struct Pending {   // one stroke in flight, parallel to strokes_[index]
    uint64_t attacker = 0;
    size_t index = 0;
    std::vector<float> lifeByLimb;    // accumulated life hp per body limb
    std::vector<float> shellByHost;   // accumulated shell hp per host limb
    std::vector<float> hardness;      // shell hardness per body limb at start
    std::vector<distant::LimbRole> roles;  // the target's limb roles at start
  };
  struct Snap {
    bool valid = false;
    uint64_t victim = 0;
    bool alive = false;
    std::vector<float> limbHp;        // body limbs
    std::vector<uint8_t> attached;    // ...that had a body (D2b: severs)
    std::vector<std::pair<int, float>> shells;   // (host, hp) per worn slot
    float held = 0;
    float lostBy[(int)DamageCause::Count] = {};
    float owed = 0;
    // this sweep's results (OnSweep)
    bool contact = false, arrested = false, shield = false;
    float power = 0, tipSpeed = 0;
    int sweeps = 0;
  };
  Pending* Find(uint64_t attacker);
  void Close(Pending& p, uint32_t tick, EndReason why);

  const ItemLibrary* items_ = nullptr;
  std::vector<Stroke> strokes_;
  std::vector<Pending> open_;
  Snap snap_;
};

// THE ONE SWITCH. Null = off.
extern Sink* g_active;
inline Sink* Active() { return g_active; }
// Install `s` for a scope, restoring whatever was there before.
struct Scope {
  Sink* prev;
  explicit Scope(Sink* s) : prev(g_active) { g_active = s; }
  ~Scope() { g_active = prev; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// ---- THE REDUCER: the log -> the distant tier's tables -----------------------
//
// What it fills (the rest of a DistantCalib -- bleed decay, burn, ranged,
// matchups -- the gate measures with its own probes):
//   styles[*]      n / hits / blocked, hitRate = hits / (n - blocked),
//                  roleWeights over hits, meanPower + powerCv over hits on
//                  UNARMOURED limbs (life hp / strike total), impactTicks, and
//                  byWeapon rows for every weapon with >= minSamples strokes;
//   fallback "*"   the same, pooled over every style;
//   blockChance    per the TARGET's guard class: arrested / started;
//   armour         exposure per kind from the hits on covered limbs (the
//                  shortfall against the style's unarmoured mean, less what
//                  ShellResponseOf already passes);
//   bleed.perHp    blood owed per hp per kind, x `drainShare` (the share of an
//                  owed budget that actually drains, measured by the gate).
// Strokes whose target was already dead, that never had a target, or that are
// still open are not counted. `report` gets one line per style.
struct ReduceStats {
  int strokes = 0, counted = 0, hits = 0, blocked = 0, misses = 0, other = 0;
};
void ReduceStrokes(const std::vector<Stroke>& log, int minSamples,
                   float drainShare, const Tuning::Gear& gear,
                   distant::DistantCalib& out, ReduceStats& stats,
                   std::string& report);

}  // namespace strokelog
