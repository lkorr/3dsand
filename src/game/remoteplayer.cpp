// remoteplayer.cpp — see remoteplayer.h for what a ghost is and why it is not
// a PlayerSession. This file is the four phase seams and nothing else.

#include "game/remoteplayer.h"

#include <algorithm>

#include "game/session.h"

// ---- the sender side -------------------------------------------------------

PlayerState MakePlayerState(const PlayerSession& s, uint32_t tick,
                            IVec3 windowOrigin) {
  const Player& p = s.player;
  PlayerState st;
  st.version = kPlayerStateVersion;
  st.tick = tick;
  // The connection's player id is the SESSION INDEX on the machine that owns
  // it. The receiving side re-keys on the handshake's assigned id (package
  // A/C); this keeps a single-machine caller honest without inventing an id
  // allocator that would then be a second source of truth.
  st.playerId = (uint32_t)s.index;

  st.pos = p.pos;
  st.vel = p.vel;
  // The body facing, as the OWNER resolved it. `avatarHeading` is
  // ResolveAvatarHeading's output and it is a function of the owner's CAMERA
  // (game/thirdperson.h) — there is no camera for a ghost on this machine, so
  // this value has to travel rather than be recomputed. That is the same
  // reason lookYaw/lookPitch travel below.
  st.heading = s.avatarHeading;
  // SetLook takes yaw RELATIVE to the body heading, wrapped, and clamps it
  // against the rig's own neck limits. Recomputed here the way phase I does:
  // camera yaw and rig heading use different conventions (Camera forward is
  // (cos yaw, ., sin yaw), a mob's is (sin h, ., cos h)), so h = pi/2 - yaw.
  {
    float lookRel = (1.5707963f - s.cam.yaw) - s.avatarHeading;
    while (lookRel > 3.14159265f) lookRel -= 6.2831853f;
    while (lookRel < -3.14159265f) lookRel += 6.2831853f;
    st.lookYaw = lookRel;
  }
  st.lookPitch = s.cam.pitch;

  st.Set(PlayerState::kGrounded, p.grounded);
  st.Set(PlayerState::kCrouching, p.crouching);
  st.Set(PlayerState::kJumped, p.jumped);
  st.Set(PlayerState::kInLiquid, p.inLiquid);
  st.Set(PlayerState::kSwimming, p.swimming);
  st.Set(PlayerState::kFly, p.fly);
  st.Set(PlayerState::kHanging, p.hanging);
  st.Set(PlayerState::kBlindFall, p.blindFall);
  // ALIVE IS THE AVATAR'S ANSWER, not the HUD's. Phase H uses `ui.playerAlive`
  // for the primary because that mirror is what the window draws; over the
  // wire the window is irrelevant and the body is the truth. A session with no
  // avatar yet reads alive, which is what phase H's own expression does.
  st.Set(PlayerState::kAlive, !s.avatar.Spawned() || s.avatar.IsAlive());

  st.mantleTimer = p.mantleTimer;
  st.hangLip[0] = p.hangLip.x;
  st.hangLip[1] = p.hangLip.y;
  st.hangLip[2] = p.hangLip.z;
  st.hangDir = p.hangDir;
  st.impactDeltaV = p.impactDeltaV;
  st.health = s.playerHealth.Get();
  st.windowOrigin[0] = windowOrigin.x;
  st.windowOrigin[1] = windowOrigin.y;
  st.windowOrigin[2] = windowOrigin.z;
  return st;
}

// ---- the receiving side ----------------------------------------------------

void RemotePlayer::Apply(const PlayerState& st) {
  last = st;
  haveState = true;
  lastStateTick = st.tick;

  ghost.pos = st.pos;
  // THE RENDER INTERPOLATION BASELINE. `prevPos` is what Player::RenderPos
  // eases from, and nothing writes it here except this line — a ghost has no
  // Update() to advance it — so it is set to the position we are LEAVING, not
  // to the one we are arriving at. Set the other way round the ghost would
  // draw with no interpolation at all and stutter at the state rate.
  ghost.prevPos = ghost.pos;
  ghost.vel = st.vel;
  ghost.grounded = st.Has(PlayerState::kGrounded);
  ghost.crouching = st.Has(PlayerState::kCrouching);
  ghost.inLiquid = st.Has(PlayerState::kInLiquid);
  ghost.swimming = st.Has(PlayerState::kSwimming);
  ghost.fly = st.Has(PlayerState::kFly);
  ghost.hanging = st.Has(PlayerState::kHanging);
  ghost.blindFall = st.Has(PlayerState::kBlindFall);
  ghost.mantleTimer = st.mantleTimer;
  ghost.hangLip = IVec3{st.hangLip[0], st.hangLip[1], st.hangLip[2]};
  ghost.hangDir = st.hangDir;
  // THE TWO ONE-TICK EDGES. Set from the state, drained by
  // RemotePlayersPreTick immediately after the avatar has had its look at
  // them — the same lifetime session.cpp's phase I gives a local player's, and
  // for the same reason: the avatar's `jump` clip keys on the RISING edge of
  // `jumped`, so a flag left standing would retrigger the clip every tick
  // until the next state arrived.
  ghost.jumped = st.Has(PlayerState::kJumped);
  ghost.impactDeltaV = st.impactDeltaV;

  heading = st.heading;
}

