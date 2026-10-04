// demon_talk.h — TALKING TO A CONTAINED DEMON, BINDING IT, AND WHAT A BOUND
// DEMON DOES (docs/PLAN_demons.md D5; DESIGN.md §17 "Demons -- conversation,
// contracts, upkeep").
//
// THE CONVERSATION. A demon's def names a dialogue file (`dialogue`, assets/
// dialogue/); T (TB_DEMON_TALK) opens it with the presser's most recent
// CONTAINED demon within kTalkRangeM. Only while contained: a demon that is
// released, unbound or gone ends the conversation that tick. While it runs the
// camera is locked on the demon (main.cpp) and the gaze is the command's: L
// held (TB_DEMON_LOOKAWAY) looks away, otherwise the summoner is looking at
// it -- the tick writes TickInput::lookFwd from that bit before D3's gaze test
// reads it, so the strain a conversation costs is a function of the command.
// The node text may say `{tell}` (D3's tell for the current margin), `{name}`,
// `{margin}`, `{weight}`, `{contract}`.
//
// THREE DIALOGUE ACTIONS (dialogue.h): `present_contract` opens the picker of
// the player's contract pages (stock pages included); `release` lets the
// demon out through D3's demon::Release with the bound contract's weight;
// `dismiss` sends it home.
//
// PRESENT (TB_DEMON_PRESENT + TickInput::contractHash + demonRef). The page is
// resolved by name hash against the presser's own pages, then the stock ones,
// compiled and weighed (contract_tariff.json). BOUND iff the demon is
// contained, `strength >= power + weight` (D3's strength), and the summoner's
// effective mana max can carry the UPKEEP (contract::UpkeepFor(power)). Else it
// refuses and the conversation reopens at its `refused` entry with a tell for
// the (negative) margin. A bound demon is still contained until released.
//
// UPKEEP. Every bound demon reserves its upkeep out of its summoner's mana max
// (CasterState::reserved, beside the spell reservations, session.cpp) for as
// long as it is bound -- contained or serving. That is what limits how many
// you hold.
//
// SERVING (state Released with a pact). The def's `released` profile (a
// neutral one) is the floor; the contract is layered on it every tick, before
// the mobs think:
//   * DUTIES, first whose `when` holds and that can act: follow / goto /
//     fetch / spin write Brain::routine (the resident layer's seam, verb Goto);
//     guard writes the brain's target. No duty: the routine is cleared and the
//     demon stands.
//   * UNDER A CONTRACT THE TARGET IS THE DUTIES'. A bound demon in D5 has no
//     motive of its own (D6 adds the motive ladder): its target is set by an
//     active guard duty or cleared, and the fence refuses any blow whose
//     target is not that duty's (counted `blowsUnsanctioned`) or is in a
//     FORBID attack set (`blowsForbidden`). Prohibitions are HARD filters.
//   * PENALTIES fire their consequence: dismiss (it goes home), destroy (it
//     is unmade), pain (it is hurt). No weighing (D6).
//   * TERM: `hours` of game time from the binding; at its end the demon
//     DEPARTS (the mob leaves the world) and the reservation is freed. D6 adds
//     the higher tiers' one free act.
//   * DISMISS (TB_DEMON_DISMISS, Shift+Y, or the dialogue act) from anywhere:
//     the demon departs, bound or contained.
//
// CAST FILTER (D4's hook): demon::AllowCastAt(w, demonId, targetId) answers
// false when a bound demon's contract forbids `cast` at that actor. D4's
// casting path must ask it before a demon's cast is emitted.
//
// BOUNDED: actors scanned are the players and the live mobs (<= kMaxMobs);
// every selector evaluation is charged against kEvalBudget node visits per
// demon per tick; fetch scans at most (2 x kFetchScanR + 1)^2 x 9 cells, at
// most once every kFetchRescanTicks.
//
// SAVED ('DMNS' world section, 'CNTR' player section): every BOUND demon (its
// def, summoner, state, mob def and origin, the contract page, the term left,
// its counters) and each player's contract pages. A loaded bound demon is
// matched to its creature (MOBS restores it under a fresh id) by def name and
// nearest origin within kRestoreMatchVox, over the first kRestoreTicks ticks.
// Contained-but-unbound demons are NOT saved (as D1): they load loose.
//
// DETERMINISM: CPU gameplay state in the tick; voxels only through the T-4
// snapshot (fetch); no float in any decision a replay could see differently
// (distances are float but computed from the same deterministic positions the
// AI uses); stable orders everywhere.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "game/contract.h"
#include "game/demon_circle.h"
#include "math3d.h"

