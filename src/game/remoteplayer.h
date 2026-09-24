// remoteplayer.h — THE OTHER PLAYER, AS A THING THIS MACHINE CAN DRAW AND HIT
// (docs/PLAN_multiplayer_m9.md, stage M9.2 package B).
//
// ---- WHY A GHOST IS NOT A SESSION ------------------------------------------
//
// M9.1 made `TickAuthority` take a LIST of `PlayerSession`s, so the obvious
// move for "a second machine's player" is a second session fed the peer's
// `TickInput`. The model of record says no, and the reason is worth stating
// once here because every field below follows from it:
//
//   NO `TickInput` CROSSES THE WIRE. The peer is AUTHORITATIVE for its own
//   controller. It runs `Player::Update` against ITS mirror, with ITS
//   `kindAt`, inside ITS residency window, and sends us the OUTCOME.
//
// Re-simulating a peer's controller here would require its collision source,
// and its collision source is a 3x3x3 chunk mirror centred on ITS chunk —
// which is exactly the thing `two-players` (M9.1 P3) proved this machine does
// not have for a body twelve chunks away. A re-simulated remote controller
// would hover, and then the two machines would disagree about where a player
// is, which is the one thing a shared world may not do.
//
// So `PlayerState` is the OUTCOME of one tick of somebody else's controller,
// and a `RemotePlayer` is a `PlayerAvatar` driven from a `Player` struct that
// is FILLED, never `Update()`d. The ghost's `Player` is a data carrier for the
// fifteen-odd fields `PlayerAvatar::PreTick` reads; it is never swept, never
// integrated, and has no `kindAt`.
//
// ---- WHAT A GHOST DOES AND DOES NOT DO --------------------------------------
//
//   DOES: animate (the rig walks, jumps, crouches, looks), occupy a kinematic
//         Jolt capsule so you cannot walk through it, register as a
//         `MobSystem` avatar so an NPC sweep recognises its limbs, and publish
//         a `PlayerActorDesc` in the banded actor id space so NPCs can target
//         it (`ai::kPlayerActorBase + <sessions> + <ghost index>`).
//
//   DOES NOT: author a single op. Its avatar's footfalls, bleed spray and fall
//         damage all try to — `PreTick` takes op/cell/spawn vectors and fills
//         them — and every one of those goes into THROW-AWAY vectors here.
//         The owner of that body is authoring the same ops on ITS machine and
//         they arrive over the wire in M9.3; emitting them here as well would
//         apply each of them TWICE, once per machine, and the two worlds would
//         diverge on the first footstep. See `RemotePlayers::scratch*`.
//
// ---- ZERO GHOSTS IS EXACTLY TODAY'S TICK ------------------------------------
//
// Every call site in session.cpp is guarded on `remotes && !remotes->list
// .empty()`, so a single-player process makes no new call at all. That is not
// a micro-optimisation, it is the acceptance criterion: the one-session
// `--record-ops` stream must stay byte-identical to the pre-package oracle,
// and a byte-identical op stream is what "the hash did not move" is made of.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "game/avatar.h"
#include "game/mob.h"
#include "game/player.h"
#include "math3d.h"
#include "sim/interest.h"
#include "sim/world.h"

struct PlayerSession;  // session.h includes THIS file; MakePlayerState is in .cpp

// THE WIRE STRUCT'S VERSION. Bumped whenever a field is added, removed or
// reinterpreted. The handshake compares it and refuses a mismatch rather than
// reading a peer's struct with our layout — the failure mode of getting that
// wrong is a player standing in the wrong place with no error anywhere.
constexpr uint32_t kPlayerStateVersion = 2;  // 2: kThrowDraw/kThrowSwing

// ONE TICK OF SOMEBODY ELSE'S CONTROLLER, AS A RESULT. POD, fixed layout, no
// pointers and no std:: anything: this struct is memcpy'd into a frame by
// `net::` in package A/C, so every field has to survive a byte copy and a
// `static_assert(sizeof(...))` has to be able to pin the layout.
//
// ---- WHY THESE FIELDS AND NOT OTHERS ---------------------------------------
//
// The contents are a CENSUS, not a guess: every `player.<field>` read inside
// `PlayerAvatar::PreTick` and `PlayerAvatar::Spawn` in avatar.cpp, and nothing
// else. That census is thirteen fields —
//
//   pos, vel, grounded, crouching, hanging, mantleTimer, inLiquid, fly,
//   hangLip, hangDir, jumped, blindFall, impactDeltaV
//
// — plus the two values the CALLER supplies beside them: the body heading
// (`ResolveAvatarHeading`'s output, resolved on the OWNER's machine because it
// is a function of the owner's camera) and `SetLook(yawRel, pitch)`.
//
// NOT CARRIED, deliberately, and this is a correction to the package text:
// `crouchKneeDrop` and `fallDamageSpeed` are NOT `Player` fields. They are
// `CurrentTuning().player.*` — avatar.cpp reads them as `player.crouchKneeDrop`
// off the TUNING struct, which a naive grep for `player\.` cannot tell apart
// from the controller. Both machines load the same tuning.json, so putting
// them on the wire would have shipped two dead floats per tick forever.
//
// `alive` and `health` are here even though `PreTick` does not read them:
// `alive` drives Despawn/Revive and the `PlayerActorDesc` an NPC targets, and
// `health` is the peer's HUD number for a future party panel. `windowOrigin`
// is here for the CHUNK AUTHORITY question (M9.3) — who is allowed to own an
// edit — and for a disconnect diagnostic that can say how far apart the two
// residency windows had drifted.
struct PlayerState {
  uint32_t version = kPlayerStateVersion;
  uint32_t tick = 0;      // the SENDER's tick this outcome belongs to
  uint32_t playerId = 0;  // stable for the connection's lifetime

