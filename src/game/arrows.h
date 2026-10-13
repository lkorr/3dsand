#pragma once
// arrows.h -- THE BOW AND ITS ARROWS (docs/PLAN_projectiles.md package C,
// 2026-10-12).
//
// TWO HALVES.
//
//   THE BOW is the player's (session.cpp PhaseG, the bow block): a bow in a
//   hand (ItemDef::IsBow) turns that hand's button into a DRAW while it is
//   held and a SHOT when it is let go. Everything it reads is the tick's
//   command (TickInput's held mask, the camera the tick turned from the look
//   delta), so a replay or a peer draws the same bow the same way. The speed
//   is BowArrowSpeed: the eased draw fraction x the bow's drawSpeed; a release
//   under the bow's `minDraw` looses nothing. One arrow leaves the kit per
//   shot (KitTakeAmmo); with none, the bow does not draw.
//
//   THE ARROWS IN FLIGHT are the world's (ArrowFlights, owned by
//   TickAuthorityCtx, stepped once a tick in phase K's slot after the peers'
//   blows and the local swings). A flight is the SpellProjectile way, not a
//   Jolt body: thin fast bodies tunnel, and CCD on them is the 3 fps trap.
//   CPU, integer (24.8 fixed voxels, the spell VM's units), gravity, swept in
//   half-voxel sub-steps against the grid (game/flightprobe.h, the probe the
//   spells use), and once a tick ONE PierceAlongRay over the tick's segment
//   for the bodies on it (game/pierce.h): through an arm and into the torso
//   behind it, through one man and into the next, at the speed each leaves.
//
// WHERE AN ARROW ENDS.
//   - EMBEDDED in a creature this machine owns: it stays in the limb
//     (Mob::EmbedArrow -- an appended follower slot, worn-shell style, at the
//     angle it flew in). A creature at its cap (Mob::kMaxEmbeddedArrows)
//     breaks the new one. On a peer's creature the owner embeds it (the
//     routed MeleeHitKind::Pierce record carries the item).
//   - EMBEDDED in loose matter (a severed piece, a crate): strapped to the
//     body it stopped in (DebrisSystem::StrapBody) and registered as the
//     item, so G takes it.
//   - GLANCED, or stopped by a body nobody carves (a door): it drops where it
//     met it, as an ordinary item body that falls.
//   - THE GROUND: it sticks, point first, as a kinematic world item that G
//     picks back up (DropItemToWorld's stuck variant). Bounded at kMaxStuck:
//     the oldest stuck arrow is cleared when a new one would pass it.
//   - Its life runs out, or it leaves the window: gone.
//
// The flat damage (ItemDef::ArrowSpec::damage, Penetrator::hp) is charged by
// Mob::PierceLimb only when the channel got IN (embedded or through), never on
// a glance -- a weapon stat, like a sword's cut.
//
// G on an arrow stuck in a creature (alive or dead) pulls it out into the kit
// (PullArrowToKit, main.cpp's take): the slot goes and nothing else changes --
// no plug, no bleed coupling (owner, 2026-10-12).

#include <cstdint>
#include <string>
#include <vector>

#include "game/item.h"
#include "game/pierce.h"
#include "game/spell.h"  // SpellFxVec / kSpellFxOne: the flight's fixed point
#include "math3d.h"

class Physics;
class MobSystem;
class DebrisSystem;
class World;
class WorldItems;
struct MicroBodySet;
struct ParticleSpawn;
struct Kit;

// ---- THE BOW, AS NUMBERS ------------------------------------------------------
// The draw after `ticks` held: linear in time to a full draw at drawTicks,
// then EASED OUT (1 - (1 - t)^2) -- the first half of a draw is most of its
// speed, the last few inches buy the rest, the way a draw-force curve feels.
float BowDrawFraction(const ItemDef& bow, int ticks);
// World voxels/s the arrow leaves at for that draw (0 below minDraw).
float BowArrowSpeed(const ItemDef& bow, int ticks);
// The penetrator an arrow flies as, at `speedMps`.
Penetrator ArrowPenetrator(const ItemDef& arrow, float speedMps, uint32_t seed);

// ---- AMMUNITION IN THE KIT -------------------------------------------------------
// How many of `ammo` the kit carries (bag, hotbar, equipment), and take one:
// hotbar first, then the bag (the quiver at hand before the pack). False when
// there is none.
int KitAmmoCount(const Kit& kit, const std::string& ammo);
bool KitTakeAmmo(Kit& kit, const std::string& ammo);

