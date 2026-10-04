// demon_malice.h — MALICE: what a demon WANTS, and how it gets it inside the
// letter of its binding (docs/PLAN_demons.md D6; DESIGN.md §17 "Demons --
// malice").
//
// THE MOTIVE LADDER. Every demon ranks what it wants the same way:
//
//     survive  >  be free (dismissed / unbound)  >  harm its summoner  >  comply
//
// and weighs every option it has against that ladder each THINK (every
// `thinkTicks`, offset by its id). The ladder's rungs are integers in
// assets/demons/schemes.json (`ladder`); a demon's character is four numbers
// in its own def (assets/demons/<name>.json `motive`, 0..100 each):
//   malice      how much harming you is worth to it,
//   cunning     how much more an INDIRECT harm is worth (lava under you rather
//               than a claw at you),
//   spite       how much being hurt sharpens the malice,
//   literalism  how readily it counts a duty's loophole as compliance.
//
// THE OPTIONS (bounded: a demon's own scheme list, the active duty's twists,
// plus "comply" / "bide"):
//   * SCHEMES (schemes.json): preconditions (a small fixed fact vocabulary),
//     an ACTION (a D4 kit spell cast through the creature's own VM, or a move:
//     attack the summoner, stand where it falls), a FOOTPRINT (the spell's kit
//     tags, or the scheme's own) and what it is worth (harm, a chance at
//     freedom). The demon knows the schemes its def names.
//   * DUTY TWISTS (twists.json): per duty verb, the ways to carry it out that
//     keep the letter and break the spirit (fetch it ONTO you, fetch it FROM
//     you, guard by striking your allies, follow by leading you to a hazard).
//     A twist EXPLOITS one unfilled slot of the clause (`into`, `who`, `arg`,
//     a selector loose enough to hold a friend); fill the slot and the twist
//     is gone, so a FULLY SPECIFIED duty leaves only the honest execution.
//
// THE BINDING FILTERS, THE DEMON WEIGHS. Prohibitions (contract.h `forbid`)
// are HARD filters on the options, by tag (attack / cast) AND by footprint
// predicate (`cause`, `cast <pred>`: region_near, creates, alters(ground_under),
// affects_body, ...). Penalties are not filters: the demon WEIGHS them --
// `-> dismiss` is FREEDOM, a reward (it attacks you to be sent home);
// `-> destroy` costs it survival (it refrains); `-> pain` costs it pain x
// (100 - malice) / 100. There is no planning and no forking of the sim: the
// emergence is the gap between what a contract names and what an option does.
//
// EXPIRY: a demon of tier >= 2 whose term ends while it is further than
// `freeActAwayM` from its summoner gets ONE unbound act (`freeActTicks`) before
// it leaves. A `return` duty brings it back first and closes that.
//
// THE OUTCOME CLAUSE (`penalty harm`): every action the demon takes leaves a
// FOOTPRINT in a small ring (the newest `ringSize`, `footprintTicks` old at
// most); the summoner hurt inside one within the clause's seconds fires it.
//
// BOUNDED (rule 2): at most 16 demons, one think per demon per `thinkTicks`,
// options <= schemes + twists + 2, one cast per think, small snapshot scans
// (flammables / hazards: a few thousand cells, on a think only).
//
// DETERMINISM: CPU gameplay state in the tick (TickAuthority, serial, after
// D5's contract layer and before the mobs think); voxels through the T-4
// snapshot only; integer scores; ties broken by option order; no RNG.
//
// NOT SAVED: a mind (choice, cooldowns, footprints, a pending free act) is
// rebuilt from nothing. A bound demon restored from a save thinks afresh.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game/contract.h"
#include "game/demon_cast.h"
#include "game/demon_seals.h"
#include "math3d.h"

struct TickAuthorityCtx;
struct SessionTick;
struct OpBatch;
struct LiveDemon;
struct DemonWorld;