RemotePlayer& RemotePlayers::Upsert(uint32_t id) {
  if (RemotePlayer* r = Find(id)) return *r;
  list.push_back(std::make_unique<RemotePlayer>(id));
  dirty = true;
  return *list.back();
}

RemotePlayer* RemotePlayers::Find(uint32_t id) {
  for (std::unique_ptr<RemotePlayer>& r : list)
    if (r->playerId == id) return r.get();
  return nullptr;
}

void RemotePlayers::Remove(uint32_t id, Physics& phys) {
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i]->playerId != id) continue;
    RemotePlayer& r = *list[i];
    // Leave nothing behind: the avatar owns real Jolt limb bodies and the
    // proxy is a real Jolt body. A disconnect that leaked either would leave
    // an invisible capsule standing in the world forever.
    if (r.spawned) r.avatar.Despawn();
    if (r.proxyBody != 0) phys.RemoveBody(r.proxyBody);
    list.erase(list.begin() + (ptrdiff_t)i);
    // The band is POSITIONAL, so removing entry j renumbers every ghost after
    // it. Nothing may target a stale id for even one tick.
    dirty = true;
    return;
  }
}

void RemotePlayers::Clear(Physics& phys) {
  for (std::unique_ptr<RemotePlayer>& r : list) {
    if (r->spawned) r->avatar.Despawn();
    if (r->proxyBody != 0) phys.RemoveBody(r->proxyBody);
  }
  list.clear();
  dirty = true;
}

// ---- PHASE B ---------------------------------------------------------------

void RemotePlayersAppendInterest(const RemotePlayers& remotes,
                                 InterestSet& interest) {
  for (const std::unique_ptr<RemotePlayer>& r : remotes.list) {
    if (!r->haveState) continue;
    const Vec3& p = r->ghost.pos;
    // Arithmetic shift, not divide: a window streamed to negative world
    // coordinates makes `/ 16` round toward zero and name the wrong chunk.
    interest.chunks.push_back(
        {ifloor(p.x) >> 4, ifloor(p.y) >> 4, ifloor(p.z) >> 4});
  }
}

// ---- PHASE H ---------------------------------------------------------------

void RemotePlayersAppendActors(const RemotePlayers& remotes,
                               std::vector<MobSystem::PlayerActorDesc>& out) {
  for (const std::unique_ptr<RemotePlayer>& r : remotes.list) {
    // A ghost with no state yet has no position, and publishing one at the
    // origin would put a target at (0,0,0) that every NPC in the window would
    // turn and walk toward. It is still APPENDED, as a dead actor, because the
    // band is positional and skipping it would renumber the ghosts after it.
    const bool alive = r->haveState && r->last.Has(PlayerState::kAlive);
    out.push_back({r->ghost.pos, Player::kHalfXZ, Player::kHalfY * 2.0f,
                   alive});
  }
}

void RemotePlayersSyncAvatars(RemotePlayers& remotes, MobSystem& mobs,
                              std::span<Mob* const> sessionAvatars) {
  std::vector<Mob*> all;
  all.reserve(sessionAvatars.size() + remotes.list.size());
  for (Mob* m : sessionAvatars) all.push_back(m);
  // GHOSTS AFTER THE SESSIONS, in list order — the SAME order
  // RemotePlayersAppendActors uses. `FindCombatantById` maps band index i to
  // `avatars_[i]`, so these two lists agreeing is not a nicety: disagreeing
  // means an NPC that targets ghost 0 resolves to session 0's body and hits
  // the local player instead.
  for (std::unique_ptr<RemotePlayer>& r : remotes.list)
    all.push_back(static_cast<Mob*>(&r->avatar));
  mobs.SetAvatars(std::span<Mob* const>(all.data(), all.size()));
  remotes.dirty = false;
}

// ---- PHASE I ---------------------------------------------------------------

