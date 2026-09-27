// session.h — THE AUTHORITY / PRESENTATION BOUNDARY
// (docs/PLAN_multiplayer_now.md package N5, audit §S4/A3).
//
// Before this file, ONE PLAYER WAS A SET OF LOCAL VARIABLES IN main(). The
// camera, the controller, the brush, the avatar, the spell caster, the melee
// state, the kit, the grab, the grenades in flight and thirty
// other things were declared side by side in main() and read by a 2,400-line
// tick body that also lived in main(). Nothing was wrong with any of it as
// code; what was wrong is that "a second player" had no shape. You could not
// name the thing that would have to be duplicated, and you could not tell,
// looking at the tick body, which of the ninety names it touched were the
// player's, the world's, or the window's.
//
// This file names them. Four types and one function:
//
//   PlayerSession  — everything that is ONE PLAYER'S. One instance today; a
//                    second could be constructed right now without a compile
//                    error and without touching a global, which is the whole
//                    acceptance criterion for this package.
//   TickAuthorityCtx — everything a tick of authority needs that is NOT one
//                    player: the engine systems (by reference), the loaded
//                    content (by reference), the per-WORLD tick scratch (owned
//                    here, because it used to be more main() locals), and —
//                    named and separated — the PRESENTATION seam the tick body
//                    still writes to.
//   OpBatch        — the tick's op vectors, owned by the caller. A server loop
//                    forwards them; the frame loop submits them.
//   FrameIntent    — what the frame layer settled before the tick.
//   TickAuthority  — one tick. The frame loop calls it; a `--frames` run calls
//                    it; a future headless server loop calls it.
//
// ============================ THE RULE ====================================
//
// PER-PLAYER STATE LIVES IN PlayerSession. SIM-AFFECTING PROCESS GLOBALS MUST
// NOT BE KEYED ON THE RESIDENCY WINDOW ORIGIN.
//
// The first half is enforceable by reading: if a new thing is one player's,
// it goes in the struct below, not in main(). The second half is the one that
// bites, because it is invisible until there are two windows. The process
// globals the sim reads are CurrentTuning(), CurrentWorldMap(), WaterBodies(),
// WorldEditLayer(), Celestial(), WindPrims() and CurrentPrims(). All but one
// are keyed on the WORLD; WaterBodies() is keyed on the residency window
// origin, which means it answers a different question for a second player
// standing 600 voxels away. That is the grandfathered exception and it is
// tracked in docs/RESEARCH_multiplayer_readiness.md S4 — do not add a second
// one.
//
// ==========================================================================
//
// WHAT THIS PACKAGE DID NOT DO, ON PURPOSE. main() still holds an ALIAS
// reference per PlayerSession member (`Camera& cam = session.cam;` and so on).
// The presentation half of main() — the render block, the character screen,
// the dev panel, shutdown — makes thousands of references to those names, and
// rewriting them to `session.cam` would have been a 600-site rename inside the
// same commit that moves a 2,400-line tick body, with a byte-identical op
// record as the only oracle. The aliases are a migration seam and nothing
// more: the STATE is in the object, so a second session compiles, and the
// names are the next package's mechanical rename.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/avatar.h"
#include "game/brush.h"
#include "game/camera.h"
#include "game/caster.h"
#include "game/container.h"
#include "game/corpses.h"
#include "game/equipment.h"
#include "game/grab.h"
#include "game/item.h"
#include "game/melee.h"
#include "game/mob.h"
#include "game/persist.h"
#include "game/player.h"
#include "game/prefab.h"
#include "game/remoteplayer.h"
#include "game/spell.h"
#include "game/strike_pick.h"
#include "game/thirdperson.h"
#include "gpu/context.h"
#include "gpu/passtimer.h"
#include "math3d.h"
#include "measure/perfscope.h"
#include "phys/debris.h"
#include "phys/physics.h"
#include "sim/farfield.h"
#include "sim/materials.h"
#include "sim/microbody.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/tickinput.h"
#include "sim/world.h"
#include "ui/overlay.h"

class WorldItems;  // game/worlditems.h; TickAuthorityCtx holds it by pointer

// THE OP EXCHANGE, BY NAME ONLY (M9.3-B). net/opsync.h includes this header
// for OpBatch, so the dependency has to point one way: a pointer to an
// incomplete type is all TickAuthorityCtx needs, and the two .cpp files that
// actually call it include the real header.
namespace net {
class OpSync;
// ...and the convergence half (M9.3-C, net/chunksync.h). By name only for the
// same reason: chunksync.h reaches World and the op record, and session.cpp is
// the only file here that calls into it.
class ChunkSync;
}

// ---- small per-player value types, moved out of main() --------------------

// Thrown bouncing bomb — the first CPU gameplay projectile (DESIGN.md §8).
// Float math is fine here: the grid only ever sees the ExplosionOp it emits,
// which travels through the deterministic MutationQueue path.
struct Grenade {
  Vec3 pos, vel;  // voxel units, voxels/s
  float fuse;     // seconds
};
// Integrate one 30 Hz tick against the voxel mirror. Returns true on detonate.
bool UpdateGrenade(Grenade& g, float dt, const Player::KindFn& kindAt);

// ONE EXPLOSION AGAINST EVERY BODY: rigidbodies crater, creatures AND players
// are carved, debris takes the impulse, the living are launched. Phase K's
// per-explosion body block, lifted out so the `blast-players` gate runs the
// code the game runs. Every player is reached through MobSystem's registered
// avatars (W1-F): a grenade carves and launches the OTHER player too, and a
// peer's ghost is carved but never launched (game/mob.h).
//
// `who` narrows the pass. Everyone (phase K, the AUTHOR's machine): debris,
// NPCs, every avatar, the impulse. OwnAvatars (phase N, a PEER's blast on the
// OWNER's machine): only this process's own avatars, carved and launched --
// the body the author's machine could only carve as a ghost.
enum class BlastBodies : uint8_t { Everyone, OwnAvatars };