  Vec3 pos{};  // centre of the nominal figure box (player.h's Box note)
  Vec3 vel{};
  float heading = 0;    // body facing, radians about +Y (ResolveAvatarHeading)
  float lookYaw = 0;    // SetLook's yawRel: head yaw RELATIVE to `heading`
  float lookPitch = 0;  // SetLook's pitch

  // ONE WORD OF BOOLEANS, not nine bytes of padding-prone `bool`. A bitfield
  // is also the only form that survives a version bump cleanly: a new flag is
  // a new bit, not a new offset for everything after it.
  uint32_t flags = 0;

  float mantleTimer = 0;   // > 0 while the body is being pulled onto a ledge
  int32_t hangLip[3] = {}; // Player::hangLip is an IVec3 (the solid voxel)
  Vec3 hangDir{1, 0, 0};
  Vec3 impactDeltaV{};     // one-tick edge: the hardest arrest of the tick
  int32_t health = 0;
  int32_t windowOrigin[3] = {};  // the SENDER's residency window, chunk coords

  enum Flag : uint32_t {
    kGrounded = 1u << 0,
    kCrouching = 1u << 1,
    kJumped = 1u << 2,  // one-tick edge
    kInLiquid = 1u << 3,
    kSwimming = 1u << 4,
    kFly = 1u << 5,
    kHanging = 1u << 6,
    kAlive = 1u << 7,
    kBlindFall = 1u << 8,
    // THE THROW, as LEVELS rather than edges: drawing (Q held, the wind-up
    // hold) and swinging (released, the arm coming through until the vessel
    // leaves the hand -- PlayerSession::throwLaunchIn, 3 ticks). A level
    // survives a dropped state where a one-tick edge would not; the ghost
    // plays the clips on the transitions it sees (RemotePlayersPreTick).
    kThrowDraw = 1u << 9,
    kThrowSwing = 1u << 10,
  };
  bool Has(Flag f) const { return (flags & (uint32_t)f) != 0; }
  void Set(Flag f, bool on) {
    if (on)
      flags |= (uint32_t)f;
    else
      flags &= ~(uint32_t)f;
  }
};

// THE LAYOUT IS PINNED. 108 bytes at 4-byte alignment: 3 u32 header (12) +
// pos/vel (24) + heading/lookYaw/lookPitch (12) + flags (4) + mantleTimer (4)
// + hangLip (12) + hangDir (12) + impactDeltaV (12) + health (4) +
// windowOrigin (12). If this fires, the wire format changed: bump
// kPlayerStateVersion in the same commit, or the peer on the other end of a
// LAN cable reads your fields at our offsets.
static_assert(sizeof(PlayerState) == 108, "PlayerState wire layout moved");
static_assert(alignof(PlayerState) == 4, "PlayerState alignment moved");

// The sender side: one session's outcome, ready to frame. `tick` is the
// SENDER's tick counter and `windowOrigin` the sender's residency window, both
// passed in because neither is the session's to know (session.h's rule: no
// sim-affecting state keyed on the window origin lives in a player object).
PlayerState MakePlayerState(const PlayerSession& s, uint32_t tick,
                            IVec3 windowOrigin);

// The mob id band ghosts live in. Session i's avatar is `0x5A11ED + i` (M9.1
// P2 pinned session 0 at exactly 0x5A11ED because that literal is baked into
// every recorded world hash and every gate's expected gore spread). Ghosts
// start 64 above that, so no plausible number of local split-screen sessions
// can ever collide with a ghost's id — a collision there would silently make
// two bodies share gore RNG.
constexpr uint64_t kGhostAvatarIdBase = 0x5A11EDU + 64;

// ONE PEER'S GHOST ON THIS MACHINE.
struct RemotePlayer {
  explicit RemotePlayer(uint32_t id)
      : playerId(id), avatar(kGhostAvatarIdBase + id) {}

  uint32_t playerId = 0;
  PlayerState last{};
  bool haveState = false;
  uint32_t lastStateTick = 0;

  // THE DATA CARRIER. Filled by Apply() from `last`; never Update()d, never
  // swept, never given a kindAt. It exists because `PlayerAvatar::PreTick`
  // takes a `const Player&` and that is the right signature — the avatar
  // should not need to know whether the body driving it is local.
  Player ghost;
  float heading = 0;