struct TickAuthorityCtx;
struct SessionTick;
struct OpBatch;
struct PlayerSession;
struct EntityIO;
struct LiveDemon;
struct DemonWorld;
struct UIState;

namespace demon {

constexpr float kTalkRangeM = 8.0f;
constexpr int32_t kEvalBudget = 20000;
constexpr int32_t kFetchScanR = 16;           // cells
constexpr uint32_t kFetchRescanTicks = 30;
constexpr float kRestoreMatchVox = 24.0f;
constexpr uint32_t kRestoreTicks = 300;
constexpr uint32_t kStrikeMemoryTicks = 300;  // a player's strike names its target this long

enum class PresentResult : uint8_t { None = 0, Bound, Refused, CannotHold, NoContract, NotContained };
const char* PresentResultName(PresentResult r);

// ---- a bound demon's contract (LiveDemon::pact) ------------------------------------

struct Pact {
  contract::Page page;     // a COPY, compiled: editing the page later changes nothing
  int32_t weight = 0;
  int32_t upkeep = 0;
  uint32_t bindTick = 0;
  uint32_t expireTick = 0;      // 0 = indefinite
  int32_t restoreLeft = -1;     // a loaded pact: ticks of term left, applied on the first tick
  std::array<int32_t, contract::kMaxCounters> counters{};
  Vec3 anchor{};                // where it was presented ("here")
  // ---- this tick (written by ContractTick, read by the fence) ----
  int activeDuty = -1;          // clause index
  uint64_t guardTarget = 0;     // the actor a guard duty names this tick, 0 = none
  // ---- fetch ----
  uint8_t fetchPhase = 0;       // 0 seek, 1 return
  bool fetchHave = false;
  IVec3 fetchCell{};
  uint32_t fetchScanTick = 0;
  uint32_t fetchBlockedUntil = 0;
  int32_t fetchTaken = 0;       // cells the last scoop took
  bool gotoLatched = false;     // a goto's arrival fired this trip
  // ---- penalties found by the fence, applied next tick ----
  int pendingPenalty = -1;
  // ---- telemetry (the gate, the HUD) ----
  uint32_t blowsAllowed = 0, blowsForbidden = 0, blowsUnsanctioned = 0;
  uint32_t blowsAtSummoner = 0;   // ASKED at the summoner (every one refused)
  uint32_t penaltiesFired = 0, dutiesDone = 0, fetches = 0;
  uint32_t budgetExhausted = 0;
  std::vector<std::pair<uint64_t, uint32_t>> blowsAt;   // target -> blows let through
};

struct PresentInfo {
  PresentResult result = PresentResult::None;
  int32_t strength = 0, power = 0, weight = 0, upkeep = 0, margin = 0;
  std::string page;
};

// ---- the per-world state (DemonWorld::talk) -----------------------------------------

struct Restore {
  std::string demon, name, mobDef;
  int session = 0;
  uint8_t state = 0;          // DemonState
  Vec3 origin{};
  CircleShape circle;         // a bound demon still in its circle keeps it
  Pact pact;
  uint32_t firstTick = 0;     // 0 = not yet seen by a tick
};

struct ActorView {
  uint64_t id = 0;
  Vec3 centre{};
  uint32_t faction = 0;
  bool hostile = false;
  bool player = false;
  uint64_t targetId = 0;
  std::string type;
};

struct TalkWorld {
  std::vector<Restore> restores;
  struct Strike {
    int session = 0;
    uint64_t mob = 0;
    uint32_t tick = 0;
  };
  std::vector<Strike> strikes;   // each player's latest strike target
  std::vector<ActorView> actors; // this tick's view (ContractTick), read by the fence
  uint32_t actorsTick = 0;
  // the last PRESENT per session, for the panel and the gate
  std::vector<std::pair<int, PresentInfo>> lastPresent;
  struct Stats {
    uint64_t talks = 0, presented = 0, bound = 0, refused = 0, released = 0;
    uint64_t dismissed = 0, expired = 0, destroyed = 0, restored = 0, restoreLost = 0;
  } stats;
};

// ---- the tick --------------------------------------------------------------------------

// BEFORE dialogue::TickSession (which zeroes a talking player's command): the
// demon bits of each command -- a strike's target (attacked_by), the talk key,
// a presented contract, a dismissal, and while talking to a demon the gaze
// (lookFwd from TB_DEMON_LOOKAWAY).
void TalkPreTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick);