// ---- WHAT ONE EXPLOSION DOES TO BODIES, IN ONE PLACE (W2-H) ----------------
//
// Every consumer of a blast -- the carve (debris and creatures), the per-body
// debris impulse and the rig launch -- reads its reach and its strength from
// here and nowhere else. It used to be read at each call site from THREE reach
// knobs (physics.explosionBodyDamageScale, physics.explosionImpulseRadiusScale
// and ragdoll.blastRadiusScale -- the last two both 3.0 and meaning the same
// thing) and two impulse scales, and a gate that replicated the block had its
// own copy. Now:
//   craterRadius  radius x physics.explosionBodyDamageScale: how far matter is
//                 blown OFF a body (smaller than the push, on purpose)
//   pushRadius    radius x physics.explosionImpulseRadiusScale: THE one push
//                 reach, for loose bodies and creatures alike
//   debrisImpulse power x physics.explosionImpulseScale: kg*m/s at the centre
//                 on EACH loose body (speed-capped per body)
//   rigImpulse    power x ragdoll.blastImpulseScale: kg*m/s at the centre on a
//                 WHOLE creature (divided by its mass). Two numbers because
//                 they are two different quantities -- a shove per body and a
//                 launch per creature; one scale for both would have changed a
//                 grenade's launch by 6.7x.
//   power         the explosion's power: the carve's terrain rule
//                 (Mob::CarveLimbRadial) and the shells in its way
struct BlastForce {
  Vec3 center{};        // the op's cell CENTRE: carve and rig launch
  Vec3 debrisCenter{};  // the op's cell CORNER, which the debris impulse has
                        // always been applied from (kept: not this change)
  float craterRadius = 0.0f;
  float pushRadius = 0.0f;
  float debrisImpulse = 0.0f;
  float rigImpulse = 0.0f;
  float power = 0.0f;
};
BlastForce BlastForceOf(const ExplosionOp& e);
void ExplosionHitsBodies(const ExplosionOp& e, World& world, Physics& phys,
                         DebrisSystem& debris, MobSystem& mobs,
                         std::vector<ParticleSpawn>& spawns,
                         BlastBodies who = BlastBodies::Everyone);

// THE OWNER-SIDE HALF OF A PEER'S GRENADE. `remoteIdx` are the merged indices
// of the peer's explosions (OpDelayQueue::Merge's remoteExpIdx); each is
// applied to this machine's own avatars with BlastBodies::OwnAvatars at the
// landing tick. Nothing is applied twice: the author's machine sees its own
// blast as LOCAL in the merge (so this skips it; phase K hit its bodies when
// it was authored), and here the author is a ghost, which OwnAvatars never
// touches. `gore` receives the carve's particle spawns; phase N runs AFTER the
// outgoing batch was stored, so the caller carries them into its NEXT local
// batch (TickAuthorityCtx::remoteBlastGore) rather than into the merged one,
// which would reach only this machine's GPU. Returns the number applied.
uint32_t RemoteExplosionsHitOwnAvatars(std::span<const ExplosionOp> exps,
                                       std::span<const uint32_t> remoteIdx,
                                       World& world, Physics& phys,
                                       DebrisSystem& debris, MobSystem& mobs,
                                       std::vector<ParticleSpawn>& gore);

// Where a spell resolved, for the renderer: a short-lived burst of sprites
// (SpellEmission::impacts). Render-only, counted down per TICK so the flash
// lasts the same world-time at any frame rate.
struct SpellFlash {
  Vec3 at;
  uint32_t color;
  float radius;
  int ttl, ttl0;
};

// A voxel a vessel scooped, flying into it (game/container.h). Render-only,
// born from the cells ContainerScoop claimed and aged per TICK like
// SpellFlash; main.cpp flies each one from its cell to the held vessel's mouth
// (re-read every frame, so the stream follows the hand). `delay` holds it
// until the clear LANDS, so the mote takes off as the cell vanishes.
struct ScoopMote {
  Vec3 from;       // cell centre, world voxels
  uint32_t color;  // 0xAABBGGRR
  uint32_t seed;   // swirl phase, size and flight-time jitter
  int delay;       // ticks until it takes off
  int age;         // ticks in flight
  int life;        // ticks from take-off to the mouth
  // APPLY MODE flies it the other way (TB_APPLY): out of the mouth onto `to`,
  // the skin point the brush struck, for what the MPM ghosts cannot carry
  // (lava, a pouch's powder). `from` is unused then.
  bool out = false;
  Vec3 to{};
};

// A combat cue raised inside the tick loop cannot be played there: audio
// drains once per frame and the tick loop may have run four times or none. So
// each cue is a one-shot request with PEAK-HOLD ON POWER — a frame in which
// the blade crossed a body on three consecutive ticks is ONE blow to the ear,
// and it should be the hardest of the three rather than three overlapping
// copies of nearly the same sample. `at` travels with it because a killing
// blow can despawn its victim before the frame drains.
struct CombatCueRequest {
  bool pending = false;
  float power = 0.0f;  // 0..1
  Vec3 at{};
};

// THE HIT-STOP DIP. `pendScale`/`pendMs` are written INSIDE the tick loop,
// peak-held so a sever landing in the same frame as a chip keeps the sever's
// dip, and drained by the FRAME loop once, at the top, before the accumulator
// fills — which is why it is presentation state and not the session's.
//
// PEAK-HOLD IS min ON THE SCALE AND max ON THE DURATION. They are peaks of the
// same quantity — "how much stop" — expressed in units that run opposite ways,
// and combining them with the same operator is how you get a sever that stops
// hard for 55 ms because a chip landed in the same frame.
struct HitStop {
  float pendScale = 1.0f;  // requested this frame; 1 = nothing requested
  float pendMs = 0.0f;
  float timeLeft = 0.0f;  // REAL seconds remaining in the live dip
  float scale = 1.0f;     // the live dip's multiplier
  void Request(float s, float ms) {
    if (ms <= 0.0f || s >= 1.0f) return;
    pendScale = std::min(pendScale, s);
    pendMs = std::max(pendMs, ms);
  }
};

// ---- A BODY REDUCED TO WHAT THE PANEL NEEDS ---------------------------------
//
// One oriented collider box per limb, tagged with the figure slot it belongs
// to, in world voxels. Framing the portrait, framing ONE limb, and outlining
// limbs on the image are three readings of this same list — and it is a plain
// list of boxes rather than three walks of the rig for one reason: THE DEAD
// HAVE NO RIG. After Mob::Die() every PartBody() is 0, so a walk answers
// nothing, while a list of boxes captured at the instant of death goes on
// answering all three questions for as long as the death screen holds it. The
// live rig and the death snapshot therefore reach the camera through one type,
// and the panel cannot behave differently on the two.
//
// Here rather than in main.cpp because DeathBody holds one, and DeathBody is
// the presentation seam the tick body still writes to.
struct LimbBoxes {
  struct Box {
    int slot = -1;  // BodySlotFor(); -1 = a part with no figure slot
    Vec3 c[8];
  };
  std::vector<Box> boxes;
  bool Empty() const { return boxes.empty(); }
};

// THE PHOTOGRAPH THE DYING OBSERVER TOOK. The body's POSE at the instant it
// died — every limb's world transform plus its collider box — so the death
// screen can be orbited, zoomed and read ten seconds later while the limbs
// themselves go on burning, sinking or rolling downhill as debris. The
// portrait pass overwrites the live GPU transforms with these before it draws
// and puts the real ones back after.
//
// PRESENTATION, not the session's: it is one window's readout of one death,
// and a headless server has no portrait to freeze.
struct DeathBody {
  // One frozen limb: the physics handle it will be carrying as debris
  // (AdoptBody keeps the handle, which is the whole reason this can be
  // matched up again) and the transform it had when it was still yours.
  struct Limb {
    uint64_t body = 0;
    BodyXformGpu xf{};
  };
  bool have = false;
  float yaw = 0.0f;         // the body's facing, so the orbit keeps a front
  std::vector<Limb> limbs;  // the GPU transform override
  LimbBoxes boxes;          // framing, limb focus and the outlines
};

