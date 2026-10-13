#pragma once
// mob_archery.h -- NPC ARCHERS (docs/PLAN_projectiles.md package E,
// 2026-10-13; DESIGN.md "Creatures that shoot").
//
// THE SPLIT IS THE CASTERS' (game/demon_cast.h). The behaviour layer decides
// WHEN: a profile's `ranged` block (ai_behavior.h RangedTuning) keeps an
// archer at its band and, on its own cadence, emits an ai::ShootRequest from
// the `shoot` intent exactly as a caster emits a CastRequest. This file is
// the request's consumer and owns everything about the bow itself:
//
//   THE DRAW. A request starts a draw on a creature that holds a bow
//   (game/arrows.h HeldBow), has two working hands and an arrow for it in its
//   own kit, is not mid-swing and has no spell in its hand. Its arms play the
//   PLAYER'S `bow_draw` clip (Mob::HandClip: the mirror for a bow in the left
//   fist) and the body is Mob::BowDrawn -- no swing, no guard, the AI holds
//   the facing (ai::SelfView::drawing) and the pose tips the torso with the
//   aim pitch (the clip's pitchFollow, the player's look). The string comes
//   back for the bow's own drawTimeMs x the profile's `draw`, then holds for
//   `holdTicks` + a counter-based jitter.
//
//   THE AIM, refreshed every tick of the draw from the LIVE target (a
//   player's figure, a creature's box -- MobTargetNow, the casters'), raised
//   by `aimRaise` of its height, led where it WILL be (ai::LeadPoint, the
//   casters' lead) at the speed the arrow will leave at, and held high by the
//   drop the flight's own gravity takes (ArrowHoldover). At the loose, an
//   error of up to `spread` radians each axis, drawn by rng::Hash3 on (mob
//   id, tick) and scaled by the rules' Tune::Aim.
//
//   THE LOOSE goes through the SAME ArrowFlights::Launch the player's bow
//   uses, from the bow fist (SpellHandWorld), at BowArrowSpeed of the draw,
//   with the archer's own limbs as the ignore list; one arrow leaves its kit
//   (KitTakeAmmo, the player's). Everything after is package C's: the flight,
//   PierceAlongRay, the embed in a creature or the player's avatar, a peer's
//   body routed to its owner, the ground. A loose the flight cap refuses
//   keeps its arrow.
//
//   OUT OF ARROWS: the bow goes into the bag and a sheathed sidearm, if it
//   has one, comes into that hand (the casters' stow/draw-back rule). With
//   none, the bow stays in the fist. Either way the profile is now the melee
//   creature its movement/attack blocks describe (RangedTuning).
//
// ARMING (ArmArcher): what a spawn path calls after SetMobBehavior -- the dev
// panel's `archer` preset (session.cpp), an `npc` ref's props.weapon
// (world/refs_npc.cpp) and a dungeon spawner's (world/refs_dungeon.cpp). For a
// profile with a `ranged` block: a creature not holding a bow is handed the
// profile's `bow` in its right fist (what that fist held goes to the sheath, a
// sidearm, else the bag; the left fist is emptied into the bag -- it is the
// string hand), and arrows are added to its pack up to `quiver`. Anything
// else: untouched. Called at SPAWN only -- a loaded creature's kit is the
// save's truth and is never restocked.
//
// DETERMINISM: authority-side CPU state, stepped once a tick in
// TickAuthority right after phase H's mobs.PreTick (which issued the
// requests) and MobCastTick; requests served in the order MobSystem issued
// them, draws stepped in the order they began; every draw by rng::Hash3 on
// (mob id, tick). The arrows it looses are ordinary flights, stepped in phase
// K like the player's, so a replay looses the same arrows the same way.
//
// BUDGETS (rule 2): at most kMaxDrawing bows drawn at once, kServedPerTick
// requests served a tick, and the flights' own cap (ArrowFlights::kMaxLive).
// Nothing asked and nothing drawn = nothing runs.
//
// NOT SAVED: a draw in progress (a loaded archer starts with the string
// slack). The arrows in its kit are the kit's, saved with it.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "math3d.h"

struct TickAuthorityCtx;
struct SessionTick;
class MobSystem;
struct ItemLibrary;

// One bow drawn by a creature.
struct MobDraw {
  uint64_t mobId = 0;
  uint64_t targetId = 0;
  int hand = 0;                // HandIndex of the bow hand
  int ticks = 0;               // the string has been coming back this long
  int drawTo = 0;              // ticks to the draw the profile asks for
  int looseAt = 0;             // drawTo + this shot's hold
  uint32_t startTick = 0;
  Vec3 targetPoint{};          // the target's centre: refreshed while it lives
  Vec3 targetVel{};            // voxels/sec (the brain's low-passed estimate)
  float targetHeight = 0.0f;   // how tall it stands (aimRaise is a share of it)
  std::string bow;             // the item drawn, and its clip on this rig
  std::string clip;
};

// What happened, for the `archer` gate and the dev readout. Bounded ring.
struct ArcheryEvent {
  enum class Kind : uint8_t {
    Draw,      // a request started a draw
    Loose,     // an arrow left: from / dir / speed / seed
    Abort,     // a draw ended with nothing loosed (`why`)
    Refused,   // a request that did not start a draw (`why`)
    Sidearm,   // out of arrows: the bow put away, `item` drawn
  };
  Kind kind = Kind::Draw;
  uint32_t tick = 0;
  uint64_t mobId = 0;
  uint64_t targetId = 0;
  const char* why = "";        // a static string
  Vec3 from{}, dir{};
  float speed = 0.0f;          // world voxels/s
  uint32_t seed = 0;
  int drawn = 0;               // ticks the string was back
  std::string item;
};

struct MobArcheryWorld {
  static constexpr size_t kMaxDrawing = 64;
  static constexpr int kServedPerTick = 16;
  static constexpr size_t kEventRing = 256;
  std::vector<MobDraw> draws;          // in the order they began
  std::vector<ArcheryEvent> events;    // the newest kEventRing, oldest first
  struct Stats {
    uint64_t requests = 0, drawn = 0, loosed = 0, refused = 0, aborted = 0,
             sidearms = 0, capRefused = 0;
  } stats;
  const MobDraw* Find(uint64_t mobId) const {
    for (const MobDraw& d : draws)
      if (d.mobId == mobId) return &d;
    return nullptr;
  }
  void Clear() {
    draws.clear();
    events.clear();
  }
};

// Right after phase H (and MobCastTick): serve this tick's shoot requests
// (MobSystem::ShootRequests, cleared here) and step every drawn bow -- the
// string, the aim, the loose into w.arrows. Free when nothing asked and
// nothing is drawn.
void MobArcheryTick(TickAuthorityCtx& w, std::span<SessionTick> players, uint32_t tick);

// Arm a creature for its profile's `ranged` block (see ARMING above). Returns
// the arrows put into its pack (0 = it already had a full quiver), or -1 when
// its profile does not shoot or no bow could be put in its hand -- then
// nothing changed. `note`, when given, says what was done, for a log line.
int ArmArcher(MobSystem& mobs, uint64_t mobId, const ItemLibrary& items,
              std::string* note = nullptr);
