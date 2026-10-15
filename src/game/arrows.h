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
//   for the bodies on it (game/pierce.h): into whatever it meets, and on
//   through only what is thin enough to leave it speed (a hand, a scrap), at
//   the speed it leaves with. SEEN as a render-only body (AppendRenderBodies,
//   game/renderonly.h) the frame loop hands to BodyRegistry: until
//   2026-10-13 nothing drew a flight at all, and an arrow appeared, already
//   stuck, a third of a second after the loose -- "instantaneous".
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

#include "game/hand.h"
#include "game/item.h"
#include "game/pierce.h"
#include "game/renderonly.h"
#include "game/spell.h"  // SpellFxVec / kSpellFxOne: the flight's fixed point
#include "math3d.h"

class Physics;
class Mob;
class MobSystem;
class DebrisSystem;
class World;
class WorldItems;
struct MicroBodySet;
struct ParticleSpawn;
struct Kit;

// ---- THE BOW, AS NUMBERS ------------------------------------------------------
// The draw after `ticks` held: 0 through the WINDUP (windupTicks: the bow
// coming up, the arrow nocked -- nothing to loose yet), then linear in time
// over the pull to a full draw drawTicks later, EASED OUT (1 - (1 - t)^2) --
// the first half of a pull is most of its speed, the last few inches buy the
// rest, the way a draw-force curve feels. The bow_draw clips are authored on
// the same timeline (scripts/gen_archery_clips.py reads items.json).
float BowDrawFraction(const ItemDef& bow, int ticks);
// Ticks held to a full draw: the windup and the whole pull.
int BowFullDrawTicks(const ItemDef& bow);
// World voxels/s the arrow leaves at for that draw (0 below minDraw).
float BowArrowSpeed(const ItemDef& bow, int ticks);
// The penetrator an arrow flies as, at `speedMps`.
Penetrator ArrowPenetrator(const ItemDef& arrow, float speedMps, uint32_t seed);

// ---- AN ARROW STAYS ONLY WITH ITS HEAD IN ----------------------------------
// (ticket 2026-10-14_22-26-58, owner: "it should only really stick out if
// it's actually embedded in the flesh. i shouldn't just see the whole arrow
// head on the outside"). The point is placed where the channel stopped
// (PierceResult::stopWorld), so an arrow that stopped a centimetre into a
// body -- spent by the arm it went through first, by a coat of mail, by a
// long flight -- was stuck to the skin with its whole head standing out in
// front of it. A real head that has not gone in past its own length has
// nothing to hold it, and falls out.
//
// The HEAD is the art's: the run of cells, back from the point along the
// shaft, in the point's own matter (ItemDef::ArrowSpec::headMaterial) -- so
// re-authoring the head re-derives how deep it has to go, as the point and
// its hardness already are. World voxels.
float ArrowHeadLength(const ItemDef& arrow);
// True when a hit that stopped in a body (Embedded) went in at least the
// head's length -- the one rule every embed path asks (the flight's, and the
// owner's for a peer's shot, game/melee.cpp ApplyRemotePierceHit). Anything
// shallower is dropped where it struck, as a glance is.
bool ArrowHeadIn(const ItemDef& arrow, const PierceResult& r);

// THE DRAWN BOW AS SEEN (2026-10-14, owner: "drawing a bow should actually
// draw the string, and there should be a visibly notched arrow that flies
// out"). For a creature whose bow_draw* clip is playing and has come in: the
// held bow is drawn from its string-less art (ItemDef::BowStringArt,
// Mob::SetHeldRenderModel), the string as two straight lengths from the tips
// to the string hand, and the arrow on it -- nock at the string hand, along
// the line to the arrow rest beside the grip, so its point stands past the
// bow. Pushed onto `out` as render-only bodies (game/renderonly.h). Run once
// per creature per frame, before the body registry builds its draw list (it
// also resets the held render model, so a bow put down shows its own art).
// Nothing here is read by the tick: on the loose the clip stops, this draws
// nothing, and the flight -- launched from the same arrow rest (session.cpp)
// -- is what flies out.
//
// `nearEye` (first person, the camera): the string is cut off `nearClip`
// world voxels short of it. The fingers are a hand's breadth under the eye,
// so the last of each half string passes the lens, and a one-cell string
// there fills a third of the view.
void PresentDrawnBow(Mob& m, const ItemLibrary& items, std::vector<RenderOnlyBody>& out,
                     const Vec3* nearEye = nullptr, float nearClip = 0.0f);