// Owner handle for wind primitives placed from the DEV PANEL, so "clear all"
// retires those and nothing else. A spell's gust owns itself (the casterId) and
// expires on its own TTL; the panel has no business reaching into gameplay.
// Shared because the panel writes it and the tick body honours it.
constexpr uint64_t kDevFanOwner = 0xDEFA11Au;

// The id the local player's spells, beams, statuses and wind primitives carry
// as their caster/owner, and the one `target` value that means "you". ONE
// constant: it was the literal 0x9134A5EE in fourteen places across the tick
// body and the HUD mirror. It is NOT per-session — every session casts as this
// id today (see the P6 report in docs/PLAN_perf_audit_2026-09-23.md).
constexpr uint64_t kPlayerCasterId = 0x9134A5EEu;

// Where the beam LEAVES the caster, in world voxels.
//
// It must clear the avatar's own head. The eye sits inside the skull part, so
// a damage ray cast from there hits the caster on its first tick and the
// dismemberment path takes their head off before the beam reaches anything —
// which is exactly what "the laser kills me instantly" was. Forward of the
// face and down-right of the eyeline, so it reads as fired from the hand.
//
// ONE definition on purpose: the damage ray (the tick) and the beam sprites
// (the render block) both call this. They used to carry separate offsets, so
// the visible beam came from the hand while the lethal one came from the
// middle of the player's face.
Vec3 LaserMuzzle(const Player& player, const Camera& cam);

// Which figure slot a rig part belongs to, for the character screen and for a
// spell aimed at a body part. A pure function of the authored part name and
// tag; lives here rather than in main.cpp because both the tick body and the
// UI ask, and a second copy is a second thing to keep in step.
int BodySlotFor(const char* name, const char* tag);

// ---- the tick's ops -------------------------------------------------------

// The op vectors ONE TICK produces, owned by the caller rather than declared
// inside the tick body. That is the shape a server loop needs: the batch is
// what would go on the wire, and TickAuthority filling a caller's struct is
// the difference between "the tick submitted something" and "the tick produced
// something the caller decides what to do with".
//
// FIVE VECTORS, NOT SIX. The plan says six; the sixth (gas spawns) is queued
// inside World by the producers themselves and joins at SubmitTick, so there
// is no vector here for the frame layer to own. Recorded rather than faked
// with an always-empty member.
struct OpBatch {
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> exps;
  std::vector<CellOp> cells;
  std::vector<ParticleSpawn> spawns;
  std::vector<FluidSpawnOp> fluid;
  // Cleared, not reconstructed: the capacity is worth keeping across ticks and
  // the values are identical either way.
  void Clear() {
    ops.clear();
    exps.clear();
    cells.clear();
    spawns.clear();
    fluid.clear();
  }
};

// WHERE A SESSION THAT OWNS NO WINDOW PUTS ITS PRESENTATION (M9.1 P1).
//
// The tick body writes a death screen, a hit-stop dip, a frozen death pose, a
// refused-spell counter and five combat cue requests. Every one of them is ONE
// WINDOW'S readout, and every one of them used to be written unconditionally
// because there was only ever one player and it was the window's.
//
// A second session on this machine — a peer's ghost (M9.2), a scripted second
// player in a gate — must not reach any of them: its death would black out
// YOUR screen and its sword would dip YOUR frame clock. Rather than guard
// forty call sites, the tick BINDS THE NAMES somewhere harmless for such a
// session (session.cpp, SV_SEAM_REFS_PLAYER). Nothing reads what lands here;
// it exists so the writes have a legal address.
//
// It has to PERSIST, which is why it hangs off the session rather than off the
// tick's scratch: the death/respawn block reads back the `deathScreen` flag it
// set on an earlier tick to decide whether the respawn hold has started.
struct PresentationSink {
  UIState ui;
  HitStop hitStop;
  DeathBody deathBody;
  bool deathFrozen = false;
};

// ---- one player -----------------------------------------------------------

// The body of PlayerSession::PrefetchAround, as a free function over a
// POSITION and a CURSOR rather than over a session.
//
// Split out because the `two-players` gate drives two bare `Player`s and two
// `PlayerAvatar`s with explicit ids (a PlayerSession's avatar is constructed
// with the default id and the gate needs 0x5A11ED and 0x5A11ED+1), so it has
// no PlayerSession to call the member on. A gate that re-implemented the
// prefetch would be asserting on its own copy of it, which is the "a fixture
// that measures itself" trap; one definition, two callers.
inline void PrefetchChunksAround(World& world, Vec3 posVox,
                                 uint32_t& cursor) {
  const IVec3 c{ifloor(posVox.x) >> 4, ifloor(posVox.y) >> 4,
                ifloor(posVox.z) >> 4};
  const uint32_t refresh = cursor % 27u;
  cursor++;
  uint32_t i = 0;
  for (int dz = -1; dz <= 1; dz++)
    for (int dy = -1; dy <= 1; dy++)
      for (int dx = -1; dx <= 1; dx++, i++) {
        const IVec3 wc{c.x + dx, c.y + dy, c.z + dz};
        // Already in the cache and not this tick's refresh slot: nothing to
        // ask for. This is the line that keeps the steady-state cost at ONE
        // fetch per tick instead of 27 (inviolable rule 2).
        if (i != refresh && world.Cached(wc) != nullptr) continue;
        world.RequestChunkFetch(wc, World::FetchSource::Mob);
      }
}

struct PlayerSession {
  // ---- who this player is, to the tick ----
  // Index in the span TickAuthority is given. 0 is the PRIMARY: every world
  // phase that needs a player at all (the dev panel's ray, the far-plume eye,
  // the chunk the submit centres on, the tick record's one command) takes
  // session 0, because those are the WINDOW's questions and the window follows
  // the primary.
  int index = 0;
  // Does this player own the window this process is drawing? True for the one
  // session the game constructs. False for a peer's ghost and for a second
  // scripted player in a gate — see PresentationSink above for what that
  // changes, plus one behaviour: a session with no view cannot be asked to
  // respawn, so it revives itself once `avatar.respawnDelay` has elapsed.
  bool localView = true;
  std::unique_ptr<PresentationSink> sink;  // allocated iff !localView
  // The splash edge detector. It was `static bool wasInLiquid` inside the tick
  // body until M9.1 — the one function static in session.cpp, and the exact
  // shape of defect the rule at the top of this file names: two players in two
  // lakes shared one edge and the second one never splashed.
  bool wasInLiquid = false;