// ---- G ON AN ARROW STUCK IN A CREATURE -----------------------------------------
// `body` is what the look ray hit. If it is an arrow slot (Mob::IsArrowSlot)
// on a creature this machine owns, the arrow goes into `kit` (bag first,
// hotbar as the overflow) and leaves the body; `itemOut` names it. False and
// nothing changed otherwise (not an arrow, a peer's creature, no room -- in
// which case `noRoom` is set).
bool IsEmbeddedArrowBody(MobSystem& mobs, uint64_t body);
bool PullArrowToKit(MobSystem& mobs, uint64_t body, Kit& kit,
                    const ItemLibrary& items, std::string* itemOut = nullptr,
                    bool* noRoom = nullptr);

// ---- THE ARROWS IN FLIGHT ------------------------------------------------------
struct ArrowFlight {
  SpellFxVec pos{}, vel{};  // 24.8 fixed world voxels; vel per TICK
  std::string item;         // what it is ("arrow"): what embeds or sticks
  uint32_t seed = 0;        // the soak's draw key, per hit
  int ticksLeft = 0;
  std::vector<uint64_t> ignore;  // the shooter's own bodies at the loose
};

// What one arrow did, for the `arrow` gate and the counters. Bounded log.
struct ArrowEvent {
  enum class Kind : uint8_t {
    Hit,       // a body on the way (r.outcome says how); more may follow
    Embedded,  // ...and it stayed in a creature: `mobId`/`slot`
    Strapped,  // ...stayed in loose matter: `body` is the arrow's, strapped
    Dropped,   // glanced / stopped by an uncarvable body: fell as an item
    Stuck,     // in the ground: `body` is the stuck world item
    Broke,     // embedded where it cannot stay (a creature at its cap, a
               // peer's creature -- whose owner embeds it)
    Expired,   // its life ran out / it left the window
  };
  Kind kind = Kind::Hit;
  uint32_t tick = 0;
  uint32_t seed = 0;        // the flight's (which arrow)
  uint64_t mobId = 0;
  int limb = -1;            // the struck slot
  int slot = -1;            // Embedded: the arrow's own slot on the creature
  uint64_t body = 0;        // Stuck/Strapped/Dropped: the arrow's body
  Vec3 at{};                // where it ended (or the hit's entry)
  Vec3 dir{};
  PierceResult r;           // Hit only
};

struct ArrowWorld {
  World& world;
  const std::vector<uint32_t>& classOf;
  Physics& phys;
  MobSystem& mobs;
  DebrisSystem& debris;
  const ItemLibrary& items;
  WorldItems* ground = nullptr;      // null: nothing can stick (a harness)
  MicroBodySet* micro = nullptr;
};

class ArrowFlights {
 public:
  static constexpr int kMaxLive = 64;        // rule 2: a hard cap, checked first
  static constexpr int kLifeTicks = 300;     // 10 s of flight, then gone
  static constexpr int kMaxStuck = 48;       // arrows standing in the ground
  static constexpr size_t kMaxEvents = 512;  // the gate's log, ring-trimmed

  // Loose one `arrow` from `fromWorld` along `dirWorld` at `speedVox` world
  // voxels/s plus `carryVox` (the shooter's own velocity). False (and nothing
  // launched) at the cap or for a zero direction -- the caller keeps the
  // arrow. Converted to fixed point ONCE, here; everything after is integer
  // (but the bodies, which PierceAlongRay prices in float on the physics
  // state, as melee does).
  bool Launch(const ItemDef& arrow, Vec3 fromWorld, Vec3 dirWorld,
              float speedVox, Vec3 carryVox, uint32_t seed,
              std::vector<uint64_t> ignore);
  void Tick(uint32_t tick, ArrowWorld& w, std::vector<ParticleSpawn>& spawns);

  size_t Live() const { return live_.size(); }
  const std::vector<ArrowFlight>& Flights() const { return live_; }
  const std::vector<ArrowEvent>& Events() const { return events_; }
  void ClearEvents() { events_.clear(); }
  // Everything forgotten (a world reset). Stuck arrows stay where they are:
  // they are world items now, the debris system's.
  void Clear() {
    live_.clear();
    events_.clear();
    stuck_.clear();
  }
  // Monotonic counters.
  struct Stats {
    uint64_t launched = 0, refused = 0, hits = 0, embedded = 0, strapped = 0,
             dropped = 0, stuck = 0, broke = 0, expired = 0, cleared = 0;
  } stats;

 private:
  void Log(const ArrowEvent& e);
  std::vector<ArrowFlight> live_;
  std::vector<ArrowEvent> events_;
  std::vector<uint64_t> stuck_;   // oldest first
};
