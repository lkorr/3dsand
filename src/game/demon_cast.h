// demon_cast.h — CREATURES THAT CAST: the mob casting seam, blink, and the
// demon spell kit (docs/PLAN_demons.md D4; DESIGN.md §17 "Demons that cast").
//
// THE SEAM IS NOT DEMON-SPECIFIC. Any creature whose behaviour profile has a
// `cast` block (ai_behavior.h CastTuning) may pick the `cast` intent; the AI
// emits a CastRequest exactly as it emits an AttackRequest, and this file is
// its consumer:
//
//   * one SpellSystem (the player-agnostic VM, game/spell.h) and one
//     CasterState (mana) PER CASTING CREATURE, beside each player's. One VM
//     per caster is what makes every per-caster number honest -- bills, the
//     wildcard's bill on resolve, statuses, refusals -- with no attribution
//     code: the VM already keeps them per system;
//   * the spell is DRAWN from the profile's list of KIT names (a repeat weighs
//     the draw), filtered to what this creature can do right now: in range,
//     affordable from mana alone (a creature never overcasts into its own
//     body), under its in-flight cap, off cooldown;
//   * it is CAST through the same SpellSystem::Cast the player's hand uses,
//     from the creature's chest along the line to its target (or its feet, or
//     itself), and its emission is OWNED AND FILTERED like the player's: every
//     ward in every VM (players' and creatures') filters it, its ops go into
//     the tick's streams (rule 3), its blasts go off in the primary's
//     explosion slot (the grenade path: island checks, body damage, carving),
//     its winds join the world's wind list, a lift on a body is that body's
//     AddBodyVelocity, a bomb is debris;
//   * BUDGETS (rule 2): at most kMaxCasters VMs, kCastsPerTick requests served
//     a tick, kOpsPerTick brush ops a tick across ALL creature magic, and each
//     creature at most CastTuning::maxLive carriers in the air.
//
// THE KIT (assets/demons/spells/<name>.json) is data: a spell is a WORD LIST in
// the ordinary grammar (glyphs.json, so the spell costs what the same words
// cost you) plus how a creature AIMS it (body | feet | self), its range, an
// optional `releaseAfterTicks` (a status it put on ANOTHER body is dropped that
// many ticks after it attached: lift, then drop) and its FOOTPRINT TAGS
// (targets / direct / creates / alters / affectsBody / region), which D5's
// prohibitions and D6's schemes filter on. `"kind": "blink"` is not a spell
// but a locomotion verb (below). Re-read when a creature starts casting.
//
// BLINK: a short teleport (MobSystem::BlinkMob) toward or away from the
// target, onto a floor found in the T-4 snapshot, line of sight optional per
// entry, a cooldown. REFUSED when the destination has no floor, when line of
// sight is asked for and blocked, and -- for a CONTAINED demon -- when D3's
// quicksilver severs its `blink` channel (demon::AllowBlink). An UNSEVERED
// blink is a loophole by design: a contained demon that blinks OUT of its
// circle is unbound by AllowBlink on the spot (it has left the circle). The
// walk fence (D1) still holds every step; only the teleport skips it.
//
// THE SEALS (D3, game/demon_seals.h) are asked at exactly three places:
// demon::AllowBlink before a hop lands, demon::AllowCastOut when a contained
// demon's carrier crosses its ring, and demon::ChannelSevered(CastOut) for
// what an instant cast or a resolve would put outside it (its ops, blasts,
// sprays, the far end of a wind, a push on a body out there).
//
// D5 HOOK: a bound demon's contract (demon::AllowCastAt, src/game/
// demon_talk.h, not on main when this landed) is asked in MobCastServe just
// before Cast(), at the line marked `// D5 hook`.
//
// DETERMINISM: CPU gameplay state in the 30 Hz tick (TickAuthority, right after
// phase H's mobs.PreTick that issued the requests), integer spell VM, draws by
// rng::Hash3 on (mob id, tick), voxels read only through the T-4 snapshot
// (WorldSpellProbe), requests served in the order MobSystem issued them.
//
// NOT SAVED: the casters (mana, statuses, bolts in flight). A loaded world's
// creatures start with full pools and nothing sustained.
//
// A CREATURE CASTS FROM ITS HAND, THE WAY THE PLAYER DOES (2026-10-08,
// game/spellhand.h). The AI's request and a demon's malice do not cast on the
// spot: MobCastBegin draws the spell, puts it INTO a hand and starts the
// delivery's gesture, and the spell leaves that hand on the gesture's release
// tick (MobHandCast). The hand: an empty usable one if the creature has one;
// else the off hand's item (or the main hand's, if the off arm is ruined) is
// PUT AWAY into its kit -- the sheath for a blade, else the bag -- for the
// cast, and drawn back into the hand after it, coat and all (CaptureHeld /
// EquipItemAsWas). While the spell is in the hand the creature is
// Mob::CastBusy: no swing, no guard, the arm is the gesture's (Mob::SetCastArm).
// A spell drawn while the last one is still in the hand casts straight from
// it. A creature with no hand to cast from (no socket, both arms ruined), or
// a spell with no gesture, casts from the chest on the spot as it used to.
// MobCastServe stays the synchronous path for scripted casts (the gates).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game/ai_behavior.h"
#include "game/equipment.h"
#include "game/spell.h"
#include "game/spellhand.h"
#include "math3d.h"