  // ---- body and view ----
  Camera cam;
  Player player;
  PlayerAvatar avatar;
  std::string avatarDefName;  // tuning.json player.model, re-read on F5
  ThirdPersonRig tpRig;
  CameraMode camMode = CameraMode::First;
  float avatarHeading = 0.0f;  // body facing, radians about +Y
  float fovNow = 0.0f;
  float respawnTimer = 0.0f;
  // Kinematic capsule proxy so debris collides with (and is shoved by) the
  // player; terrain collision stays in the AABB controller.
  uint64_t playerBody = 0;
  // How this player's sweeps read the world. A std::function because a second
  // session would want its own mirror source (player.h's KindFn note). Bound
  // ONCE, before the frame loop, rather than rebuilt as a lambda every frame.
  Player::KindFn kindAt;

  // ---- tools ----
  Brush brush;
  PrefabPlacer placer;
  // THE KIT IS THE AVATAR'S (W2-M): hotbar, bag and equipment are
  // `avatar.GetKit()` / `avatar.KitMut()` (Mob::kit_), and the rig is dressed
  // from it by Mob::DressFromKit. The session used to hold `hotbar` and a
  // `PlayerKit` here plus the `wearTried`/`wearDye` latches its per-tick wear
  // loop reconciled the rig through.
  Kit& kit() { return avatar.KitMut(); }
  Inventory& hotbar() { return avatar.KitMut().hotbar; }

  // ---- looking at things, and looting them (game/corpses.h) ----
  // What the reach ray found this frame (a debris body handle or 0), the
  // corpse the loot panel is open on, and whether E opened the character
  // screen to show it.
  uint64_t lookBody = 0;
  uint64_t lootCorpse = 0;
  bool lootOpenedScreen = false;
  // ---- the physics grab (game/grab.h) ----
  // E IS TWO BINDINGS ON ONE KEY: tapped it is the pickup/loot, held past
  // player.grabHoldTime it lifts whatever the reach ray is on.
  GrabHold grab;
  float eHeld = 0.0f;
  bool eGrabbed = false;
  bool ePrevDown = false;
  std::vector<uint64_t> lookIgnore;  // the avatar's own limbs, per frame

  // ---- magic ----
  SpellSystem spells;
  PlayerCaster caster;
  CasterHealth playerHealth;
  std::vector<SpellFlash> spellFlashes;
  // A body part clicked in the inspector with a sentence on the stack: the
  // slot, or -1. NOT a TickInput field — it is a click in an ImGui panel on a
  // specific rig part, a UI transaction rather than a player command, and it
  // has no meaning on a remote peer.
  int castAtPartQueued = -1;
  // ...and with a filled VESSEL chosen on the character screen's FLASKS row
  // the portrait is a BRUSH instead (game/container.h, MobSystem::PourOnBody):
  // while the button is held, the world ray main.cpp built from the portrait
  // camera through the cursor, the disc radius, and WHICH stack pays for it
  // (a pack or hotbar slot, not the hand -- the flask need not be held). HELD, not latched -- main.cpp rewrites it every frame and
  // the tick pours every tick it is `active`. Same reasoning as the latch
  // above for why it is not a TickInput field.
  struct PourStroke {
    bool active = false;
    Vec3 ro{}, rd{};
    float radius = 0.5f;
    KitRef vessel{};
  } pourStroke;
  uint32_t pourStrokeTicks = 0;  // ticks this stroke has poured
  // Eighths owed but not yet taken, in 1/1000ths: the drain is a rate that
  // depends on the brush size (PourBrushCellsPerSec), so it is paid off an
  // accumulator rather than a fixed per-tick clock.
  int64_t pourSpendMilli = 0;
  // APPLY MODE (TB_APPLY): the same brush, on whatever body the crosshair is
  // on -- another creature, a corpse -- with the vessel in the HAND. Its own
  // accumulator and stroke, so the portrait brush and this never pay for each
  // other. `applyMob` is who the stroke is on, so the message names a new
  // creature once rather than every tick.
  int64_t applySpendMilli = 0;
  uint32_t applyTicks = 0;
  uint64_t applyMob = 0;
  // What this player's scoop has taken that the snapshot cannot see yet.
  ContainerScoopMemo scoopMemo;
  std::vector<ScoopMote> scoopMotes;  // render-only, see ScoopMote
  // THE THROW'S WIND-UP (game/container.h ContainerThrowSpeed): ticks Q has
  // been held with a throwable vessel in hand, 0 when it is not. Counted on
  // the tick, released on the tick; cancelled when the hand changes.
  // Ticks the `pour` clip has held the arm out (0 = not pouring); the
  // stream waits for the arm (session.cpp kPourRaiseTicks).
  int pourPoseTicks = 0;
  int pourPoseHand = 0;   // which arm the `pour` clip is on (HandClip)
  // ---- THE ALCHEMY BENCH IN THE HANDS (game/alchemy_bench.h) ----
  // While the bench has vessels on its table the character holds them: one
  // flask in one hand, two in both, and the one being tilted on the bench
  // tilted over the other's mouth. Written by main.cpp every frame from the
  // bench's published poses (an input, like the pour stroke); the tick
  // equips the hands with these instead of the kit's and poses them.
  // Presentation only: nothing here reaches the sim.
  struct BenchHold {
    bool active = false;
    struct HandView {
      // The vessel's item NAME, not a def pointer: this is written by the
      // frame and read by the next tick, and an R reload between the two
      // rebuilds the item library. Empty = this hand holds nothing extra.
      std::string item;
      alchemy::Composition contents;   // the bench's live contents
      float angle = 0;                 // bench tilt, radians, CCW on screen
      // Where the vessel's middle is on the bench (sim pixels, y up) and how
      // tall it is there: the carried one's arm follows its bench motion.
      float cx = 0, cy = 0, height = 0;
      bool carried = false;            // in the bench's hand right now
    } hand[kHands];
  } benchHold;
  // Ticks the bench hold has been live (the arm-claim ramp), and which clip
  // each hand is playing for it.
  int benchHoldTicks = 0;
  bool benchClip[kHands] = {false, false};
  float benchAimW[kHands] = {0.0f, 0.0f};
  float benchPourW = 0.0f;
  int benchPourHand = -1;
  Vec3 benchPourCmd{};          // the pour arm's command (PoseBenchHands)
  bool benchPourCmdValid = false;
  // THE CARRY'S ANCHOR, taken the tick the bench hand picks the vessel up:
  // its bench middle and its 3D middle then, the scales that turn bench
  // pixels into voxels, and the other flask's place (bench and 3D) so moving
  // toward it on the bench moves toward it in the hands.
  struct BenchCarry {
    bool anchored = false;
    Vec3 c0{};                 // the carried flask's 3D middle at pickup
    float bx = 0, by = 0;      // ...and its bench middle
    float sx = 0, sy = 0;      // voxels per bench pixel: across, up
    bool other = false;
    Vec3 o3{};                 // the other flask's 3D middle at pickup
    float sepB = 0;            // bench x from the carried one to the other
  } benchCarry;
  int throwTicks = 0;
  // The HAND the throw is drawn in (Hand as an int), -1 = none. It used to
  // be a hotbar slot; the vessel is now in a kit hand slot (dual wielding).
  int throwSlot = -1;
  // THE RELEASE: Q let go starts the `throw` clip and the vessel leaves the
  // hand `throwLaunchIn` ticks later, where the arm has swung it to — not
  // from behind your head. >0 while the arm is coming through; the speed is
  // fixed at the release so the delay cannot add charge.
  int throwLaunchIn = 0;
  float throwLaunchSpeed = 0.0f;