  // The avatar: a real Mob, with real limb bodies, registered with MobSystem
  // so an NPC's melee sweep recognises its limbs instead of melting them as
  // debris (mob.h's SetAvatars note). Not copyable/movable — hence the
  // unique_ptr in RemotePlayers below.
  PlayerAvatar avatar;
  // The kinematic Jolt capsule, so a local player cannot walk through a ghost.
  uint64_t proxyBody = 0;
  bool spawned = false;  // our own record, so Despawn/Revive stay symmetric
  // The throw levels as last played on the rig, so a clip starts on the
  // transition and not every tick. Cleared while the avatar is despawned (a
  // respawned rig has no clips running).
  bool throwDraw = false, throwSwing = false;

  // Fill `ghost` + `heading` from a freshly arrived state. The ONE-TICK EDGES
  // (`jumped`, `impactDeltaV`) are set from the state here and drained after
  // PreTick, exactly as session.cpp's phase I drains a local player's: they
  // are "this happened during the tick that produced this state", and a latch
  // that is never drained only ratchets upward.
  void Apply(const PlayerState& st);
};

// EVERY GHOST ON THIS MACHINE, plus the tick scratch that keeps them from
// authoring anything.
struct RemotePlayers {
  std::vector<std::unique_ptr<RemotePlayer>> list;
  // Set whenever the membership changed, so the next tick re-registers the
  // avatar list and the actor list with MobSystem. Those two lists are
  // INDEX-ALIGNED with the actor id band, so a rebuild has to be all-or-
  // nothing: registering a ghost without re-publishing the actors would leave
  // an NPC chasing an id that now names a different body.
  bool dirty = false;

  // ---- THE THROW-AWAY VECTORS -------------------------------------------
  // A ghost's avatar authors ops; we discard them. See the file header: the
  // owner of that body is authoring the same ops on its own machine and they
  // arrive over the wire in M9.3. Applying them here as well would double
  // every footfall scuff and every drop of blood, and the two worlds diverge.
  //
  // Kept as members rather than locals so the per-tick discard does not
  // allocate, and so a gate can ASSERT they were non-empty at least once —
  // "the ghost tried to author and nothing reached the batch" is two claims
  // and a fixture that cannot see the first one cannot prove the second.
  std::vector<BrushOp> scratchOps;
  std::vector<CellOp> scratchCells;
  std::vector<ParticleSpawn> scratchSpawns;
  // Cumulative count of ops the ghosts authored and we threw away. The gate
  // reads it; a HUD could too. Never reset inside the tick.
  uint64_t discardedOps = 0;

  RemotePlayer& Upsert(uint32_t id);
  RemotePlayer* Find(uint32_t id);
  // Despawns the avatar, removes the Jolt proxy and marks dirty. `phys` is
  // passed because a RemotePlayer does not own the physics world.
  void Remove(uint32_t id, Physics& phys);
  // Every ghost, unconditionally — disconnect, world reload, shutdown.
  void Clear(Physics& phys);
};

// ---- the four seams session.cpp's phases call, as free functions -----------
//
// Free functions rather than methods on TickAuthorityCtx so THE GATE CALLS THE
// SAME CODE THE GAME RUNS. A gate that reimplemented the per-ghost sequence
// would be a fixture measuring itself: it would keep passing while the real
// phase drifted out from under it.

// PHASE B: a ghost's chunk joins the interest set. INFORMATION ONLY today —
// `primary` stays 0 and only `primary` moves the window (session.cpp's note).
// What residency should DO about a second point of interest is the chunk
// authority decision, M9.3.
void RemotePlayersAppendInterest(const RemotePlayers& remotes,
                                 InterestSet& interest);

// PHASE H: one `PlayerActorDesc` per ghost, APPENDED AFTER the sessions'.
// The band is positional: actor id `ai::kPlayerActorBase + i` is entry i of
// this list, so a ghost appended at index `sessions + j` is targetable at
// `kPlayerActorBase + sessions + j` and `FindCombatantById` resolves it
// through the index-aligned avatar list below.
void RemotePlayersAppendActors(const RemotePlayers& remotes,
                               std::vector<MobSystem::PlayerActorDesc>& out);

// PHASE H, when `dirty`: rebuild MobSystem's avatar list as
// (sessions' avatars, then ghosts' avatars) so it is index-aligned with the
// actor list above. Clears `dirty`. Cheap and idempotent, but only called on
// a membership change — a per-tick `assign` would be a vector rebuild for
// nothing.
void RemotePlayersSyncAvatars(RemotePlayers& remotes, MobSystem& mobs,
                              std::span<Mob* const> sessionAvatars);

// PHASE I, after the sessions': spawn on first state, look, PreTick into the
// throw-away vectors, drain the edges, move the capsule.
void RemotePlayersPreTick(RemotePlayers& remotes, uint32_t tick, float dt,
                          World& world, Physics& phys, MobSystem& mobs,
                          DebrisSystem& debris,
                          const std::vector<MaterialDef>& mats,
                          const std::string& avatarDefName);

// PHASE O, after the sessions': the rig's post-solver settle. No ragdoll
// follow and no push-out — a ghost's position is the wire's to decide, and a
// local solver moving it would be this machine disagreeing with its owner.
void RemotePlayersPostStep(RemotePlayers& remotes);