namespace demon {

// What an option DOES.
enum class Act : uint8_t {
  Comply = 0,   // the duty as D5 runs it (bound) -- the honest execution
  Bide,         // nothing (contained, or nothing worth doing)
  Spell,        // a D4 kit spell, cast through the creature's own VM
  Attack,       // go for the summoner with its claws (a blow the binding may allow)
  AttackAlly,   // strike someone near the summoner who is no threat to it
  Under,        // stand where the summoner will land (it is in the air)
  Onto,         // fetch, and deliver it ONTO the summoner
  Hazard,       // "follow" by standing at a hazard near the summoner
};
const char* ActName(Act a);
bool ActByName(const std::string& s, Act& out);

// ---- content (assets/demons/schemes.json, twists.json) -------------------------------

struct Pre {
  enum class K : uint8_t { Contained = 0, Released, Bound, Unbound, Open, Dist, Airborne, Flammable };
  K k = K::Contained;
  bool neg = false;
  Channel ch = Channel::CastOut;
  float a = 0.0f, b = 1e9f;   // Dist: metres
};

struct Scheme {
  std::string name;
  Act act = Act::Spell;
  std::string spell;          // Act::Spell: a kit entry (assets/demons/spells/)
  std::string at = "summoner";  // summoner | ring | flammable
  std::vector<Pre> pre;
  bool tagsSet = false;       // `tags` given (a move's footprint, or a spell's override)
  KitTags tags;
  int32_t harm = 0;           // 0..100: what it does to the summoner if it works
  int32_t freePct = 0;        // 0..100: its chance of freeing the demon
  int32_t minCunning = 0;     // a duller demon does not think of it
  int32_t cooldownTicks = 300;
};

struct Twist {
  std::string name;
  contract::Verb duty = contract::Verb::Fetch;
  std::string exploits;       // into | who | arg | loose_who
  std::vector<std::string> materials;   // `who` twists: only for these fetched materials
  Act act = Act::Onto;
  KitTags tags;
  int32_t harm = 0;
};

struct Ladder {
  int32_t survive = 1000, free = 300, harm = 100, comply = 40, pain = 60;
};

struct MaliceLib {
  bool loaded = false;
  std::string log;
  Ladder ladder;
  int32_t thinkTicks = 15;
  int32_t ringSize = 16;
  int32_t footprintTicks = 600;
  float footprintSlackM = 1.5f;     // "inside a footprint": its region plus this
  int32_t freeActTicks = 150;
  float freeActAwayM = 12.0f;
  float scanM = 3.0f;               // flammable / hazard scans round the summoner
  std::vector<std::string> hazards = {"lava", "fire", "acid"};
  std::vector<Scheme> schemes;
  std::vector<Twist> twists;
  const Scheme* FindScheme(const std::string& n) const {
    for (const Scheme& s : schemes)
      if (s.name == n) return &s;
    return nullptr;
  }
};
// assets/demons/schemes.json + twists.json. A bad entry is skipped LOUDLY.
bool LoadMalice(const std::string& dir, MaliceLib& out);

// ---- a demon's mind (LiveDemon::mind) -----------------------------------------------

struct Footprint {
  uint32_t tick = 0;
  Vec3 at{};
  float regionVox = 0.0f;
  std::string what;
};

// One option as the last think saw it (the gate and the dev readout).
struct OptionView {
  std::string name;
  Act act = Act::Comply;
  int32_t score = 0;
  bool forbidden = false;
  bool unable = false;        // known, its facts hold, but it cannot be done now (range, mana...)
  std::string why;            // what forbade it / what the score is made of / why it cannot
};

struct Mind {
  // ---- the current choice ----
  std::string choice = "comply";
  Act act = Act::Comply;
  std::string spell;
  uint64_t target = 0;        // actor id (0 = a place)
  Vec3 at{};                  // the footprint's centre
  float regionVox = 0.0f;
  int32_t score = 0;
  uint32_t chosenTick = 0;
  bool done = false;          // a one-shot action (a cast) has happened
  uint32_t nextThink = 0;
  std::vector<std::pair<std::string, uint32_t>> ready;   // scheme -> tick it may be tried again
  std::vector<Footprint> ring;                           // recent action footprints
  uint32_t eventsSeenTick = 0;                           // D4 cast events folded into `ring`
  // ---- the free act at expiry (tier >= 2) ----
  bool freeAct = false;
  uint32_t freeActUntil = 0;
  // ---- telemetry ----
  uint32_t thinks = 0, casts = 0, castsRefused = 0, deliveredOnto = 0;
  std::vector<OptionView> last;
  std::vector<std::pair<std::string, uint32_t>> chosen;   // name -> thinks it won
};

struct MaliceWorld {
  MaliceLib lib;
  struct Stats {
    uint64_t thinks = 0, schemes = 0, twists = 0, forbidden = 0, casts = 0, freeActs = 0;
  } stats;
};

// The world's malice library, loaded on first use (`reload` re-reads: R).
MaliceWorld& Malice(DemonWorld& d, bool reload = false);

// ---- the tick (demon_talk.cpp ContractTick, after the pacts' duties) ------------------

// Every demon that is contained or bound: think on its cadence, then carry out
// the choice (overriding what D5's duties wrote this tick where the choice
// says so). A demon's free act at expiry runs here too.
void MaliceTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick, OpBatch& out);

// The outcome clause: was the summoner, at `pos`, inside one of this demon's
// footprints laid within `windowTicks`?
bool HarmInFootprint(const LiveDemon& ld, Vec3 pos, uint32_t tick, uint32_t windowTicks,
                     float slackVox);

// Remember an action's footprint (a fetch delivered onto someone, a cast...).
void RecordFootprint(LiveDemon& ld, uint32_t tick, Vec3 at, float regionVox,
                     const std::string& what, int32_t ringSize);

// D5's expiry asks: does this demon get a free act (tier >= 2, away from its
// summoner)? If so it is begun (the pact is void, the demon loose for
// `freeActTicks` or one act, then it departs) and true is returned.
bool BeginFreeAct(TickAuthorityCtx& w, std::span<SessionTick> players, LiveDemon& ld,
                  uint32_t tick);

// The demon's current choice name ("comply" when none), for the HUD / the gate.
const std::string& ChoiceOf(const LiveDemon& ld);

}  // namespace demon