  // ---- melee ----
  MeleeState melee;
  StrikePicker strikePicker;
  StrokeCursor playerStrike;
  int strikeQueued = -1;    // style index latched at the press, -1 = none
  int strikeBuffered = -1;  // ONE strike banked mid-swing, fired at recover
  // ...and WHICH HAND each was pressed with (dual wielding: LMB the right,
  // RMB the left). Carried beside the style rather than folded into it,
  // because the same compass style is thrown by either arm.
  Hand strikeQueuedHand = Hand::Right;
  Hand strikeBufferedHand = Hand::Right;
  // THE DRIVER'S FRAME. `melee` lives across strokes (its recover and idle
  // unwind outlast the cursor), so whether it runs in the MIRRORED basis
  // (strokes.h "THE LEFT HAND IS THE RIGHT, MIRRORED") is the session's to
  // remember, changed only when a stroke begins. A cursor-held flag would
  // flip the basis under a recover the moment the cursor resets.
  bool strikeMirrored = false;
  // The hand the player last used (a strike or a vessel): F cycles ITS
  // vessel's mode, and the idle ready pose stays on it.
  Hand lastHand = Hand::Right;
  // The BASE style (the compass's index, before ResolveForm) of the stroke in
  // playerStrike, -1 = none: what StrikeChains asks "the opposite side of".
  int strikeBase = -1;
  // THE LAST ATTACK PRESS, as the picker read it — for the HUD's strike
  // compass (overlay.cpp) and nothing else; the sim never reads it.
  // `flicked` false = no flick, the neutral alternate fired. `serial` bumps
  // per press so the HUD can age the readout without a clock of its own.
  struct StrikePickNote {
    int style = -1;
    float fx = 0, fy = 0;
    bool flicked = false;
    uint32_t serial = 0;
  };
  StrikePickNote lastStrikePick;
  Vec3 lastEdgeBase{}, lastEdgeTip{};
  bool lastEdgeValid = false;
  // The STRIKE aim ray's ignore list. Its own vector rather than a share of
  // `lookIgnore`: that one is filled in the frame block and read by the E
  // prompt, and the melee ray runs inside the tick loop, where overwriting it
  // would silently change what E offers.
  std::vector<uint64_t> strikeIgnore;
  // Who the player's stroke struck this tick, and whether a bite landed on
  // them. Per-player because they are one stroke's readout.
  std::vector<uint64_t> playerStruck;
  bool playerBitten = false;
  CombatCueRequest combatWhooshCue, combatFleshCue, combatClangCue,
      combatStrikeCue, combatCutCue;
  bool combatStrikeEdged = true;
  // LIQUID LANDING IN A VESSEL, one entry per cell paid in (ContainerSettle),
  // each the receiving vessel's fill fraction 0..1 after that cell. Raised on
  // the tick, drained and cleared by the frame (audio::Cues::FlaskFill), like
  // the combat cues above. `flaskFillAt` is where the last one landed.
  std::vector<float> flaskFills;
  Vec3 flaskFillAt{};

  // ---- projectiles ----
  std::vector<Grenade> grenades;
  // EXPLOSIONS AUTHORED OUTSIDE THE TICK, for its next grenade slot: the
  // alchemy bench's (a reaction that blew up in the character's hands,
  // alchemy_bench.h BenchOutcome), written by the frame, drained by the tick
  // into `exps` beside the grenades so they carve and shove the same way.
  std::vector<ExplosionOp> pendingBlasts;

  // ---- the frame layer's command accumulator (package N2) ----
  // The frame layer WRITES held state and the axes wholesale every frame, ORs
  // in every pressed edge it sees, and adds the frame's raw mouse pixels. Each
  // tick CONSUMES one TickInput.
  TickInputFeeder feeder;
  bool captured = true;  // is the cursor grabbed for this session's window

  // ---- M9.1 P3: KEEPING A NON-PRIMARY PLAYER'S GROUND IN THE CACHE --------
  //
  // Only session 0 gets the snapshot's 3x3x3 mirror (the window follows the
  // primary — see `index` above). Every other session reads the world through
  // `World::KindAtCached`, which answers from the on-demand chunk cache and
  // falls back to the analytic terrain column on a miss. The fallback is
  // correct-ish and cheap-ish, but it is only correct where the world still
  // looks like worldgen made it, so the cache has to be kept warm AHEAD of the
  // body rather than filled by the misses the body is already suffering.
  //
  // WHY NOT JUST REQUEST ALL 27 EVERY TICK. `World::RequestChunkFetch`
  // coalesces a chunk that is ALREADY QUEUED, but a chunk that has already
  // landed is dropped from the queue, so a blind 27-per-tick prefetch costs 27
  // of the 64-chunk-per-tick fetch budget forever — it would scale with world
  // residency rather than with activity, which is inviolable rule 2. So:
  //
  //   * every chunk of the 3x3x3 cube that is NOT cached is requested (the
  //     body needs it NOW and the miss path is already paying for it);
  //   * exactly ONE cached chunk is re-requested per tick, round-robin, so the
  //     whole cube refreshes every 27 ticks (0.9 s at 30 Hz) and a cell another
  //     player dug stops being a ghost floor within a second.
  //
  // Steady state on flat ground is therefore ONE fetch per tick per non-primary
  // session, and a session that is standing still costs the same one. The
  // cursor is per-session state, not a process global (DESIGN.md §10).
  uint32_t prefetchCursor = 0;
  void PrefetchAround(World& world) {
    PrefetchChunksAround(world, player.pos, prefetchCursor);
  }
};

// The 'PLYR' save section, built from ONE PLAYER plus the two content
// libraries. See the note over PlayerKitRefs (game/persist.h): the section used
// to be assembled out of whichever main() locals were in scope, and "these five
// belong to the same player" was a fact nobody could check.
inline PlayerKitRefs PlayerKitOf(PlayerSession& s, const GlyphLibrary& glyphs,
                                 const ItemLibrary& items) {
  return PlayerKitRefs{&s.caster, &glyphs, &s.kit(), &items, &s.avatar};
}

