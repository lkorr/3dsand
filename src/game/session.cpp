// session.cpp - one tick of authority. See session.h for the boundary this
// file draws and for the rule that comes with it.

#include "game/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio/cues.h"
#include "game/ai_behavior.h"
#include "game/bodyreg.h"
#include "game/dye.h"
#include "game/persist.h"
#include "game/strokes.h"
#include "game/worlditems.h"
#include "lab/lab.h"
#include "measure/perfnodes.h"
#include "sim/celestial.h"
#include "sim/currentprim.h"
#include "sim/microvox.h"
#include "sim/oprecord.h"
#include "sim/plants.h"
#include "sim/rng.h"
#include "sim/trample.h"
#include "sim/tuning.h"
#include "sim/voxload.h"
#include "sim/waterbody.h"
#include "sim/wind.h"
#include "sim/windprim.h"
#include "sim/worldedit.h"
#include "test/support.h"

using namespace sandvox;


// Integrate one 30 Hz tick against the voxel mirror. Returns true on detonate.
bool UpdateGrenade(Grenade& g, float dt, const Player::KindFn& kindAt) {
  g.fuse -= dt;
  if (g.fuse <= 0.0f) return true;
  g.vel.y -= (9.81f / kVoxelMeters) * dt;
  IVec3 at{ifloor(g.pos.x), ifloor(g.pos.y), ifloor(g.pos.z)};
  if (kindAt(at) == CellKind::Liquid) g.vel = g.vel * 0.90f;  // water drag

  for (int axis = 0; axis < 3; axis++) {
    float& v = axis == 0 ? g.vel.x : axis == 1 ? g.vel.y : g.vel.z;
    float d = v * dt;
    if (d == 0) continue;
    float* c = axis == 0 ? &g.pos.x : axis == 1 ? &g.pos.y : &g.pos.z;
    float step = d > 0 ? 0.4f : -0.4f;
    int n = (int)std::ceil(std::abs(d) / 0.4f);
    for (int i = 0; i < n; i++) {
      float prev = *c;
      float next = (i == n - 1) ? *c + (d - step * i) : *c + step;
      *c = next;
      IVec3 cell{ifloor(g.pos.x), ifloor(g.pos.y), ifloor(g.pos.z)};
      if (kindAt(cell) == CellKind::Solid) {
        *c = prev;
        v = -v * 0.45f;  // bounce with restitution
        if (axis != 0) g.vel.x *= 0.8f;
        if (axis != 1) g.vel.y *= 0.8f;
        if (axis != 2) g.vel.z *= 0.8f;
        break;
      }
    }
  }
  return false;
}

// Where the beam LEAVES the caster, in world voxels.
//
// It must clear the avatar's own head. The eye sits inside the skull part, so
// a damage ray cast from there hits the caster on its first tick and the
// dismemberment path takes their head off before the beam reaches anything —
// which is exactly what "the laser kills me instantly" was. Forward of the
// face and down-right of the eyeline, so it reads as fired from the hand.
//
// ONE definition on purpose: the damage ray and the beam sprites both call
// this. They used to carry separate offsets, so the visible beam came from the
// hand while the lethal one came from the middle of the player's face.
Vec3 LaserMuzzle(const Player& player, const Camera& cam) {
  return player.EyePos() + cam.Forward() * 1.2f + cam.Right() * 0.7f -
         cam.Up() * 0.5f;
}

// ---- body-condition HUD mirror (ui/overlay.h UIState::body) -----------------
//
// Maps the avatar's limbs onto the fixed stick-figure slots. The mapping is by
// authored TAG ("head"/"spine"/"arm"/"hand"/"leg"/"foot") plus the ".L"/".R"
// side suffix and an upper/lower discriminator, NOT by mina's exact part names
// — a different humanoid rig with the same tags fills the same figure, and a
// rig missing a part leaves that slot absent rather than mis-drawn.
//
// Upper vs lower within a limb is read from the name ("armU"/"armL"), which is
// the convention the rigs already use; when a rig has only one segment per limb
// it lands in the upper slot and the figure simply draws a shorter limb.
int BodySlotFor(const char* name, const char* tag) {
  const std::string n = name, t = tag;
  const bool left = n.size() >= 2 && n.compare(n.size() - 2, 2, ".L") == 0;
  const bool right = n.size() >= 2 && n.compare(n.size() - 2, 2, ".R") == 0;
  // The segment letter is the character just before the side suffix:
  // "armU.L" -> 'U' (upper), "armL.L" -> 'L' (lower). Anything without a side
  // suffix, or with any other letter there, is treated as the upper segment.
  const bool lower =
      (left || right) && n.size() >= 3 && n[n.size() - 3] == 'L';

  if (t == "head") return UIState::kSlotHead;
  if (t == "spine") {
    // The rig's root is the hips; every other spine part is the torso.
    return n.find("hip") != std::string::npos ? UIState::kSlotHips
                                              : UIState::kSlotTorso;
  }
  if (t == "hand") return left ? UIState::kSlotHandL : UIState::kSlotHandR;
  if (t == "foot") return left ? UIState::kSlotFootL : UIState::kSlotFootR;
  if (t == "arm") {
    if (left) return lower ? UIState::kSlotArmLL : UIState::kSlotArmUL;
    if (right) return lower ? UIState::kSlotArmLR : UIState::kSlotArmUR;
  }
  if (t == "leg") {
    if (left) return lower ? UIState::kSlotLegLL : UIState::kSlotLegUL;
    if (right) return lower ? UIState::kSlotLegLR : UIState::kSlotLegUR;
  }
  return -1;  // props, held items, anything the figure has no place for
}