void RemotePlayersPreTick(RemotePlayers& remotes, uint32_t tick, float dt,
                          World& world, Physics& phys, MobSystem& mobs,
                          DebrisSystem& debris,
                          const std::vector<MaterialDef>& mats,
                          const std::string& avatarDefName) {
  // ONE CLEAR PER TICK, not one per ghost: `discardedOps` counts across the
  // whole tick and a per-ghost clear would only ever report the last one.
  remotes.scratchOps.clear();
  remotes.scratchCells.clear();
  remotes.scratchSpawns.clear();

  for (std::unique_ptr<RemotePlayer>& rp : remotes.list) {
    RemotePlayer& r = *rp;
    // NOTHING HAPPENS BEFORE THE FIRST STATE. A ghost created by the
    // handshake but not yet spoken for has no position, and spawning a rig at
    // the origin is a body dropped into the middle of the world.
    if (!r.haveState) continue;

    // ---- spawn on first state ------------------------------------------
    if (!r.spawned) {
      // Init is idempotent and cheap; SetDefs despawns first, so calling both
      // again on a re-spawn cannot leak limb bodies. The def is the LOCAL
      // player's model name: every machine in a session loads the same
      // assets, and a per-peer model is a content decision for a later stage.
      r.avatar.Init(&phys, &world, &debris, mats, &mobs);
      r.avatar.SetDefs(&mobs.Defs(), avatarDefName);
      if (!r.avatar.HasDef()) continue;  // no such model: draw nothing, quietly
      if (!r.avatar.Spawn(r.ghost, r.heading)) continue;
      r.spawned = true;
      // The capsule is created ONCE and kept across death/revive: a
      // despawned avatar still occupies space to its owner, and churning a
      // Jolt body every death is how handles leak.
      if (r.proxyBody == 0)
        r.proxyBody = phys.CreatePlayerBody(Player::kHalfXZ, Player::kHalfY);
      remotes.dirty = true;  // a new Mob* has to reach MobSystem's avatar list
    }

    // ---- death and revival, driven by the wire, not by local damage -----
    // The OWNER decides whether its body is alive. Local damage to a ghost
    // (an NPC's sweep, a spell) still carves its rig — that is presentation
    // and it is the right presentation — but the authoritative alive/dead
    // edge is the one that arrives in the state.
    const bool wantAlive = r.last.Has(PlayerState::kAlive);
    if (r.spawned && !wantAlive && r.avatar.IsAlive()) {
      r.avatar.Despawn();
      r.spawned = false;
      remotes.dirty = true;
    } else if (!r.spawned && wantAlive) {
      // Revive() is Despawn()+Spawn(); a not-spawned avatar takes Spawn.
      if (r.avatar.HasDef() && r.avatar.Spawn(r.ghost, r.heading)) {
        r.spawned = true;
        remotes.dirty = true;
      }
    }
    if (!r.spawned) continue;

    // ---- drive the rig ---------------------------------------------------
    // SetLook before PreTick, for the reason phase I states: PreTick is what
    // flattens the pose and submits the kinematic limb targets, so a look
    // pushed in after it is a tick late.
    r.avatar.SetLook(r.last.lookYaw, r.last.lookPitch);

    // THE THROW-AWAY VECTORS. This is the line the whole package exists
    // around: a ghost's footfall scuffs, bleed spray and fall-damage gore all
    // land in `remotes.scratch*` and are discarded at the top of the next
    // tick. The owner of this body is authoring the identical ops on its own
    // machine and they arrive over the wire in M9.3; emitting them here as
    // well would apply every one of them twice and the two worlds would
    // diverge on the first footstep.
    const size_t before = remotes.scratchOps.size() +
                          remotes.scratchCells.size() +
                          remotes.scratchSpawns.size();
    r.avatar.PreTick(tick, r.ghost, r.heading, dt, world, remotes.scratchOps,
                     remotes.scratchCells, remotes.scratchSpawns);
    remotes.discardedOps += (remotes.scratchOps.size() +
                             remotes.scratchCells.size() +
                             remotes.scratchSpawns.size()) -
                            before;

    // Drain the one-tick edges, whether or not the avatar consumed them —
    // same rule and same wording as phase I's local drain. A peak-hold that is
    // never drained only ratchets upward.
    r.ghost.impactDeltaV = Vec3{0, 0, 0};
    r.ghost.jumped = false;

    // The capsule follows the wire's position. Kinematic, like the local
    // player's proxy: it pushes debris and blocks a local player, and nothing
    // pushes it back — the owner decides where this body is.
    if (r.proxyBody != 0) phys.MovePlayerBody(r.proxyBody, r.ghost.pos, dt);
  }
}

// ---- PHASE O ---------------------------------------------------------------

void RemotePlayersPostStep(RemotePlayers& remotes) {
  for (std::unique_ptr<RemotePlayer>& r : remotes.list)
    if (r->spawned) r->avatar.PostStep();
  // NO RagdollFollow AND NO PlayerPushOut, on purpose. Both of those write
  // `player.pos` from the local solver, and a ghost's position is the wire's.
  // A machine that let its own physics move somebody else's body would
  // immediately disagree with the machine that owns it, and there is no
  // reconciliation channel in this stage to settle the argument.
}