// THE DROP, PAID IN ADVANCE: the point to aim at so an arrow loosed from
// `from` at `speedVox` world voxels/s comes down onto `to` -- `to` raised by
// what this flight's own gravity takes off it on the way (the integrator's
// n(n+1)/2 ticks of kArrowGravity over the straight-line flight of n ticks).
// What a creature's aim adds (game/mob_archery.cpp); the player aims by eye.
Vec3 ArrowHoldover(Vec3 from, Vec3 to, float speedVox);

// ---- AMMUNITION IN THE KIT -------------------------------------------------------
// How many of `ammo` the kit carries (bag, hotbar, equipment), and take one:
// hotbar first, then the bag (the quiver at hand before the pack). False when
// there is none.
int KitAmmoCount(const Kit& kit, const std::string& ammo);
bool KitTakeAmmo(Kit& kit, const std::string& ammo);

// ---- THE BOW IN A CREATURE'S HAND (the NPC archers, game/mob_archery.h) ----------
// The bow a creature holds -- the right hand's, else the left's -- or null.
// `handOut` = which. And the shots it can loose now: arrows in its own kit for
// that bow (0 with no bow). The AI's SelfView::ammo and the owner's draw both
// ask these two, so "can it shoot" has one answer.
const ItemDef* HeldBow(const Mob& m, const ItemLibrary& items, Hand* handOut = nullptr);
int MobShotsLeft(const Mob& m, const ItemLibrary& items);

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
// ...the same take by rig SLOT on a creature already in hand: the player
// pulling an arrow out of their own body from the health inspector, where
// there is no look ray to name a body.
bool PullArrowSlotToKit(Mob& m, int slot, Kit& kit, const ItemLibrary& items,
                        std::string* itemOut = nullptr, bool* noRoom = nullptr);

// ---- THE ARROWS IN FLIGHT ------------------------------------------------------
struct ArrowFlight {
  SpellFxVec pos{}, vel{};  // 24.8 fixed world voxels; vel per TICK
  // Where the point was at the START of the last step: RENDER-ONLY, the other
  // end of the frame's tick interpolation (AppendRenderBodies). Nothing the
  // flight decides reads it.
  SpellFxVec prev{};
  std::string item;         // what it is ("arrow"): what embeds or sticks
  uint32_t seed = 0;        // the soak's draw key, per hit
  int ticksLeft = 0;
  std::vector<uint64_t> ignore;  // the shooter's own bodies at the loose
  // ---- STEALTH: WHO LOOSED IT, AND FROM WHERE --------------------------
  // The shooter's ACTOR id (a mob id, or a player's band id; 0 = nobody --
  // a gate's arrow) and the loose point: what a struck creature that never
  // saw the archer searches toward, what decides a sneak attack, and where
  // the bow's twang is heard (`twanged` once it has been).
  uint64_t shooter = 0;
  Vec3 from{};
  bool twanged = false;
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
              std::vector<uint64_t> ignore, uint64_t shooterActor = 0);
  void Tick(uint32_t tick, ArrowWorld& w, std::vector<ParticleSpawn>& spawns);

  size_t Live() const { return live_.size(); }
  const std::vector<ArrowFlight>& Flights() const { return live_; }
  // THE ARROWS AS THEY ARE SEEN (game/renderonly.h): one body per live flight,
  // its art placed point-first exactly as the embed and the stuck item place
  // it, at the point INTERPOLATED between the last two ticks by `tickAlpha`
  // (the frame's leftover accumulator over a tick -- the camera's own lerp,
  // so a shot leaves the string at the frame rate rather than hopping a
  // metre and a half per tick), along the segment it is flying. Appends;
  // flights whose item has no micro model draw nothing. Render-only.
  void AppendRenderBodies(const ItemLibrary& items, float tickAlpha,
                          std::vector<RenderOnlyBody>& out) const;
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