void TickAuthority(TickAuthorityCtx& w, PlayerSession& s,
                   const FrameIntent& intent, const TickInput& ti,
                   uint32_t tick, OpBatch& out) {
  // ---- THE UNPACK, AND WHY IT IS A WALL OF ALIASES ------------------------
  //
  // The body below is a BYTE-FOR-BYTE MOVE of what used to sit between the
  // accumulator loop's braces in main(). That is deliberate and it is the
  // whole reason this block exists: the acceptance for this package is a
  // recorded session that is byte-identical before and after, and a proof like
  // that is only worth something if the thing it is run against is provably a
  // move rather than a rewrite. `diff` can check a move; it cannot check 2,400
  // lines of renaming.
  //
  // So every name the body closed over is bound here, once, and the body is
  // untouched. Turning `player` into `s.player` at a hundred sites is the next
  // package's mechanical rename, with a compiler and a green record behind it.
  GpuContext& ctx = w.ctx;
  World& world = w.world;
  Simulation& sim = w.sim;
  Stream& stream = w.stream;
  FarField& far = w.far;
  Physics& phys = w.phys;
  MobSystem& mobs = w.mobs;
  DebrisSystem& debris = w.debris;
  MicroBodySet& mbSet = w.mbSet;
  const std::vector<MaterialDef>& mats = w.mats;
  const GlyphLibrary& glyphs = w.glyphs;
  const ItemLibrary& items = w.items;
  const std::vector<Prefab>& prefabs = w.prefabs;
  const std::vector<uint32_t>& classOf = w.classOf;
  const int labScene = w.labScene;
  // per-world tick scratch (section D of TickAuthorityCtx)
  auto& sphereModels = w.sphereModels;
  auto& aiSpawnedMobs = w.aiSpawnedMobs;
  bool& everExploded = w.everExploded;
  uint32_t& lastExplosionTick = w.lastExplosionTick;
  uint32_t& fluidCount = w.fluidCount;
  auto& fluidPendingSpawns = w.fluidPendingSpawns;
  uint32_t& fluidCueMat = w.fluidCueMat;
  uint32_t& lastFluidTick = w.lastFluidTick;
  uint32_t* fluidSpeciesMat = w.fluidSpeciesMat;
  uint32_t& labTick = w.labTick;
  bool& duelDummySpawned = w.duelDummySpawned;
  // the presentation seam (section C)
  UIState& ui = w.ui;
  const double now = w.frameTime;
  HitStop& hitStop = w.hitStop;
  DeathBody& deathBody = w.deathBody;
  bool& deathFrozen = w.deathFrozen;
  const bool liveTimed = w.liveTimed;
  PassTimer& liveTimer = w.liveTimer;
  sandvox::PerfSample& liveSample = w.liveSample;
  const uint32_t liveFrameNo = w.liveFrameNo;
  float& tickMsSmooth = w.tickMsSmooth;
  uint64_t& g_farEntries = w.farEntries;
  uint64_t& g_farTicks = w.farTicks;
  uint64_t& g_farBiggest = w.farBiggest;
  // the player (session.h)
  Camera& cam = s.cam;
  Player& player = s.player;
  PlayerAvatar& avatar = s.avatar;
  std::string& avatarDefName = s.avatarDefName;
  ThirdPersonRig& tpRig = s.tpRig;
  CameraMode& camMode = s.camMode;
  float& avatarHeading = s.avatarHeading;
  float& respawnTimer = s.respawnTimer;
  const uint64_t playerBody = s.playerBody;
  const Player::KindFn& kindAt = s.kindAt;
  Brush& brush = s.brush;
  PrefabPlacer& placer = s.placer;
  Inventory& hotbar = s.hotbar;
  PlayerKit& kit = s.kit;
  std::string (&wearTried)[kEquipSlotCount] = s.wearTried;
  uint32_t (&wearDye)[kEquipSlotCount] = s.wearDye;
  GrabHold& grab = s.grab;
  SpellSystem& spells = s.spells;
  PlayerCaster& caster = s.caster;
  CasterHealth& playerHealth = s.playerHealth;
  auto& spellFlashes = s.spellFlashes;
  int& castAtPartQueued = s.castAtPartQueued;
  MeleeState& melee = s.melee;
  SwingPhase& meleePhasePrev = s.meleePhasePrev;
  StrikePicker& strikePicker = s.strikePicker;
  StrokeCursor& playerStrike = s.playerStrike;
  int& strikeQueued = s.strikeQueued;
  int& strikeBuffered = s.strikeBuffered;
  Vec3& lastEdgeBase = s.lastEdgeBase;
  Vec3& lastEdgeTip = s.lastEdgeTip;
  bool& lastEdgeValid = s.lastEdgeValid;
  auto& strikeIgnore = s.strikeIgnore;
  auto& playerStruck = s.playerStruck;
  bool& playerBitten = s.playerBitten;
  CombatCueRequest& combatWhooshCue = s.combatWhooshCue;
  CombatCueRequest& combatFleshCue = s.combatFleshCue;
  CombatCueRequest& combatClangCue = s.combatClangCue;
  CombatCueRequest& combatStrikeCue = s.combatStrikeCue;
  CombatCueRequest& combatCutCue = s.combatCutCue;
  bool& combatStrikeEdged = s.combatStrikeEdged;
  auto& grenades = s.grenades;
  bool& captured = s.captured;
  // what the frame layer decided (FrameIntent)
  const bool brushActive = intent.brushActive;
  const bool meleeArmed = intent.meleeArmed;
  const bool meleeReady = intent.meleeReady;
  const ItemDef* heldItem = intent.heldItem;
  // THE TICK'S OPS. Aliased out of the caller's batch instead of declared in
  // the body, which is the one structural change the move makes: the batch is
  // what a server would forward. Cleared rather than reconstructed, so the
  // values are identical and the capacity survives the tick.
  out.Clear();
  std::vector<BrushOp>& ops = out.ops;
  std::vector<ExplosionOp>& exps = out.exps;
  std::vector<CellOp>& cellOps = out.cells;
  std::vector<ParticleSpawn>& spawns = out.spawns;
  std::vector<FluidSpawnOp>& fluidSpawns = out.fluid;
  {
      // ================= THE PLAYER, ON THE TICK (N2) =====================
      //
      // Package N2 of docs/PLAN_multiplayer_now.md moved the controller from
      // the frame loop into here. It runs at kTickDt with ONE TickInput, so
      // the trajectory is a function of the command stream and nothing else —
      // which is the property a networked client has to reproduce, and the
      // one the `tick-input` gate asserts (identical position to the bit at
      // 1 and 4 ticks per frame).
      //
      // FIRST IN THE TICK BODY, and the order is load-bearing three ways:
      //   * AFTER the ProcessEvents pump above, so the sweeps read the mirror
      //     this tick delivered rather than last frame's.
      //   * BEFORE `playerChunkNow` below, so the residency window recenters
      //     on where the body IS, not where it was a tick ago.
      //   * BEFORE mobs.PreTick, because SetPlayerActor pushes this position
      //     to the NPCs and the avatar poses to it.
      // Dismemberment drives movement: the active AnimStateRule's speedScale and
      // the leg-liveness-derived jump scale come straight from the avatar, so
      // losing a leg slows the player down and losing both stops them jumping.
      // Fly mode deliberately ignores all of it — a debug camera should not be
      // crippled by the character's injuries.
      {
        const AvatarLocomotion loco = avatar.Locomotion();
        const bool couple = avatar.Spawned() && !player.fly;
        player.speedScale = couple ? loco.speedScale : 1.0f;
        player.jumpScale = couple ? loco.jumpScale : 1.0f;
        player.canJump = couple ? loco.canJump : true;
        // ...and what you are DRAGGING multiplies the same scale (game/grab.h):
        // a light crate costs nothing, a corpse walks you at half pace, and the
        // heaviest thing the grab will accept is close to the tuned floor. It
        // stacks with the injury scale on purpose — a one-legged man carrying an
        // anvil is slower than either.
        //
        // Fly mode is exempt for the same reason the injuries are: the debug
        // camera should not be encumbered.
        if (!player.fly) player.speedScale *= grab.SpeedScale(CurrentTuning().player);
      }
      // ---- component 9: impact ripples -------------------------------------
      // The event source, and it is a RISING EDGE rather than a per-tick test:
      // an impact happens once. Sampled around player.Update because the entry
      // speed is what sizes the splash and it is gone a tick later.
      //
      // Render-only, bounded by the ring, and it is deliberately NOT an audio
      // cue's twin — a cue fires on the same event but through a different
      // system, and coupling them would make one of them the other's trigger.
      {
        static bool wasInLiquid = false;
        const float enterSpeed = -player.vel.y;
        // A limp or rising body owns the player, not the controller: no
        // input, no gravity, no sweeps. The capsule is moved onto the body
        // after each physics step (PlayerAvatar::RagdollFollow, below).
        if (avatar.Spawned() && avatar.Ragdolled()) {
          player.vel = {};
          player.SnapRender();   // the rig owns the body: nothing to lerp
        } else {
          player.Update(kTickDt, ti, kindAt);
        }
        if (player.inLiquid && !wasInLiquid && enterSpeed > 2.0f) {
          // Crest height in metres, from the entry speed, capped: a splash from
          // a great fall is bigger, but not without limit — an unbounded
          // amplitude here would tilt the surface normal past the shore and the
          // whole lake would go black (the ripple steepness note in
          // raymarch.wgsl is the same trap).
          const float amp = std::min(0.02f + enterSpeed * 0.004f, 0.12f);
          WaveImpacts().Add(player.pos.x, player.pos.z, (float)now, amp);
        }
        wasInLiquid = player.inLiquid;
      }
      // --autofly-surface / --autofly-park, AS A HOOK (session.h section E).
      // The altitude pin and the park latch are argv state the FRAME layer
      // owns, and both have to apply BETWEEN player.Update and everything
      // that reads the position -- which is exactly here. Null in the game.
      if (w.afterPlayerUpdate) w.afterPlayerUpdate(player, tick);
      // THE COMMAND JOINS THE RECORD (package N3, sim/oprecord.h). Stashed
      // here rather than passed to SubmitTick at the bottom of the tick: the
      // command is decided at the TOP of the body and threading it through a
      // signature four harnesses share to carry a value only the game has is
      // the worse trade — the gen list is stashed for the same reason.
      sandvox::opstream::NoteTickInput(tick, ti);
      // ---- THE LOOK DELTA, ROUTED BY MODE, ONCE PER TICK ------------------
      // Both consumers integrate a whole tick's raw pixels at kTickDt now.
      // Feeding both would double-integrate the same motion into the swing
      // accumulator and bend every authored cut (D10 of the discrete-strikes
      // plan), so this is still an either/or — it simply happens on the tick
      // clock instead of the frame clock.
      if (CurrentTuning().melee.controlMode == 1) {
        melee.Feed(ti.lookDx, ti.lookDy);
      } else {
        strikePicker.Feed(ti.lookDx, ti.lookDy, kTickDt);
        // DISCRETE STRIKES: the click is the whole input, and the flick is
        // read at the tick that consumes the press edge — the freshest read
        // available, since the delta above is every pixel moved since the
        // previous tick. A strike fired from the buffer later still cuts the
        // direction that was flicked, aimed wherever the camera is THEN.
        if (ti.Pressed(TB_ATTACK) && meleeReady) {
          const StyleLibrary& styleLib = mobs.AttackStyles();
          // WHICH COMPASS: the sword's, or the fists'. Two maps rather than one
          // filtered set, because the two are different SHAPES (strokes.h
          // StyleLibrary::playerUnarmed) — a punch compass has a jab and a
          // cross where a sword's has an overhead and a thrust.
          const PlayerStrikeMap& map =
              meleeArmed ? styleLib.player : styleLib.playerUnarmed;
          float fx = 0, fy = 0;
          int si = -1;
          if (strikePicker.Pick(CurrentTuning().melee.pickMinSpeed, fx, fy))
            si = QuantizeStrike(map, fx, fy);
          if (si < 0) {
            // No flick: alternate the two horizontals so plain clicking is a
            // usable L/R rhythm rather than the same cut stamped.
            si = NeutralStrike(map, strikePicker.altRight);
            strikePicker.altRight = !strikePicker.altRight;
          }
          if (si >= 0) strikeQueued = si;
        }
      }

      // recenter the residency window on the player (between ticks only; at
      // most one 1-chunk shift per axis)
      IVec3 playerChunkNow{ifloor(player.pos.x) >> 4, ifloor(player.pos.y) >> 4,
                           ifloor(player.pos.z) >> 4};
      // ...and the CONTINUOUS position, for the far-plume crossfade weights.
      // The chunk coord above is what the window recentres on and is therefore
      // the wrong input for a weight: it moves 16 voxels at a time. See
      // world.h kGasFarEyeSlackVox. This is the only caller — a headless
      // harness never sets it and keeps the window-centre weights it had.
      if (world.farPlumes)
        world.farPlumes->SetEye({ifloor(player.pos.x), ifloor(player.pos.y),
                                 ifloor(player.pos.z)});

      // ---- THE INTEREST SET (src/sim/interest.h) -----------------------
      // Residency's input, built explicitly here rather than passed as a bare
      // IVec3, because THIS is the call site a second player arrives at. One
      // local player => one entry, and it is primary; the streaming and
      // cascade calls below are then behaviour-identical to what they were.
      // A remote player adds a chunk coord to this vector and nothing else
      // changes shape (what the window then DOES about it is the chunk
      // authority decision, DESIGN.md §10).
      InterestSet interest;
      interest.chunks.push_back(playerChunkNow);
      interest.primary = 0;
      uint32_t farCount = 0;
      {
        // ---- STREAM: the row that flying lights up --------------------
        // The toroidal window shift, chunk fetch/evict and the far-field
        // cascade recentre. THIS is the cost the Performance tab was reporting
        // as "input" — it had no timer, so it fell into the residual, and the
        // residual was billed to the input row.
        sandvox::PerfSpan spanStream(sandvox::PerfScope::Stream);
        stream.Update(interest, tick);
        // THE HORIZON ARRIVING. `far`/`fardown` compile on a background thread
        // (docs/PLAN_shader_compile.md package A) and this is the one place
        // that notices they landed. Nothing was recorded for the cascades
        // before that, so a wholesale refill is what puts terrain back past
        // the residency window. Fires exactly once.
        if (sim.PollFarPipelines()) far.FullRefill(interest);
        // ...and until it does, FarField MUST NOT DRAIN (2026-09-10). It used
        // to: PrepareTick popped, EncodeFarFill found no pipeline and dropped
        // the entries, and pending_ emptied — so for the whole compile (2.5 s
        // warm, 20.6 s on a cold shader cache, measured) the fog reported the
        // full 6.5 km horizon and the valid box reported every level marchable
        // over cascade buffers that were still zeros. Rays escaped through the
        // ground into the sky and the clouds were drawn UNDER the terrain,
        // then the refill landed and slammed the fog onto the window. Holding
        // the queue keeps both readings honest for those seconds — closed fog,
        // empty valid box — and costs nothing else, since the entries were
        // being thrown away anyway. Update() is held with it: recentring while
        // blind would pile incoming planes onto a queue the FullRefill above
        // is about to clear.
        const bool farBlind = sim.FarFillsDeferred();
        // The refill's per-tick slice, live from tuning so F5 moves it (see
        // Tuning::Render::farRefillRate — this is the knob that turned the
        // horizon's arrival from a 16 s 2 fps stall into a fog that opens).
        far.SetBulkCap((uint32_t)CurrentTuning().render.farRefillRate);
        // And ORDINARY TRAVEL's slice, the one a moving player actually feels
        // (Tuning::Render::farPlaneFillRate). Live from tuning for the same
        // reason: what it trades against is the sieve's per-entry GPU cost,
        // which a shader edit can move by more than 2x without a rebuild.
        far.SetPlaneCap((uint32_t)CurrentTuning().render.farPlaneFillRate);
        // far-field cascades track the same interest set (render-only)
        if (!farBlind) far.Update(interest);
        farCount = far.PrepareTick(ctx.queue, !farBlind);
        if (farCount) {
          g_farEntries += farCount;
          g_farTicks++;
          g_farBiggest = std::max<uint64_t>(g_farBiggest, farCount);
        }
      }
      // ---- GAME LOGIC: the rest of the tick body up to the submit ---------
      // Brush, laser, melee, spells, mob/avatar/debris PreTick, explosions.
      // Closed at `t0` below, where the submit sequence starts.
      sandvox::PerfSpan spanGame(sandvox::PerfScope::GameLogic);

      // (`ops` is aliased out of the caller's OpBatch at the top.)
      brush.radius = ui.brushRadius;
      brush.material = (uint32_t)ui.brushMaterial;

      // laser (PLAN §C1/C2): laser tool + LMB, or hold F from any tool.
      // Bodies are tested first — a mob limb or debris chunk in the beam
      // takes the hit instead of the wall behind it.
      struct LaserCut {
        uint64_t body = 0;
        Vec3 at{};
        float radius = 0;
        bool limb = false;  // a live mob limb carves; plain debris melts
      } laserCut;
      if (ti.Held(TB_LASER)) {
        const WorldSnapshot& lsnap = world.Snap();
        Vec3 fwd = cam.Forward();
        // MUZZLE, NOT EYE. The avatar's own head occupies the eye position, so
        // a ray cast from there hits the caster's skull on frame 1 and
        // avatar.Damage() below decapitates them instantly. Emit from in FRONT
        // of the head instead — the same reason the spell block muzzles its
        // bolt, and the same offset the beam sprites below already draw from,
        // so what cuts and what is visible are finally the same ray.
        const Vec3 muzzle = LaserMuzzle(player, cam);
        float gridDist = 1e9f;
        IVec3 hit{};
        if (lsnap.valid && lsnap.pick[0] != 0) {
          hit = {(int)lsnap.pick[2], (int)lsnap.pick[3], (int)lsnap.pick[4]};
          // PROJECTED onto fwd, not the euclidean distance from the muzzle.
          // The two distances compared below have to be the same measurement,
          // and they come from two DIFFERENT rays: bodyDist is along fwd from
          // the muzzle, while this pick cell was found by sim_pick.wgsl casting
          // from R.camPos (the EYE) — the muzzle is offset right and down of
          // that line, so a euclidean distance mixes in the ~0.86 vox lateral
          // displacement. That inflates nothing but shortens plenty: the eye
          // ray clips ground or a wall edge the muzzle ray misses, gridDist
          // comes back shorter than the mob actually is, and the body branch
          // never runs — which is "the laser stopped hurting mobs".
          gridDist =
              (Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f} - muzzle).dot(fwd);
        }
        float frac = 1.0f;
        const float kLaserRange = CurrentTuning().tools.laserRange;
        uint64_t hitBody = phys.CastRayBody(muzzle, fwd, kLaserRange, frac);
        float bodyDist = frac * kLaserRange;
        // A weapon must not cut its wielder (main.cpp melee sweep, same rule).
        // The muzzle above clears the head, but an arm swings through the beam
        // line constantly and a severed-then-still-owned part lingers there —
        // so the reject is by OWNERSHIP, not by distance. Dropping the hit
        // entirely (rather than falling through to the grid branch) is
        // deliberate: the beam is occluded by the limb it refuses to cut.
        if (hitBody != 0 && avatar.OwnsBody(hitBody)) hitBody = 0;

        if (hitBody != 0 && bodyDist < gridDist) {
          // body cut (PLAN §C2): mob limbs take damage (instant sever when
          // the beam crosses a joint); plain debris is MELTED where the beam
          // lands. The beam bores a channel tick by tick and the body splits
          // when that channel actually severs it (DebrisSystem::MeltBodyAt) —
          // no cutting plane is chosen, so what falls apart is decided by the
          // geometry the player carved, not by camera orientation.
          Vec3 hitPos = muzzle + fwd * bodyDist;
          // The avatar is checked alongside mobs, but the OwnsBody reject
          // above means this can no longer be one of the caster's LIVE parts.
          // It still has to run: a part that was severed and whose hold has
          // expired is back on MOVING and no longer owned, so it takes the
          // beam like any other debris — which is the intent. What it must
          // never again do is take the beam off the head it was fired from.
          if (avatar.Damage(hitBody, CurrentTuning().tools.laserDamage,
                            hitPos)) {
            // handled by the avatar
          } else if (mobs.Damage(hitBody, CurrentTuning().tools.laserDamage,
                                 hitPos)) {
            // A limb hit is now BOTH: the hp/sever logic above (joint
            // crossings, flinch, loco states) AND a real channel bored through
            // the flesh. Deferred like the melt below, for the same reason.
            //
            // Damage() may have severed the limb outright, in which case this
            // handle is no longer a live limb — the carve then simply misses
            // (CarveLimbRadial returns false) rather than touching stale state.
            laserCut = {hitBody, hitPos,
                        (float)CurrentTuning().tools.laserCarveRadius, true};
          } else {
            // Deferred: the melt needs the `spawns` list that debris.PreTick
            // fills further down, and the ray must be cast HERE where the
            // camera and physics state for this tick are current. Carrying the
            // hit forward is cheaper than reordering the tick.
            float br = (float)CurrentTuning().tools.laserMeltRadius;
            laserCut = {hitBody, hitPos + fwd * (br * 0.5f), br, false};
          }
        } else if (gridDist < 1e8f) {
          const int r = CurrentTuning().tools.laserMeltRadius;
          ops.push_back({hit.x, hit.y, hit.z, r, 0, 2u /*melt*/, 0, 0});
          // cutting through a support must drop the far side: rate-limited
          // island checks over the cut (support-loss flags catch the rest)
          if (tick % 8 == 0)
            debris.AddDestructionEvent(tick, {hit.x - r, hit.y - r, hit.z - r},
                                       {hit.x + r, hit.y + r, hit.z + r});
        }
      }

      // mob spawn (mob tool LMB, or M): drop the selected def at the picked
      // surface, feet on the last empty cell
      // The lab is mob-free by design (plan §4.1): a wandering mob is exactly
      // the confounding load the lab exists to exclude. Consume the request
      // so it cannot latch across a mode where it would fire.
      bool spawnMobNow = ti.Pressed(TB_SPAWN);
      if (labScene >= 0) spawnMobNow = false;
      // ---- --duel-dummy: one target, once, three metres ahead ---------------
      // Deferred to the tick loop rather than done at load because the player's
      // position and the terrain height under it are only settled here, and a
      // dummy spawned inside a hill is not a dummy. A mob with no AI stands
      // where it is put, so nothing else is needed to keep it still.
      if (w.duelDummy && !duelDummySpawned && avatar.Spawned()) {
        duelDummySpawned = true;
        int humanDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++)
          if (mobs.Defs()[i].name == "human") humanDef = (int)i;
        if (humanDef >= 0) {
          const MobDef& d = mobs.Defs()[humanDef];
          const Vec3 fwd = cam.Forward();
          const float dist = MetresToCells(3.0f);
          const int sx = ifloor(player.pos.x + fwd.x * dist) - d.prefab.size.x / 2;
          const int sz = ifloor(player.pos.z + fwd.z * dist) - d.prefab.size.z / 2;
          const int sy = World::TerrainHeight(sx + d.prefab.size.x / 2,
                                              sz + d.prefab.size.z / 2,
                                              kDefaultSeed) + 1;
          const uint64_t id = mobs.Spawn(humanDef, {sx, sy, sz});
          // ARMED, so the dummy is the same body a real opponent will be:
          // a sword in its hand is a limb the player's edge can bind against
          // and, in Phase C, an arm that will swing back.
          const ItemDef* sword = items.At(items.Find("sword"));
          if (id && sword) mobs.EquipItem(id, sword);
          std::printf("--duel-dummy: human at (%d,%d,%d)%s\n", sx, sy, sz,
                      id ? "" : " FAILED");
        } else {
          std::fprintf(stderr, "--duel-dummy: no \"human\" mob def\n");
        }
      }
      if (spawnMobNow) {
        const WorldSnapshot& msnap = world.Snap();
        if (msnap.valid && msnap.pick[0] != 0 && !mobs.Defs().empty()) {
          if (ui.mobSelected >= (int)mobs.Defs().size()) ui.mobSelected = 0;
          const MobDef& d = mobs.Defs()[ui.mobSelected];
          mobs.Spawn(ui.mobSelected,
                     {(int)msnap.pick[5] - d.prefab.size.x / 2,
                      (int)msnap.pick[6],
                      (int)msnap.pick[7] - d.prefab.size.z / 2});
        }
      }

      // ---- NPC AI panel: spawn / despawn / retarget (game/ai_behavior.h) ---
      //
      // Producers on the SAME path gameplay uses: MobSystem::Spawn, then
      // EquipItem, then SetMobBehavior. There is no dev-only entry into the AI,
      // which is why the panel's duelist and a duelist placed by content are
      // the same creature.
      if (labScene >= 0) {
        ui.aiSpawnDummy = ui.aiSpawnStatic = ui.aiSpawnDuelist = false;
        ui.aiSpawnOwn = false;
        ui.aiKillSpawned = false;
      }
      if (ui.aiSpawnDummy || ui.aiSpawnStatic || ui.aiSpawnDuelist ||
          ui.aiSpawnOwn) {
        // EMPTY MEANS "THE CREATURE'S OWN", resolved once the def is known --
        // see UIState::aiSpawnOwn for what the override was costing.
        const char* profile = ui.aiSpawnOwn         ? ""
                              : ui.aiSpawnDummy     ? "dummy"
                              : ui.aiSpawnStatic    ? "swordsman_static"
                                                    : "duelist";
        ui.aiSpawnDummy = ui.aiSpawnStatic = ui.aiSpawnDuelist = false;
        ui.aiSpawnOwn = false;
        // A humanoid that can actually HOLD the sword: picked by capability
        // (a `held_right` socket) rather than by name, so renaming an asset
        // cannot silently spawn an unarmed creature.
        // WHAT THE PANEL PICKED, resolved BY NAME at spawn time for the reason
        // the weapon pick below is: a def index is directory order, and the
        // list can be rebuilt by an R reload between the click and here.
        const std::string& creature =
            ui.aiCreatureNames.empty()
                ? avatarDefName
                : ui.aiCreatureNames[ui.aiCreaturePick <
                                             (int)ui.aiCreatureNames.size()
                                         ? ui.aiCreaturePick
                                         : 0];
        int aiDef = -1;
        for (size_t i = 0; i < mobs.Defs().size(); i++) {
          if (mobs.Defs()[i].FindSocket("held_right") < 0) continue;
          // Fall back to the old rule — first eligible def, preferring the
          // avatar's own species — if the pick names a def that has gone away
          // under an R reload. Never spawn nothing because a name went stale.
          if (aiDef < 0 || mobs.Defs()[i].name == avatarDefName) aiDef = (int)i;
          if (mobs.Defs()[i].name == creature) { aiDef = (int)i; break; }
        }
        // ...PLUS WHATEVER IS WRONG WITH IT. The ticked effects go through the
        // same MobSystem::DefWithEffects a bitten villager gets up as, so the
        // panel spawns the creature the GAME makes and not a dev-only object:
        // an authored `jujunud_zombie.json` wins if it exists, an identical
        // request the session already composed is reused, and otherwise the
        // recipe is built once. That is why `zombie` stopped being a row in the
        // creature combo — it was one composition of many, written down.
        if (aiDef >= 0 && !ui.aiEffectNames.empty()) {
          std::vector<std::string> fx;
          for (int i = 0; i < (int)ui.aiEffectNames.size(); i++)
            if (i < (int)ui.aiEffectOn.size() && ui.aiEffectOn[i] != 0)
              fx.push_back(ui.aiEffectNames[i]);
          if (!fx.empty()) {
            // By name, because composing appends to the def vector and any
            // index (or reference into it) taken above is stale afterwards.
            const std::string base = mobs.Defs()[aiDef].name;
            std::string flog;
            const int composed = mobs.DefWithEffects(base, fx, &flog);
            // A refusal is the derived-def cap or a missing factory, neither of
            // which is a reason to spawn nothing: the plain body is still the
            // creature you asked for, minus the modifier, and the log says so.
            if (composed >= 0) aiDef = composed;
            if (!flog.empty()) std::printf("%s", flog.c_str());
            if (composed < 0)
              std::printf("ai panel: could not compose %s + effects\n",
                          base.c_str());
          }
        }
        if (aiDef >= 0) {
          // Crosshair hit when there is one, otherwise a few metres ahead on
          // the ground: spawning behind you is useless for watching a duelist.
          const WorldSnapshot& asnap = world.Snap();
          IVec3 at{};
          const MobDef& d = mobs.Defs()[aiDef];
          if (asnap.valid && asnap.pick[0] != 0) {
            at = IVec3{(int)asnap.pick[5] - d.prefab.size.x / 2,
                       (int)asnap.pick[6],
                       (int)asnap.pick[7] - d.prefab.size.z / 2};
          } else {
            const Vec3 fwd = cam.Forward();
            const int sx = ifloor(player.pos.x + fwd.x * 40.0f);
            const int sz = ifloor(player.pos.z + fwd.z * 40.0f);
            at = IVec3{sx, World::TerrainHeight(sx, sz, kDefaultSeed) + 1, sz};
          }
          const uint64_t nid = mobs.Spawn(aiDef, at);
          if (nid != 0) {
            // WHAT THE PANEL PICKED, resolved by name at spawn time. Entry 0
            // is "fists" and Find() returns -1 for it, so the empty hand needs
            // no special case — At(-1) is nullptr and the mob spawns with
            // nothing in its fist, which is no longer a creature that cannot
            // fight: its `natural` weapons and its profile's fallback punches
            // are what it swings (docs/PLAN_impact_unarmed.md §3/§5).
            const std::string& pick =
                ui.aiWeaponNames[ui.aiWeaponPick < (int)ui.aiWeaponNames.size()
                                     ? ui.aiWeaponPick
                                     : 0];
            const ItemDef* weapon = items.At(items.Find(pick));
            if (weapon != nullptr) mobs.EquipItem(nid, weapon);
            // WHAT IT WEARS (UIState::aiOutfit). Through MobSystem::WearItem,
            // the path the player's own equip slots take, so a spawn dressed
            // here is a creature in armour and not a dev-only object: its
            // plate dents, its cloth burns, and its corpse can be looted.
            //
            // Slot by slot in EquipSlotId order, one piece per slot. The
            // random outfit and the dye are hashed off the tick and the mob id
            // rather than rand(), so a replay dresses the same crowd.
            {
              int dressed = 0;
              for (int s = 0; s < (int)ui.aiWearNames.size(); s++) {
                if (!EquipSlotIsWorn(s)) break;
                const ItemDef* piece = nullptr;
                const uint32_t roll = rng::Hash3(tick, (uint32_t)nid, (uint32_t)s);
                if (ui.aiOutfit == 1) {
                  // RANDOM CLOTHES: a dyeable piece of this slot's kind. The
                  // commoner set is chest / legs / boots, so a helmet slot has
                  // no candidates and stays bare, which is the point.
                  std::vector<const ItemDef*> cands;
                  for (const ItemDef& it : items.items)
                    if (it.dyeable && EquipSlotAccepts(s, it.kind))
                      cands.push_back(&it);
                  if (!cands.empty()) piece = cands[roll % cands.size()];
                } else if (ui.aiOutfit == 2) {
                  // FULL PLATE: the slot's `iron_*` piece. By name prefix
                  // rather than by material because the stock set is authored
                  // that way (scripts/gen_stock_armor.py) and nothing on an
                  // ItemDef says "this is armour rather than a shirt".
                  for (const ItemDef& it : items.items)
                    if (EquipSlotAccepts(s, it.kind) &&
                        it.name.rfind("iron_", 0) == 0) {
                      piece = &it;
                      break;
                    }
                } else if (ui.aiOutfit == 3) {
                  const int wp = s < (int)ui.aiWearPick.size() ? ui.aiWearPick[s]
                                                                : 0;
                  if (wp > 0 && wp < (int)ui.aiWearNames[s].size())
                    piece = items.At(items.Find(ui.aiWearNames[s][wp]));
                }
                if (piece == nullptr) continue;
                // A dyeable piece gets a colour, the same "full saturation at a
                // middling value" the wardrobe's random button uses: a random
                // point in the RGB cube is mostly mud.
                uint32_t dye = 0;
                if (piece->dyeable) {
                  float rgb[3];
                  DyeFromHsv((float)(roll % 3600u) / 3600.0f,
                             0.45f + (float)((roll >> 12) % 100u) / 100.0f * 0.5f,
                             0.35f + (float)((roll >> 20) % 100u) / 100.0f * 0.5f,
                             rgb);
                  dye = DyePack(rgb[0], rgb[1], rgb[2]);
                }
                if (mobs.WearItem(nid, piece, s, dye)) dressed++;
                else
                  std::printf("AI panel: \"%s\" would not go on %s\n",
                              piece->name.c_str(), d.name.c_str());
              }
              if (ui.aiOutfit != 0)
                std::printf("AI panel: %s spawned wearing %d piece(s)\n",
                            d.name.c_str(), dressed);
            }
            // THE SIDECAR'S OWN PROFILE when the button asked for it, and the
            // same `empty() ? "duelist" : behavior` fallback --shot-strike
            // uses, so a def that names none still gets something that fights.
            const std::string prof =
                *profile != '\0' ? std::string(profile)
                : d.behavior.empty() ? std::string("duelist")
                                     : d.behavior;
            if (!mobs.SetMobBehavior(nid, prof)) {
              // A NAMED PROFILE THAT IS NOT LOADED leaves the creature on the
              // legacy wander, where it walks off and never fights — and it
              // did it silently. The panel is a dev tool; say so.
              std::printf(
                  "AI panel: \"%s\" asked for behaviour \"%s\", which is not in"
                  " behaviors.json — it will not fight\n",
                  d.name.c_str(), prof.c_str());
            }
            aiSpawnedMobs.push_back(nid);
          }
        }
      }
      if (ui.aiKillSpawned) {
        ui.aiKillSpawned = false;
        // Kill rather than delete: a corpse ragdolls, its limbs become debris,
        // and the whole teardown path gets exercised. Silently dropping the
        // rig would be a second despawn implementation.
        for (uint64_t mid : aiSpawnedMobs)
          if (Mob* m = mobs.FindMobById(mid)) m->Die();
        aiSpawnedMobs.clear();
      }
      if (ui.aiRagdollSpawned) {
        ui.aiRagdollSpawned = false;
        for (uint64_t mid : aiSpawnedMobs)
          mobs.RagdollMob(mid, CurrentTuning().ragdoll.devSeconds);
      }
      if (ui.ragdollMe) {
        ui.ragdollMe = false;
        if (avatar.Spawned() && avatar.IsAlive()) {
          avatar.StartRagdoll(CurrentTuning().ragdoll.devSeconds, "dev button");
          // A nudge backwards and up so the body keels over instead of
          // folding straight down onto its own feet.
          const Vec3 back{-std::sin(avatarHeading), 0.35f,
                          -std::cos(avatarHeading)};
          avatar.SetLimbVelocities(back.normalized() * MetresToCells(1.5f));
        }
      }
      // ---- WARDROBE panel: make a set of clothes in a colour --------------
      //
      // Producers on the SAME paths the game uses: PlayerKit's own containers
      // and the ordinary equip slots, so a tunic this button made is a tunic,
      // not a dev-only object. The only thing the panel adds to an item that
      // picking one off the ground would not is the dye word.
      if (ui.wardrobeRandomColor) {
        ui.wardrobeRandomColor = false;
        // Hashed off the tick so it is reproducible from a replay and does not
        // reach for rand(). Full saturation at a middling value is where
        // clothes live: a random point in the RGB cube is mostly mud.
        const uint32_t h = rng::Hash3(tick, 0xD1E5u, 0x9E37u);
        const float hue = (float)(h % 3600u) / 3600.0f;
        const float sat = 0.45f + (float)((h >> 12) % 100u) / 100.0f * 0.5f;
        const float val = 0.35f + (float)((h >> 20) % 100u) / 100.0f * 0.5f;
        DyeFromHsv(hue, sat, val, ui.wardrobeColor);
      }
      if (ui.wardrobeSpawnSet || ui.wardrobeWearSet || ui.wardrobeDyeWorn) {
        const bool wear = ui.wardrobeWearSet;
        const bool redye = ui.wardrobeDyeWorn;
        ui.wardrobeSpawnSet = ui.wardrobeWearSet = ui.wardrobeDyeWorn = false;
        const uint32_t dye = DyePack(ui.wardrobeColor[0], ui.wardrobeColor[1],
                                     ui.wardrobeColor[2]);
        const std::string colour = DyeName(dye);
        if (redye) {
          // RE-DYE WHAT IS ON. Only the dyeable pieces: the wizard's robe is
          // painted in real colours and multiplying them by another colour is
          // not a feature (ItemDef::dyeable). The wear sync a few hundred lines
          // down notices the changed dye and rebuilds those shells, which is
          // why nothing here touches the rig.
          int n = 0;
          for (int s = 0; s < kEquipSlotCount; s++) {
            ItemStack& st = kit.equip.slots[s];
            const ItemDef* d = items.At(st.Empty() ? -1 : st.def);
            if (d == nullptr || !d->dyeable) continue;
            st.dye = dye;
            n++;
          }
          ui.wardrobeStatus = n ? ("re-dyed " + std::to_string(n) +
                                   " worn piece(s) " + colour)
                                : "nothing you are wearing takes a dye";
        } else {
          // THE PICKERS NAME THE PIECES; entry 0 of each is "(none)" and
          // Find() returns -1 for it, so an outfit of trousers and nothing
          // else needs no special case.
          auto picked = [&](const std::vector<std::string>& names, int pick) {
            return (pick > 0 && pick < (int)names.size()) ? names[pick]
                                                          : std::string();
          };
          const std::string want[3] = {
              picked(ui.wardrobeShirts, ui.wardrobeShirtPick),
              picked(ui.wardrobeLegs, ui.wardrobeLegsPick),
              picked(ui.wardrobeFeet, ui.wardrobeFeetPick)};
          int made = 0, refused = 0;
          std::string names;
          for (const std::string& w : want) {
            if (w.empty()) continue;
            const int di = items.Find(w);
            const ItemDef* d = items.At(di);
            if (d == nullptr) continue;
            // WEAR puts it in the equip slot its kind belongs to (swapping
            // out whatever was there, which lands back in the pack); the
            // plain spawn goes to the hotbar, falling back to the bag exactly
            // as a corpse's gear does (game/corpses.h).
            bool ok = false;
            if (wear) {
              const int slot = EquipSlotFor(d->kind, kit.equip);
              if (slot >= 0) {
                const ItemStack was = kit.equip.At(slot);
                if (!was.Empty()) kit.bag.Add(was.def, was.count, was.dye);
                kit.equip.slots[slot] = ItemStack{di, 1, dye};
                ok = true;
              }
            } else {
              ok = hotbar.Add(di, 1, dye) >= 0 ||
                   kit.bag.Add(di, 1, dye) >= 0;
            }
            if (ok) {
              if (!names.empty()) names += ", ";
              names += w;
              made++;
            } else {
              refused++;
            }
          }
          if (made == 0) {
            ui.wardrobeStatus = refused ? "no room for any of it"
                                        : "pick something first";
          } else {
            ui.wardrobeStatus = colour + " " + names +
                                (wear ? " — worn" : " — in your pack");
            if (refused)
              ui.wardrobeStatus += "  (" + std::to_string(refused) +
                                   " refused: no room)";
          }
        }
        ui.kitMessage = ui.wardrobeStatus;
        ui.kitMessageAge = 0.0f;
      }

      if (ui.aiApplyBehavior) {
        ui.aiApplyBehavior = false;
        if (ui.aiMobSelected >= 0 &&
            ui.aiMobSelected < (int)ui.aiMobIds.size() &&
            ui.aiBehaviorPick >= 0 &&
            ui.aiBehaviorPick < (int)ui.aiProfileNames.size())
          mobs.SetMobBehavior(ui.aiMobIds[ui.aiMobSelected],
                              ui.aiProfileNames[ui.aiBehaviorPick]);
      }

      // rolling sphere (K): a rigidbody ball, half the player's height in
      // diameter, made of the current brush material. The collider is a true
      // Jolt sphere (CreateSphereBody) so it rolls smoothly; rendering is a
      // scale-2 MICROVOXEL ball (PLAN §C) — twice the voxels across the same
      // radius, so the silhouette carries real curvature. Models are packed
      // lazily into the shared micro pool, one per material (micro voxels
      // bake material ids), cached, and the pool re-uploaded on first use.
      // Mass comes from the material's density — which is also what decides
      // how far the player can shove it.
      if (ui.spawnSphere) {
        ui.spawnSphere = false;
        const WorldSnapshot& ssnap = world.Snap();
        uint32_t sphereMat = (uint32_t)ui.brushMaterial;
        if (ssnap.valid && ssnap.pick[0] != 0 && sphereMat < mats.size()) {
          const float r = Player::kHalfY * 0.5f;  // vox: diameter = height/2
          const uint32_t kSphereScale = 2;        // micro voxels per world voxel
          const float rm = r * (float)kSphereScale;  // radius, micro voxels
          const int dims = (int)std::ceil(2.0f * rm);
          // brick coords run [0..dims); the ball centre sits mid-brick
          auto inBall = [&](int x, int y, int z) {
            float dx = x + 0.5f - dims * 0.5f, dy = y + 0.5f - dims * 0.5f,
                  dz = z + 0.5f - dims * 0.5f;
            return dx * dx + dy * dy + dz * dz <= rm * rm;
          };
          auto mit = sphereModels.find(sphereMat);
          if (mit == sphereModels.end() && sphereMat <= 255) {
            std::vector<PrefabVoxel> mv;
            for (int z = 0; z < dims; z++)
              for (int y = 0; y < dims; y++)
                for (int x = 0; x < dims; x++)
                  if (inBall(x, y, z))
                    mv.push_back({(int16_t)x, (int16_t)y, (int16_t)z,
                                  (uint16_t)sphereMat});
            std::string slog;
            int mi = MicroBodyPack(mbSet, mv, {dims, dims, dims}, kSphereScale,
                                   "sphere:" + mats[sphereMat].name, slog);
            if (!slog.empty()) std::fprintf(stderr, "%s", slog.c_str());
            MicroBodyRef packed{};
            if (mi >= 0) {
              packed.model = (uint32_t)mi;
              packed.skinScale = kSphereScale;
              sim.UploadMicroBodies(ctx.queue, mbSet);
            }
            // a failed pack caches as invalid: fall back to the cube path
            // below rather than re-attempting (and re-logging) every K press
            mit = sphereModels.emplace(sphereMat, packed).first;
          }
          MicroBodyRef ref =
              mit != sphereModels.end() ? mit->second : MicroBodyRef{};

          // sphere centre drops just above the picked surface cell
          Vec3 center{(float)ssnap.pick[5] + 0.5f,
                      (float)ssnap.pick[6] + r + 1.5f,
                      (float)ssnap.pick[7] + 0.5f};
          std::vector<DebrisVoxel> ball;
          BodyTransform sxf{};
          sxf.quat[3] = 1;
          uint64_t sh = 0;
          if (ref.Valid()) {
            // micro body: min-corner origin shared by the brick march and the
            // collider (sphere shape offset to the brick centre). Body voxels
            // are in MICRO units, which is what settle-back's downsample and
            // AdoptBody's radius calculation expect.
            for (int z = 0; z < dims; z++)
              for (int y = 0; y < dims; y++)
                for (int x = 0; x < dims; x++)
                  if (inBall(x, y, z))
                    ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0,
                                    (uint16_t)sphereMat});
            sxf.pos = center - Vec3{r, r, r};
            sh = phys.CreateSphereBody(center, r,
                                       (float)mats[sphereMat].gpu.density,
                                       Vec3{r, r, r});
          } else {
            // cube-path fallback (material id > 255 or pack failure):
            // world-unit ball centered on the body origin, as before
            int ext = (int)std::ceil(r);
            for (int z = -ext; z < ext; z++)
              for (int y = -ext; y < ext; y++)
                for (int x = -ext; x < ext; x++) {
                  float dx = x + 0.5f, dy = y + 0.5f, dz = z + 0.5f;
                  if (dx * dx + dy * dy + dz * dz <= r * r)
                    ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0,
                                    (uint16_t)sphereMat});
                }
            sxf.pos = center;
            sh = phys.CreateSphereBody(center, r,
                                       (float)mats[sphereMat].gpu.density);
          }
          if (sh) debris.AdoptBody(sh, std::move(ball), sxf, ref);
        }
      }

      // ---- MLS-MPM fluid pour (docs/PLAN_mpm_fluids.md prototype) ----------
      // Hold LMB with the mpm tool: a small sphere of cells above the brush
      // target gains 8 particles each (the rest density), per tick, budget
      // permitting. Spawn data is part of the tick's input stream — positions
      // are jittered by a hash of (tick, index), never by frame state, so a
      // replayed op stream reproduces the pour exactly.
      // (`fluidSpawns` is aliased out of the caller's OpBatch.)
      if (ui.clearFluid) {
        ui.clearFluid = false;
        // The count is GPU-owned now: zero the live word directly. Deferred
        // queue writes drain at the head of the NEXT command buffer — this
        // tick's — so the seam's compaction reads 0 and every particle is
        // gone before the substeps run (the deferred-WriteBuffer ordering
        // gotcha, used in the right direction for once).
        uint32_t zero = 0;
        ctx.queue.WriteBuffer(world.fluidArgsStage, 7 * 4, &zero, 4);
        fluidCount = 0;
        fluidPendingSpawns.clear();
      }
      if (ui.tool == UIState::kToolFluid && !ui.magicMode &&
          ti.Held(TB_ATTACK)) {
        const WorldSnapshot& fsnap = world.Snap();
        IVec3 at;
        if (fsnap.valid && fsnap.pick[0] != 0) {
          at = {(int)fsnap.pick[5], (int)fsnap.pick[6] + 2, (int)fsnap.pick[7]};
        } else {
          Vec3 p = player.EyePos() + cam.Forward() * 24.0f;
          at = {ifloor(p.x), ifloor(p.y), ifloor(p.z)};
        }
        const int rr = std::min(std::max(ui.brushRadius / 2, 1), 3);
        const uint32_t fluidSpecies = (uint32_t)ui.fluidSpecies & 3u;
        fluidSpeciesMat[fluidSpecies] = fluidCueMat;
        for (int z = -rr; z <= rr && fluidSpawns.size() < kMaxFluidSpawnsPerTick; z++)
          for (int y = -rr; y <= rr; y++)
            for (int x = -rr; x <= rr; x++) {
              if (x * x + y * y + z * z > rr * rr) continue;
              // Budget charged BEFORE emitting (rule 2): a cell that does not
              // fit its 8 particles is refused whole.
              if (fluidCount + fluidSpawns.size() + 8 > kFluidCap) break;
              if (fluidSpawns.size() + 8 > kMaxFluidSpawnsPerTick) break;
              for (int s = 0; s < 8; s++) {
                // 8 per cell on the half-cell lattice (rest density), with a
                // deterministic sub-lattice jitter so columns don't stack into
                // visible strings.
                uint32_t h = (tick * 9781u + (uint32_t)fluidSpawns.size() * 6271u) *
                                 747796405u + 2891336453u;
                FluidSpawnOp op{};
                op.px = ((at.x + x) << 16) + ((s & 1) ? 49152 : 16384) +
                        (int32_t)(h % 8192u) - 4096;
                op.py = ((at.y + y) << 16) + ((s & 2) ? 49152 : 16384) +
                        (int32_t)((h >> 13) % 8192u) - 4096;
                op.pz = ((at.z + z) << 16) + ((s & 4) ? 49152 : 16384) +
                        (int32_t)((h >> 19) % 8192u) - 4096;
                op.vx = 0; op.vy = -19661; op.vz = 0;  // gentle -0.3 cells/tick
                op.species = fluidSpecies;
                op.mat = fluidCueMat;
                fluidSpawns.push_back(op);
              }
            }
      }

      BrushOp op;
      if (brushActive && ti.Held(TB_ATTACK) &&
          brush.BuildOp(world.Snap(), player.EyePos(), cam.Forward(), false, op))
        ops.push_back(op);
      if (brushActive && ti.Held(TB_ALT) &&
          brush.BuildOp(world.Snap(), player.EyePos(), cam.Forward(), true, op)) {
        ops.push_back(op);
        // erasing can cut supports: queue an island check around the hole
        debris.AddDestructionEvent(tick, {op.x - op.radius, op.y - op.radius, op.z - op.radius},
                                   {op.x + op.radius, op.y + op.radius, op.z + op.radius});
      }

      // prefab placement: stamp at the last-empty pick cell, anchored at the
      // rotated footprint's bottom center
      if (ti.Pressed(TB_PLACE)) {
        const WorldSnapshot& snap = world.Snap();
        if (snap.valid && snap.pick[0] != 0 && !prefabs.empty() &&
            ui.prefabSelected < (int)prefabs.size()) {
          const Prefab& pf = prefabs[ui.prefabSelected];
          IVec3 rs = PrefabPlacer::RotatedSize(pf, ui.prefabRot);
          IVec3 at{(int)snap.pick[5] - rs.x / 2, (int)snap.pick[6],
                   (int)snap.pick[7] - rs.z / 2};
          IVec3 blo, bhi;
          placer.Place(pf, at, ui.prefabRot, ui.prefabOverwrite, mats, blo, bhi);
          stream.MarkModifiedBox(blo, bhi);
        }
      }

      // Declared before mobs.PreTick so bleed spray and dismemberment gore
      // share the one per-tick spawn stream with debris shatter — the ring and
      // its 4096-op budget are global, so a single list is what keeps the two
      // systems honest about the shared limit.
      // (`spawns` is aliased out of the caller's OpBatch.)
      // Declared here rather than beside debris.PreTick because per-voxel limb
      // burning emits REAL fire voxels into the grid, and mobs run first.
      // (`cellOps` is aliased out of the caller's OpBatch.)

      // The day phase both body-burn passes gate their reactions on, taken
      // from the ONE function that also puts it on TickParams — see
      // sim/reactcpu.h for why the CPU has to agree with the GPU here.
      mobs.SetDayPhase(DayPhaseNow(tick));
      debris.SetDayPhase(DayPhaseNow(tick));

      // WHO THE NPCs ARE FIGHTING, pushed once per TICK rather than per frame.
      // The tick loop runs 0..4 times per frame, and a target position sampled
      // on the frame clock would make an NPC's decisions a function of frame
      // rate — the same reason the burn pass and the gait live in here.
      // Deliberately the capsule, not the avatar rig: the capsule is what the
      // player actually occupies, and it exists even before the avatar spawns.
      mobs.SetPlayerActor(player.pos, Player::kHalfXZ, Player::kHalfY * 2.0f,
                          ui.playerAlive);

      // mobs: kinematic walk drive, terrain anchors for ManageTerrain,
      // bleeding ops, per-voxel burning — must run before debris.PreTick
      // consumes the anchors
      mobs.PreTick(tick, world, ops, cellOps, spawns);

      // ---- player avatar ----
      // Same slot in the tick order as mobs, and for the same reason: it
      // drives kinematic bodies and appends bleeding ops that debris.PreTick
      // must see. The body's FACING is decided here rather than inside the
      // avatar because it is a game-design policy, not a rig property: in
      // first person the body always faces the camera (you are looking down
      // its own axis), while in third person it turns toward its MOTION and
      // only snaps to the camera when standing still — which is what stops
      // the character from moon-walking sideways across the screen.
      {
        const auto& av = CurrentTuning().avatar;
        // Camera yaw and rig heading use different conventions: Camera's
        // forward is (cos yaw, ., sin yaw) while a mob's is (sin h, ., cos h),
        // so h = pi/2 - yaw. Getting this wrong makes the avatar face 90
        // degrees off its travel direction.
        const float camHeading = 1.5707963f - cam.yaw;
        // WHERE THE BODY FACES is a policy, not a rig property, and it now
        // lives in ResolveAvatarHeading (game/thirdperson.h) rather than
        // inline here. It was moved because both bugs it has had — an inverted
        // glance, and a dead zone with no restoring term that froze the arms
        // off-view — were invisible to every gate, for the simple reason that
        // the policy only ever ran inside this render loop. It is pure, so the
        // selftest can drive it directly.
        avatarHeading = ResolveAvatarHeading(
            camMode, camHeading, avatarHeading,
            Vec3{player.vel.x, 0, player.vel.z}, kTickDt);

        // FLY MODE HAS NO BODY. Two things go wrong otherwise, and the second
        // one is what makes flying feel possessed:
        //   1. The rig walks/IKs against ground it is nowhere near, so the
        //      avatar flails or stretches toward the terrain below.
        //   2. Far worse — the 16 limb bodies are real Jolt bodies, and
        //      PlayerPushOut resolves the player against everything it
        //      overlaps. Flying leaves the player sitting inside their OWN
        //      limbs, so the body shoves its own player around the sky. Fly
        //      mode already ignores voxel collision for exactly this reason;
        //      the avatar has to follow the same rule.
        const bool wantAvatar = av.enabled && !player.fly;
        if (wantAvatar && avatar.HasDef() && !avatar.Spawned()) {
          avatar.Spawn(player, avatarHeading);
          // A new rig wears nothing (Mob::BuildRig clears its shells), so the
          // armour sync below has to be offered every slot again.
          for (std::string& w : wearTried) w.clear();
          for (uint32_t& d : wearDye) d = 0;
          tpRig.Snap();   // re-entering from fly: don't ease across the gap
        }
        if (!wantAvatar && avatar.Spawned()) avatar.Despawn();
        // ---- melee: drive the swing, then pose the arm (game/melee.h) -------
        // BEFORE PreTick, because PreTick is what flattens the pose and
        // submits the kinematic limb targets — a weapon pose pushed in after
        // it would be a frame late and the blade would trail the mouse.
        {
          // THE BASIS THE STROKE IS EXPRESSED IN is the camera's, yawed back
          // toward the body by the neck's own law (melee.aimYaw /
          // aimReleaseYaw; game/thirdperson.h ResolveSwingBasis). Without
          // this, a third-person camera orbited behind the character made
          // every strike cut backwards at the lens. Resolved once per tick
          // off the heading the policy above just produced, so the swing and
          // the head agree about where "behind" is. No body (fly mode): the
          // raw camera, as before.
          Vec3 swRight = cam.Right(), swUp = cam.Up(), swFwd = cam.Forward();
          if (avatar.Spawned())
            ResolveSwingBasis(camHeading, avatarHeading, cam.Right(), cam.Up(),
                              cam.Forward(), swRight, swUp, swFwd);
          if (avatar.Spawned()) {
            // Equip/unequip only on a CHANGE. EquipItem builds a body and a
            // joint, so calling it every tick with the same weapon would
            // rebuild the sword 60 times a second; comparing against what is
            // already in the hand keeps this a no-op in the common case.
            //
            // BEFORE melee.Update, not after: the arm read below needs the item
            // in the hand to know WHICH arm is the weapon arm, and the tick the
            // player selects the blade and clicks can be the same tick.
            const ItemDef* want = meleeArmed ? heldItem : nullptr;
            const std::string wantName = want ? want->name : std::string();
            if (avatar.HeldItem() != wantName) avatar.EquipItem(want);
            // WHERE THE BLADE IS, so taking control of it is not a teleport:
            // the stroke seeds itself from the live point AND the live hand,
            // takes its blade length from the pair, and bounds itself by the
            // rig's own reach (game/melee.h SetStroke).
            Vec3 handNow, tipNow, flatNow;
            float reachNow = 0;
            if (avatar.WeaponStrokePose(handNow, tipNow, flatNow, reachNow))
              melee.SetStroke(handNow, tipNow, flatNow, reachNow);
            else
              melee.ClearArm();
            // ...and where the avatar's own head is, so neither an authored
            // windup nor a freeform drag sweeps the blade through it
            // (melee.h SetKeepOut; the clamp lives in RebuildFrame).
            {
              Vec3 kc;
              float kr = 0;
              if (avatar.HeadKeepOut(kc, kr))
                melee.SetKeepOut(kc, kr);
              else
                melee.ClearKeepOut();
            }
            // The NPC driver always did this and the player's never had —
            // without it the asymmetric azimuth window (azOut on the weapon
            // side) assumes a right-handed rig whatever the avatar holds.
            melee.SetHandSign(avatar.HandSign());
          } else {
            melee.ClearArm();
          }
          // ---- WHICH MODE FEEDS THE DRIVER (melee.controlMode; tuning.h) ---
          // Read HERE and nowhere else. Downstream, the pose push, Arrest and
          // Nudge are mode-blind; the sweep and the whoosh key on
          // `melee.Cutting() || playerStrike.Cutting()`, which needs no mode
          // read because a freeform tick keeps the cursor Idle.
          const int meleeMode = CurrentTuning().melee.controlMode;
          // The program entered its cut THIS tick (the discrete whoosh edge —
          // see the whoosh block below for why Slash cannot be the trigger).
          bool strikeCutEdge = false;
          if (meleeMode == 1) {
            // FREEFORM: the original law, untouched. A mode flip mid-swing
            // (F5) drops any live program rather than leaving it half-run.
            playerStrike.Reset();
            strikeQueued = strikeBuffered = -1;
            melee.Update(kTickDt, meleeReady && ti.Held(TB_ATTACK),
                         meleeReady, swRight, swUp, swFwd);
          } else {
            // DISCRETE: consume the press latch, then step the program. Begin
            // and first step land on the SAME tick, exactly as the NPC's
            // BeginStroke/StepStroke pair does.
            if (!meleeReady || !avatar.Spawned()) {
              // Weapon stowed (or body gone) mid-swing: drop the claim the
              // same way MobSystem's teardown guard does.
              playerStrike.Reset();
              strikeBuffered = -1;
              if (avatar.Spawned()) avatar.ClearStrikeEffector();
            }
            if (strikeQueued >= 0 && meleeReady && avatar.Spawned()) {
              if (!playerStrike.Active()) {
                if (const AttackStyle* sty =
                        mobs.AttackStyles().At(strikeQueued)) {
                  // ---- POINT THE DRIVER AT THE PART THIS STYLE SWINGS -----
                  // Exactly what MobSystem::BeginStroke does, and through the
                  // same call: `player_punch_r` names `fist.R`, so the arm
                  // chain is claimed on the fist and `avatar.WeaponEdge`
                  // reports the knuckles. A style whose weapon this body has
                  // not got refuses HERE, before the program begins, so a
                  // strike never runs with no edge on the end of it.
                  if (avatar.ArmForStyle(*sty)) {
                    // Fresh sliders per swing — MobSystem::BeginStroke says
                    // why (MeleeTuning is a copy; F5 must reach the next cut).
                    ApplyMeleeTuning(melee.tuning);
                    // The seed is (who, when), like the NPC's; the player's
                    // styles author jitter 0, so it only matters if an author
                    // turns jitter back on — and then it still replays.
                    BeginStrokeProgram(playerStrike, *sty, strikeQueued,
                                       rng::Hash3(0x504Cu, tick, 0x5747u));
                    // ...and the style's body animation, exactly as the NPC's
                    // BeginStroke does (strokes.h AttackStyle::clip).
                    if (!sty->clip.empty()) avatar.PlayClip(sty->clip);
                  }
                }
              } else if (playerStrike.phase == StrokeCursor::Phase::Cut ||
                         playerStrike.phase == StrokeCursor::Phase::Recover) {
                // ONE strike banks mid-swing (last click wins); a click during
                // the windup is dropped — the windup IS the commitment.
                strikeBuffered = strikeQueued;
              }
              strikeQueued = -1;
            }
            bool stepped = false;
            if (playerStrike.Active()) {
              const AttackStyle* sty = mobs.AttackStyles().At(playerStrike.style);
              if (sty == nullptr) {
                // The style library reloaded out from under a live swing.
                playerStrike.Reset();
              } else {
                const bool wasCutting = playerStrike.Cutting();
                // ---- THE AIM: WHERE THE CROSSHAIR IS, NOT WHERE IT POINTS --
                //
                // This used to pass (0, 0, 0) — "the camera IS the aim" — and
                // that is true of a DIRECTION and false of a BLOW. The stroke
                // is a bearing about the arm's own pivot (strokes.h
                // StrokeAimAt), and the shoulder sits a couple of voxels under
                // the eye and a couple to the side of it; copying the camera's
                // bearing therefore lands the fist a whole shoulder offset low
                // and wide of whatever is under the crosshair. At sword range
                // that is a few degrees. AT PUNCHING RANGE IT IS THE
                // DIFFERENCE BETWEEN A HEAD AND A COLLARBONE, which is the
                // "my punches don't go where I'm pointing" report.
                //
                // So resolve a POINT and take the bearing to it. Nearest of:
                //   * the first dynamic BODY down the crosshair line (a mob's
                //     head — the case the whole thing is for), the rig's own
                //     limbs excluded for the reason the E ray excludes them
                //     (the eye sits inside your own head collider and Jolt
                //     reports a shape the ray starts in as a hit at t=0);
                //   * the first solid VOXEL, marched on the CPU mirror. Not
                //     `snap.pick`, which the brush reads: that ray starts at
                //     the RENDER camera, and a punch may not move because the
                //     player pushed the third-person boom out.
                //   * failing both, a point far down the line, which reproduces
                //     the old camera-parallel aim to within a few degrees —
                //     so a strike at open air is unchanged and the two cases
                //     meet continuously instead of snapping.
                //
                // From `player.EyePos()` along `cam.Forward()`, the same pair
                // the brush, the laser and the grenade use, so the camera
                // cannot change where a strike lands.
                float aimAz = 0, aimEl = 0, aimDist = 0;
                {
                  Vec3 pivot;
                  if (avatar.StrokePivotWorld(pivot)) {
                    // Far enough that the fallback is effectively the camera
                    // line, short enough to stay inside the CPU mirror's
                    // 3x3x3 window for the voxel half (world.h KindAt).
                    constexpr float kAimRange = 40.0f;
                    const Vec3 eye = player.EyePos();
                    const Vec3 look = cam.Forward();
                    float hitDist = kAimRange;
                    strikeIgnore.clear();
                    avatar.AppendLiveLimbBodies(strikeIgnore);
                    float frac = 1.0f;
                    if (phys.CastRayBody(eye, look, kAimRange, frac,
                                         strikeIgnore))
                      hitDist = std::min(hitDist, frac * kAimRange);
                    // The voxel half, quarter-voxel steps from a half-voxel
                    // out (step 0 is the cell the eye is already in). Unknown
                    // is NOT a hit — the projectile convention, and the mirror
                    // answers Unknown past ~48 voxels.
                    for (float d = 0.5f; d < hitDist; d += 0.25f) {
                      const Vec3 p = eye + look * d;
                      if (kindAt(IVec3{ifloor(p.x), ifloor(p.y),
                                       ifloor(p.z)}) == CellKind::Solid) {
                        hitDist = d;
                        break;
                      }
                    }
                    StrokeAimAt(pivot, eye + look * hitDist, swRight, swUp,
                                swFwd, aimAz, aimEl, aimDist);
                  }
                }
                const StrokeStepResult r = StepStrokeProgram(
                    playerStrike, sty, melee, aimAz, aimEl, aimDist, kTickDt,
                    swRight, swUp, swFwd);
                stepped = r != StrokeStepResult::Idle;
                strikeCutEdge = playerStrike.Cutting() && !wasCutting;
                if (r == StrokeStepResult::Finished) {
                  playerStrike.Reset();
                  // ...and hand the part back, like MobSystem::StepStroke's
                  // Finished branch. With a sword drawn this falls straight
                  // through to the held item, so an armed player's arm claim
                  // between strikes is exactly what it always was.
                  avatar.ClearStrikeEffector();
                  // Chain the banked strike: promoted to the latch, so it
                  // begins on the next tick through the same door as a fresh
                  // click (one tick of gap, invisible at 30 Hz).
                  if (strikeBuffered >= 0) {
                    strikeQueued = strikeBuffered;
                    strikeBuffered = -1;
                  }
                }
              }
            }
            if (!stepped) {
              // No program stepped the driver this tick: it idles/unwinds
              // exactly as a released button always did, so the arm hands
              // back over the usual ramp. ONE advance per tick either way.
              melee.Update(kTickDt, false, meleeReady, swRight, swUp, swFwd);
            }
          }
          // ---- THE SWING WHOOSH, on the EDGE into Slash --------------------
          // A commit is a moment, so this fires once per cut rather than on
          // every tick the slash is live. Latched (not played here) because
          // this is inside the tick loop and audio drains once per frame.
          //
          // THE VOLUME AND PITCH COME OFF THE SPEED THE GAME ACTUALLY READ,
          // not off the tuning's threshold, so a whoosh is the audible half of
          // "speed is the damage" (melee.h note 2): the player hears the same
          // number the damage curve used, and a lazy wave under
          // combatfx.whooshMinSpeed makes no sound at all rather than a quiet
          // one — which is the honest report that it was not a cut.
          //
          // TWO TRIGGERS, ONE PER MODE, never both: freeform commits Slash by
          // mouse speed, but an authored THRUST drives mostly the radial
          // channel, which commitSpeed never sees — it can finish its whole
          // cut without ever entering Slash (the player-styles gate found
          // exactly this), so in discrete mode the cue keys on the PROGRAM's
          // own cut edge instead. OR-ing the two would whoosh a committed
          // discrete cut twice, one tick apart.
          const bool slashEdge = melee.Phase() == SwingPhase::Slash &&
                                 meleePhasePrev != SwingPhase::Slash;
          if (meleeMode == 0 ? strikeCutEdge : slashEdge) {
            const Tuning::CombatFx& fx = CurrentTuning().combatfx;
            const float lo = fx.whooshMinSpeed;
            const float hi = std::max(melee.tuning.commitSpeed, lo + 1.0f);
            const float sp = melee.MouseSpeed();
            if (sp > lo) {
              combatWhooshCue.pending = true;
              combatWhooshCue.power = std::clamp((sp - lo) / (hi - lo), 0.0f, 1.0f);
              // A POINT ALONG THE BLADE, not either end of it, and the SAME
              // point the voice then follows for the length of the sample (the
              // audio block moves it every frame). The hand alone was where this
              // used to sit: safe, but it barely travels, so a cut across the
              // body made no pan at all; the tip alone swings a metre wide of the
              // player holding it. `combatfx.whooshEdgeFrac` is the dial.
              Vec3 eb, et, ef;
              float ehw = 0;
              combatWhooshCue.at =
                  avatar.WeaponEdge(eb, et, ehw, &ef)
                      ? eb + (et - eb) * fx.whooshEdgeFrac
                      : player.pos;
            }
          }
          meleePhasePrev = melee.Phase();
          if (avatar.Spawned()) {
            // Weight rises while the weapon is up and FADES over the releasing
            // recover, so the arm is handed back to the walk cycle across a
            // couple of hundred ms instead of being dropped in one tick from
            // wherever the player left it (melee.h PoseWeight). The whole pose
            // travels as ONE value — hand, blade axis, blade roll and the
            // elbow's bend pole — because the rig needs all four to put the
            // sword where the stroke says it is.
            avatar.SetWeaponPose(melee.Pose());
          }
        }
        // ---- ARMOUR: what the equipment says vs what the body wears --------
        //
        // The SAME change-detection seam the weapon uses one block up, and in
        // the same place in the frame for the same reason: WearItem builds
        // bodies and joints, so it must run once per CHANGE rather than once
        // per tick, and it must run before PreTick flattens the pose and
        // submits the kinematic targets — a shell appended after that would
        // sit at its spawn pose for a tick and visibly snap into place.
        //
        // Compared BY NAME, which is also what makes this survive an R
        // hot-reload: library indices renumber, the name does not. A respawn
        // clears the rig's shells (BuildRig) and this loop simply puts them
        // back on the next tick, with no despawn/respawn bookkeeping anywhere.
        //
        // `wearTried` is what stops a REFUSED piece from being retried every
        // tick. Comparing against what the body is actually wearing is not
        // enough on its own: a piece that finds no limb to hang on (a helm on
        // a headless mob) leaves the slot un-worn, so the two would disagree
        // forever and WearItem would rebuild nothing, loudly, 30 times a
        // second. This records the last ATTEMPT, which the slot changing is
        // what clears.
        if (avatar.Spawned()) {
          for (int s = 0; s < kEquipSlotCount; s++) {
            if (!EquipSlotIsWorn(s)) continue;
            const ItemStack& st = kit.equip.At(s);
            const ItemDef* want = items.At(st.Empty() ? -1 : st.def);
            const std::string wantName = want ? want->name : std::string();
            const uint32_t wantDye = st.Empty() ? 0u : st.dye;
            if (avatar.WornItem(s) == wantName && wearTried[s] == wantName &&
                wearDye[s] == wantDye)
              continue;
            if (wearTried[s] == wantName && wearDye[s] == wantDye &&
                avatar.WornItem(s).empty() && !wantName.empty())
              continue;   // already refused this one; nothing has changed
            // TAKING IT OFF KEEPS ITS WOUNDS. The shells are the only place
            // the damage lives while the piece is on, and they are destroyed
            // with the slots — so it is read out here, one call before the
            // rig forgets it, and handed back on the next wear. Without this
            // pair, changing boots mends the pair you took off.
            const std::string had = avatar.WornItem(s);
            if (!had.empty()) {
              WornDamage d;
              if (avatar.CaptureWorn(s, d)) kit.SetDamage(had, std::move(d));
            }
            wearTried[s] = wantName;
            wearDye[s] = wantDye;
            if (wantName.empty()) {
              avatar.UnwearItem(s);
            } else if (!avatar.WearItem(want, s, kit.Damage(wantName),
                                        wantDye)) {
              ui.kitMessage = "that does not fit you";
              ui.kitMessageAge = 0.0f;
            }
          }
        }
        // Head look, the other half of the turn policy above: whatever yaw the
        // body did NOT take is what the head is asked for. Computed from the
        // post-turn heading so the two never disagree by a tick — and passed
        // unclamped, because SetLook clamps against the same headLookYaw this
        // block just used and a rig that quietly over-rotates when the two
        // drift is worse than a head that stops at its stop.
        //
        // Third person gets it too, and it is arguably more valuable there:
        // the body faces its travel direction, so strafing or running past a
        // target is exactly when you want the character visibly looking where
        // the player is looking.
        if (avatar.Spawned()) {
          float lookRel = camHeading - avatarHeading;
          while (lookRel > 3.14159265f) lookRel -= 6.2831853f;
          while (lookRel < -3.14159265f) lookRel += 6.2831853f;
          float lookPitch = cam.pitch;
          // WHILE THE CHARACTER SCREEN IS OPEN, the head follows the CURSOR
          // over the portrait instead of the camera — the camera is not
          // moving, so the ordinary rule would leave the character staring
          // fixedly past you while you look them over.
          //
          // The panel reports where the pointer is (portraitLook, normalized
          // to the frame) and this turns it into a look; posing the rig stays
          // game-side, which is why the UI reports a cursor rather than a
          // pose. SetLook clamps against the rig's own neck limits, so the
          // generous gains here cannot over-rotate anything.
          if (ui.inventoryOpen && ui.portraitLookValid) {
            lookRel = ui.portraitLook[0] * 0.9f;
            lookPitch = ui.portraitLook[1] * 0.5f;
          }
          avatar.SetLook(lookRel, lookPitch);
        }
        if (avatar.Spawned())
          avatar.PreTick(tick, player, avatarHeading, kTickDt, world, ops,
                         cellOps,
                         spawns);
        // DEAD AVATAR: HOLD, AND WAIT TO BE ASKED. Nothing rebuilds the body
        // on a timer any more, and NOTHING OPENS A MENU OVER THE DEATH either:
        // the corpse lies where it fell (its parts are DebrisSystem's and
        // settle like any other debris) and the screen comes up when — and
        // only when — you press I for it. A panel that snaps open the instant
        // you die takes the view away at the one moment you want to look at it,
        // and the readout it holds is not going anywhere: every value on it is
        // the photograph the dying observer took, so a death can still be READ
        // ten seconds later — which limb was gone, what was alight, what was
        // still worn, and what the engine says killed you. The HUD carries the
        // only thing that has to be said immediately (overlay.cpp: "DEAD - I to
        // respawn"). `respawnDelay` is the minimum the body lies there before
        // the button will take the press, which keeps a fumbled click from
        // erasing the evidence in the same second it appeared (0 in the tuner
        // disables the wait).
        if (avatar.Spawned() && !avatar.IsAlive()) {
          if (!ui.deathScreen) {
            ui.deathScreen = true;
            ui.deathScreenOpened = false;
            ui.respawnRequest = false;
            ui.deathRespawnAfter = av.respawnDelay;
            respawnTimer = 0;
          }
          respawnTimer += kTickDt;
          ui.deathHoldSec = respawnTimer;
          if (ui.respawnRequest && respawnTimer >= av.respawnDelay) {
            ui.respawnRequest = false;
            ui.deathScreen = false;
            ui.deathScreenOpened = false;
            ui.deathCause.clear();
            deathFrozen = false;   // the mirror is live again from here
            deathBody.have = false;
            deathBody.limbs.clear();
            deathBody.boxes.boxes.clear();
            respawnTimer = 0;
            avatar.Revive(player, avatarHeading);
            tpRig.Snap();
            // Put the player back where the body is, not in a menu.
            if (ui.inventoryOpen) {
              ui.inventoryOpen = false;
              ui.inspectSelected = -1;
              // The cursor is the WINDOW's, and there is no window here.
              if (w.restoreCursorAfterUi) w.restoreCursorAfterUi();
            }
          }
        } else {
          respawnTimer = 0;
          ui.respawnRequest = false;
          // Revived by any other route (a reload, a loaded save, the avatar
          // being switched off): the hold has nothing left to hold.
          if (ui.deathScreen && avatar.IsAlive()) {
            ui.deathScreen = false;
            ui.deathScreenOpened = false;
            ui.deathCause.clear();
            deathFrozen = false;
            deathBody.have = false;
            deathBody.limbs.clear();
            deathBody.boxes.boxes.clear();
          }
        }
        // Drain the impact peak at the END of the tick that produced it (N2:
        // Player::Update now runs at the top of this same tick body, so this
        // is a one-tick lifetime rather than a latch that had to survive a
        // frame batch). It is still a PEAK and not the last value written,
        // because one tick can arrest the body more than once — the unstick
        // lift, the vertical sweep and the horizontal sweep each cancel
        // velocity, and the largest single arrest is the impact.
        //
        // Cleared even when no avatar consumed it (fly mode, avatar disabled,
        // dead and awaiting respawn). A peak-hold that is never drained only
        // ratchets upward, and the next avatar to spawn would inherit the
        // hardest hit the session ever recorded and die on its first tick.
        player.impactDeltaV = Vec3{0, 0, 0};
        // Same drain, same reason (see Player::jumped): the avatar's `jump`
        // clip is edge-triggered off this flag, and Player::Update set it
        // earlier in THIS tick.
        player.jumped = false;
      }

      // ---- magic (game/spell.h) ---------------------------------------------
      // Same slot in the tick order as mobs and the avatar, and for the same
      // reason: everything a spell does leaves as ops on the streams assembled
      // below. The VM never touches a voxel buffer (thesis 1 / rule 3).
      std::vector<ExplosionOp> spellExps;
      {
        caster.mana.Tick();
        // Dev-panel overrides of the pool (ui/overlay.h devMana*). Applied
        // HERE, after the regen tick and before the cast/bill sites below, so
        // "infinite" is a full pool at every point a tariff is resolved
        // against it. The player's caster only; mobs keep their own pools.
        if (ui.devManaMaxRequest >= 0) {
          caster.mana.manaMax = std::max(1, ui.devManaMaxRequest);
          ui.devManaMaxRequest = -1;
          caster.mana.mana = std::min(caster.mana.mana, caster.mana.EffectiveMax());
        }
        if (ui.devManaFill || ui.devManaInfinite) {
          caster.mana.mana = caster.mana.EffectiveMax();
          caster.mana.regenAccum = 0;
          ui.devManaFill = false;
        }
        SpellEmission emit;

        // What the VM may ask about bodies: where an adopted bomb is, and the
        // nearest mob for a seeking bolt (never the caster's own body).
        struct SpellBodyCtx {
          Physics* phys;
          MobSystem* mobs;
          const Player* player;
          uint64_t playerId;
          const PlayerAvatar* avatar;
        } bodyCtx{&phys, &mobs, &player, 0x9134A5EEu, &avatar};
        SpellBodyProbe bodyProbe;
        bodyProbe.ctx = &bodyCtx;
        // WHAT A FLIGHT RUNS INTO: the first MOVING-layer body on the tick's
        // segment (a mob limb, debris, a bomb), through the same ray the laser
        // and the melee sweep use. The caster's own parts are rejected by
        // OWNERSHIP, not distance (the laser's rule): an arm swings through
        // the muzzle line constantly, and a bolt that went off in the hand
        // would read as the overcast backfire it is not.
        bodyProbe.bodyHit = [](void* c, Vec3 from, Vec3 to, uint64_t casterId, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          const Vec3 seg = to - from;
          const float len = seg.len();
          if (len < 1e-4f) return false;
          const Vec3 dir = seg * (1.0f / len);
          float frac = 1.0f;
          const uint64_t h = bc.phys->CastRayBody(from, dir, len, frac);
          if (h == 0) return false;
          if (casterId == bc.playerId && bc.avatar->OwnsBody(h)) return false;
          out = from + dir * (frac * len);
          return true;
        };
        // The body a status attaches to: the nearest mob origin within the
        // radius, else the player. Ids are the mob's own and the player's
        // caster id — opaque to the VM either way.
        bodyProbe.bodyIdAt = [](void* c, Vec3 from, float radius, uint64_t& id, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          float best = radius * radius;
          bool found = false;
          for (uint32_t i = 0; i < bc.mobs->MobCount(); i++) {
            const uint64_t mid = bc.mobs->MobIdAt(i);
            const Vec3 p = bc.mobs->MobOrigin(mid);
            const Vec3 d = p - from;
            const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 <= best) {
              best = d2;
              id = mid;
              out = p;
              found = true;
            }
          }
          const Vec3 dp = bc.player->pos - from;
          // The figure box is ~1.7 m: a point anywhere on the body counts.
          const float pr = radius + 9.0f;
          if (!found && dp.x * dp.x + dp.y * dp.y + dp.z * dp.z <= pr * pr) {
            id = bc.playerId;
            out = bc.player->pos;
            found = true;
          }
          return found;
        };
        bodyProbe.bodyPos = [](void* c, uint64_t id, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          if (id == bc.playerId) {
            out = bc.player->pos;
            return true;
          }
          for (uint32_t i = 0; i < bc.mobs->MobCount(); i++)
            if (bc.mobs->MobIdAt(i) == id) {
              out = bc.mobs->MobOrigin(id);
              return true;
            }
          return false;
        };
        bodyProbe.bodyAt = [](void* c, uint64_t h, Vec3& out) {
          BodyTransform xf;
          if (!((SpellBodyCtx*)c)->phys->GetTransform(h, xf)) return false;
          out = xf.pos;
          return true;
        };
        bodyProbe.nearestTarget = [](void* c, Vec3 from, uint64_t, Vec3& out) {
          MobSystem& m = *((SpellBodyCtx*)c)->mobs;
          float best = 96.0f * 96.0f;   // seek range, voxels squared
          bool found = false;
          for (uint32_t i = 0; i < m.MobCount(); i++) {
            const Vec3 p = m.MobOrigin(m.MobIdAt(i));
            const Vec3 d = p - from;
            const float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
            if (d2 < best) {
              best = d2;
              out = p;
              found = true;
            }
          }
          return found;
        };
        // Consume the latch on the FIRST tick of the frame that sees it, and
        // clear it even when there is nothing spoken — otherwise a click on an
        // empty stack stays queued and fires the next spell the moment one is
        // spoken. Clearing outside the inner test is what makes this a one-shot
        // rather than a pending intent.
        const bool castNow = ti.Pressed(TB_CAST);
        // The inspector's "cast it on this part": `self` resolves at the
        // clicked limb's centre, with the effect radii clamped to the part.
        // Same Cast(), one extra argument (plan §7); the VM never learns what
        // a part is.
        const int castPart = castAtPartQueued;
        castAtPartQueued = -1;
        if (castPart >= 0 && !caster.stack.Empty() && avatar.Spawned()) {
          int part = -1;
          if (const MobDef* def = avatar.Def()) {
            for (int i = 0; i < (int)def->limbs.size(); i++)
              if (BodySlotFor(avatar.PartName(i), avatar.PartTag(i)) == castPart &&
                  avatar.PartAlive(i))
                part = i;
          }
          Vec3 at;
          Quat rot;
          if (part >= 0 && avatar.PartWorldTransform(part, at, rot)) {
            const SpellFxVec selfAt{SpellFxFromFloat(at.x), SpellFxFromFloat(at.y),
                                    SpellFxFromFloat(at.z)};
            const Vec3 body = player.pos;
            const SpellFxVec originFx{SpellFxFromFloat(body.x), SpellFxFromFloat(body.y),
                                      SpellFxFromFloat(body.z)};
            const SpellFxVec dirFx{0, kSpellFxOne, 0};
            const SpellProbe probe = WorldSpellProbe(world);
            CastResult res = spells.Cast(caster.compiled, caster.mana, playerHealth,
                                         0x9134A5EEu, originFx, dirFx, tick, emit, &probe,
                                         &selfAt, &bodyProbe);
            caster.lastOutcome = res.outcome;
            if (res.outcome != CastOutcome::Nothing) caster.Clear(glyphs);
          }
        }
        if (castNow && !caster.stack.Empty()) {
          // Origin at the muzzle — in front of the eye so the bolt does not
          // spawn inside the caster's own head. Direction is the aim ray.
          const Vec3 eye = player.EyePos();
          const Vec3 fwd = cam.Forward();
          const Vec3 muzzle = eye + fwd * 1.5f;
          SpellFxVec originFx{SpellFxFromFloat(muzzle.x),
                              SpellFxFromFloat(muzzle.y),
                              SpellFxFromFloat(muzzle.z)};
          SpellFxVec dirFx{SpellFxFromFloat(fwd.x), SpellFxFromFloat(fwd.y),
                           SpellFxFromFloat(fwd.z)};
          // A FATAL cast runs its effect at the CASTER instead — and does so
          // through the same ApplySpellEffect call, with the caster's position
          // as the argument (thesis 2). Nothing here branches on the spell.
          const bool fatal =
              ResolveCast(caster.mana, playerHealth.Get(),
                          caster.compiled.manaCost).outcome == CastOutcome::Fatal;
          if (fatal) {
            const Vec3 body = player.pos;
            originFx = {SpellFxFromFloat(body.x), SpellFxFromFloat(body.y),
                        SpellFxFromFloat(body.z)};
          }
          const SpellProbe probe = WorldSpellProbe(world);
          CastResult res =
              spells.Cast(caster.compiled, caster.mana, playerHealth,
                          0x9134A5EEu /*casterId*/, originFx, dirFx, tick, emit,
                          &probe, nullptr, &bodyProbe);
          caster.lastOutcome = res.outcome;
          if (res.outcome != CastOutcome::Nothing) caster.Clear(glyphs);
        }

        // A held beam follows the aim while RMB stays down.
        {
          const Vec3 eye = player.EyePos();
          const Vec3 fwd = cam.Forward();
          const Vec3 muzzle = eye + fwd * 1.5f;
          spells.HoldBeam(0x9134A5EEu,
                          {SpellFxFromFloat(muzzle.x), SpellFxFromFloat(muzzle.y),
                           SpellFxFromFloat(muzzle.z)},
                          {SpellFxFromFloat(fwd.x), SpellFxFromFloat(fwd.y),
                           SpellFxFromFloat(fwd.z)},
                          ti.Held(TB_ALT));
        }
        if (ti.Pressed(TB_DROP)) {
          spells.DropNewestStatus(0x9134A5EEu);
        }
        spells.Tick(tick, world, classOf, emit, &bodyProbe);
        // Impact flashes: born from this tick's resolves, aged per tick.
        for (SpellFlash& fl : spellFlashes) fl.ttl--;
        spellFlashes.erase(std::remove_if(spellFlashes.begin(), spellFlashes.end(),
                                          [](const SpellFlash& f) { return f.ttl <= 0; }),
                           spellFlashes.end());
        for (const SpellImpactFx& fx : emit.impacts) {
          if (spellFlashes.size() >= 24) break;
          SpellFlash fl;
          fl.at = Vec3{SpellFxToFloat(fx.at.x), SpellFxToFloat(fx.at.y), SpellFxToFloat(fx.at.z)};
          fl.color = fx.tint != 0 && fx.tint < mats.size()
                         ? mats[fx.tint].gpu.color0
                         : glyphs.Delivery(fx.deliveryGlyph).look.color;
          fl.radius = (float)std::clamp(fx.radius, 1, 12);
          fl.ttl = fl.ttl0 = 9;
          spellFlashes.push_back(fl);
        }

        // THE PER-TICK BILL. Statuses and a held beam pay the same tariff as
        // they emit (plan §4): mana first, then the body, and when neither
        // can pay the caster has run dry and everything they sustain drops.
        {
          int32_t bill = 0;
          for (const SpellBill& sb : emit.bills)
            if (sb.casterId == 0x9134A5EEu) bill += sb.amount;
          if (bill > 0) {
            const int32_t fromMana = std::min(bill, caster.mana.mana);
            caster.mana.mana -= fromMana;
            bill -= fromMana;
            if (bill > 0) {
              const int32_t hp = playerHealth.Get();
              if (hp > bill) {
                playerHealth.Spend(bill);
              } else {
                spells.DropAll(0x9134A5EEu);   // dry: it all goes out
              }
            }
          }
          caster.mana.reserved = spells.ReservationFor(0x9134A5EEu);
        }
        // A sustained gravity mod on the player's own body.
        for (const SpellBodyImpulse& bi : emit.bodyImpulses)
          if (bi.target == 0x9134A5EEu) player.vel.y += bi.vps.y;
        // GRAFTS: the world half already left as ops; the body half fills the
        // caster's missing anatomy cells with that matter, root-first. The VM
        // cannot reach a body (thesis 4); the owner does it.
        for (const SpellRestore& rs : emit.restores) {
          if (rs.casterId == 0x9134A5EEu) {
            if (avatar.Spawned()) avatar.RestoreBody(rs.material, rs.count);
          } else {
            mobs.RestoreMob(rs.casterId, rs.material, rs.count);
          }
        }
        // WARDS filter the spell's OWN emission too (a fire aura inside an
        // anti-fire ward is refused like anyone else's).
        ui.spellRefused = spells.FilterStreams(emit.ops, emit.explosions, emit.spawns, emit.winds);

        // BOMBS ARE DEBRIS. The VM asked for a rigid body; this is the owner
        // making one through the same path a dropped item takes (a Jolt
        // sphere so it rolls, a voxel ball to draw it, adopted by the debris
        // system so it falls, settles, burns and can be blown apart) and
        // handing the handle back. When the fuse runs out the VM resolves the
        // payload where the body is and asks for it to be taken away.
        for (const SpellBodyRequest& rq : emit.bodyRequests) {
          const float r = rq.radius;
          std::vector<DebrisVoxel> ball;
          const int ext = (int)std::ceil(r);
          for (int z = -ext; z < ext; z++)
            for (int y = -ext; y < ext; y++)
              for (int x = -ext; x < ext; x++) {
                const float dx = x + 0.5f, dy = y + 0.5f, dz = z + 0.5f;
                if (dx * dx + dy * dy + dz * dz <= r * r)
                  ball.push_back({(int8_t)x, (int8_t)y, (int8_t)z, 0, (uint16_t)rq.material});
              }
          BodyTransform xf{};
          xf.pos = rq.pos;
          xf.quat[3] = 1;
          const float density =
              rq.material < mats.size() ? (float)mats[rq.material].gpu.density : 2000.0f;
          const uint64_t h = phys.CreateSphereBody(rq.pos, r, density);
          if (!h) continue;
          phys.SetBodyVelocity(h, rq.vel);
          phys.ReleaseToWorldWhenClear(h);
          debris.AdoptBody(h, std::move(ball), xf);
          spells.AdoptBody(rq.token, h);
        }
        for (uint64_t h : emit.bodyDone) debris.DestroyBody(h);
        // An anchored gravity Mod acting on the caster's own body (a hop).
        if (emit.casterImpulseVps.y != 0.0f) player.vel.y += emit.casterImpulseVps.y;

        // THE WILDCARD'S BILL. `anything` is priced by what it turned out to
        // be, when it resolves (plan §4): mana first, then the body, exactly
        // the crossover a spoken cost pays -- except that this one lands after
        // the fact, which is the danger the word is for.
        if (emit.billOnResolve > 0) {
          int32_t bill = emit.billOnResolve;
          const int32_t fromMana = std::min(bill, caster.mana.mana);
          caster.mana.mana -= fromMana;
          bill -= fromMana;
          if (bill > 0) playerHealth.Spend(bill);
          ui.spellLastBill = emit.billOnResolve;
          ui.spellLastBillAge = 0.0f;
        }

        // The caster's own body pays for a fatal overcast: severed parts, then
        // death, all through the existing dismemberment/gore pipeline. The
        // avatar half is separate from the world half above only because the
        // VM may not reach into PlayerAvatar (thesis 4).
        if (emit.carveCaster && avatar.Spawned())
          avatar.SelfDestruct(emit.carveAt, emit.carveRadius, world, spawns);

        // OP BUDGET FAIRNESS (§F). Magic gets an explicit reservation rather
        // than silently sharing the 64-op tick budget with mob bleeding (6)
        // and avatar bleeding (6). Anything past it is COUNTED, not dropped
        // silently — a spell that sometimes doesn't fire is miserable to
        // diagnose, so the overflow is visible in the HUD.
        int spellOps = 0;
        for (const BrushOp& b : emit.ops) {
          if (spellOps >= SpellSystem::kSpellOpsPerTick ||
              ops.size() >= kMaxOpsPerTick) {
            ui.spellOpsDropped++;
            continue;
          }
          ops.push_back(b);
          spellOps++;
        }
        // Explosions are carried to the explosion block below, where `exps`
        // exists — a spell blast must go through the SAME path as a grenade
        // (island checks, body damage, mob carving, impulse), not a second one.
        spellExps = std::move(emit.explosions);
        for (const ParticleSpawn& p : emit.spawns) spawns.push_back(p);
        // WIND PRIMITIVES (docs/RESEARCH_wind.md §4.3). The VM reported the
        // intent; this is the owner splicing it on, exactly as it does for
        // brush ops and particle spawns above. `spawnTick` is stamped HERE and
        // not in the VM, because a primitive's whole motion and lifetime are
        // f(t - spawnTick) and the tick a spell resolves on is the caller's
        // fact, not the VM's.
        //
        // Spawn() refuses when the world list is full (32) rather than evicting
        // someone's fan, and the refusal is COUNTED for the same reason the op
        // overflow above is: "my gust sometimes does nothing" is miserable to
        // diagnose from silence.
        for (WindPrim w : emit.winds) {
          w.spawnTick = tick;
          w.ownerId = 0x9134A5EEu;   // the same casterId the projectile carries
          if (!WindPrims().Spawn(w)) ui.windPrimsDropped++;
        }

        // ---- the dev-panel producer (docs/RESEARCH_wind.md §4.3) ------------
        // A placed fan, from the same panel the wind multipliers live on. It
        // exists because the gameplay producers are CONTENT — a gust is a glyph
        // in glyphs.json, a fan will be a prefab tag — and neither is a good way
        // to answer "what does a 30 m/s cone actually do to that dune?".
        //
        // It goes through WindPrims().Spawn() like everything else. There is no
        // dev-only path into the wind system, which is what makes what you see
        // here the same thing a spell would produce.
        //
        // ANCHORED IN FRONT OF THE CAMERA, aimed along the view ray, with an
        // INFINITE TTL: that is what makes it a fan rather than a gust, and it
        // is why "clear all" exists next to it. A fan holding its footprint
        // awake forever is a rule-2 leak if you cannot retire it.
        if (ui.placeWindFan) {
          ui.placeWindFan = false;
          const Vec3 fwd = cam.Forward();
          const Vec3 at = player.EyePos() + fwd * 2.0f;
          WindPrim w{};
          w.x = ifloor(at.x);
          w.y = ifloor(at.y);
          w.z = ifloor(at.z);
          w.kind = ui.windFanKind == 1   ? kWindPrimBurst
                   : ui.windFanKind == 2 ? kWindPrimVortex
                                         : kWindPrimCone;
          w.radius = ui.windFanRadius;
          w.reach = ui.windFanReach;
          w.ttl = kWindPrimForever;
          w.spawnTick = tick;
          w.ownerId = kDevFanOwner;
          w.flags = kWindPrimAir |
                    (ui.windFanEntrain ? kWindPrimEntrain : 0u);
          // A vortex with no swirl is a cone with extra steps, so the two
          // shares are given here rather than left to the panel: 1.0 of the
          // core speed tangentially and 0.5 axially is a tornado that both
          // spins and lifts. They are the primitive's parameters, not knobs
          // anyone has asked for yet.
          if (w.kind == kWindPrimVortex) {
            w.swirlQ = 65536;
            w.riseQ = 32768;
          }
          WindPrimAim(w, fwd, ui.windFanSpeed);
          if (!WindPrims().Spawn(w)) ui.windPrimsDropped++;
        }
        if (ui.clearWindFans) {
          ui.clearWindFans = false;
          // Only the dev-placed ones: a spell's gust owns itself and expires on
          // its own TTL, and clearing it from here would be the panel reaching
          // into gameplay.
          WindPrims().RetireOwner(kDevFanOwner);
          ui.windPrimsDropped = 0;
        }
      }

      // ---- --fell-tree: the tree-fell gate's cut, in the live frame loop ----
      // A HOOK (session.h section E): it reads g_frameMs, the frame layer's
      // own whole-frame ring, so it cannot live on this side of the seam.
      if (w.fellTree) w.fellTree(tick, cellOps);

      // support-loss flags from the sim (burnt stems, undermined slabs) feed
      // the same island-check pipeline as explosions and brush erases
      debris.QueueSupportEvents(world.Snap());
      // island detection results + body burn + terrain collision upkeep
      // (may add cell ops and particle spawns from shattered bodies)
      debris.PreTick(tick, world, cellOps, spawns);
      // ---- fluid lab scene driver (lab/lab.h) ----
      // Advances the scene clock once per SIM tick (never per frame): the
      // build CellOps land on scene tick 1 and the pour follows its fixed
      // schedule, so a run — and every L-reset replay — is deterministic.
      // Pour budget is charged against the live estimate before emission,
      // the same rule as the mpm tool above.
      if (labScene >= 0) {
        labTick++;
        LabSceneBuildOps(labScene, labTick, fluidCueMat, cellOps);
        // The pond scenes' whole experiment: a still body, then a plug pulled
        // from under it. Same op stream as the build, so L replays it.
        if (LabScenePlugTick(labScene) == labTick)
          LabScenePlugOps(labScene, cellOps);
        LabScenePour(labScene, labTick, fluidCount, fluidCueMat, fluidSpawns);
      }
      // laser kerf into a body, deferred from the input block above so it can
      // reach `spawns` (a cut that severs the body sheds the loose bits)
      if (laserCut.body) {
        // A live limb is carved (eject=false: the beam vaporizes, and a held
        // laser spraying gobbets every tick would drain the particle ring);
        // plain debris melts. The two paths are the same operation on the two
        // populations — see game/mob.h.
        if (laserCut.limb)
          mobs.CarveLimbRadial(laserCut.body, laserCut.at, laserCut.radius,
                               false /*ragged*/, false /*eject*/, world, spawns);
        else
          debris.MeltBodyAt(laserCut.body, laserCut.at, laserCut.radius, world,
                            spawns);
      }
      // ---- the sword bites (game/melee.h) ---------------------------------
      // THE POSE IS THE HITBOX. The blade's authored `edge` segment is read
      // through its LIVE transform and swept from where it was last tick to
      // where it is now; anything that quad passes through is cut. Nothing
      // here consults the camera, so what you hit is exactly what the visible
      // blade travelled through — which is what makes the wound land where the
      // player aimed rather than where a cone in front of the crosshair says.
      //
      // Deferred to this point for the same reason the laser kerf is: a carve
      // needs the `spawns` list debris.PreTick fills just above.
      avatar.SetSwinging(melee.Cutting() || playerStrike.Cutting());
      if (avatar.Spawned() && meleeReady) {
        Vec3 eb, et, ef;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw, &ef)) {
          // EITHER cut counts, and no mode read is needed: a freeform tick
          // keeps the cursor Idle, and a discrete one may cut without ever
          // committing Slash — an authored THRUST drives mostly the radial
          // channel, which commitSpeed never sees, so gating on the driver
          // alone made thrusts free actions (the player-styles gate caught
          // it). This is the NPC's own contract: MobSystem::StepStroke sweeps
          // on the CURSOR's cut, and tip speed still scales the damage.
          // ONE STROKE, ONE IMPULSE PER SLOT (melee.h EdgeSweep::struck). The
          // set is cleared on the first tick neither cut state is live, which
          // is the same "is a cut happening" test the sweep gates on -- so a
          // freeform wave and a discrete program both get exactly one blunt
          // hit per body per swing with no mode read of their own.
          if (!(melee.Cutting() || playerStrike.Cutting())) {
            playerStruck.clear();
            playerBitten = false;
          }
          if (lastEdgeValid && (melee.Cutting() || playerStrike.Cutting())) {
            EdgeSweep sw;
            sw.aPrev = lastEdgeBase;
            sw.bPrev = lastEdgeTip;
            sw.aNow = eb;
            sw.bNow = et;
            sw.flatNow = ef;
            sw.dt = kTickDt;
            sw.halfWidth = ehw;
            // ---- WHOSE BLOW IS THIS: THE SWORD'S, OR THE FIST'S? ----------
            // `WeaponEdge` above already reported whichever the effector
            // names, so this is the matching half: the numbers come from the
            // same weapon the segment did. A fist's profile passes through
            // `StrikeProfileFor`, so a worn gauntlet upgrades the player's
            // punch exactly as it upgrades an NPC's (plan section 3). No
            // blade behind a fist, so the neutral heft (EdgeSweep::heft's
            // documented default) rather than an item's volume ratio.
            if (const MobNaturalWeaponDef* fist = avatar.EffectorWeapon()) {
              sw.strike = avatar.StrikeProfileFor(*fist);
            } else if (heldItem != nullptr) {
              // The whole profile, not a number (game/impact.h): a sword
              // fills mostly `cut` and behaves exactly as it did, a mace
              // fills `blunt`.
              sw.strike = heldItem->strike;
              sw.carveBonus = heldItem->carveBonus;
              // HEFT: the weapon's own volume against the reference, so a
              // greatsword cuts deeper than a knife because it IS bigger
              // (item.h ItemDef::heftVolume). Derived from the art, resolved
              // here because only the caller knows which item is in the fist.
              const auto& goreT = CurrentTuning().gore;
              sw.heft = heldItem->HeftFactor(goreT.woundHeftRef,
                                             goreT.woundHeftMax);
            }
            sw.tick = tick;
            // BLUNT AND BITE ARE IMPULSES (melee.h EdgeSweep::struck). The
            // player has TWO cut states -- the discrete program's cursor and
            // the freeform driver's own Slash -- so the set is owned here and
            // cleared on the tick neither is cutting, which is mode-blind and
            // is the same line the sweep gate below already reads.
            sw.struck = &playerStruck;
            sw.bitten = &playerBitten;
            // A FIST IS PART OF THE ARM THAT THROWS IT (melee.h selfMounted).
            sw.selfMounted = avatar.EffectorWeapon() != nullptr;
            sw.valid = true;
            // ---- WHAT THE BLOW WAS, MEASURED FROM WHAT IT LEFT BEHIND ------
            //
            // The sweep reports how many bodies it hit and how hard, but not
            // WHAT it hit. Rather than widen EdgeSweepResult — which would put
            // the answer inside MeleeSweepDamage, where two people are working
            // — the tier is read off the two event queues the sweep already
            // fills as a side effect, by differencing them across the call:
            //
            //   severs grew  -> a limb came off. The biggest of everything.
            //   voices grew  -> a LIVE creature was hurt: flesh.
            //   neither, but bodiesHit -> debris, a dropped item, a held
            //                             weapon. A chip.
            //
            // Both queues are frame-drained (down in the audio block), so the
            // sizes only ever grow within a frame and a difference is exactly
            // "what this call added". No new plumbing, no shared struct.
            //
            // KNOWN AND ACCEPTED: Hurt voices de-duplicate per mob per drain
            // window (MobSystem::PushVoice), so a SECOND cut into the same
            // creature in the same frame reads as a chip rather than as flesh.
            // The blow still flashes and still hurts — only the tier of the
            // dip and the choice of cue are affected, and only for a repeat
            // inside 16 ms. The alternative is a per-call "did I hurt live
            // flesh" flag threaded out of the sweep, which is the shared
            // surface this is avoiding.
            //
            // A PARRY IS NOT ONE OF THESE TIERS, and deliberately so: an
            // arrested sweep returns before any probe runs, so `bodiesHit` is
            // 0 and none of this fires. The clang and the dip for a blocked
            // blow come off the BlockEvent queue instead (the drain below the
            // AI readout), which is the only place that knows a block from a
            // chip. One blow, one cue, whichever way it ended.
            const size_t sev0 = mobs.SeverEvents().size();
            const size_t voi0 = mobs.VoiceEvents().size();
            // The third queue, for the population the other two cannot see:
            // dead flesh (DebrisSystem::GoreEvent).
            const size_t gore0 = debris.GoreEvents().size();
            const EdgeSweepResult res = MeleeSweepDamage(
                sw, melee.tuning, avatar, phys, mobs, debris, world, spawns);
            // PARRIED BY AN NPC'S BLADE. The sweep reports; ending the stroke
            // is the caller's job, because MeleeSweepDamage has no business
            // reaching into whichever driver happens to own this swing
            // (melee.h EdgeSweepResult). The player's cut stops here: no
            // follow-through, no second bite past the blade that stopped it.
            if (res.arrested) {
              melee.Arrest();
              // The program mirrors the driver, exactly as the NPC's stroke
              // does on a parry (MobSystem::StepStroke): the remaining cut
              // ticks are abandoned and the recover starts now.
              if (playerStrike.Cutting()) {
                playerStrike.phase = StrokeCursor::Phase::Recover;
                playerStrike.phaseTick = 0;
              }
            }
            if (res.bodiesHit > 0) {
              const bool severedLive = mobs.SeverEvents().size() > sev0;
              // ---- ...AND DEAD FLESH IS STILL FLESH (2026-09-20) ---------
              //
              // This differencing trick has a blind spot and it is a whole
              // population: a CORPSE fills neither queue — it has no voice to
              // cry and its pieces are DebrisSystem's, not MobSystem's — so
              // every blow on one fell through to the `chip` tier and a body
              // being hacked apart on the ground sounded like a crate. The
              // sweep now answers the question directly for that case
              // (EdgeSweepResult::hitDeadFlesh), and the corpse's own sever
              // rides the debris gore queue drained beside the mob one below.
              const bool flesh =
                  mobs.VoiceEvents().size() > voi0 || res.hitDeadFlesh;
              const bool severed =
                  severedLive || debris.GoreEvents().size() > gore0;
              const Tuning::CombatFx& fx = CurrentTuning().combatfx;
              // LATCHED, not acted on. This is inside the tick loop, which
              // runs 0..4 times a frame; the frame loop drains it at the top
              // of the next frame (see HitStop's note).
              if (severed)
                hitStop.Request(fx.hitStopSeverScale, fx.hitStopSeverMs);
              else if (flesh)
                hitStop.Request(fx.hitStopFleshScale, fx.hitStopFleshMs);
              else
                hitStop.Request(fx.hitStopChipScale, fx.hitStopChipMs);
              // The impact cue, latched for the same reason and peak-held on
              // power so one frame carrying four tick-hits plays the hardest
              // of them once rather than four overlapping copies of nearly the
              // same sound.
              CombatCueRequest& q = flesh ? combatFleshCue : combatClangCue;
              const float pw = res.power * res.edgeAlign;
              // AT THE CONTACT POINT, not at the middle of the blade. The
              // segment midpoint is where the WEAPON is; on a sword that is up
              // to half a metre from the wound, and a listener standing right in
              // front of what they just hit could hear the blow off to one side
              // (reported 2026-09-19). `hitAt` is the probe ray's own hit
              // position — the same place the kerf is bored.
              const Vec3 hitAt = res.hasHitAt ? res.hitAt
                                              : (sw.aNow + sw.bNow) * 0.5f;
              if (!q.pending || pw > q.power) {
                q.power = pw;
                q.at = hitAt;
              }
              q.pending = true;
              // Weapon impact layer: the sword's ring or the mace's thud,
              // on ANY body contact regardless of flesh/armor.
              const bool edged = sw.strike.cut > sw.strike.blunt;
              if (!combatStrikeCue.pending || pw > combatStrikeCue.power) {
                combatStrikeCue.power = pw;
                combatStrikeCue.at = hitAt;
                combatStrikeEdged = edged;
              }
              combatStrikeCue.pending = true;
              // Wet cutting layer: edged weapons contacting flesh.
              if (flesh && edged) {
                if (!combatCutCue.pending || pw > combatCutCue.power) {
                  combatCutCue.power = pw;
                  combatCutCue.at = hitAt;
                }
                combatCutCue.pending = true;
              }
            }
          }
          lastEdgeBase = eb;
          lastEdgeTip = et;
          lastEdgeValid = true;
        } else {
          lastEdgeValid = false;   // sheathed or severed: no segment to sweep
        }
      } else {
        lastEdgeValid = false;
      }

      // prefab stamps drain after island ops (they win same-cell conflicts)
      placer.PreTick(world, cellOps);

      // explosions: X-detonate at the crosshair + grenade fuses + spell blasts
      // (`exps` is aliased out of the caller's OpBatch.)
      // Spell blasts join here rather than getting their own path, so a
      // firebolt's detonation gets the island checks, body damage, mob carving
      // and impulse a grenade already gets — for free, and consistently.
      for (const ExplosionOp& e : spellExps)
        if (exps.size() < kMaxExplosionsPerTick) exps.push_back(e);
      if (ui.pendingDetonate) {
        ui.pendingDetonate = false;
        const WorldSnapshot& snap = world.Snap();
        if (snap.valid && snap.pick[0] != 0) {
          exps.push_back({(int)snap.pick[2], (int)snap.pick[3], (int)snap.pick[4],
                          CurrentTuning().tools.detonateRadius,
                          CurrentTuning().tools.detonatePower, 0, 0, 0});
        }
      }
      for (size_t i = 0; i < grenades.size();) {
        if (UpdateGrenade(grenades[i], kTickDt, kindAt)) {
          if (exps.size() < kMaxExplosionsPerTick) {
            exps.push_back({ifloor(grenades[i].pos.x), ifloor(grenades[i].pos.y),
                            ifloor(grenades[i].pos.z),
                            CurrentTuning().grenade.blastRadius,
                            CurrentTuning().grenade.blastPower, 0, 0, 0});
          }
          grenades[i] = grenades.back();
          grenades.pop_back();
        } else {
          i++;
        }
      }
      if (!exps.empty()) {
        everExploded = true;
        lastExplosionTick = tick;
        for (const ExplosionOp& e : exps) {
          debris.AddDestructionEvent(tick, {e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                     {e.x + e.radius, e.y + e.radius, e.z + e.radius});
          // Blow voxels OFF the bodies in range before shoving what survives:
          // an explosion next to a rigidbody now craters it, and splits it into
          // separate bodies when the crater severs it. Runs first so the
          // impulse below acts on the post-damage bodies (including the new
          // fragments, which is what makes a blown-apart object scatter).
          const Vec3 ec{(float)e.x + 0.5f, (float)e.y + 0.5f, (float)e.z + 0.5f};
          const float edr =
              (float)e.radius * CurrentTuning().physics.explosionBodyDamageScale;
          debris.DamageBodiesRadial(ec, edr, world, spawns);
          // Living flesh craters too: a blast next to a mob tears voxels off
          // its limbs, and takes a limb clean off when it removes enough of it.
          // Same call shape as the debris line above — that parallel is the
          // point (game/mob.h).
          mobs.CarveMobsRadial(ec, edr, world, spawns);
          avatar.CarveRadial(ec, edr, world, spawns);
          // The per-body impulse is for DEBRIS. A living creature's limbs are
          // skipped whether kinematic (standing) or dynamic (already limp
          // from an earlier blast): impulse / limb mass on a 0.3 kg hand is
          // 170 m/s and the joints drag the rest of the rig after it —
          // "bodies zoom across the map". The rig takes ONE launch below.
          std::vector<uint64_t> rigBodies;
          mobs.AppendLiveLimbBodies(rigBodies);
          if (avatar.Spawned()) avatar.AppendLiveLimbBodies(rigBodies);
          std::sort(rigBodies.begin(), rigBodies.end());
          phys.ApplyRadialImpulse(
              Vec3{(float)e.x, (float)e.y, (float)e.z},
              (float)e.radius * CurrentTuning().physics.explosionImpulseRadiusScale,
              (float)e.power * CurrentTuning().physics.explosionImpulseScale,
              &rigBodies);
          // ...and the LIVING are knocked flying. A standing creature's limbs
          // are kinematic, so the impulse above never touched them; this is
          // the blast's other half (Mob::BlastRadial): go limp, take a launch
          // velocity of impulse / body mass toward away-from-the-blast,
          // capped at ragdoll.maxLaunchSpeed, and get back up once landed.
          {
            const auto& rg = CurrentTuning().ragdoll;
            const float reach = (float)e.radius * rg.blastRadiusScale;
            const float impulse = (float)e.power * rg.blastImpulseScale;
            mobs.BlastMobsRadial(ec, reach, impulse);
            if (avatar.Spawned()) avatar.BlastRadial(ec, reach, impulse);
          }
          stream.MarkModifiedBox({e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                 {e.x + e.radius, e.y + e.radius, e.z + e.radius});
        }
      }
      // CPU-known writes mark chunks modified now — eviction can't wait for
      // the latent dirty-flag snapshot
      for (const BrushOp& b : ops)
        stream.MarkModifiedBox({b.x - b.radius, b.y - b.radius, b.z - b.radius},
                               {b.x + b.radius, b.y + b.radius, b.z + b.radius});
      // body-shatter spawns keep the particle passes alive exactly like
      // explosions do (a fragment must fly and land on later ticks too)
      if (!spawns.empty()) {
        everExploded = true;
        lastExplosionTick = tick;
      }
      bool particlesActive =
          everExploded &&
          (tick - lastExplosionTick < 400 || world.Snap().particleCount > 0);
      // MPM fluid sheds micro droplets into the particle system (sim_fluid
      // g2p), so live fluid must keep the particle passes awake too — and for
      // a droplet-lifetime tail after the fluid clears, so spray in flight
      // finishes its arc instead of freezing mid-air. Derived from the
      // CPU-owned count + tick only (rule 1).
      if (fluidCount > 0) lastFluidTick = tick;
      particlesActive = particlesActive ||
          (lastFluidTick != 0 && tick - lastFluidTick < 300);

      IVec3 pc{ifloor(player.pos.x) / (int)kChunk, ifloor(player.pos.y) / (int)kChunk,
               ifloor(player.pos.z) / (int)kChunk};
      double t0 = NowSeconds();
      spanGame.Close();
      // ---- the celestial clock (sim/world.h) -----------------------------
      // Advanced exactly ONCE per sim tick, here, immediately before the
      // submit that reads it. The dev overlay's time-speed slider scales it,
      // and it feeds BOTH the rendered sky and TickParams.dayPhase — so
      // cranking time makes the world react (freezing, melting, evaporation)
      // instead of just racing the sun over a world that ignores it.
      //
      // The clock stays DISENGAGED until the slider first leaves 1.0x, at
      // which point it adopts the current tick so the sky does not jump. While
      // disengaged the celestial tick is the sim tick byte for byte, which is
      // what makes every headless path (and the pinned hash) unaffected.
      Celestial().SetScale(ui.timeScale, tick);
      Celestial().Advance();
      // ---- the authored edit layer (sim/worldedit.h) ----------------------
      // Whatever worldgen produced this tick — startup, a window shift, a
      // regen — the layer's chunks were queued at the point of generation and
      // are paid out here, on the ordinary CellOp stream, bounded by the same
      // per-tick cap everything else on that stream respects. Rule 3: this is
      // the ONLY place the layer touches the world, and it touches it through
      // the queue.
      if (WorldEditLayer().HasPending() && cellOps.size() < kMaxCellOpsPerTick)
        WorldEditLayer().Drain(world, cellOps,
                               kMaxCellOpsPerTick - (uint32_t)cellOps.size());
      phys.MovePlayerBody(playerBody, player.pos, kTickDt);
      // ---- the physics grab's servo (game/grab.h) -------------------------
      // HERE, on the tick, and immediately before the Step it is setting up:
      // it writes a VELOCITY, which is only meaningful for the step that then
      // integrates it. Per-frame would write two or three velocities for one
      // step at 100 fps and make how hard you can drag a crate depend on the
      // frame rate.
      //
      // The carry point is the player's own eye and the CAMERA's forward — the
      // same pair the reach ray uses, so a thing picked up under the crosshair
      // stays under the crosshair. Nothing here can reach the hashed grid: it
      // sets rigid-body velocities, and bodies only re-enter the world through
      // the op stream.
      grab.Tick(phys, debris, CurrentTuning().player, player.EyePos(),
                cam.Forward(), kTickDt);
      // WARDS, AT THE SPLICE (DESIGN.md §8): a live filter refuses ops of its
      // word's kind within its radius, whoever produced them — the brush, a
      // mob, a spell. The op stream, never the CA: acid already flowing still
      // flows, and that is the counterplay.
      {
        std::vector<WindPrim> noWinds;
        ui.spellRefused += spells.FilterStreams(ops, exps, spawns, noWinds);
      }
      // ---- W3: THE SWIMMER'S WAKE (docs/PLAN_water_relevel.md §5) --------
      //
      // HERE, on the TICK, and not beside player.Update above — which runs per
      // FRAME. The impulse queue is consumed once per sim tick, so a frame-rate
      // emitter would queue three or four records for one swimmer at 120 Hz and
      // shove the lake harder on a fast machine than on a slow one. One tick,
      // one record.
      //
      // THE SAME DOOR THE BLAST USES (SubmitTick's WaterBodyNoteBlast call):
      // one queue, one record type, one kernel term. The conversion — velocity
      // to Q8, submersion threshold, strength — lives in
      // WaterBodyNoteSwimmer so the gate exercises the arithmetic the game
      // runs rather than a copy of it.
      //
      // `player.vel` is VOXELS PER SECOND (player.cpp divides its m/s knobs by
      // kVoxelMeters); the door wants voxels per TICK.
      //
      // MOBS ARE NOT WIRED and that is a gap, not an omission of effort: a Mob
      // carries no submersion or swim state at all today (game/mob.h has no
      // such field), so there is nothing to read. The door is generic — a mob
      // that gains one is this same call with different arguments.
      //
      // At sim.waveSwimWake 0 nothing is queued at all.
      if (player.inLiquid) {
        WaterBodyNoteSwimmer(WaterBodies(), ifloor(player.pos.x),
                             ifloor(player.pos.z), player.vel.x * kTickDt,
                             player.vel.z * kTickDt, player.submersion,
                             CurrentTuning().sim.waveSwimWake);
      }
      double tSubmit0 = NowSeconds();
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps,
                 tick % 15 == 0 /*hash occasionally*/, pc, true, particlesActive,
                 spawns, farCount, fluidSpawns, fluidCount, fluidSpeciesMat,
                 ui.showDirtyVoxels);
      // Conservative estimate refresh: the newest snapshot's GPU-owned count
      // plus every spawn batch it has not seen yet. Settles decay it (the
      // snapshot count shrinks); excites grow it one snapshot late, which the
      // seam's recording predicate covers (Simulation::EncodeTick).
      if (!fluidSpawns.empty())
        fluidPendingSpawns.push_back({tick, (uint32_t)fluidSpawns.size()});
      {
        const WorldSnapshot& fsn = world.Snap();
        uint32_t pend = 0;
        if (fsn.valid) {
          std::erase_if(fluidPendingSpawns,
                        [&](const std::pair<uint32_t, uint32_t>& p) {
                          return p.first <= fsn.tick;
                        });
          for (const auto& p : fluidPendingSpawns) pend += p.second;
          fluidCount = std::min(fsn.fluidLive + pend, kFluidCap);
        } else {
          fluidCount = std::min(fluidCount + (uint32_t)fluidSpawns.size(),
                                kFluidCap);
        }
      }
      ui.fluidCount = fluidCount;
      double tSubmit1 = NowSeconds();
      phys.Step(kTickDt);   // CPU physics overlaps the GPU tick
      double tPhys1 = NowSeconds();
      debris.PostStep();
      mobs.PostStep();
      avatar.PostStep();
      // ---- the player follows a ragdolled body ----
      // Limp: the capsule rides the pelvis wherever Jolt threw it, so the
      // camera goes with the body. Getting up: it sits on the standing spot
      // the get-up chose, so the controller resumes exactly there. The body
      // facing is copied back into the heading policy so the first driven
      // tick after the get-up does not snap the rig round to the camera.
      bool avatarRagdolled = false;
      {
        Vec3 follow;
        if (avatar.RagdollFollow(follow)) {
          player.pos = follow;
          player.vel = {};
          avatarHeading = avatar.Heading();
          avatarRagdolled = true;
        }
      }
      // debris that ended the step overlapping the player pushes the player
      // out (fly mode ignores collision entirely, matching the voxel rules;
      // a ragdolled player is being placed by the body, not the solver)
      if (!player.fly && !avatarRagdolled)
        player.ApplyPush(phys.PlayerPushOut(playerBody, player.pos), kindAt);
      double tEnd = NowSeconds();
      tickMsSmooth += ((float)((tEnd - t0) * 1000.0) - tickMsSmooth) * 0.1f;
      // Accumulate this tick into the frame's sample rather than sending it.
      // A frame may run 0..4 ticks, and a chart whose x axis is FRAMES has to
      // show what the frame cost — sending per tick made a 4-tick frame look
      // like four cheap frames and a 0-tick frame look like a gap.
      {
        using sandvox::PerfScope;
        // t0..tSubmit0 is the celestial clock, the authored edit-layer drain
        // and MovePlayerBody — game logic, not streaming. It was billed to
        // `stream` and it is one of the reasons that row never matched what the
        // window shift actually cost.
        //
        // tSubmit0..tSubmit1 is NOT billed here any more: SubmitTick bills
        // ITSELF, in five spans (upload / waterBody / pageTableCpu / encode /
        // submit), because "submit spiked to 30 ms" was never a diagnosis. The
        // spans go into the process-global accumulator and are drained into
        // this sample at the bottom of the frame.
        sandvox::PerfScopeAdd(PerfScope::GameLogic, t0, tSubmit0);
        sandvox::PerfScopeAdd(PerfScope::Physics, tSubmit1, tPhys1);
        sandvox::PerfScopeAdd(PerfScope::PostStep, tPhys1, tEnd);
        liveSample.tick = tick;
      }
      if (liveTimed) liveTimer.KickDeferred(ctx, liveFrameNo);

  }
}
