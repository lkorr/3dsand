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
// DETERMINISM: CPU gameplay state in the 30 Hz tick (TickAuthority, right after
// phase H's mobs.PreTick that issued the requests), integer spell VM, draws by
// rng::Hash3 on (mob id, tick), voxels read only through the T-4 snapshot
// (WorldSpellProbe), requests served in the order MobSystem issued them.
//
// NOT SAVED: the casters (mana, statuses, bolts in flight). A loaded world's
// creatures start with full pools and nothing sustained.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "game/ai_behavior.h"
#include "game/spell.h"
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

struct MobCaster {
  uint64_t mobId = 0;
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

// Every creature's flight carriers, for the renderer (they are drawn like the
// player's; main.cpp's projectile loop).
void MobCastAppendLive(const TickAuthorityCtx& w, std::vector<const SpellProjectile*>& out);