struct TickAuthorityCtx;
struct SessionTick;
struct OpBatch;

// ---- the kit (content) ----------------------------------------------------------

// What a spell DOES to the world, as tags a prohibition can name (D5) and a
// scheme can be chosen by (D6). Authored per kit entry; not derived from the
// words, because "this is a fireball AT YOU" is intent, not chemistry.
struct KitTags {
  std::string targets;              // "body" | "ground" | "self" | "area"
  bool direct = false;              // aimed AT a body (vs. the place near it)
  std::vector<std::string> creates; // materials it puts into the world ("fire", "lava")
  std::string alters;               // "" | "ground_under_target" | ...
  bool affectsBody = false;         // moves / burns / binds a body it lands on
  int32_t region = 0;               // footprint radius, world voxels
};

enum class KitAim : uint8_t { Body = 0, Feet, Self };
const char* KitAimName(KitAim a);

struct KitSpell {
  std::string name;                 // the file stem: what a profile's `cast.spells` names
  bool blink = false;               // `"kind": "blink"`: a locomotion verb, not a spell
  std::string words;                // the spell, in the grammar ("fire fire bolt")
  KitAim aim = KitAim::Body;
  float rangeMin = 0.0f, rangeMax = 48.0f;   // centre-to-centre, world voxels
  bool lead = true;                 // aim where a moving target WILL be
  int32_t releaseAfterTicks = 0;    // drop its statuses on OTHER bodies after this
  bool once = false;                // refused while the caster sustains one on itself
  KitTags tags;
  // blink
  float distance = 20.0f;           // world voxels per hop
  float standoff = 6.0f;            // stop this short of the target (toward)
  bool toward = true;               // toward the target (false: away from it)
  bool los = true;                  // the hop needs a clear line
  int32_t cooldownTicks = 150;
};

struct CreatureKit {
  std::vector<KitSpell> spells;
  const KitSpell* Find(const std::string& n) const {
    for (const KitSpell& s : spells)
      if (s.name == n) return &s;
    return nullptr;
  }
};
// assets/demons/spells/*.json. A bad entry is skipped LOUDLY into `log`.
bool LoadCreatureKit(const std::string& dir, CreatureKit& out, std::string& log);

// ---- the world's casters ----------------------------------------------------------