// What the FRAME layer decided this frame that the tick has to act under.
// These three are derived from the tool selector, magic mode and the hotbar —
// UI state, settled before the tick and passed in rather than re-derived,
// because the frame settles the hand before the tick and re-deriving here is how
// two reads of one fact stop agreeing.
struct FrameIntent {
  bool brushActive = false;
  // THE HANDS ARE UP: the melee tool is selected and magic mode is off, so
  // the mouse buttons belong to the hands (LMB the right, RMB the left).
  // WHAT the hands hold is not the frame's to say any more (dual wielding,
  // 2026-09-27): it is the kit's HandR/HandL, which the tick reads itself
  // (ResolveHands -> SessionTick::hands), so a gate that puts a sword in a
  // kit hand has put it in the rig's hand with no frame at all.
  bool handsUp = false;
  // ...and WHERE IT POURS (game/container.h ContainerPourPoint): the point on
  // the crosshair ray the frame drew the marker sphere on. The crosshair ray
  // starts at the RENDER eye, which only the frame knows; handing the point
  // over is what keeps the stream on the sphere. Invalid = the tick works it
  // out from the head (headless harnesses have no camera).
  bool pourAimValid = false;
  Vec3 pourAim{};
  // APPLY MODE's brush disc, world voxels: the portrait brush's size
  // (UIState::pourRadius), so one setting governs both. And the crosshair ray
  // it picks along -- from the RENDER eye, as pourAim is -- so the body the
  // HUD names is the body that is brushed. Invalid = from the head.
  float applyRadius = 0.5f;
  bool aimFromValid = false;
  Vec3 aimFrom{};
};

// ---- the world a tick runs in ---------------------------------------------

// EVERY REFERENCE MEMBER COMES FIRST, then everything with a default. Not a
// taste call: the caller builds this with designated initializers, C++20 wants
// them in declaration order, and a reference has no default to skip. So the
// groups below are ordered by what the language needs and labelled by what
// they mean.
struct TickScratch;  // session.cpp; see TickAuthorityCtx::tickScratch
struct TickAuthorityCtx {
  // ---- A. the engine, by reference. One per process today; one per WORLD in
  // the shape this is heading for.
  GpuContext& ctx;
  World& world;
  Simulation& sim;
  Stream& stream;
  FarField& far;
  Physics& phys;
  MobSystem& mobs;
  DebrisSystem& debris;
  MicroBodySet& mbSet;

  // ---- B. the loaded content, by reference. Hot-reloaded on R, so these are
  // references to main()'s owners rather than copies.
  const std::vector<MaterialDef>& mats;
  const GlyphLibrary& glyphs;
  const ItemLibrary& items;
  const std::vector<Prefab>& prefabs;
  // Material COLLISION class LUT for the mirror queries. Not raw klass:
  // BuildCollisionClasses remaps passable vegetation to gas.
  const std::vector<uint32_t>& classOf;

  // ---- C. THE PRESENTATION SEAM. Everything here is the WINDOW's, not the
  // world's, and the tick body still touches it. Named and grouped so the next
  // package knows exactly what to cut: a headless server would pass a dummy
  // UIState and dead timers, and nothing in section D would notice.
  UIState& ui;
  HitStop& hitStop;  // a hit REQUESTS a dip; the frame loop ages it
  DeathBody& deathBody;
  bool& deathFrozen;
  bool& liveTimed;  // telemetry: are per-pass GPU timings armed
  PassTimer& liveTimer;
  sandvox::PerfSample& liveSample;
  uint32_t& liveFrameNo;
  float& tickMsSmooth;
  // --frames far-field tally: entries dispatched over the run, ticks that
  // dispatched at least one, and the largest single tick's count. Harness
  // telemetry the frame layer prints at exit.
  uint64_t& farEntries;
  uint64_t& farTicks;
  uint64_t& farBiggest;

  // ---- D. per-WORLD tick scratch, OWNED HERE. Every one of these was a
  // main() local; none of them is one player's, and a second session must not
  // need a second copy.
  int labScene = -1;  // --lab scene index, or -1
  std::unordered_map<uint32_t, MicroBodyRef> sphereModels;  // material -> model
  std::vector<uint64_t> aiSpawnedMobs;  // what the AI panel put in the world
  bool everExploded = false;
  uint32_t lastExplosionTick = 0;
  // The CONSERVATIVE MLS-MPM live estimate the CPU owns (world.h fluid block).
  uint32_t fluidCount = 0;
  std::vector<std::pair<uint32_t, uint32_t>> fluidPendingSpawns;
  // Gore from a PEER's blast on this machine's own avatars (phase N,
  // RemoteExplosionsHitOwnAvatars), drained into the next tick's local
  // spawns in phase L so it travels to the peer like any gore authored here.
  // Always empty without a connected peer.
  std::vector<ParticleSpawn> remoteBlastGore;
  uint32_t fluidCueMat = 0;
  uint32_t lastFluidTick = 0;
  // The liquid the mpm tool's keys 1-4 pour (UIState::fluidPour indexes it;
  // 0 = fall back to fluidCueMat, water). Resolved by name at startup.
  uint32_t fluidPourMat[4] = {0, 0, 0, 0};
  // BROKEN VESSELS (game/container.h ContainerSpill): what is still coming
  // out of a flask that shattered, drained under the spawn budgets. Nearly
  // always empty; one entry for a tick when a flask breaks.
  std::vector<ContainerSpill> vesselSpills;
  // THE SCOOP LEDGER, ONE PER WORLD (container.h ContainerScoopLedger): the
  // GPU's scoop counter is world-wide, so every session's claims draw on one
  // per-tick pot here instead of each reading the whole delta for itself.
  ContainerScoopLedger scoopLedger;
  // ---- REACTION EFFECTS IN THE WORLD (docs/PLAN_alchemy_chemistry.md A) ----
  // The world consumer of reactions.json "effects" (materials.h RuleFx). The
  // GPU reports which rules-with-effects fired where (sim_step.wgsl
  // reactFxNote -> World::TakeReactFx, fixed latency), bodies report the same
  // from the burn pass (MobSystem::TakeBodyReactFx), and the primary session's
  // explosion slot (phase K) turns each `explode` effect into an ExplosionOp
  // -- the grenade path: crater, body carve, debris impulse, rig launch --
  // plus a brief ring of fire and smoke laid into the crater the NEXT tick
  // (IfAir cell ops, so it fills only what the blast opened). Bounded:
  // kReactBlastsPerTick per tick, one blast per `radius` neighbourhood.
  struct ReactFxWorld {
    struct Aftermath {
      IVec3 c{};
      int radius = 0;
      uint32_t tick = 0;  // the tick the blast went off on
    };
    std::vector<Aftermath> aftermath;
    // Telemetry, monotonic; the chem-* gates read these.
    uint64_t events = 0;    // effect firings drained (grid slot winners + bodies)
    uint64_t bodyEvents = 0;
    uint64_t blasts = 0;    // ExplosionOps issued
    uint64_t refused = 0;   // explode effects refused (per-tick cap / merged)
    uint64_t aftermathCells = 0;  // fire + smoke cell ops laid
    std::vector<ExplosionOp> recent;  // the last kRecent blasts issued
    static constexpr size_t kRecent = 16;
  } reactFx;
  // Each vessel body's velocity last tick, for the break test's "velocity
  // jump" witness. One entry per vessel lying or flying in the world.
  std::vector<std::pair<uint64_t, Vec3>> vesselVel;
  uint32_t labTick = 0;
  bool duelDummySpawned = false;
  // Wall clock, for the render-only ripple timestamps. A VALUE the frame layer
  // rewrites each frame, not a reference: `now` is a frame-loop local, and a
  // reference to it would be the frame layer keeping state for the authority.
  double frameTime = 0;
  // The tick body's own scratch (session.cpp WorldScratch + one PlayerScratch
  // per session), HELD across ticks so its vectors keep their capacity, and
  // reset field by field at the top of every TickAuthority. Opaque here: the
  // types are the tick body's business. A shared_ptr rather than a unique_ptr
  // only because the caller builds this struct with designated initializers
  // in a TU where the type is incomplete, and a shared_ptr's deleter is bound
  // where it is created (session.cpp), not where it is destroyed.
  std::shared_ptr<TickScratch> tickScratch;