// AFTER DemonTick, before the mobs think: the dialogue's demon actions, the
// conversation's guard (contained only), restores, expiry, duties, penalties,
// fetch (its scoop ops into `out`), the panel's mirror.
void ContractTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
                  OpBatch& out);

// Mana the bound demons of `session` reserve (session.cpp adds it to the
// spells' reservation).
int32_t UpkeepFor(const TickAuthorityCtx& w, int session);

// The contract weight a release of this demon carries (0 = no contract).
int32_t ContractWeight(const LiveDemon& ld);

// The fence's question for a demon with a pact (demon.cpp FenceAllow): may it
// strike its brain's current target? Counts, and finds attack penalties.
bool PactAllowBlow(DemonWorld& d, LiveDemon& ld, uint64_t targetId);

// THE CAST-FILTER HOOK FOR D4: may demon `demonId` cast at actor `targetId`
// (a mob id or ai::kPlayerActorBase + session)? False iff it is bound and a
// forbid `cast` clause's set holds that actor. Not a demon / no pact: true.
bool AllowCastAt(TickAuthorityCtx& w, uint64_t demonId, uint64_t targetId);

// Bind attempt (what TB_DEMON_PRESENT runs; public for the gate).
PresentInfo Present(TickAuthorityCtx& w, std::span<SessionTick> players, int session,
                    uint64_t demonId, const contract::Page& page, uint32_t tick);

// Send a demon home (dismiss / expiry / a penalty): the mob leaves the world.
void Depart(TickAuthorityCtx& w, LiveDemon& ld, uint32_t tick, const char* why);

// The pact on a live demon, or null.
Pact* PactOf(TickAuthorityCtx& w, uint64_t demonId);

// The pages a session may present: its own, then the stock ones.
std::vector<const contract::Page*> PresentablePages(const PlayerSession& s);

// ---- presentation ------------------------------------------------------------------

// Fill the node text's {tell} {name} {margin} {weight} {contract} for a
// conversation with demon `mobId` (main.cpp, after dialogue::MakeView).
std::string TalkText(const TickAuthorityCtx& w, uint64_t mobId, const std::string& text,
                     uint32_t salt);
// Is this mob a live demon (the conversation is a demon's)?
bool IsDemon(const TickAuthorityCtx& w, uint64_t mobId);

// ---- persistence -------------------------------------------------------------------

// Append 'DMNS' (world) and 'CNTR' (player `s`) to an EntityIO. The sections
// capture `w` and `s` by reference.
void AppendSaveSections(EntityIO& io, TickAuthorityCtx& w, PlayerSession& s);

}  // namespace demon