enum class MobCastOutcome : uint8_t {
  Cast = 0,        // the spell left the creature
  Blinked,         // it moved
  NothingInRange,  // no kit spell on its list fits this distance / its mana / its cap
  BlinkNoFloor,    // no floor (or no headroom, or unseen) where the hop would land
  BlinkNoSight,    // the hop's line is blocked and the entry wants sight
  BlinkSevered,    // D3: contained, and the `blink` channel is severed
  NoRoom,          // kMaxCasters VMs already, or kCastsPerTick served this tick
  Forbidden,       // D5: a bound demon's contract forbids this cast at this target
  // Accepted: the spell is in the creature's hand and the gesture is running;
  // it leaves on the release (MobCastBegin). Appended, so nothing renumbers.
  Gesturing,
  Busy,            // a cast is already in the hand, mid-gesture
};
const char* CastOutcomeName(MobCastOutcome o);

// One served request, kept (bounded ring) for the gate and the dev readout.
struct CastEvent {
  uint32_t tick = 0;
  uint64_t mobId = 0;
  std::string spell;
  MobCastOutcome outcome = MobCastOutcome::Cast;
  Vec3 from{}, to{};      // cast origin and aim point (blink: old / new centre)
  int32_t cost = 0;
};

// THE CAST IN A CREATURE'S HAND (MobCastBegin / MobCastTick). One at a time.
struct MobHandCast {
  enum class Phase : uint8_t {
    Idle = 0,
    Raise,     // the spell is in the hand (what it held put away), the arm comes up armed
    Gesture,   // the delivery's gesture; the spell leaves on its release
    Hold,      // the spell lingers in the hand: another draw casts straight from it
    Lower,     // the spell is let go; then what the hand held is drawn back
  };
  static constexpr int kRaiseTicks = 6;   // spell_ready's blend-in (200 ms)
  static constexpr int kHoldTicks = 24;
  static constexpr int kLowerTicks = 6;
  Phase phase = Phase::Idle;
  int hand = 0;              // HandIndex of the casting hand
  int ticks = 0;             // in this phase
  CastGestureRun run;
  CastArmState arm;
  bool clipOn[kHands] = {false, false};
  // The cast waiting for its release: the kit entry the draw chose and the
  // request (its target point is refreshed every tick from the live target).
  std::string spell;
  ai::CastRequest req;
  int gesture = -1;
  uint32_t color = 0;        // the light in the fist (the flight's colour rule)
  Vec3 aimDir{0, 0, 1};      // the line to the target now, world
  // What the hand held before the spell went into it, and where it went.
  bool stowed = false;
  KitRef stowedAt;
  std::string stowedItem;
  uint32_t stows = 0, draws = 0, releases = 0;
};

struct MobCaster {
  uint64_t mobId = 0;
  MobHandCast hand;
  SpellSystem spells;
  CasterState mana;
  uint32_t blinkReadyAt = 0;
  // Statuses on OTHER bodies, released after `releaseAfter` ticks (lift, then
  // drop): status id -> the tick it was first seen attached.
  int32_t releaseAfter = 0;
  std::vector<std::pair<uint32_t, uint32_t>> seen;
  bool gone = false;       // the creature died / left: drain, then forget
  uint32_t casts = 0, blinks = 0, refused = 0;
  // Carriers (SpellProjectile::seq) already ALLOWED out of a contained demon's
  // ring by demon::AllowCastOut: asked once, at the crossing.
  std::vector<uint32_t> passedRing;
};