  // ---- E. THE SCRIPTED-FLIGHT / MEASUREMENT HARNESSES, AS HOOKS. Every one
  // of these is argv state the FRAME layer owns, supplied as a callback so
  // game/ does not link against main.cpp's globals. All null in the game and
  // on a server.
  //
  // --autofly-surface pins the altitude analytically and --autofly-park stops
  // the flight; both have to apply BETWEEN player.Update and everything that
  // reads the position.
  std::function<void(Player&, uint32_t tick)> afterPlayerUpdate;
  // --fell-tree: the tree-fell gate's cut driven from the live frame loop. It
  // reads g_frameMs — the frame layer's own whole-frame ring — which is
  // exactly why it is a callback and not code in this file.
  std::function<void(uint32_t tick, std::vector<CellOp>& cellOps)> fellTree;
  // Respawn out of an open inventory has to hand the cursor back to the
  // window: `captureBeforeUi`, glfwSetInputMode and the cursor-position reset
  // are all the WINDOW's, and there is no window here.
  std::function<void()> restoreCursorAfterUi;
  // --duel-dummy: spawn one armed target three metres ahead, once.
  bool duelDummy = false;

  // ---- F. THE PEERS' BODIES (M9.2 package B, game/remoteplayer.h).
  //
  // Per-WORLD, not per-player: a ghost is somebody else's body standing in
  // THIS world, and both local sessions of a split screen would see the same
  // one. Owned by main() (the frame loop is what polls the socket), borrowed
  // here, and NULL IN EVERY HARNESS — which is the point. Zero ghosts means
  // phases B, H, I and O make no call they did not make before this package,
  // and the one-session `--record-ops` stream is byte-identical to the
  // pre-package oracle. That identity is the acceptance criterion; the
  // guards at each of the four call sites are what buys it.
  RemotePlayers* remotes = nullptr;

  // ---- G. THE OP EXCHANGE (M9.3 package B, net/opsync.h).
  //
  // Per-WORLD, like the ghosts, and owned by main() for the same reason (the
  // frame loop is what polls the socket). NULL IN EVERY HARNESS and in every
  // single-player frame, and even when it is non-null it does nothing until
  // `Connected()`.
  //
  // WHAT IT CHANGES WHEN IT IS CONNECTED, in one sentence: the batch this tick
  // produces is not the batch this tick SUBMITS. Ops authored at T are labelled
  // T + D, travel to the peer inside that tick's TickBatch, and both machines
  // submit the merged (local + peer) batch for label T at tick T — in author
  // order, so the two op vectors are identical and rule 3's "lowest op index
  // owns the cell" means the same thing on both. Phase N is the only phase
  // that merges; phases C, G and K read `Delay()` because the CPU-side
  // consumers they raise (debris.AddDestructionEvent) are tick-LABELLED and the
  // crater they are about to be told to rescan does not exist for D ticks yet.
  //
  // Forward-declared rather than included: net/opsync.h includes THIS header
  // for OpBatch, so including it here would be a cycle. session.cpp and
  // main.cpp include it.
  net::OpSync* opsync = nullptr;

  // ---- H. THE CONVERGENCE HALF (M9.3 package C, net/chunksync.h).
  //
  // Per-WORLD and owned by main(), like the two above. NULL IN EVERY HARNESS
  // except the `chunk-resync` gate, which drives ReplaceChunk directly.
  //
  // WHAT IT CHANGES WHEN IT IS NON-NULL: phase B, before `stream.Update`,
  // drains whatever chunk copies arrived from the peer and installs them with
  // `Stream::ReplaceChunk`. BEFORE Update and therefore before the tick's
  // submit, so the tick's CA sees the corrected chunk rather than running once
  // more on the wrong one — and so the replace is inside the same tick the op
  // record attributes it to.
  //
  // Everything else it does (publishing digests, drilling, requesting,
  // answering) is the FRAME layer's, because it is socket work; this pointer
  // exists only so the install lands at the right point in the tick.
  net::ChunkSync* chunksync = nullptr;

  // ---- H2. ITEMS ON THE GROUND (game/worlditems.h), owned by main().
  //
  // The tick THROWS vessels into it (DropItemToWorld with a launch speed) and
  // breaks the ones that hit something hard (PhaseH's vessel pass). Null in
  // every harness that does not bind it, and then nothing is thrown or broken.
  WorldItems* ground = nullptr;

  // ---- I. THE TWO-PROCESS SMOKE'S DELIBERATE DIVERGENCE (M9.3-C).
  //
  // Cell ops appended to the batch AFTER the op merge and immediately before
  // the submit, so they reach THIS machine's GPU and never the wire. That is
  // the entire point and it is why this cannot be the `fellTree` hook (phase
  // D, before the merge): an op that travelled would be applied on both
  // machines and there would be nothing to converge.
  //
  // It exists because a resync cannot be smoke-tested without a divergence,
  // and every NATURAL divergence is either a bug (so it cannot be summoned on
  // demand) or a window-edge case the comparable rule excludes on purpose.
  // SANDVOX_NET_SMOKE_DRIFT=1 in main.cpp is the only thing that ever sets it;
  // it is null in the game, on a server and in every harness, so the
  // single-player op record is untouched.
  std::function<void(uint32_t tick, std::vector<CellOp>& cells)> driftCells;

  // ---- J. THE SELFTEST HARNESS (rule-unification W2-O; test/tickrig.h) ----
  //
  // A gate runs THIS tick now (support::RunTicks), not a hand-rolled copy of
  // part of it. What a gate's world lacks is a player standing in it: the
  // harness session is a fly-mode body with no avatar and no input, and these
  // four pins say what that body must NOT drag along with it. All four are
  // inert in the game and on a server (default-constructed = off), so the
  // frame loop's tick and its `--record-ops` stream are untouched.
  struct Harness {
    // THE WINDOW STAYS WHERE THE GATE PUT IT. Phase B recentres the residency
    // window on the primary, two chunks of hysteresis on every axis — a gate's
    // fixture sits on the terrain, well off the window's vertical centre, so a
    // player standing in it would drag the window down a plane a tick and
    // regenerate the world under the fixture. Pinned, Stream::Update still runs
    // (the harvests and deferred wakes it owes are ticks, not motion) with an
    // empty interest set, which is its own "no shift" answer.
    bool pinWindow = false;
    // The chunk the submit centres the CPU mirror on (SubmitTick's
    // playerChunk), when it is not the player's: the gate's FIXTURE chunk,
    // which is what every hand-rolled ticker passed.
    bool haveSubmitChunk = false;
    IVec3 submitChunk{};
    // The gate owns the NPC targeting layer's player list: phase H does not
    // overwrite it with the harness body (who is not a combatant).
    bool ownsActors = false;
    // The gate's own ops for this tick, pushed at the TOP of the tick before
    // any system authors anything, so they take the lowest op indices — the
    // position every hand-rolled ticker gave them (CLAUDE.md rule 3).
    std::function<void(uint32_t tick, OpBatch& out)> inject;
    // The gate's EXPLOSIONS for this tick. Not injected with the other ops:
    // a blast is more than an op (phase K's crater scan, body carve, debris
    // impulse and rig launch all key off the explosions a session authored),
    // so they go off in the primary's explosion slot, as a grenade would.
    std::vector<ExplosionOp> blasts;
    // WHERE THE MOB PHASE'S OWN AUTHORING SITS IN THIS TICK'S BATCH: the
    // vector sizes either side of phase H's mobs.PreTick, written every tick
    // (six size reads). An accounting claim about what the creature system
    // emitted — "every drop it bled was charged as hp" — is scoped to this
    // span; the rest of the batch is other authors (a severed piece bleeding
    // through debris, a gate's own ops).
    struct Span {
      size_t ops0 = 0, ops1 = 0, cells0 = 0, cells1 = 0, spawns0 = 0,
             spawns1 = 0;
    } mobPhase;
  } harness;
};

// ---- one tick of authority, for N players ---------------------------------
//
// Everything between the accumulator loop's braces, minus the pacing, the
// readback pump and the park probe the frame layer owns. Takes ONE command PER
// PLAYER, produces ONE OpBatch for the whole tick, and submits it. No
// behaviour change from the version that lived in main(): the acceptance for
// that claim is a `--frames 600 --autofly-hard` op record that is
// byte-identical before and after (N5 step 5, re-run for M9.1 P1).
//
// ---- WHAT A PHASE IS, AND WHY THE ORDER IS WHAT IT IS ---------------------
//
// The body is cut into sixteen PHASES (session.cpp, PhaseA..PhaseP). A phase
// is a maximal run of the old single-player body with ONE scope: a PLAYER
// phase touches one PlayerSession and runs once per session in index order; a
// WORLD phase touches the world, the engine systems and the per-tick world
// scratch and runs once. TickAuthority is nothing but their alternation,
//
//     A* B C* D E* F G* H I* J K* L M* N O* P      (* = once per session)
//
// and that alternation IS the old body's order, segment for segment. That is
// the whole design constraint: the tick was never "all the player's work then
// all the world's". The controller has to run before the residency window
// recentres on it; the window has to shift before the laser ray reads the
// grid; the dev panel spawns before the brush writes; mobs.PreTick has to see
// the avatar's bleed ops; the submit has to come after every producer. Merging
// the four player runs into one, or the four world runs into one, reorders the
// op stream — and the op stream's order is the tick's identity (CLAUDE.md rule
// 3: the lowest op index owns the cell).
//
// WHERE A REMOTE GHOST PLUGS IN (M9.2 package B — LANDED): phase B (the
// interest set learns its chunk), phase H (SetPlayerActors lists it and the
// avatar list is re-registered beside it), phase I (its avatar's PreTick) and
// phase O (PostStep). Four guarded call sites on `TickAuthorityCtx::remotes`,
// all no-ops when it is null or empty — see game/remoteplayer.h. The package
// text said phase B for the actor list; the actor list has been phase H's
// since P1 and the code is the truth. NOT as a
// SessionTick: no TickInput crosses the wire in the model of record
// (docs/PLAN_multiplayer_m9.md) — the peer is authoritative for its own
// controller and sends the OUTCOME, a PlayerState, from which a ghost's
// `Player` fields are filled and its PlayerAvatar driven. A ghost is a
// RemotePlayer entry on TickAuthorityCtx that those two phases walk beside
// the span, with `localView = false` semantics (nothing it does reaches the
// window's presentation seam).

// ONE PLAYER'S INPUT TO ONE TICK. The session, what the frame layer settled
// for it, and the command it consumes. By pointer rather than reference so the
// span is assignable and a caller can build it in a vector.
// ---- THE HANDS THIS TICK (dual wielding) -----------------------------------
//
// Resolved once per tick per player from the kit's two hand slots, the frame's
// `handsUp` and the rig (TickAuthority -> ResolveHands), and read by every
// phase that asks "what is in which fist and may it swing". One resolution,
// because the equip seam, the strike press, the stroke's weapon form, the
// sweep's item and the vessel block each used to re-derive "the held item"
// from the hotbar selection on their own.
struct HandNow {
  const ItemDef* item = nullptr;   // what the kit's hand slot holds, or null
  bool weapon = false;             // ItemKind::Melee
  bool vessel = false;             // a container (flask, pouch)
  // THIS hand may begin a strike: the hands are up, the arm is whole, and it
  // holds a weapon — or is EMPTY and one of the unarmed compass's styles can
  // be thrown with it (a fist round a flask does not punch).
  bool ready = false;
};
struct HandsNow {
  HandNow hand[kHands];
  bool AnyWeapon() const { return hand[0].weapon || hand[1].weapon; }
  bool AnyReady() const { return hand[0].ready || hand[1].ready; }
  const HandNow& operator[](Hand h) const { return hand[HandIndex(h)]; }
};

struct SessionTick {
  PlayerSession* s;
  FrameIntent intent;
  TickInput ti;
  HandsNow hands{};   // filled by TickAuthority; never by the caller
};

void TickAuthority(TickAuthorityCtx& w, std::span<SessionTick> players,
                   uint32_t tick, OpBatch& out);

// ONE PLAYER IS A SPAN OF ONE. Kept because the frame loop and every harness
// fixture read better this way, not because the authority is singular — the
// same reason MobSystem::SetPlayerActor is kept beside SetPlayerActors.
inline void TickAuthority(TickAuthorityCtx& w, PlayerSession& s,
                          const FrameIntent& intent, const TickInput& ti,
                          uint32_t tick, OpBatch& out) {
  SessionTick one{&s, intent, ti};
  TickAuthority(w, std::span<SessionTick>(&one, 1), tick, out);
}