struct MobCastWorld {
  static constexpr size_t kMaxCasters = 64;   // creatures with a VM (rule 2)
  static constexpr int kCastsPerTick = 8;     // requests served a tick
  static constexpr int kOpsPerTick = 32;      // brush ops a tick, all creature magic
  static constexpr size_t kEventRing = 128;
  CreatureKit kit;
  bool kitLoaded = false;
  std::string kitLog;
  std::vector<MobCaster> casters;
  std::vector<CastEvent> events;   // the newest kEventRing, oldest first
  struct Stats {
    uint64_t requests = 0, cast = 0, blinked = 0, refused = 0;
    uint64_t opsDropped = 0;        // over kOpsPerTick
    uint64_t filtered = 0;          // refused by a ward (any VM's)
    uint64_t ringRefused = 0;       // carriers / ops stopped at a ring (cast_out severed)
    uint64_t absorbed = 0;          // carriers absorbed by another VM's ward
    uint64_t released = 0;          // statuses dropped by releaseAfterTicks
    uint64_t unsupported = 0;       // strikes / summons a creature's spell asked for
    uint64_t forbidden = 0;         // casts a bound demon's contract refused (D5)
    int maxLiveSeen = 0;            // the most carriers one creature had in the air
    int opsThisTick = 0;
  } stats;
  MobCaster* Find(uint64_t mobId) {
    for (MobCaster& c : casters)
      if (c.mobId == mobId) return &c;
    return nullptr;
  }
};

// The world's casters, created on first use.
MobCastWorld& MobCasters(TickAuthorityCtx& w);

// Right after phase H: serve this tick's cast requests (MobSystem::
// CastRequests, cleared here), tick every creature VM, splice the emission into
// `out`. Free when no creature has ever asked.
void MobCastTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick,
                 OpBatch& out);

// After phase M: the creatures' wards at the MutationQueue splice, over the
// whole tick's streams (a demon's `null` refuses YOUR ops as yours refuse its).
void MobCastWardFilter(TickAuthorityCtx& w, OpBatch& out);

// Serve ONE request now through the real path (what MobCastTick does per
// request). Public so a gate can script a cast by name: `spell` empty = draw
// from the profile like the AI's request; else that kit entry (still checked
// for range, mana, cap, cooldown).
MobCastOutcome MobCastServe(TickAuthorityCtx& w, std::span<SessionTick> players,
                          const ai::CastRequest& req, uint32_t tick,
                          const std::string& spell, OpBatch& out);
// THE HAND PATH (what the AI's requests and a demon's malice go through): draw
// the spell as MobCastServe would and put it INTO a hand, stowing what the
// hand held; Gesturing when accepted (the spell leaves on the gesture's
// release, a few ticks on, through MobCastTick), Busy while a cast is already
// mid-gesture, Blinked for a blink (no hand in that), or the draw's refusal.
// Falls back to MobCastServe for a creature with no hand to cast from.
MobCastOutcome MobCastBegin(TickAuthorityCtx& w, std::span<SessionTick> players,
                            const ai::CastRequest& req, uint32_t tick,
                            const std::string& spell, OpBatch& out);
// The cast in a creature's hand, or null (the gates and the renderer read it).
const MobHandCast* MobHandCastOf(const TickAuthorityCtx& w, uint64_t mobId);
// (The light in a casting creature's fist is Mob::SpellLight, set by
// MobCastTick while the spell is in the hand; game/spellhand.h
// AppendSpellHandLights draws it, on this machine and on a peer's.)

// D6 (game/demon_malice.h): the kit entry of that name (the kit is read on
// first use), or null.
const KitSpell* KitSpellNamed(TickAuthorityCtx& w, const std::string& spell);
// D6: could this creature cast kit entry `spell` at `distance` (world voxels)
// RIGHT NOW -- the draw's own filter: the entry's range, mana alone (its pool,
// or the profile's full pool before its first cast), its carrier cap, `once`,
// a blink's cooldown? A creature whose profile has no `cast` block cannot.
// `costOut` = the compiled mana cost (0 for a blink).
bool MobCanCast(TickAuthorityCtx& w, uint64_t mobId, const std::string& spell, float distance,
                uint32_t tick, int32_t* costOut = nullptr);

// Every creature's flight carriers, for the renderer (they are drawn like the
// player's; main.cpp's projectile loop).
void MobCastAppendLive(const TickAuthorityCtx& w, std::vector<const SpellProjectile*>& out);
