// session.cpp - one tick of authority. See session.h for the boundary this
// file draws and for the rule that comes with it.

#include "game/session.h"
#include "world/refs_game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "audio/cues.h"
#include "game/ai_behavior.h"
#include "game/bodyreg.h"
#include "game/demon.h"
#include "game/demon_lore.h"
#include "game/demon_talk.h"
#include "game/dye.h"
#include "game/itemcoat.h"
#include "game/persist.h"
#include "game/strokes.h"
#include "game/worlditems.h"
#include "lab/lab.h"
#include "measure/perfnodes.h"
#include "net/chunksync.h"   // M9.3-C: the resync install at the phase-B position
#include "net/opsync.h"
#include "sim/celestial.h"
#include "sim/currentprim.h"
#include "sim/microvox.h"
#include "sim/oprecord.h"
#include "sim/plants.h"
#include "sim/rng.h"
#include "sim/trample.h"
#include "sim/solutes.h"
#include "sim/tuning.h"
#include "sim/voxload.h"
#include "sim/waterbody.h"
#include "sim/weather.h"
#include "sim/wind.h"
#include "sim/windprim.h"
#include "sim/worldedit.h"
#include "test/support.h"

using namespace sandvox;


// Integrate one 30 Hz tick against the voxel mirror. Returns true on detonate.
bool UpdateGrenade(Grenade& g, float dt, const Player::KindFn& kindAt) {
  g.fuse -= dt;
  if (g.fuse <= 0.0f) return true;
  // grenade.restitution / friction / waterDrag. Literals 0.45 / 0.8 / 0.90
  // here until 2026-09-24 while the identical rows loaded into nothing.
  const Tuning::Grenade& gt = CurrentTuning().grenade;
  g.vel.y -= (9.81f / kVoxelMeters) * dt;
  IVec3 at{ifloor(g.pos.x), ifloor(g.pos.y), ifloor(g.pos.z)};
  if (kindAt(at) == CellKind::Liquid) g.vel = g.vel * gt.waterDrag;

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
        v = -v * gt.restitution;  // bounce
        if (axis != 0) g.vel.x *= gt.friction;
        if (axis != 1) g.vel.y *= gt.friction;
        if (axis != 2) g.vel.z *= gt.friction;
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

Vec3 SpellHandPoint(const PlayerAvatar& avatar, const Player& player,
                    const Camera& cam, Hand h) {
  if (avatar.Spawned()) {
    const int part = h == Hand::Left ? avatar.Parts().handL : avatar.Parts().handR;
    Vec3 at;
    Quat rot;
    if (avatar.PartAlive(part) && avatar.PartWorldTransform(part, at, rot)) return at;
    // A hand that is gone: the spell stays where the hand would be — the pose
    // still carries it (PoseSpellReady keeps the ready clip up for it).
    if (part >= 0 && avatar.PartPosedWorld(part, at, rot)) return at;
  }
  return player.EyePos() + cam.Forward() * 1.2f +
         cam.Right() * (0.7f * HandSideSign(h)) - cam.Up() * 0.5f;
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
//
// Views, not std::strings: this runs per limb per frame (FillBodyUI) and per
// limb per cast, and two heap copies of a name to compare its last letters
// were the whole of its cost.
int BodySlotFor(const char* name, const char* tag) {
  const std::string_view n = name, t = tag;
  const bool left = n.ends_with(".L");
  const bool right = n.ends_with(".R");
  // The segment letter is the character just before the side suffix:
  // "armU.L" -> 'U' (upper), "armL.L" -> 'L' (lower). Anything without a side
  // suffix, or with any other letter there, is treated as the upper segment.
  const bool lower =
      (left || right) && n.size() >= 3 && n[n.size() - 3] == 'L';

  if (t == "head") return UIState::kSlotHead;
  if (t == "spine") {
    // The rig's root is the hips; every other spine part is the torso.
    return n.find("hip") != std::string_view::npos ? UIState::kSlotHips
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

// ===========================================================================
// THE TICK, CUT INTO PHASES (M9.1 package P1)
// ===========================================================================
//
// TickAuthority used to be ONE 2,270-line body closed over one player's
// aliases. It ran the world's work and that player's work interleaved, which
// was correct and unremarkable while there was exactly one player — and
// impossible to run twice, which is what a second session needs.
//
// The body is now SIXTEEN functions and NOT ONE LINE OF IT MOVED RELATIVE TO
// ANY OTHER. A phase is a maximal run of the old body with a single scope:
//
//   PLAYER phase — reads and writes ONE PlayerSession. Runs once per session,
//                  in index order.
//   WORLD  phase — reads and writes the world, the engine systems and the
//                  per-tick world scratch. Runs ONCE. Where it needs a player
//                  at all (the dev panel's ray, the far-plume eye, the chunk
//                  the submit centres on) it takes THE PRIMARY, session 0.
//
// and TickAuthority is the alternation:
//
//   A* B C* D E* F G* H I* J K* L M* N O* P        (* = once per session)
//
// WHY SIXTEEN AND NOT NINE. The plan (docs/PLAN_multiplayer_m9.md §2, P1)
// tabulates nine phases and merges four separate player runs into its "A" and
// four separate world runs into its "B". THE FILE DISAGREES, and the file
// wins: at 54fe241 the body alternates player/world thirteen times before the
// submit, so running all of the plan's A and then all of its B would have put
// the laser ray BEFORE the window shift and the dev panel's mob spawn AFTER
// the brush — a reordering of the op stream, which is the one thing this
// package is not allowed to do. The mapping is:
//
//   plan A = A, C, E, G   plan D = J   plan G = M
//   plan B = B, D, H      plan E = K   plan H = N
//   plan C = I            plan F = L   plan I = O
//
// plus phase F (the AI panel's "apply behaviour", old :895-903, a world block
// sitting between the wardrobe and the sphere) which the plan's table does not
// mention at all, and phase P (the perf accounting the plan leaves "at the
// end"). For ONE session the concatenation A..P is the old body, line for
// line, which is why the op record is byte-identical by construction rather
// than by measurement.
//
// WHERE A REMOTE GHOST PLUGS IN (M9.2). A peer-driven player is a
// PlayerSession with `localView = false` whose TickInput arrives off the wire
// instead of out of a TickInputFeeder. It needs no new phase: it joins the
// span, phase B puts its chunk in the interest set and phase H puts its body
// in the NPC actor list — those two are the whole of "the world knows another
// player is standing there".
//
// THE ALIAS WALL IS NOW SIX MACROS. Same names, same bindings, same reason as
// the note the old unpack carried: the bodies below are a MOVE, and a move is
// only checkable if the names it closed over are unchanged. Expanding an
// unused alias costs nothing (MSVC bills it C4189, level 4; this build is
// /W3) and buys a diff a reviewer can read.

// ---- what one TICK needs that is not in any of the two structs -------------

// PER-WORLD TICK SCRATCH. Every one of these was a local in the old body whose
// live range crossed a phase boundary. Fields of a per-call struct rather than
// statics, on purpose: a static here is the exact defect N5 removed and
// DESIGN.md §10 forbids.
//
// A FIELD ADDED HERE MUST BE RESET IN TickScratch::Reset below: the struct is
// held across ticks, and it cannot simply be reassigned from a fresh one
// because PerfSpan (in `spanGame`) is neither copyable nor movable.
struct WorldScratch {
  uint32_t farCount = 0;        // phase B -> phase N (SubmitTick)
  bool particlesActive = false; // phase L -> phase N
  IVec3 pc{};                   // phase L -> phase N: the submit's centre chunk
  double t0 = 0;                // phase L -> phase P
  double tSubmit0 = 0;          // phase N -> phase P
  double tSubmit1 = 0;
  double tPhys1 = 0;
  // The GameLogic span. Opened ONCE, between phase B and the first session's
  // phase C (exactly where the old body declared it), closed in phase L. An
  // optional because PerfSpan starts its clock in its constructor and is
  // neither copyable nor movable.
  std::optional<sandvox::PerfSpan> spanGame;
  // The NPC targeting layer's player list, rebuilt each tick in phase H. Held
  // here rather than declared there so the per-tick rebuild does not allocate.
  std::vector<MobSystem::PlayerActorDesc> actors;
  // The sessions' avatars, for the M9.2 ghost re-registration in phase H.
  // Same reason as `actors`: held here so the rebuild does not allocate. Only
  // ever filled when there is at least one ghost AND its membership changed.
  std::vector<Mob*> sessionAvatars;
};

// PER-SESSION TICK SCRATCH, same rule: a local whose live range crossed a
// phase boundary, and which is ONE PLAYER'S. Same reset rule as WorldScratch.
struct LaserCut {
  uint64_t body = 0;
  Vec3 at{};
  float radius = 0;
  bool limb = false;  // a live mob limb carves; plain debris melts
};
struct PlayerScratch {
  LaserCut laserCut;                   // phase C -> phase K
  std::vector<ExplosionOp> spellExps;  // phase I -> phase K
};

// Both of the above, HELD in TickAuthorityCtx::tickScratch across ticks.
// They were built fresh at the top of every TickAuthority, which made the
// "held here so the per-tick rebuild does not allocate" notes on `actors` and
// `sessionAvatars` untrue: a fresh struct is a fresh vector. Held now, and
// reset to exactly the state a fresh one had — every value field back to its
// default, every vector emptied but keeping its capacity — so no phase can
// see anything the previous tick left behind.
struct TickScratch {
  WorldScratch ws;
  std::vector<PlayerScratch> players;

  void Reset(size_t sessions) {
    ws.farCount = 0;
    ws.particlesActive = false;
    ws.pc = IVec3{};
    ws.t0 = ws.tSubmit0 = ws.tSubmit1 = ws.tPhys1 = 0;
    ws.spanGame.reset();
    ws.actors.clear();
    ws.sessionAvatars.clear();
    players.resize(sessions);
    for (PlayerScratch& p : players) {
      p.laserCut = LaserCut{};
      p.spellExps.clear();
    }
  }
};

#define SV_ENGINE_REFS                                     \
  GpuContext& ctx = w.ctx;                                 \
  World& world = w.world;                                  \
  Simulation& sim = w.sim;                                 \
  Stream& stream = w.stream;                               \
  FarField& far = w.far;                                   \
  Physics& phys = w.phys;                                  \
  MobSystem& mobs = w.mobs;                                \
  DebrisSystem& debris = w.debris;                         \
  MicroBodySet& mbSet = w.mbSet;                           \
  const std::vector<MaterialDef>& mats = w.mats;           \
  const GlyphLibrary& glyphs = w.glyphs;                   \
  const ItemLibrary& items = w.items;                      \
  const std::vector<Prefab>& prefabs = w.prefabs;          \
  const std::vector<uint32_t>& classOf = w.classOf;        \
  const int labScene = w.labScene;

#define SV_SCRATCH_REFS                                    \
  auto& sphereModels = w.sphereModels;                     \
  auto& aiSpawnedMobs = w.aiSpawnedMobs;                   \
  bool& everExploded = w.everExploded;                     \
  uint32_t& lastExplosionTick = w.lastExplosionTick;       \
  uint32_t& fluidCount = w.fluidCount;                     \
  auto& fluidPendingSpawns = w.fluidPendingSpawns;         \
  uint32_t& fluidCueMat = w.fluidCueMat;                   \
  uint32_t& lastFluidTick = w.lastFluidTick;               \
  uint32_t* fluidPourMat = w.fluidPourMat;                 \
  uint32_t& labTick = w.labTick;                           \
  bool& duelDummySpawned = w.duelDummySpawned;             \
  const double now = w.frameTime;                          \
  uint32_t& farCount = ws.farCount;                        \
  bool& particlesActive = ws.particlesActive;              \
  IVec3& pc = ws.pc;                                       \
  double& t0 = ws.t0;                                      \
  double& tSubmit0 = ws.tSubmit0;                          \
  double& tSubmit1 = ws.tSubmit1;                          \
  double& tPhys1 = ws.tPhys1;

#define SV_TELEMETRY_REFS                                  \
  const bool liveTimed = w.liveTimed;                      \
  PassTimer& liveTimer = w.liveTimer;                      \
  sandvox::PerfSample& liveSample = w.liveSample;          \
  const uint32_t liveFrameNo = w.liveFrameNo;              \
  float& tickMsSmooth = w.tickMsSmooth;                    \
  uint64_t& g_farEntries = w.farEntries;                   \
  uint64_t& g_farTicks = w.farTicks;                       \
  uint64_t& g_farBiggest = w.farBiggest;

#define SV_OPS_REFS                                        \
  std::vector<BrushOp>& ops = out.ops;                     \
  std::vector<ExplosionOp>& exps = out.exps;               \
  std::vector<CellOp>& cellOps = out.cells;                \
  std::vector<ParticleSpawn>& spawns = out.spawns;         \
  std::vector<FluidSpawnOp>& fluidSpawns = out.fluid;

#define SV_PLAYER_REFS                                     \
  PlayerSession& s = *st.s;                                \
  const TickInput& ti = st.ti;                             \
  Camera& cam = s.cam;                                     \
  Player& player = s.player;                               \
  PlayerAvatar& avatar = s.avatar;                         \
  std::string& avatarDefName = s.avatarDefName;            \
  ThirdPersonRig& tpRig = s.tpRig;                         \
  CameraMode& camMode = s.camMode;                         \
  float& avatarHeading = s.avatarHeading;                  \
  float& respawnTimer = s.respawnTimer;                    \
  const uint64_t playerBody = s.playerBody;                \
  const Player::KindFn& kindAt = s.kindAt;                 \
  bool& wasInLiquid = s.wasInLiquid;                       \
  Brush& brush = s.brush;                                  \
  PrefabPlacer& placer = s.placer;                         \
  Kit& kit = s.kit();                                      \
  Inventory& hotbar = kit.hotbar;                          \
  GrabHold& grab = s.grab;                                 \
  SpellSystem& spells = s.spells;                          \
  PlayerCaster& caster = s.caster;                         \
  CasterHealth& playerHealth = s.playerHealth;             \
  auto& spellFlashes = s.spellFlashes;                     \
  int& castAtPartQueued = s.castAtPartQueued;              \
  MeleeState& melee = s.melee;                             \
  StrikePicker& strikePicker = s.strikePicker;             \
  StrokeCursor& playerStrike = s.playerStrike;             \
  int& strikeQueued = s.strikeQueued;                      \
  int& strikeBuffered = s.strikeBuffered;                  \
  int& strikeBase = s.strikeBase;                          \
  Vec3& lastEdgeBase = s.lastEdgeBase;                     \
  Vec3& lastEdgeTip = s.lastEdgeTip;                       \
  bool& lastEdgeValid = s.lastEdgeValid;                   \
  auto& strikeIgnore = s.strikeIgnore;                     \
  auto& playerStruck = s.playerStruck;                     \
  bool& playerBitten = s.playerBitten;                     \
  CombatCueRequest& combatWhooshCue = s.combatWhooshCue;   \
  CombatCueRequest& combatFleshCue = s.combatFleshCue;     \
  CombatCueRequest& combatClangCue = s.combatClangCue;     \
  CombatCueRequest& combatStrikeCue = s.combatStrikeCue;   \
  CombatCueRequest& combatCutCue = s.combatCutCue;         \
  bool& combatStrikeEdged = s.combatStrikeEdged;           \
  auto& grenades = s.grenades;                             \
  bool& captured = s.captured;                             \
  const bool brushActive = st.intent.brushActive;          \
  const HandsNow& hands = st.hands;                        \
  const bool meleeArmed = hands.AnyWeapon();               \
  const bool meleeReady = hands.AnyReady();                \
  LaserCut& laserCut = ps.laserCut;                        \
  std::vector<ExplosionOp>& spellExps = ps.spellExps;

// THE PRESENTATION SEAM, BOUND TWO WAYS.
//
// A world phase writes the real window's UIState, HitStop and DeathBody — it
// IS the window's tick. A PLAYER phase writes them only when that player owns
// the view; a session whose window is on another machine writes into its own
// throw-away PresentationSink instead. Binding the sink once, here, rather
// than guarding forty call sites, is what makes the rule checkable: there is
// no path from a non-view session to the window's death screen, its hit-stop
// dip, its refused-spell counter or its combat cues, because the NAMES those
// sites use do not reach it.
//
// The sink PERSISTS on the session (it is not per-tick scratch) because the
// death block reads back what it wrote last tick: `if (!ui.deathScreen)` is
// what starts the respawn hold, and a sink rebuilt every tick would restart
// that hold forever.
#define SV_SEAM_REFS_WORLD                                 \
  UIState& ui = w.ui;                                      \
  HitStop& hitStop = w.hitStop;                            \
  DeathBody& deathBody = w.deathBody;                      \
  bool& deathFrozen = w.deathFrozen;

#define SV_SEAM_REFS_PLAYER                                \
  UIState& ui = s.sink ? s.sink->ui : w.ui;                \
  HitStop& hitStop = s.sink ? s.sink->hitStop : w.hitStop; \
  DeathBody& deathBody = s.sink ? s.sink->deathBody : w.deathBody; \
  bool& deathFrozen = s.sink ? s.sink->deathFrozen : w.deathFrozen;

// ---- WHEN THIS TICK'S OPS ACTUALLY LAND (M9.3-B) --------------------------
//
// Single-player: now. Connected: T + net::kOpLabelAhead, because the ops this
// tick authored are labelled that far ahead, cross the wire inside that tick's
// TickBatch, and are submitted by BOTH machines under that label
// (net/opsync.h, which explains why the number is D + 1 and not D).
//
// Everything that asks "which tick should I rescan / re-settle after this
// edit?" has to be told the LANDING tick, not the authoring tick. Told T, the
// debris island scan runs five ticks before the crater exists, finds the rock
// intact, and the overhang that the blast just cut free never falls — a bug
// that would look like "explosions stopped dropping things over the network"
// and have nothing to do with explosions.
//
// Three call sites read it, all of them debris.AddDestructionEvent: the laser
// kerf (phase C), the brush's erase mode (phase G) and the explosion loop
// (phase K). They were found by grepping AddDestructionEvent|lastExplosionTick
// across this file; `lastExplosionTick` is deliberately NOT shifted — it only
// feeds `particlesActive`'s 400-tick keep-awake window, where four ticks of
// slack is inside the noise and shifting it would keep the particle passes
// awake five ticks longer for no observable gain.
// ---- THE ALCHEMY BENCH IN THE HANDS (session.h BenchHold) -----------------
// Runs after the melee pose, so a pour's arm claim wins over the idle one.
// Each hand holding a bench vessel plays `bench_hold` (its mirror on the
// left) and its flask is turned to the bench's tilt (Mob::SetHeldAim); the
// one tilted over the other is IK'd so its mouth is above the other's.
// Everything eases in and out, so opening the bench, lifting a flask and
// setting it down never snaps an arm.
// "; the venom stops spreading" / "; the rot and the venom stop spreading":
// the remedy line names the infections the disinfect actually stopped, by
// their authored `infect.label` (materials.json), never "the rot" for all.
static std::string InfectionStoppedText(const MobSystem& mobs,
                                        const std::vector<MaterialDef>& mats) {
  std::vector<std::string> names;
  for (uint32_t m : mobs.LastCuredInfections()) {
    if (m >= mats.size()) continue;
    const std::string& l =
        mats[m].infectLabel.empty() ? mats[m].name : mats[m].infectLabel;
    if (std::find(names.begin(), names.end(), l) == names.end())
      names.push_back(l);
  }
  if (names.empty()) return "; the infection stops spreading";
  std::string s = "; the ";
  for (size_t i = 0; i < names.size(); i++) {
    if (i > 0) s += i + 1 == names.size() ? " and the " : ", the ";
    s += names[i];
  }
  s += names.size() > 1 ? " stop spreading" : " stops spreading";
  return s;
}

static void PoseBenchHands(PlayerSession& s, PlayerAvatar& avatar) {
  const PlayerSession::BenchHold& bh = s.benchHold;
  const float ease = 1.0f - std::exp(-kTickDt * 8.0f);
  int pourHand = -1;
  for (int hk = 0; hk < kHands; hk++) {
    const Hand h = HandAt(hk);
    const PlayerSession::BenchHold::HandView& v = bh.hand[hk];
    const bool holds = bh.active && !v.item.empty() && avatar.HeldItem(h) == v.item &&
                       avatar.HeldSlot(h) >= 0;
    const std::string clip = avatar.HandClip("bench_hold", h);
    // Re-played if it is gone while it should be up: an R/F5 reload rebuilds
    // the rig's clip state under a hold that is still live.
    const bool lost = holds && s.benchClip[hk] && avatar.ClipWeight(clip.c_str()) <= 0.0f;
    if ((holds != s.benchClip[hk] || lost) && !clip.empty()) {
      if (holds) avatar.PlayClip(clip);
      else avatar.StopClip(clip);
    }
    s.benchClip[hk] = holds;
    s.benchAimW[hk] += ((holds ? 1.0f : 0.0f) - s.benchAimW[hk]) * ease;
    if (s.benchAimW[hk] < 0.01f && !holds) s.benchAimW[hk] = 0.0f;
    // The bench is the portrait's mirror image: the character faces you, so
    // the bench's right is the body's left (model +X), and a CCW tilt on the
    // screen swings the mouth toward the body's right (model -X).
    const float a = v.angle;
    avatar.SetHeldAim(h, s.benchAimW[hk], Vec3{-std::sin(a), std::cos(a), 0.0f});
    if (holds && v.carried) pourHand = hk;
  }
  // ---- the carried flask: its arm follows it ---------------------------------
  // The vessel in the bench's hand is moved in the character's hand the way
  // it moves on the bench: up on the bench is up in the hand, toward the
  // other flask on the bench is toward the other hand -- so lifting a bottle
  // only raises that arm, and the hands meet only when the bottles do. Its
  // tilt is SetHeldAim's (above), about the wrist, so turning it never moves
  // the arm.
  const bool carry = pourHand >= 0;
  if (carry) s.benchPourHand = pourHand;
  s.benchPourW += ((carry ? 1.0f : 0.0f) - s.benchPourW) * ease;
  if (!carry && s.benchPourW < 0.02f) s.benchPourW = 0.0f;
  if (s.benchPourW <= 0.0f || s.benchPourHand < 0) {
    s.benchPourCmdValid = false;
    s.benchCarry.anchored = false;
    return;
  }
  const Hand ph = HandAt(s.benchPourHand), oh = OtherHand(ph);
  const bool fresh = avatar.StrikeHand() != ph || !s.benchPourCmdValid;
  avatar.SetStrikeHand(ph);
  Vec3 handFromShoulder;
  float reach = 0;
  if (!avatar.WeaponArmPose(handFromShoulder, reach)) return;
  // THE COMMAND IS CLOSED ON THE FLASK ITSELF. The point the arm's IK places
  // (the wrist) is not the flask (it hangs off the wrist at the bench's angle),
  // so no open-loop hand target lands. The command -- an offset from the
  // shoulder, in the frame WeaponArmPose reads -- starts where the arm is and
  // is nudged each tick by how far the flask's middle is from its goal.
  if (fresh) {
    s.benchPourCmd = handFromShoulder;
    s.benchPourCmdValid = true;
    s.benchCarry.anchored = false;
  }
  const Quat yaw = QuatAxisAngle({0, 1, 0}, avatar.Heading());
  const Vec3 left = QuatRotate(yaw, Vec3{1, 0, 0});    // bench +x (the mirror)
  const Vec3 fwd = QuatRotate(yaw, Vec3{0, 0, 1});
  // A held flask's middle and length: halfway between its base and its lip
  // along its own axis (the bench's angle).
  auto middle = [&](Hand h, float ang, Vec3& c, float& len) {
    const Vec3 ax = QuatRotate(yaw, Vec3{-std::sin(ang), std::cos(ang), 0.0f});
    Vec3 lip, base;
    if (!avatar.HeldMouthWorld(ax, lip, h) || !avatar.HeldMouthWorld(ax * -1.0f, base, h))
      return false;
    c = (lip + base) * 0.5f;
    len = (lip - base).len();
    return true;
  };
  const PlayerSession::BenchHold::HandView& pv = bh.hand[s.benchPourHand];
  PlayerSession::BenchCarry& bc = s.benchCarry;
  Vec3 c3{}, goal{};
  float len = 0;
  if (carry && middle(ph, pv.angle, c3, len)) {
    if (!bc.anchored) {
      // At pickup the flask is where the hold clip has it: that is the zero.
      bc = PlayerSession::BenchCarry{};
      bc.anchored = true;
      bc.c0 = c3;
      bc.bx = pv.cx;
      bc.by = pv.cy;
      // Up: the bench's own scale (a flask's height on the bench is its
      // length in the hand), so lifting it over the other clears it there too.
      bc.sy = len / std::max(pv.height, 1.0f);
      bc.sx = bc.sy;
      // Across: the gap between the two flasks on the bench is the gap
      // between the hands, so arriving at the other bottle on the bench
      // arrives at it here.
      const PlayerSession::BenchHold::HandView& ov = bh.hand[HandIndex(oh)];
      Vec3 o3;
      float olen = 0;
      if (s.benchClip[HandIndex(oh)] && middle(oh, ov.angle, o3, olen)) {
        const float sepB = ov.cx - pv.cx, sep3 = (o3 - c3).dot(left);
        if (std::fabs(sepB) > 4.0f && sep3 * sepB > 0.0f) {
          bc.other = true;
          bc.o3 = o3;
          bc.sepB = sepB;
          bc.sx = std::clamp(sep3 / sepB, bc.sy * 0.25f, bc.sy * 2.0f);
        }
      }
    }
    const float dx = pv.cx - bc.bx, dy = pv.cy - bc.by;
    // The bench's headroom (AlchemyBench::kLiftH) lets a flask go ~1.3 of
    // its lengths above where it stood, and a tall screen's grid further:
    // the arm follows up to 1.4 flask lengths and holds there (about the
    // face), rather than straightening overhead. The bench is not clamped.
    const float up = std::min(dy * bc.sy, 1.4f * std::max(len, 0.0f));
    goal = bc.c0 + left * (dx * bc.sx) + Vec3{0, up, 0};
    // ...and in depth, toward the other flask as it is approached, so the
    // two meet rather than pass one in front of the other.
    if (bc.other) {
      const float t = std::clamp(dx / bc.sepB, 0.0f, 1.0f);
      goal = goal + fwd * ((bc.o3 - bc.c0).dot(fwd) * t);
    }
    s.benchPourCmd = s.benchPourCmd + (goal - c3) * 0.35f;
    // ON A LEASH TO THE HAND (anti-windup). Whatever the arm cannot serve --
    // a pose limit, the reach -- the command used to keep integrating past,
    // and the clamps' answers out there jump with small changes of the ask;
    // that was half of the arm "tweaking out" with a flask held high (the
    // other half was the pole and the fist's roll: below, Mob::ApplyHeldAim).
    // Held within a step of where the hand actually is, the command rests on
    // a limit instead of winding past it. Only once the claim is whole: while
    // it eases in, the hand is still partly the hold clip's and the gap is
    // not a refusal. `SANDVOX_BENCH_HIGH=1 SANDVOX_BENCH_DEBUG=2 --shot-bench`
    // is the repro: a flask lifted to the top and swept, every tick printed.
    if (s.benchPourW > 0.95f) {
      const Vec3 lead = s.benchPourCmd - handFromShoulder;
      const float ll = lead.len(), kLeash = 0.6f;
      if (ll > kLeash) s.benchPourCmd = handFromShoulder + lead * (kLeash / ll);
    }
  }
  const float l = s.benchPourCmd.len();
  if (reach > 0 && l > reach * 0.98f) s.benchPourCmd = s.benchPourCmd * (reach * 0.98f / l);
  WeaponPose wp;
  wp.hand = s.benchPourCmd;
  wp.weight = s.benchPourW;
  wp.steerBlade = false;
  // ELBOW DOWN, not the rig's elbow-back. Lifting the flask puts the hand
  // straight out in front at shoulder height, where a back pole is
  // antiparallel to the reach and the elbow flipped every tick (the arm
  // "tweaking out" high up); down is well-conditioned everywhere the bench
  // can take the hand, and for a low reach it names the same bend plane.
  wp.usePole = true;
  wp.bendPole = Vec3{0, -1, 0};
  // The elbow's hinge follows that plane (the axis cone off, as the melee
  // tuning ships it): penned near the authored axis, the clamp refuses the
  // bend the down pole asks for.
  wp.elbowAxisCone = 3.14159265f;
  avatar.SetWeaponPose(wp);
  if (std::getenv("SANDVOX_BENCH_DEBUG")) {
    static int n = 0;
    static const bool every = std::getenv("SANDVOX_BENCH_DEBUG")[0] == '2';
    const Mob::WeaponArmDiag& dg = avatar.WeaponArmDiagnostics();
    if (every || (n++ % 15) == 0)
      std::printf("benchcarry: w %.2f flask (%.1f %.1f %.1f) goal (%.1f %.1f %.1f) cmd (%.1f %.1f %.1f) sx %.4f sy %.4f other %d reach %.1f"
                  " | hand (%.2f %.2f %.2f) ikMiss %.2f shClamp %.2f elClamp %.2f clShift %.2f\n",
                  s.benchPourW, c3.x, c3.y, c3.z, goal.x, goal.y, goal.z, s.benchPourCmd.x,
                  s.benchPourCmd.y, s.benchPourCmd.z, bc.sx, bc.sy, (int)bc.other, reach,
                  handFromShoulder.x, handFromShoulder.y, handFromShoulder.z, dg.ikMiss,
                  dg.shoulderClamp, dg.elbowClamp, dg.clampShift);
  }
}

// ---- A SPELL ARMED IN THE HAND (assets/anims/spell_ready.json) ---------------
// Every hand holding a spell holds it up: forearm level and pointing ahead,
// the open hand presenting it (the Skyrim ready pose). A CLIP, not the arm
// claim, so both hands can be armed at once; the left plays the library's
// derived mirror. The cast's thrust (PoseSpellHands) is IK over the top of it
// and fades back into it.
static void PoseSpellReady(PlayerSession& s, PlayerAvatar& avatar,
                           const HandsNow& hands) {
  for (int hk = 0; hk < kHands; hk++) {
    const Hand h = HandAt(hk);
    // Also an arm whose hand is GONE but still has a spell equipped: the pose
    // is what places that spell (SpellHandPoint reads the absent hand's posed
    // point), so the stump presents it where the hand would.
    const bool armed = avatar.Spawned() &&
                       (hands.hand[hk].spell ||
                        (hands.hand[hk].item == nullptr && s.caster.hand[hk].Equipped()));
    const std::string clip = avatar.HandClip("spell_ready", h);
    if (clip.empty()) continue;
    // Re-played if it is gone while it should be up: an R/F5 reload rebuilds
    // the rig's clip state under a hand that is still armed.
    const bool lost = armed && s.spellClip[hk] && avatar.ClipWeight(clip.c_str()) <= 0.0f;
    if (armed != s.spellClip[hk] || lost) {
      if (armed) avatar.PlayClip(clip);
      else avatar.StopClip(clip);
    }
    s.spellClip[hk] = armed;
  }
}

// ---- THE CASTING ARM (session.h spellArm*) ----------------------------------
// A cast from a spell hand throws that arm out the way the spell flies: the
// hand is drawn back toward the shoulder, pushed out to nearly the arm's full
// reach along the aim, held there while the hand's button is held (a beam's
// sustain), then the claim fades and the animation takes the arm back.
//
// It drives the ONE weapon-arm claim, pointed at the empty fist itself
// (StrikeEffectorMode::Chain on the hand part, the way an unarmed style is),
// so it runs after the melee pose and PoseBenchHands and wins over the idle
// ready pose. It gives way to a live strike and to the bench's carried flask.
//
// The target is recomputed from the aim EVERY tick, so turning while the arm
// is out swings it with you. Only the start point is remembered: the push
// begins where the arm actually was, so nothing snaps.
static void PoseSpellHands(PlayerSession& s, PlayerAvatar& avatar,
                           const HandsNow& hands, const TickInput& ti,
                           Vec3 aim, bool strikeLive) {
  // Seconds: the draw-back, the push, and the hold after it (at least).
  constexpr float kCock = 0.07f, kThrust = 0.10f, kHold = 0.20f;
  // Reach fractions: how far out the drawn-back hand sits and where the push
  // ends (short of straight, where the two-bone solve is ill-conditioned).
  constexpr float kCockReach = 0.42f, kOutReach = 0.95f;
  if (s.spellCastHand >= 0) {
    s.spellArmHand = s.spellCastHand;
    s.spellArmT = 0.0f;
    s.spellArmFresh = true;
    s.spellCastHand = -1;
  }
  const int hk = s.spellArmHand;
  if (hk < 0) return;
  const Hand h = HandAt(hk);
  auto drop = [&] {
    s.spellArmHand = -1;
    s.spellArmW = 0.0f;
    s.spellArmFresh = false;
  };
  // A strike took the arm (its effector is the strike's now: leave it), or
  // the bench is carrying a flask.
  if (strikeLive || s.benchPourW > 0.0f) {
    drop();
    return;
  }
  const int part = h == Hand::Left ? avatar.Parts().handL : avatar.Parts().handR;
  // The hand stopped being a spell hand (an item arrived, the arm was ruined)
  // or the body is gone: hand the arm straight back.
  if (!avatar.Spawned() || !hands.hand[hk].spell || part < 0 || !avatar.HandUsable(h)) {
    avatar.ClearStrikeEffector();
    drop();
    return;
  }
  avatar.SetStrikeHand(h);
  avatar.SetStrikeEffector(part, StrikeEffectorMode::Chain);
  Vec3 live;
  float reach = 0;
  if (!avatar.WeaponArmPose(live, reach) || reach <= 1e-3f) {
    avatar.ClearStrikeEffector();
    drop();
    return;
  }
  if (s.spellArmFresh) {
    s.spellArmFrom = live;
    s.spellArmFresh = false;
  }
  const float t = s.spellArmT;
  s.spellArmT += kTickDt;
  const bool held = ti.Held(h == Hand::Right ? TB_ATTACK : TB_ALT);
  const bool out = t < kCock + kThrust + kHold || held;
  if (out) {
    s.spellArmW = 1.0f;
  } else {
    s.spellArmW *= std::exp(-kTickDt * 10.0f);
    if (s.spellArmW < 0.02f) {
      avatar.ClearStrikeEffector();
      drop();
      return;
    }
  }
  // THE AIM, never through the body: a camera looking back at the caster's
  // face would ask the arm to reach behind its own back. The horizontal part
  // is kept in front of the body's facing; pitch is the aim's.
  const Vec3 bodyFwd = QuatRotate(QuatAxisAngle({0, 1, 0}, avatar.Heading()), Vec3{0, 0, 1});
  Vec3 dir = aim.len() > 1e-4f ? aim.normalized() : bodyFwd;
  const float along = dir.x * bodyFwd.x + dir.z * bodyFwd.z;
  if (along < 0.3f) dir = (dir + bodyFwd * (0.3f - along)).normalized();
  const Vec3 cocked = dir * (reach * kCockReach) + Vec3{0, reach * 0.12f, 0};
  const Vec3 goal = dir * (reach * kOutReach);
  auto smooth = [](float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
  };
  Vec3 target;
  if (t < kCock) {
    target = s.spellArmFrom + (cocked - s.spellArmFrom) * smooth(t / kCock);
  } else if (t < kCock + kThrust) {
    // Ease OUT: the push is fastest as it leaves, the way a throw is.
    const float u = std::clamp((t - kCock) / kThrust, 0.0f, 1.0f);
    const float e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
    target = cocked + (goal - cocked) * e;
  } else {
    target = goal;
  }
  WeaponPose wp;
  wp.hand = target;
  wp.weight = s.spellArmW;
  wp.steerBlade = false;
  // ELBOW DOWN, as the bench's carried flask: a hand straight out in front
  // at shoulder height is antiparallel to the rig's own back pole and the
  // elbow would flip from tick to tick (PoseBenchHands says so at length).
  wp.usePole = true;
  wp.bendPole = Vec3{0, -1, 0};
  wp.elbowAxisCone = 3.14159265f;
  avatar.SetWeaponPose(wp);
}

static inline uint32_t OpLandingTick(const TickAuthorityCtx& w, uint32_t tick) {
  return tick + (w.opsync ? w.opsync->Delay() : 0u);
}

// THE FLICK -> A COMPASS STYLE FOR THIS HAND, -1 when the flick names none.
// A MIRRORED STROKE TRAVELS THE MIRROR OF ITS SECTOR (strokes.h "THE LEFT HAND
// IS THE RIGHT, MIRRORED"): the left hand swinging a right-authored style
// reflects it through the body, so the sector the flick named would cut the
// OTHER way across the screen. Pick with the flick reflected instead, so the
// blade goes where the mouse went — kept only if that pick is mirrored too
// (the unarmed compass has native left-side punches). The press and the
// charged hold's re-aim both ask this, so they can never disagree.
static int FlickStyleFor(const StyleLibrary& lib, const PlayerStrikeMap& map,
                         Hand hand, float fx, float fy) {
  int si = QuantizeStrike(map, fx, fy);
  const AttackStyle* raw = lib.At(si);
  if (raw && StrokeMirrored(*raw, hand)) {
    const int ri = QuantizeStrike(map, -fx, fy);
    const AttackStyle* refl = lib.At(ri);
    if (refl && StrokeMirrored(*refl, hand)) si = ri;
  }
  return si;
}

// ---- PHASE A (PLAYER) - the controller, the look delta and the strike quantize
// Verbatim from the single-body TickAuthority, lines 246-361 at 54fe241.
static void PhaseA(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  // M9.1 P3, AND IT IS THE WHOLE PACKAGE'S ONE LINE IN THIS FILE. Session 0
  // reads the world through the snapshot's 3x3x3 mirror, which is centred on
  // IT (phase H submits with session 0's chunk); every other session reads it
  // through World::KindAtCached, whose cache this keeps warm one chunk ahead of
  // the body. Gated on `index > 0` so session 0's tick — and therefore the
  // one-session op record P1 pinned byte-for-byte — is untouched by
  // construction: the primary never enters here, so no fetch is queued that
  // was not queued before and the fetch FIFO's order is unchanged.
  if (s.index > 0) s.PrefetchAround(w.world);
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
        // Slippery feet: a coat whose material lists "slippery" in its
        // coat.effects (oil) on the SOLES of the limbs tagged "foot" ramps
        // ground grip from 1 down to player.slipGrip. Read off the coat
        // ledger, so it follows the coat as it is shed onto the floor and
        // washed off. Sole, not whole foot: the whole-limb fraction divided a
        // stepped-in coat by the foot's interior and never reached onset.
        player.groundGrip = 1.0f;
        if (couple) {
          const auto& pt = CurrentTuning().player;
          const float f = mobs.CoatTagFraction(avatar.Id(), "slippery", "foot",
                                               /*sole=*/true);
          const float span = std::max(1e-4f, pt.slipCoatFull - pt.slipCoatStart);
          const float t = std::clamp((f - pt.slipCoatStart) / span, 0.0f, 1.0f);
          player.groundGrip = 1.0f + (pt.slipGrip - 1.0f) * t;
        }
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
        // `wasInLiquid` IS THE SESSION'S (M9.1 P1). It was the one
        // function static in this file, which is exactly the shape
        // DESIGN.md S10 forbids: two players wading into two lakes would
        // have shared one edge detector and the second splash would never
        // have fired.
        const float enterSpeed = -player.vel.y;
        // A limp or rising body owns the player, not the controller: no
        // input, no gravity, no sweeps. The capsule is moved onto the body
        // after each physics step (PlayerAvatar::RagdollFollow, below).
        if (avatar.Spawned() && avatar.Ragdolled()) {
          player.vel = {};
          player.SnapRender();   // the rig owns the body: nothing to lerp
          // Update() is what normally ages the step-smoothing offsets, and it
          // is not running. Left frozen they would hold the camera (and now
          // the art, Player::RenderBodyOffset) a step off the ground for the
          // whole ragdoll — so age them here on the same clock.
          player.DecayViewSmooth(kTickDt,
                                 CurrentTuning().player.viewSmoothHalflife);
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
      // ...FOR THE PRIMARY ONLY. --autofly-surface pins an altitude and
      // --autofly-park latches a stop; both are the FRAME layer's script
      // for the window's own player, and running them for every session
      // would fly a remote body on this machine's argv.
      if (s.index == 0 && w.afterPlayerUpdate) w.afterPlayerUpdate(player, tick);
      // THE COMMAND JOINS THE RECORD (package N3, sim/oprecord.h). Stashed
      // here rather than passed to SubmitTick at the bottom of the tick: the
      // command is decided at the TOP of the body and threading it through a
      // signature four harnesses share to carry a value only the game has is
      // the worse trade — the gen list is stashed for the same reason.
      // ONE COMMAND PER TICK IN THE RECORD, and it is the primary's: a
      // Frame carries a single `cmd` (sim/oprecord.h), so a second session
      // calling this would overwrite the first. The format that carries one
      // command per player is M9.2's wire, not this file's record.
      if (s.index == 0) sandvox::opstream::NoteTickInput(tick, ti);
      // ---- THE LOOK DELTA, ONCE PER TICK, TO THE STRIKE PICKER --------------
      // The picker integrates a whole tick's raw pixels at kTickDt. It is the
      // ONLY consumer: the swing driver is fed by the stroke program, and
      // feeding it the mouse as well would bend every authored cut (D10 of the
      // discrete-strikes plan — the freeform mode that did is gone).
      {
        // ...and it LATCHES the remembered direction (strike_pick.h) that a
        // CHARGED hold re-aims to; a normal press still reads the live flick.
        // On the tick clock, from the command, like the rest.
        strikePicker.Feed(ti.lookDx, ti.lookDy, kTickDt,
                          CurrentTuning().melee.pickMinSpeed,
                          CurrentTuning().melee.chargeFlickTicks,
                          CurrentTuning().melee.chargeFlickPx);
        // DISCRETE STRIKES: the click is the whole input, and the flick is
        // read at the tick that consumes the press edge — the freshest read
        // available, since the delta above is every pixel moved since the
        // previous tick. A strike fired from the buffer later still cuts the
        // direction that was flicked, aimed wherever the camera is THEN.
        // TWO BUTTONS, TWO HANDS (dual wielding): LMB is the right hand's,
        // RMB the left's. A hand that holds a vessel is not here — its
        // button is the vessel's (the VESSELS block) — and one that cannot
        // swing (no weapon, no usable fist, a ruined arm) is not ready.
        // Both on one tick: the right is read first and the left banks
        // behind it, through the same latch a mid-swing click uses.
        int pressHand = -1;
        if (ti.Pressed(TB_ATTACK) && hands[Hand::Right].ready) pressHand = 0;
        else if (ti.Pressed(TB_ALT) && hands[Hand::Left].ready) pressHand = 1;
        if (pressHand >= 0) {
          const Hand ph = HandAt(pressHand);
          // WHICH COMPASS: three maps keyed on what is in THIS fist
          // (strokes.h PlayerCompass; the HUD's compass asks the same).
          const PlayerStrikeMap& map =
              PlayerCompass(mobs.AttackStyles(), hands[ph].weapon);
          float fx = 0, fy = 0;
          int si = -1;
          const bool flicked =
              strikePicker.Pick(CurrentTuning().melee.pickMinSpeed, fx, fy);
          if (flicked) si = FlickStyleFor(mobs.AttackStyles(), map, ph, fx, fy);
          if (si < 0) {
            // No flick after a SHORT BLADE'S STAB (the compass's
            // `clickRepeat`): stab again with the hand that stabbed, and it
            // chains (chainsNow below), so clicking is a fast repeated stab.
            // Flick any direction to leave it.
            const ItemDef* pItem = hands[ph].weapon ? hands[ph].item : nullptr;
            if (s.strikeBaseHand == ph &&
                StrikeRepeats(map, strikeBase,
                              FormForItem(pItem)))
              si = strikeBase;
          }
          if (si < 0) {
            // No flick: alternate the two horizontals so plain clicking is a
            // usable L/R rhythm rather than the same cut stamped.
            si = NeutralStrike(map, strikePicker.altRight);
            strikePicker.altRight = !strikePicker.altRight;
          }
          if (si >= 0) {
            strikeQueued = si;
            s.strikeQueuedHand = ph;
            s.lastHand = ph;
          }
          // A hold of this strike starts from what it threw (strike_pick.h
          // Seed): a neutral click remembers nothing, so it re-aims only on
          // a flick made while held.
          strikePicker.Seed(flicked, fx, fy);
          s.lastStrikePick.style = si;
          s.lastStrikePick.fx = fx;
          s.lastStrikePick.fy = fy;
          s.lastStrikePick.flicked = flicked;
          s.lastStrikePick.serial++;
        }
      }

  }
}

// ---- PHASE B (WORLD) - the interest set, the window shift and the far cascades
// Verbatim from the single-body TickAuthority, lines 362-432 at 54fe241.
static void PhaseB(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {
      // ---- M9.3-C: INSTALL WHAT THE PEER SENT, BEFORE ANYTHING ELSE -------
      //
      // A chunk whose digest diverged from its AUTHORITY's has arrived whole
      // (net/chunksync.h reassembled the slices) and this is where it goes in.
      // THE POSITION IS THE POINT and there are three reasons for it, in
      // descending order of how badly getting it wrong would hurt:
      //
      //  1. BEFORE THE TICK'S SUBMIT (phase N), so the CA that runs this tick
      //     runs on the corrected chunk. One tick later would mean one more
      //     tick of the wrong world propagating into its neighbours, and the
      //     next hash comparison would still see a mismatch.
      //  2. BEFORE `stream.Update` below, so a window shift on this tick
      //     cannot recycle the slot between the residency test inside
      //     ReplaceChunk and the upload it does.
      //  3. INSIDE `tick`, so `NoteChunkReplace` files it under the frame the
      //     op record is about to write. That is what makes a replay of a
      //     networked session reproduce the sync (oprecord.h Frame).
      //
      // Null pointer in every harness and in every single-player frame, so
      // nothing here runs that did not run before this package — the oracle
      // op-record comparison is the proof.
      if (w.chunksync != nullptr) {
        for (net::ChunkSync::PendingReplace& pr : w.chunksync->TakePending()) {
          if (stream.ReplaceChunk(pr.wc, pr.words.data())) {
            w.chunksync->NoteApplied();
            // RE-ENCODE RATHER THAN CARRY THE WIRE BYTES. The RLE that
            // arrived is the authority's; what the record must reproduce is
            // what was INSTALLED, and ReplaceChunk's decode/encode round trip
            // is byte-exact (stream.h's kPersistMask note) so the two agree —
            // but if they ever stopped agreeing, recording the installed
            // words is the one that keeps a replay faithful.
            std::vector<uint32_t> rle;
            RleEncodeChunk(pr.words.data(), rle);
            sandvox::opstream::NoteChunkReplace(tick, pr.wc, rle);
          } else {
            // The window shifted between the request and the reply. Counted,
            // never silent: a chunk that did not arrive and a chunk that
            // arrived for somewhere we no longer hold are different facts.
            w.chunksync->NoteRefusedNotResident();
          }
        }
      }
      // ...and the REPLAY twin, at the identical point. Unconditional because
      // it is one pointer test when nothing is replaying; the two drive loops
      // that bypass TickAuthority entirely (selftest_sim's `ops-replay` and
      // main's `--replay-ops`) call it themselves, at their own phase-B
      // position, for the same reason.
      sandvox::opstream::ReplaceChunksIfReplaying(tick, stream);

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
      // ONE ENTRY PER SESSION, THE PRIMARY FIRST. `primary = 0` is the only
      // entry Stream::Update and FarField::Update consume, and session 0 is
      // the window's: THE WINDOW FOLLOWS THE PRIMARY. The rest are
      // information for a residency policy that does not exist yet (M9.3),
      // so adding them cannot move a byte today - and for one session this
      // list is the same single entry it has always been.
      interest.chunks.push_back(playerChunkNow);
      for (size_t pi = 1; pi < players.size(); pi++) {
        const Vec3& pp = players[pi].s->player.pos;
        interest.chunks.push_back(
            {ifloor(pp.x) >> 4, ifloor(pp.y) >> 4, ifloor(pp.z) >> 4});
      }
      // ...AND THE PEERS' BODIES (M9.2 package B). Appended after every local
      // session, so `primary = 0` still names session 0 and the window still
      // follows the window's own player. Information only: nothing downstream
      // consumes an entry past `primary` yet, which is exactly why adding
      // them cannot move a byte. Guarded on non-empty so a single-player
      // process makes no call here at all — the oracle op record is the
      // proof and the guard is what earns it.
      if (w.remotes != nullptr && !w.remotes->list.empty())
        RemotePlayersAppendInterest(*w.remotes, interest);
      interest.primary = 0;
      farCount = 0;
      {
        // ---- STREAM: the row that flying lights up --------------------
        // The toroidal window shift, chunk fetch/evict and the far-field
        // cascade recentre. THIS is the cost the Performance tab was reporting
        // as "input" — it had no timer, so it fell into the residual, and the
        // residual was billed to the input row.
        sandvox::PerfSpan spanStream(sandvox::PerfScope::Stream);
        // A HARNESS PINS THE WINDOW (session.h section J): an empty interest
        // set is Stream::Update's own "no shift", and it still pays the
        // harvests and deferred wakes it owes.
        stream.Update(w.harness.pinWindow ? InterestSet{} : interest, tick);
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
        if (!farBlind && !w.harness.pinWindow) far.Update(interest);
        farCount = far.PrepareTick(ctx.queue, !farBlind);
        if (farCount) {
          g_farEntries += farCount;
          g_farTicks++;
          g_farBiggest = std::max<uint64_t>(g_farBiggest, farCount);
        }
      }
  }
}

// ---- PHASE C (PLAYER) - brush size/material and the laser ray
// Verbatim from the single-body TickAuthority, lines 437-535 at 54fe241.
static void PhaseC(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {

      // (`ops` is aliased out of the caller's OpBatch at the top.)
      brush.radius = ui.brushRadius;
      brush.grainEighths = (uint32_t)ui.brushGrain;
      brush.material = (uint32_t)ui.brushMaterial;

      // laser (PLAN §C1/C2): laser tool + LMB, or hold F from any tool.
      // Bodies are tested first — a mob limb or debris chunk in the beam
      // takes the hit instead of the wall behind it.
      // `laserCut` is the SESSION'S tick scratch (PlayerScratch, top of
      // this file): the cut is decided HERE, where this tick's camera and
      // physics are current, and applied in phase K where `spawns` exists.
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
          //
          // ONE CHARGE (W2-H). This was `avatar.Damage(...)` and then
          // `mobs.Damage(...)`, and the second already walks every avatar
          // (MobSystem's registered list, W1-F) -- so the first was a copy
          // that could only ever find what the second finds, and a hit it did
          // find was charged but never bored. MobSystem::LaserHit is the one
          // charge, through the shell table on a worn slot.
          float bore = 0.0f;
          if (mobs.LaserHit(hitBody, hitPos, bore)) {
            // A limb hit is now BOTH: the hp/sever logic above (joint
            // crossings, flinch, loco states) AND a real channel bored through
            // the flesh. Deferred like the melt below, for the same reason.
            //
            // Damage() may have severed the limb outright, in which case this
            // handle is no longer a live limb — the carve then simply misses
            // (CarveLimbRadial returns false) rather than touching stale state.
            laserCut = {hitBody, hitPos, bore, true};
          } else {
            // Deferred: the melt needs the `spawns` list that debris.PreTick
            // fills further down, and the ray must be cast HERE where the
            // camera and physics state for this tick are current. Carrying the
            // hit forward is cheaper than reordering the tick.
            //
            // A CORPSE IS STILL FLESH. The kill usually lands a few ticks into
            // a held beam, and the melt radius (2 voxels, sized for rock) then
            // bored a ball out of the dead head every tick — the clean hole
            // the live carve had started turned into a crater. Dead flesh
            // takes the same sub-voxel bore a living limb does.
            if (debris.BodyIsDeadFlesh(hitBody)) {
              laserCut = {hitBody, hitPos,
                          (float)CurrentTuning().tools.laserCarveRadius, false};
            } else {
              float br = (float)CurrentTuning().tools.laserMeltRadius;
              laserCut = {hitBody, hitPos + fwd * (br * 0.5f), br, false};
            }
          }
        } else if (gridDist < 1e8f) {
          const int r = CurrentTuning().tools.laserMeltRadius;
          ops.push_back({hit.x, hit.y, hit.z, r, 0, 2u /*melt*/, 0, 0});
          // cutting through a support must drop the far side: rate-limited
          // island checks over the cut (support-loss flags catch the rest)
          if (tick % 8 == 0)
            debris.AddDestructionEvent(OpLandingTick(w, tick),
                                       {hit.x - r, hit.y - r, hit.z - r},
                                       {hit.x + r, hit.y + r, hit.z + r});
        }
      }
  }
}

// ---- PHASE D (WORLD) - the mob-spawn key, --duel-dummy and the dev/AI panel
// Verbatim from the single-body TickAuthority, lines 536-794 at 54fe241.
static void PhaseD(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {

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
        ui.aiSpawnOwn = ui.aiSpawnRandom = false;
        ui.aiKillSpawned = false;
      }
      if (labScene >= 0) ui.aiSpawnProfile.clear();
      if (ui.aiSpawnDummy || ui.aiSpawnStatic || ui.aiSpawnDuelist ||
          ui.aiSpawnOwn || ui.aiSpawnRandom || !ui.aiSpawnProfile.empty()) {
        // A RANDOM HUMAN (UIState::aiSpawnRandom): every pick below that the
        // panel would have made -- body, weapon, outfit, behaviour -- is rolled
        // instead, off the tick and this panel's spawn count, the way the dye
        // already was: a replay spawns the same stranger. The body is one of
        // the baked pool (MobSystem::PoolDef); the ticked effects still apply.
        const bool rnd = ui.aiSpawnRandom;
        ui.aiSpawnRandom = false;
        const uint32_t rseed = rng::Hash3(tick, 0x68756d61u,
                                          (uint32_t)aiSpawnedMobs.size());
        auto rpick = [&](uint32_t salt, uint32_t n) {
          return n == 0 ? 0u : rng::Hash3(rseed, salt, 0x9e37u) % n;
        };
        // EMPTY MEANS "THE CREATURE'S OWN", resolved once the def is known --
        // see UIState::aiSpawnOwn for what the override was costing.
        // The profiles a random human may get: the FIGHTING STYLES
        // (behaviors.json, 2026-09-27), which read their own wounds, your
        // weapon and your swings. Not the gate fixtures (`crowder` never
        // swings, `retreater` only backs away), not `dummy` /
        // `training_dummy` (they do nothing) and not `zombie` (a living human
        // that bites is not a trait).
        static const char* const kRandomProfiles[] = {
            "swordsman", "fencer", "brawler", "berserker", "guardian"};
        const std::string pickedProfile = ui.aiSpawnProfile;
        ui.aiSpawnProfile.clear();
        const char* profile =
            rnd ? kRandomProfiles[rpick(4, 5)]
            : !pickedProfile.empty() ? pickedProfile.c_str()
            : ui.aiSpawnOwn    ? ""
            : ui.aiSpawnDummy  ? "dummy"
            : ui.aiSpawnStatic ? "swordsman_static"
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
        if (rnd) {
          const std::vector<std::string> pool = mobs.PoolNames();
          if (pool.empty()) {
            std::printf("AI panel: no random-human pool (assets/mobs/pool/ is "
                        "empty; bake it with `node scripts/bake_human_pool.mjs`) "
                        "-- spawning the picked creature instead\n");
          } else {
            std::string plog;
            aiDef = mobs.PoolDef(pool[rpick(1, (uint32_t)pool.size())], &plog);
            if (!plog.empty()) std::printf("%s", plog.c_str());
          }
        }
        // (A random spawn that got a pool body skips the picker entirely; one
        // that did not falls through to it.)
        for (size_t i = 0; !(rnd && aiDef >= 0) && i < mobs.Defs().size(); i++) {
          // The panel's eligibility (main.cpp rebuildAiCreatures): a body that
          // can hold a weapon, or one that fights with its own (a snake).
          if (mobs.Defs()[i].FindSocket("held_right") < 0 &&
              mobs.Defs()[i].natural.empty())
            continue;
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
            const int weaponPick =
                rnd ? (int)rpick(2, (uint32_t)ui.aiWeaponNames.size())
                    : ui.aiWeaponPick;
            const std::string pick =
                ui.aiWeaponNames.empty()
                    ? std::string()
                    : ui.aiWeaponNames[weaponPick < (int)ui.aiWeaponNames.size()
                                           ? weaponPick
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
            // A random human's outfit mode: mostly everyday clothes, some in
            // plate, some mixed piece by piece (mode 4, below), a few in
            // nothing but their underwear.
            int outfit = ui.aiOutfit;
            if (rnd) {
              const uint32_t o = rpick(3, 100);
              outfit = o < 10 ? 0 : o < 65 ? 1 : o < 80 ? 2 : 4;
            }
            {
              int dressed = 0;
              for (int s = 0; s < (int)ui.aiWearNames.size(); s++) {
                if (!EquipSlotIsWorn(s)) break;
                const ItemDef* piece = nullptr;
                const uint32_t roll = rng::Hash3(tick, (uint32_t)nid, (uint32_t)s);
                if (outfit == 1) {
                  // RANDOM CLOTHES: a dyeable piece of this slot's kind. The
                  // commoner set is chest / legs / boots, so a helmet slot has
                  // no candidates and stays bare, which is the point.
                  std::vector<const ItemDef*> cands;
                  for (const ItemDef& it : items.items)
                    if (it.dyeable && EquipSlotAccepts(s, it.kind))
                      cands.push_back(&it);
                  if (!cands.empty()) piece = cands[roll % cands.size()];
                } else if (outfit == 2) {
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
                } else if (outfit == 3) {
                  const int wp = s < (int)ui.aiWearPick.size() ? ui.aiWearPick[s]
                                                                : 0;
                  if (wp > 0 && wp < (int)ui.aiWearNames[s].size())
                    piece = items.At(items.Find(ui.aiWearNames[s][wp]));
                } else if (outfit == 4) {
                  // MIXED: any piece this slot takes, or none -- entry 0 of
                  // the slot list is "(none)", so a bare slot is one of the
                  // outcomes rather than a special case.
                  const std::vector<std::string>& names = ui.aiWearNames[s];
                  const size_t wp =
                      names.empty() ? 0 : (size_t)(roll % (uint32_t)names.size());
                  if (wp > 0 && wp < names.size())
                    piece = items.At(items.Find(names[wp]));
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
              if (outfit != 0)
                std::printf("AI panel: %s spawned wearing %d piece(s)\n",
                            d.name.c_str(), dressed);
            }
            // THE SIDECAR'S OWN PROFILE when the button asked for it, and the
            // same `empty() ? "duelist" : behavior` fallback --shot-strike
            // uses, so a def that names none still gets something that fights.
            // A body with no hand (a snake: natural weapons only) cannot fight
            // the way a duelist profile drives it, so unless a profile was
            // picked by name it keeps its own -- the default buttons are
            // swordplay presets.
            const bool handless = d.FindSocket("held_right") < 0;
            const std::string prof =
                (handless && pickedProfile.empty() && !d.behavior.empty())
                    ? d.behavior
                : *profile != '\0' ? std::string(profile)
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
            if (rnd)
              std::printf("AI panel: random human %s -- %s, outfit %d, "
                          "behaviour %s\n",
                          d.name.c_str(), pick.empty() ? "fists" : pick.c_str(),
                          outfit, prof.c_str());
            aiSpawnedMobs.push_back(nid);
          }
        }
      }
      if (ui.aiKillSpawned) {
        ui.aiKillSpawned = false;
        // Kill rather than delete: the creature goes limp as a dead Mob and the
        // whole death path gets exercised. Silently dropping the
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
  }
}

// ---- PHASE E (PLAYER) - the wardrobe panel
// Verbatim from the single-body TickAuthority, lines 795-894 at 54fe241.
static void PhaseE(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {
      // ---- WARDROBE panel: make a set of clothes in a colour --------------
      //
      // Producers on the SAME paths the game uses: the kit's own containers
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
      // ---- F1 Spawn tab: a vessel filled to capacity, into your inventory ---
      // Resolved BY NAME (UIState::giveVesselNames) and admitted by the same
      // ContainerAccepts rule a scoop obeys, so the panel cannot make a flask
      // of sand. Hotbar first, then the bag, as every other give does.
      if (ui.giveVessel) {
        ui.giveVessel = false;
        const bool okV = ui.giveVesselPick >= 0 &&
                         ui.giveVesselPick < (int)ui.giveVesselNames.size() &&
                         ui.giveVesselPick < (int)ui.giveVesselMats.size();
        const std::string vname = okV ? ui.giveVesselNames[ui.giveVesselPick] : "";
        const std::vector<std::string>* ml =
            okV ? &ui.giveVesselMats[ui.giveVesselPick] : nullptr;
        const std::string mname =
            ml && ui.giveMatPick >= 0 && ui.giveMatPick < (int)ml->size()
                ? (*ml)[ui.giveMatPick] : "";
        const int di = items.Find(vname);
        const ItemDef* d = items.At(di);
        uint32_t mat = 0;
        for (size_t m = 1; m < mats.size() && !mat; m++)
          if (mats[m].name == mname) mat = (uint32_t)m;
        const char* why = nullptr;
        if (!d || !mat) {
          ui.giveVesselStatus = "pick a vessel and a material";
        } else if (!ContainerAccepts(*d, StackOf(items, di), mat, mats, &why)) {
          ui.giveVesselStatus = why ? why : "refused";
        } else {
          ItemStack full = StackOf(items, di);
          full.ClearFill();
          full.contents.Add((uint16_t)mat, (uint32_t)d->container.capacity);
          const bool ok = hotbar.Add(full) >= 0 || kit.bag.Add(full) >= 0;
          ui.giveVesselStatus = ok ? vname + " of " + mname + " — in your pack"
                                   : "no room in the hotbar or the bag";
        }
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
          // not a feature (ItemDef::dyeable). Mob::DressFromKit a few hundred
          // lines down notices the changed dye, carries the shells' damage back
          // into the stack and rebuilds them in the new colour, which is why
          // nothing here touches the rig.
          int n = 0;
          for (int s = 0; s < kEquipSlotCount; s++) {
            ItemStack& st = kit.equip.slots[s];
            const ItemDef* d = items.Of(st);
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
                // Off the body WITH its holes (Mob::KitTake flushes the
                // shells), and into the pack as the object it is.
                const ItemStack was =
                    avatar.KitTake(KitRef{KitSpace::Equip, slot});
                if (!was.Empty()) kit.bag.Add(was);
                kit.equip.slots[slot] = StackOf(items, di, 1, dye);
                ok = true;
              }
            } else {
              ok = hotbar.Add(StackOf(items, di, 1, dye)) >= 0 ||
                   kit.bag.Add(StackOf(items, di, 1, dye)) >= 0;
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

  }
}

// ---- PHASE F (WORLD) - the AI panel's behaviour apply
// Verbatim from the single-body TickAuthority, lines 895-904 at 54fe241.
static void PhaseF(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {
      if (ui.aiApplyBehavior) {
        ui.aiApplyBehavior = false;
        if (ui.aiMobSelected >= 0 &&
            ui.aiMobSelected < (int)ui.aiMobIds.size() &&
            ui.aiBehaviorPick >= 0 &&
            ui.aiBehaviorPick < (int)ui.aiProfileNames.size())
          mobs.SetMobBehavior(ui.aiMobIds[ui.aiMobSelected],
                              ui.aiProfileNames[ui.aiBehaviorPick]);
      }

  }
}

// ---- PHASE G (PLAYER) - sphere, fluid pour, brush op and prefab stamp
// Verbatim from the single-body TickAuthority, lines 905-1085 at 54fe241.
static void PhaseG(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {
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
        uint32_t pourMat = fluidPourMat[(uint32_t)ui.fluidPour & 3u];
        if (pourMat == 0) pourMat = fluidCueMat;
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
                op.mat = pourMat;
                fluidSpawns.push_back(op);
              }
            }
      }

      // ---- VESSELS (game/container.h): the hand's button uses it ------------
      //
      // The hands are up with a flask or a pouch in one of them (the kit's
      // HandR/HandL, session.h HandsNow). DUAL WIELDING: the vessel is used
      // with ITS hand's button — LMB for the right, RMB for the left — and
      // what that button does is the hand's MODE (F cycles it: pour, scoop,
      // apply; TB_SCOOP/TB_APPLY and their _L twins), so a flask in either
      // fist has all three uses on one button and the other fist stays free
      // to swing. Both halves go out through the MutationQueue: a scoop is
      // conditional clears, a pour is grid particles plus the SplatterEvent
      // that lets whoever is standing in the stream be wet by it.
      // PAY WHAT THE GPU TOOK (ContainerSettle): every tick, held or not, so
      // a scoop in flight when the flask is put away still lands in it. The
      // claim goes to the held vessel, else the first vessel on the hotbar or
      // in the pack that holds the same thing or nothing.
      // Scoop motes (render-only, ScoopMote): wait out the op latency, fly,
      // and are gone at the mouth.
      for (ScoopMote& m : s.scoopMotes) {
        if (m.delay > 0) m.delay--;
        else m.age++;
      }
      s.scoopMotes.erase(std::remove_if(s.scoopMotes.begin(), s.scoopMotes.end(),
                                        [](const ScoopMote& m) { return m.age > m.life; }),
                         s.scoopMotes.end());
      // The ledger itself was observed once for every session before phase G
      // (TickAuthority, w.scoopLedger); here this session's claims draw on it.
      // A claim is paid into ONE vessel at a time (ContainerDeposit refuses a
      // stack: a stack of empties is split first, ContainerIsolateOne), and
      // spread over as many as it takes. What no vessel can hold -- the flask
      // was thrown or set down with its scoop still in flight -- goes back
      // into the world as a spill at the hands, never into nothing.
      if (!s.scoopMemo.claims.empty()) {
        const WorldSnapshot& lsnap = world.Snap();
        if (lsnap.valid) {
          // POCKET CHEMISTRY (container.h ContainerPocketExplosion): a
          // deposit that brings two substances together that the world's
          // rules make explode goes off in the hands, after the payout.
          struct Pocket { ItemStack* st; ReactionEffect fx; };
          std::vector<Pocket> pockets;
          auto depositInto = [&](ItemStack* v, int n, int i, ItemStack* spill,
                                 int nSpill, uint16_t mat, int units) {
            ItemStack& cand = v[i];
            if (cand.Empty() || cand.stoppered) return 0;
            const ItemDef* d = items.Of(cand);
            // Another substance is fine (contents mix) until the vessel holds
            // kMaxSubstances of them.
            if (!d || !d->IsContainer() ||
                (cand.contents.AmountOf(mat) == 0 &&
                 cand.contents.n >= alchemy::kMaxSubstances))
              return 0;
            if (mat >= mats.size() || mats[mat].gpu.klass > 31 ||
                ((d->container.holds >> mats[mat].gpu.klass) & 1u) == 0)
              return 0;
            if (cand.count > 1 && !ContainerIsolateOne(v, n, i, spill, nSpill))
              return 0;
            const int before = (int)cand.FillTotal();
            const int put = ContainerDeposit(*d, cand, mat, units);
            // The fill cue: one per CELL that landed (a cell is up to eight
            // eighths), each at the fill it brought the vessel to. Liquid
            // only -- a pouch of sand does not bubble.
            // Capped: headless runs have no frame loop to drain it.
            if (put > 0 && mats[mat].gpu.klass == CLASS_LIQUID &&
                d->container.capacity > 0 && s.flaskFills.size() < 256) {
              const float cap = (float)d->container.capacity;
              for (int e = std::min(8, put);; e = std::min(e + 8, put)) {
                s.flaskFills.push_back((float)(before + e) / cap);
                if (e >= put) break;
              }
              s.flaskFillAt = player.EyePos() + cam.Forward() * MetresToCells(0.35f) -
                              Vec3{0, MetresToCells(0.15f), 0};
            }
            ReactionEffect fx;
            if (put > 0 && ContainerPocketExplosion(cand.contents, mats, mobs.Reactions(), fx, mat)) {
              bool seen = false;
              for (const Pocket& p : pockets) seen |= p.st == &cand;
              if (!seen) pockets.push_back({&cand, fx});
            }
            return put;
          };
          auto deposit = [&](uint16_t mat, int units) {
            int put = 0;
            // The vessels IN HAND first (either fist), then the hotbar, then
            // the pack.
            for (int hk = 0; hk < kHands && put < units; hk++)
              put += depositInto(kit.equip.slots, kEquipSlotCount,
                                 EquipSlotOfHand(HandAt(hk)), kit.bag.slots,
                                 Bag::kSlots, mat, units - put);
            for (int i = 0; i < kItemSlots && put < units; i++)
              put += depositInto(hotbar.slots, kItemSlots, i, kit.bag.slots,
                                 Bag::kSlots, mat, units - put);
            for (int i = 0; i < Bag::kSlots && put < units; i++)
              put += depositInto(kit.bag.slots, Bag::kSlots, i, hotbar.slots,
                                 kItemSlots, mat, units - put);
            return put;
          };
          std::vector<ContainerUnpaid> unpaid;
          const int paidWater =
              ContainerSettle(w.scoopLedger, s.scoopMemo, lsnap.tick, deposit, &unpaid);
          // THE DISSOLVED HALF (container.h ContainerSoluteLedger): the salt the
          // scooped brine carried, paid to the session whose water claims were
          // just paid, into the first vessel in hand / hotbar / pack that now
          // holds a liquid. What does not fit (a dissolved eighth takes room,
          // composition.h) spills with the unpaid water -- as grains, through
          // ContainerDissolvedToWorld -- never into nothing.
          if (paidWater > 0) {
            for (uint32_t sp = 1; sp <= kSolScoopSpecies; sp++) {
              const SoluteDef* sd = CurrentSoluteById(sp);
              if (!sd || !sd->from) continue;
              const int n = ContainerSoluteTake(w.soluteLedger, sp,
                                                std::max<uint32_t>(1, sd->yieldPerVoxel / 8));
              if (n <= 0) continue;
              const uint16_t dmat = (uint16_t)(alchemy::kDissolvedBit | sd->from);
              int put = 0;
              auto tryInto = [&](ItemStack& cand) {
                if (put >= n || cand.Empty() || cand.stoppered || cand.count != 1) return;
                const ItemDef* d = items.Of(cand);
                if (!d || !d->IsContainer()) return;
                bool liquid = false;
                for (int i = 0; i < cand.contents.n; i++) {
                  const uint16_t m = cand.contents.p[i].mat;
                  liquid |= !alchemy::IsDissolved(m) && m < mats.size() &&
                            mats[m].gpu.klass == CLASS_LIQUID;
                }
                if (liquid) put += ContainerDeposit(*d, cand, dmat, n - put);
              };
              for (int hk = 0; hk < kHands; hk++)
                tryInto(kit.equip.slots[EquipSlotOfHand(HandAt(hk))]);
              for (int i = 0; i < kItemSlots; i++) tryInto(hotbar.slots[i]);
              for (int i = 0; i < Bag::kSlots; i++) tryInto(kit.bag.slots[i]);
              if (put < n) unpaid.push_back({dmat, n - put});
            }
          }
          // It went off: the vessel is gone, what was in it bursts out at the
          // hands, and a real blast (the grenade slot) carves whoever held it.
          for (const Pocket& p : pockets) {
            const Vec3 hands = player.EyePos() + cam.Forward() * MetresToCells(0.35f) -
                               Vec3{0, MetresToCells(0.15f), 0};
            ContainerSpill sp;
            sp.at = hands;
            sp.vel = player.vel;
            sp.away = Vec3{0, 1, 0};
            sp.rest = p.st->contents;
            sp.seed = rng::Hash3(0xB0C4E7u, tick, p.st->contents.Total());
            w.vesselSpills.push_back(sp);
            s.pendingBlasts.push_back({ifloor(hands.x), ifloor(hands.y), ifloor(hands.z),
                                       std::clamp(p.fx.radius, 1, kMaxExplosionRadius),
                                       std::max(1, p.fx.power), 0, 0, 0});
            p.st->ClearFill();
            p.st->stoppered = false;
            // A vessel in a HAND leaves through the body's own door (Mob::
            // KitTake marks the slot stale, so the held model is rebuilt), as
            // the bench's break does; the hotbar and pack are plain slots.
            const ptrdiff_t eq = p.st - kit.equip.slots;
            if (eq >= 0 && eq < (ptrdiff_t)kEquipSlotCount) {
              avatar.KitTake(KitRef{KitSpace::Equip, (int)eq}, 1);
            } else {
              p.st->count -= 1;
              if (p.st->count <= 0) *p.st = ItemStack{};
            }
            ui.kitMessage = "it explodes in your hands!";
            ui.kitMessageAge = 0.0f;
          }
          for (const ContainerUnpaid& u : unpaid) {
            ContainerSpill sp;
            const Vec3 fwd = cam.Forward();
            sp.at = player.EyePos() + fwd * MetresToCells(0.35f) -
                    Vec3{0, MetresToCells(0.15f), 0};
            sp.vel = player.vel;
            sp.away = Vec3{fwd.x, 0.0f, fwd.z};
            const float al = sp.away.len();
            sp.away = al > 1e-4f ? sp.away * (1.0f / al) : Vec3{};
            sp.mat = u.mat;
            sp.units = u.units;
            sp.seed = rng::Hash3(0x5C0FEDu, tick, (uint32_t)u.units);
            w.vesselSpills.push_back(sp);
            ui.kitMessage = "there is nothing to hold it; it spills";
            ui.kitMessageAge = 0.0f;
          }
        }
      }
      // ---- THE THROW (game/container.h ContainerThrowSpeed): G held winds it
      // up, G released lets go. Off whenever the hand stops holding a
      // throwable vessel, so changing what is in hand mid-draw cancels it.
      // WHICH HAND: the right's vessel if it can be thrown, else the left's.
      {
        int tslot = -1;   // the throwing HAND (as an int), -1 = none
        if (st.intent.handsUp)
          for (int hk = 0; hk < kHands && tslot < 0; hk++)
            if (hands.hand[hk].vessel && ContainerThrowable(*hands.hand[hk].item))
              tslot = hk;
        const ItemStack* ts =
            tslot >= 0 ? &kit.equip.InHand(HandAt(tslot)) : nullptr;
        const ItemDef* tdef = ts ? items.Of(*ts) : nullptr;
        // The throw's clips are authored on the right arm; the left plays
        // their mirror (Mob::HandClip).
        if (!tdef || !ContainerThrowable(*tdef) || tslot != s.throwSlot) {
          // The draw being cancelled is on the hand it was STARTED in.
          const std::string wasClip = avatar.HandClip(
              "throw_windup", HandAt(s.throwSlot >= 0 ? s.throwSlot : 0));
          if (s.throwTicks > 0 && avatar.Spawned() && !wasClip.empty())
            avatar.StopClip(wasClip);
          s.throwTicks = 0;
          s.throwLaunchIn = 0;  // the hand changed mid-swing: nothing leaves it
          s.throwSlot = tdef && ContainerThrowable(*tdef) ? tslot : -1;
        }
        const Hand throwHand = HandAt(s.throwSlot >= 0 ? s.throwSlot : 0);
        const std::string windupClip = avatar.HandClip("throw_windup", throwHand);
        // The arm draws back while G is held (a looping hold, eased in by the
        // clip's blend) and whips through on the release; the vessel leaves
        // the hand kThrowLaunchTicks later, at the front of the swing (the
        // `throw` clip's release key, assets/anims/throw.json).
        constexpr int kThrowLaunchTicks = 3;
        bool launch = false;
        if (s.throwLaunchIn > 0) {
          launch = --s.throwLaunchIn == 0;
        } else if (s.throwSlot >= 0 && ti.Held(TB_THROW)) {
          if (s.throwTicks == 0 && avatar.Spawned() && !windupClip.empty())
            avatar.PlayClip(windupClip);
          s.throwTicks = std::min(s.throwTicks + 1, 1 << 20);
        } else if (s.throwSlot >= 0 && s.throwTicks > 0) {
          s.throwLaunchSpeed = ContainerThrowSpeed(*tdef, s.throwTicks);
          s.throwTicks = 0;
          if (avatar.Spawned()) {
            if (!windupClip.empty()) avatar.StopClip(windupClip);
            const std::string throwClip = avatar.HandClip("throw", throwHand);
            if (!throwClip.empty()) avatar.PlayClip(throwClip);
            s.throwLaunchIn = kThrowLaunchTicks;
          } else {
            launch = true;  // no body to swing (fly mode): straight away
          }
        }
        if (launch && s.throwSlot >= 0) {
          ItemStack& vs = kit.equip.InHand(throwHand);
          const float speed = s.throwLaunchSpeed;
          const Vec3 fwd = cam.Forward();
          // A few degrees of lift, because a throw aimed AT the crosshair is
          // released above it.
          Vec3 dir = fwd + Vec3{0, 0.08f, 0};
          dir = dir * (1.0f / std::max(1e-4f, dir.len()));
          const Vec3 vel = dir * speed + player.vel;
          // FROM THE HAND: the held rig part's pose, centred on the item's
          // own voxels (`at` is the body's min corner). Without a body, from
          // in front of the eye. Either way it starts inside the capsule and
          // among the thrower's own limbs, which is why the release below is
          // Thrown: on the plain exempt layer it struck the thrower's head
          // and arm on its first step and burst in their face. Owned by THIS
          // player's capsule, so it clears its thrower and still hits anyone
          // else it is thrown at (Physics::BodyRole, THROWN(P)).
          Vec3 at = player.EyePos() + fwd * 2.0f - Vec3{0, 0.5f, 0};
          {
            Vec3 hp;
            Quat hq;
            const int hs = avatar.Spawned() ? avatar.HeldSlot(throwHand) : -1;
            uint32_t gscale = 1;
            const std::vector<PrefabVoxel>* gv = ItemGroundVoxels(*tdef, gscale);
            if (hs >= 0 && avatar.PartWorldTransform(hs, hp, hq)) {
              Vec3 c{};
              if (gv && !gv->empty()) {
                for (const PrefabVoxel& v : *gv)
                  c = c + Vec3{v.x + 0.5f, v.y + 0.5f, v.z + 0.5f};
                c = c * (1.0f / ((float)gv->size() * (float)std::max(1u, gscale)));
              }
              at = hp - c;
            }
          }
          // ONE of the stack leaves, as the object it is: its dye, its fill.
          const uint64_t body =
              w.ground ? DropItemToWorld(*tdef, vs.One(), at, vel, phys, debris,
                                         &mbSet, *w.ground)
                       : 0;
          if (body) phys.SetBodyRole(body, Physics::BodyRole::Thrown, playerBody);
          if (body) {
            // End over end about the throw's own right axis: a spun flask
            // reads as thrown, a translating one as teleported.
            Vec3 right = Vec3{dir.z, 0.0f, -dir.x};
            const float rl = right.len();
            right = rl > 1e-4f ? right * (1.0f / rl) : Vec3{1, 0, 0};
            phys.SetBodyVelocities(body, vel, right * (-4.0f - speed * 0.02f));
            if (--vs.count <= 0) {
              vs = ItemStack{};
            } else {
              // The fill was ONE flask's (ItemInstance::StacksWith) and it just left
              // in the thrown one; what stays behind is empty, not a copy.
              vs.ClearFill();
            }
          } else {
            ui.kitMessage = "there is nowhere to throw that";
            ui.kitMessageAge = 0.0f;
          }
        }
        ui.throwCharge = s.throwTicks > 0 && tdef
                             ? ContainerThrowCharge(*tdef, s.throwTicks)
                             : -1.0f;
      }
      // WHICH VESSEL ACTS THIS TICK: the one whose hand's button is down
      // (the right's first), else the last-used hand's, else the only one.
      int vh = -1;
      if (st.intent.handsUp && s.throwTicks == 0 && s.throwLaunchIn == 0) {
        const bool r = hands[Hand::Right].vessel, l = hands[Hand::Left].vessel;
        if (r && ti.Held(TB_ATTACK)) vh = 0;
        else if (l && ti.Held(TB_ALT)) vh = 1;
        else if (r && l) vh = HandIndex(s.lastHand);
        else if (r) vh = 0;
        else if (l) vh = 1;
      }
      const bool vesselHands = vh >= 0;
      const Hand vHand = HandAt(vh >= 0 ? vh : 0);
      // The hand's button and its mode bits (tickinput.h).
      const uint32_t useBit = vHand == Hand::Left ? TB_ALT : TB_ATTACK;
      const uint32_t scoopBit = vHand == Hand::Left ? TB_SCOOP_L : TB_SCOOP;
      const uint32_t applyBit = vHand == Hand::Left ? TB_APPLY_L : TB_APPLY;
      const int vSlot = EquipSlotOfHand(vHand);
      if (vesselHands && ti.Pressed(useBit)) s.lastHand = vHand;
      // The pour pose ends with the hands: a flask put away (or thrown)
      // mid-pour must not leave the arm out — and moves with the hand.
      const std::string pourClipWas =
          avatar.HandClip("pour", HandAt(s.pourPoseHand));
      if ((!vesselHands || s.pourPoseHand != vh) && s.pourPoseTicks > 0) {
        if (avatar.Spawned() && !pourClipWas.empty()) avatar.StopClip(pourClipWas);
        s.pourPoseTicks = 0;
      }
      if (vesselHands) {
        ItemStack& vs = kit.equip.InHand(vHand);
        const ItemDef* vdef = items.Of(vs);
        const WorldSnapshot& vsnap = world.Snap();
        const Vec3 eye = player.EyePos();
        const Vec3 fwd = cam.Forward();
        auto say = [&](const char* why) {
          if (!why || !*why) return;
          ui.kitMessage = why;
          ui.kitMessageAge = 0.0f;
        };
        // THE MOUTH: the held rig part when there is one, so the water
        // visibly leaves (and enters) the thing in your hand; else just in
        // front of and below the eye (fly mode, no body).
        // `toward` is where the contents are going (or coming from): the
        // mouth is the vessel's own lip on that side (Mob::HeldMouthWorld),
        // so with the arm out in the `pour` clip the stream leaves the flask.
        auto vesselMouth = [&](Vec3 toward) {
          Vec3 mouth = eye + fwd * MetresToCells(0.35f) -
                       Vec3{0, MetresToCells(0.15f), 0};
          Vec3 hp;
          Quat hq;
          const int hs = avatar.Spawned() ? avatar.HeldSlot(vHand) : -1;
          if (hs >= 0 && avatar.PartWorldTransform(hs, hp, hq)) {
            mouth = hp + Vec3{0, MetresToCells(0.08f), 0};
            Vec3 lip;
            if (avatar.HeldMouthWorld(toward - hp, lip, vHand)) mouth = lip;
          }
          return mouth;
        };
        // THE POUR POSE (assets/anims/pour.json): the arm comes up and out
        // while LMB pours or applies from a filled vessel, and nothing leaves
        // the flask until it is up (kPourRaiseTicks), so the stream starts at
        // the outstretched hand rather than at the hip. No body = no wait.
        constexpr int kPourRaiseTicks = 4;
        const bool pourPose = vdef && vdef->IsContainer() && vs.Filled() &&
                              ti.Held(useBit) && !ti.Held(scoopBit) &&
                              avatar.Spawned();
        const std::string pourClip = avatar.HandClip("pour", vHand);
        if (pourPose) {
          if (s.pourPoseTicks == 0 && !pourClip.empty()) avatar.PlayClip(pourClip);
          s.pourPoseHand = vh;
          s.pourPoseTicks = std::min(s.pourPoseTicks + 1, 1 << 20);
        } else if (s.pourPoseTicks > 0) {
          if (!pourClip.empty()) avatar.StopClip(pourClip);
          s.pourPoseTicks = 0;
        }
        const bool armUp = !avatar.Spawned() || s.pourPoseTicks > kPourRaiseTicks;
        // Particles the MPM pool can still take this tick (rule 2: charged
        // BEFORE emission, against the cap and the spawn stream both).
        auto fluidRoom = [&]() -> uint32_t {
          const size_t used = (size_t)fluidCount + fluidSpawns.size();
          if (used >= kFluidCap || fluidSpawns.size() >= kMaxFluidSpawnsPerTick)
            return 0u;
          return (uint32_t)std::min<size_t>(kFluidCap - used,
                                            kMaxFluidSpawnsPerTick - fluidSpawns.size());
        };
        if (vdef && vdef->IsContainer() && ti.Held(useBit) && ti.Held(scoopBit)) {
          const char* why = nullptr;
          if (!vsnap.valid || vsnap.pick[0] == 0) {
            why = "there is nothing there to scoop";
          } else {
            const IVec3 hit{(int)vsnap.pick[2], (int)vsnap.pick[3],
                            (int)vsnap.pick[4]};
            const Vec3 hc{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f};
            // Arm's reach plus the vessel's own pour range: you scoop where
            // you could pour, and no further.
            if ((hc - eye).len() > vdef->container.pourRange + MetresToCells(1.0f)) {
              why = "too far away";
            } else if (!ContainerIsolateOne(kit.equip.slots, kEquipSlotCount,
                                            vSlot, kit.bag.slots, Bag::kSlots)) {
              why = "no room to set the others down";
            } else {
              const uint32_t landing = OpLandingTick(w, tick);
              ContainerScoop(*vdef, vs, hit,
                             [&](IVec3 c, uint32_t& wd) {
                               return ContainerSnapWord(world, c, wd);
                             },
                             world, mats, cellOps, &why, &s.scoopMemo,
                             landing);
              // Every cell just claimed flies into the vessel: the memo stamps
              // each with the tick its clear lands, and this is the one scoop
              // call per tick, so `landing` picks out exactly this call's.
              // A liquid the solver can hold flies as GHOST MPM water
              // (ContainerScoopStream), spawned in this tick's batch so it
              // appears the tick the clear lands; anything else (a pouch's
              // sand, a flask of lava) as the render-only motes.
              const uint16_t mmat = s.scoopMemo.PendingMat();
              const bool asFluid =
                  mmat < mats.size() && ContainerPoursAsFluid(mats[mmat]);
              const Vec3 smouth = vesselMouth(Vec3{hit.x + 0.5f, hit.y + 0.5f, hit.z + 0.5f});
              int k = 0;
              for (const ContainerScoopMemo::Taken& t : s.scoopMemo.cells) {
                if (t.tick != landing) continue;  // claimed on an earlier tick
                if (asFluid) {
                  const float d = (Vec3{t.c.x + 0.5f, t.c.y + 0.5f, t.c.z + 0.5f} -
                                   smouth).len();
                  ContainerScoopStream(t.c, mmat, smouth,
                                       6 + (int)(d / MetresToCells(0.5f)),
                                       0x5C0F1Au ^ (uint32_t)k++, tick,
                                       fluidRoom(), fluidSpawns);
                  continue;
                }
                if (s.scoopMotes.size() >= 96) break;
                uint32_t h = ((uint32_t)t.c.x * 73856093u) ^
                             ((uint32_t)t.c.y * 19349663u) ^
                             ((uint32_t)t.c.z * 83492791u) ^ (tick * 2654435761u);
                h ^= h >> 15;
                h *= 2246822519u;
                h ^= h >> 13;
                ScoopMote m;
                m.from = Vec3{t.c.x + 0.5f, t.c.y + 0.5f, t.c.z + 0.5f};
                m.color = mmat < mats.size() ? mats[mmat].gpu.color0 : 0xFFFFFFFFu;
                m.mat = mmat < mats.size() ? mmat : 0u;
                m.seed = h;
                // Staggered by a tick per cell so a held button reads as a
                // stream rather than volleys of four.
                m.delay = (int)(landing - tick) + (k++ & 1);
                m.age = 0;
                m.life = 7 + (int)(h % 4u);
                s.scoopMotes.push_back(m);
              }
              if (!ti.Pressed(useBit)) why = nullptr;
            }
          }
          if (ti.Pressed(useBit)) say(why);
        }
        // ---- APPLY MODE (TB_APPLY, F with a vessel in hand) ----------------
        //
        // LMB is the health panel's pour brush turned outward: the crosshair
        // ray meets a creature's collider (Physics::CastRayBody -> FindOwner,
        // living or a corpse, never your own limbs), then its skin cells
        // (MobSystem::PickBody), and the disc round the hit takes coat every
        // tick the button is held (PourOnBody) -- water WASHES blood off, acid
        // coats and eats, a salve runs its `coat.effects`. The spend is the
        // portrait brush's (PourBrushCellsPerSec, off its own accumulator) and
        // only on ticks the ray meets skin. The picture is the scoop stream
        // backwards: ghosts (or motes) from the flask's mouth onto the spot.
        if (vdef && vdef->IsContainer() && ti.Held(useBit) && !ti.Held(scoopBit) &&
            ti.Held(applyBit)) {
          bool applied = false;
          if (!vs.Filled() || vs.stoppered) {
            if (ti.Pressed(useBit)) say(vs.stoppered ? "it is stoppered" : "it is empty");
          } else if (!ContainerIsolateOne(kit.equip.slots, kEquipSlotCount,
                                          vSlot, kit.bag.slots, Bag::kSlots)) {
            if (ti.Pressed(useBit)) say("no room to set the others down");
          } else {
            const Vec3 ro = st.intent.aimFromValid ? st.intent.aimFrom : eye;
            const Vec3 rd = fwd.normalized();
            // Reach is measured from the CHARACTER, not the camera boom: the
            // part of the ray behind the head is free.
            const float maxT = std::max(0.0f, (eye - ro).dot(rd)) +
                               vdef->container.pourRange + MetresToCells(1.0f);
            std::vector<uint64_t> own;
            avatar.AppendLiveLimbBodies(own);
            float frac = 1.0f;
            const uint64_t body = phys.CastRayBody(ro, rd, maxT, frac, own);
            Mob* target = body ? mobs.FindOwner(body) : nullptr;
            if (target && avatar.Spawned() && target->Id() == avatar.Id())
              target = nullptr;
            MobSystem::BodyRayHit hit;
            if (target) hit = mobs.PickBody(target->Id(), ro, rd, maxT);
            if (hit.hit && !armUp) {
              applied = true;  // the arm is still coming up: aimed, not yet pouring
            } else if (hit.hit) {
              applied = true;
              // What reaches the skin is what is at the mouth: the top layer.
              const uint32_t mat = ContainerTopMat(vs, &mats);
              const float radius = std::max(0.1f, st.intent.applyRadius);
              s.applySpendMilli += (int64_t)std::llround(
                  PourBrushCellsPerSec(*vdef, radius) * kContainerUnitsPerCell *
                  1000.0f / 30.0f);
              const int units = (int)(s.applySpendMilli / 1000);
              s.applySpendMilli -= (int64_t)units * 1000;
              if (units > 0) {
                vs.contents.Take((uint16_t)mat, (uint32_t)units);
              }
              uint32_t marked = 0;
              // +3 a tick, as the portrait brush: five ticks on one spot soak it.
              const uint32_t did = mobs.PourOnBody(target->Id(), hit, rd, radius,
                                                   mat, 3u, tick, &marked);
              if (s.applyTicks == 0 || s.applyMob != target->Id() || did) {
                const MobDef* md = target->Def();
                const std::string who =
                    std::string(target->Alive() ? "the " : "the dead ") +
                    (md ? md->name : std::string("creature"));
                std::string part;
                if (md && hit.limb >= 0 && hit.limb < (int)md->limbs.size())
                  part = "'s " + md->limbs[hit.limb].name;
                std::string msg = "you apply " +
                                  (mat < mats.size() ? mats[mat].name : std::string("it")) +
                                  " to " + who + part;
                if (did & MobSystem::kRemedyStanch) msg += "; the bleeding stops";
                if (did & MobSystem::kRemedyDisinfect) msg += InfectionStoppedText(mobs, mats);
                if (did & MobSystem::kRemedyRestore) msg += "; the flesh begins to knit";
                ui.kitMessage = msg;
                ui.kitMessageAge = 0.0f;
              }
              s.applyMob = target->Id();
              s.applyTicks++;
              // THE PICTURE: out of the mouth onto the spot. Liquids the
              // solver can carry go as ghost MPM water, anything else as
              // sprite motes (ScoopMote::out). Flight time grows with the
              // distance, as the scoop's does.
              const Vec3 amouth = vesselMouth(hit.pos);
              const float d = (hit.pos - amouth).len();
              const int life = 4 + (int)(d / MetresToCells(0.5f));
              if (mat < mats.size() && ContainerPoursAsFluid(mats[mat])) {
                ContainerApplyStream(amouth, hit.pos, mat, life, 4,
                                     0xA991Eu ^ (uint32_t)vSlot,
                                     tick, fluidRoom(), fluidSpawns);
              } else if (s.scoopMotes.size() < 96) {
                for (int k = 0; k < 2; k++) {
                  uint32_t h = rng::Hash3(0xA991Eu, tick, (uint32_t)k + 1u);
                  ScoopMote m;
                  m.out = true;
                  m.from = amouth;
                  m.to = hit.pos;
                  m.color = mat < mats.size() ? mats[mat].gpu.color0 : 0xFFFFFFFFu;
                  m.mat = mat < mats.size() ? mat : 0u;
                  m.seed = h;
                  m.delay = k;
                  m.age = 0;
                  m.life = life + (int)(h % 3u);
                  s.scoopMotes.push_back(m);
                }
              }
            } else if (ti.Pressed(useBit)) {
              say("there is nobody there to apply it to");
            }
          }
          if (!applied) {
            s.applyTicks = 0;
            s.applySpendMilli = 0;
          }
        } else {
          s.applyTicks = 0;
          s.applySpendMilli = 0;
        }
        if (vdef && vdef->IsContainer() && ti.Held(useBit) && !ti.Held(scoopBit) &&
            !ti.Held(applyBit)) {
          if (!vs.Filled()) {
            if (ti.Pressed(useBit)) say("it is empty");
          } else if (!ContainerIsolateOne(kit.equip.slots, kEquipSlotCount,
                                          vSlot, kit.bag.slots, Bag::kSlots)) {
            // A filled stack (a save from before fills stopped stacking): its
            // one fill is one flask's, so only one of them may pour it.
            if (ti.Pressed(useBit)) say("no room to set the others down");
          } else {
            // AIMED, always, at the pour point (ContainerPourPoint): a metre
            // along the look, or the first thing in the way -- the point
            // main.cpp draws the marker sphere on. The stream is solved to pass
            // through it, so looking ahead pours into the air in front of you
            // and looking down pours onto the ground.
            Vec3 target = st.intent.pourAim;
            if (!st.intent.pourAimValid) {
              std::vector<uint64_t> own;
              avatar.AppendLiveLimbBodies(own);
              target = ContainerPourPoint(*vdef, eye, eye, fwd, s.kindAt, phys, own);
            }
            // OUT OF THE FLASK, from its lip on the target's side -- once the
            // arm is up (the pour pose above).
            const Vec3 mouth = vesselMouth(target);
            if (armUp && world.CellInWindow({ifloor(mouth.x), ifloor(mouth.y),
                                    ifloor(mouth.z)})) {
              // Water leaves as MLS-MPM fluid (ContainerPourFluid) when the
              // solver can hold it; lava, blood and the pouch's powders keep
              // the grid particles.
              SplatterEvent splat;
              const uint32_t pseed = 0x0F1A5Cu ^ (uint32_t)vSlot;
              const int poured =
                  ContainerTopMat(vs, &mats) < mats.size() &&
                          ContainerPoursAsFluid(mats[ContainerTopMat(vs, &mats)])
                      ? ContainerPourFluid(*vdef, vs, mouth, fwd, &target, tick,
                                           pseed, fluidRoom(), fluidSpawns, &splat,
                                           &mats, &w.solutePours)
                      : ContainerPour(*vdef, vs, mouth, fwd, &target,
                                      CurrentTuning().sim.partGravity, tick,
                                      pseed, spawns, &splat,
                                      ContainerParticleRoom(vsnap.valid,
                                                            vsnap.particleCount,
                                                            spawns.size()),
                                      &mats, &w.solutePours);
              if (poured > 0) {
                splat.sourceMob = avatar.Spawned() ? avatar.Id() : 0;
                mobs.QueueSplatter(splat);
              }
            }
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
        debris.AddDestructionEvent(OpLandingTick(w, tick),
                                   {op.x - op.radius, op.y - op.radius, op.z - op.radius},
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
  }
}

// ---- PHASE H (WORLD) - day phase, the player actor list and mobs.PreTick
// Verbatim from the single-body TickAuthority, lines 1086-1114 at 54fe241.
static void PhaseH(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {

      // Declared before mobs.PreTick so bleed spray and dismemberment gore
      // share the one per-tick spawn stream with debris shatter — the ring and
      // its 4096-op budget are global, so a single list is what keeps the two
      // systems honest about the shared limit.
      // (`spawns` is aliased out of the caller's OpBatch.)
      // Declared here rather than beside debris.PreTick because per-voxel limb
      // burning emits REAL fire voxels into the grid, and mobs run first.
      // (`cellOps` is aliased out of the caller's OpBatch.)

      // The day phase the body-reaction evaluator gates its rules on (every
      // body population, debris included, goes through MobSystem's), taken
      // from the ONE function that also puts it on TickParams — see
      // sim/reactcpu.h for why the CPU has to agree with the GPU here.
      mobs.SetDayPhase(DayPhaseNow(tick));
      // ...and the weather: the tick's rain word, LATCHED here once and taken
      // by SubmitTick for TickParams (weather::TakeTickRain), so the body
      // reactions and the GPU kernels read one value even if the weather pin
      // moves between here and the submit.
      {
        const weather::TickRain rain =
            weather::LatchTickRain(CurrentTuning(), kDefaultSeed, tick);
        mobs.SetWeatherRain(rain.word);
        // The slope the body rain exposure walks along (MobSystem
        // RainExposedCpu), the same TickParams will carry.
        mobs.SetRainSlope(rain.slopeQx, rain.slopeQz);
      }

      // ---- BROKEN VESSELS (game/container.h ContainerShouldBreak) -----------
      //
      // Every vessel lying or flying in the world, against the two witnesses:
      // last step's NEW contacts (Physics clears them at the head of Step, so
      // here they are the step phase N ran last tick) and the body's velocity
      // jump since last tick. A break destroys the body -- the registry entry
      // goes with it through OnBodyGone -- and leaves a SPILL at its centre
      // that drains below this tick, through the spawn streams, so the
      // contents are in the world on the tick the glass went.
      //
      // Bounded by the ground registry (a few entries) and the contact list
      // (capped per step); a ghost body is the peer's to break.
      //
      // The drain runs with or without a ground registry: phase G also
      // queues spills here (a scoop settled with no vessel to hold it).
      {
        if (w.ground)
          ContainerBreakPass(*w.ground, items, phys, debris, w.vesselVel,
                             w.vesselSpills);
        // ...and every vessel still on the ground or in the air shows what
        // it holds (render-only, phys/fillview.h). Re-stated every tick, so a
        // drop, a throw, a load and a peer's announce need no hook of their
        // own.
        if (w.ground)
          for (const WorldItem& wi : w.ground->All())
            if (const ItemDef* d = items.Of(wi); d && d->IsContainer())
              debris.SetBodyFill(wi.body, ContainerHeldFillOf(*d, wi));
        for (ContainerSpill& sp : w.vesselSpills) {
          const size_t used = (size_t)fluidCount + fluidSpawns.size();
          const uint32_t room =
              used >= kFluidCap || fluidSpawns.size() >= kMaxFluidSpawnsPerTick
                  ? 0u
                  : (uint32_t)std::min<size_t>(
                        kFluidCap - used,
                        kMaxFluidSpawnsPerTick - fluidSpawns.size());
          SplatterEvent splat;
          splat.count = 0;
          const bool first = !sp.splatted;
          const WorldSnapshot& psnap = world.Snap();
          // Gas (a bench's fumes, a stoppered flask of chlorine that broke)
          // leaves on the CPU gas stream, charged against its room first.
          std::vector<GasSpawnOp> gasOut;
          const uint32_t gasRoom = kGasCpuSpawnPerTick - std::min(kGasCpuSpawnPerTick, world.PendingGasSpawns());
          ContainerSpillStep(sp, mats, tick, room, fluidSpawns, spawns, &splat,
                             ContainerParticleRoom(psnap.valid, psnap.particleCount,
                                                   spawns.size()),
                             &gasOut, gasRoom, &w.solutePours);
          if (!gasOut.empty()) world.QueueGasSpawns(gasOut.data(), (uint32_t)gasOut.size());
          if (first && sp.splatted) mobs.QueueSplatter(splat);
        }
        std::erase_if(w.vesselSpills,
                      [](const ContainerSpill& sp) { return sp.Done(); });
        // THE SOLUTE POURS THAT LAND THIS TICK (container.h): every pour's
        // and spill's dissolved share, sent as CellOpSolute ops on the tick
        // its stream arrives. After the spills above, so a flask that broke
        // this tick queues its salt before the queue is read.
        ContainerSolutePoursDue(w.solutePours, tick, world, cellOps);
      }

      // WHO THE NPCs ARE FIGHTING, pushed once per TICK rather than per frame.
      // The tick loop runs 0..4 times per frame, and a target position sampled
      // on the frame clock would make an NPC's decisions a function of frame
      // rate — the same reason the burn pass and the gait live in here.
      // Deliberately the capsule, not the avatar rig: the capsule is what the
      // player actually occupies, and it exists even before the avatar spawns.
      // A LIST, IN SESSION ORDER (M9.1 P1). SetPlayerActor was the
      // span-of-one wrapper; the NPC targeting layer has taken a list since
      // N5, so this is the plumbing catching up and not a new mechanic.
      // `alive` is the window's HUD mirror for the primary (byte-identical
      // to what this line always passed) and the avatar's own answer for a
      // session whose window is somewhere else.
      ws.actors.clear();
      for (SessionTick& p : players) {
        const PlayerSession& pl = *p.s;
        const bool alive = pl.localView
                               ? ui.playerAlive
                               : (!pl.avatar.Spawned() || pl.avatar.IsAlive());
        // WHAT THE WEAPON IS DOING, for the NPCs that read blows (ai::Action):
        // the discrete strike's own phase while one runs, else a held guard.
        ai::Action act = ai::Action::None;
        switch (pl.playerStrike.phase) {
          case StrokeCursor::Phase::Windup: act = ai::Action::Windup; break;
          case StrokeCursor::Phase::Cut: act = ai::Action::Cut; break;
          case StrokeCursor::Phase::Recover: act = ai::Action::Recover; break;
          default:
            if (pl.melee.Phase() == SwingPhase::Guard) act = ai::Action::Guard;
            break;
        }
        ws.actors.push_back({pl.player.pos, Player::kHalfXZ,
                             Player::kHalfY * 2.0f, alive, (uint8_t)act});
      }
      // THE PEERS' BODIES ARE TARGETS TOO (M9.2 package B). Appended AFTER
      // every local session because the actor id band is POSITIONAL: entry i
      // of this list is `ai::kPlayerActorBase + i`, so a ghost appended at
      // index `sessions + j` is what an NPC names when it targets that id.
      // The avatar list MobSystem::FindCombatantById resolves that index
      // through is rebuilt in the same breath, and only on a membership
      // change — the two lists agreeing is what keeps an NPC that targets a
      // ghost from resolving to the local player's body.
      if (w.remotes != nullptr && !w.remotes->list.empty()) {
        RemotePlayersAppendActors(*w.remotes, ws.actors);
        if (w.remotes->dirty) {
          ws.sessionAvatars.clear();
          for (SessionTick& p : players)
            ws.sessionAvatars.push_back(static_cast<Mob*>(&p.s->avatar));
          RemotePlayersSyncAvatars(
              *w.remotes, mobs,
              std::span<Mob* const>(ws.sessionAvatars.data(),
                                    ws.sessionAvatars.size()));
        }
      }
      // A gate that owns the actor list keeps it (session.h section J): the
      // harness body is not a combatant.
      if (!w.harness.ownsActors) mobs.SetPlayerActors(ws.actors);

      // mobs: kinematic walk drive, terrain anchors for ManageTerrain,
      // bleeding ops, per-voxel burning — must run before debris.PreTick
      // consumes the anchors
      w.harness.mobPhase.ops0 = ops.size();
      w.harness.mobPhase.cells0 = cellOps.size();
      w.harness.mobPhase.spawns0 = spawns.size();
      mobs.PreTick(tick, world, ops, cellOps, spawns);
      // (session.h Harness::mobPhase: what this phase authored, for a gate.)
      w.harness.mobPhase.ops1 = ops.size();
      w.harness.mobPhase.cells1 = cellOps.size();
      w.harness.mobPhase.spawns1 = spawns.size();
  }
}

// ---- PHASE I (PLAYER) - the avatar (incl. death/respawn) and magic
// Verbatim from the single-body TickAuthority, lines 1115-2006 at 54fe241.
// ---- STRIKES (docs/PLAN_electricity.md E3, game/lightning.h) ---------------
//
// The two callers of LightningStrike that live in the tick: a spell's strike
// (phase I, below) and a storm's ground strike (phase K, WeatherStrikes). Both
// record what fired into w.strikes for the frame (flash + thunder) and the
// gates.
static void EnsureStrikeMats(TickAuthorityCtx& w) {
  auto& sk = w.strikes;
  if (sk.matsData != (const void*)w.mats.data() || sk.mats.resolvedFor != w.mats.size()) {
    sk.mats.Resolve(w.mats);
    sk.matsData = (const void*)w.mats.data();
  }
}

static void RecordStrike(TickAuthorityCtx& w, const StrikePlan& plan, uint32_t tick,
                         bool weather, bool emitted, bool atAim) {
  auto& sk = w.strikes;
  TickAuthorityCtx::StrikeWorld::Event ev;
  ev.tick = tick;
  ev.target = plan.target;
  ev.foot = atAim ? plan.target : IVec3{plan.target.x, plan.target.y + 1, plan.target.z};
  ev.weather = weather;
  ev.conductive = plan.targetConductive;
  ev.emitted = emitted;
  ev.cells = (uint32_t)plan.cells.size();
  if (emitted) ev.paths = plan.paths;   // the frame's bolt (render-only)
  if (sk.events.size() < TickAuthorityCtx::StrikeWorld::kMaxEvents) sk.events.push_back(ev);
  sk.recent.push_back(ev);
  if (sk.recent.size() > TickAuthorityCtx::StrikeWorld::kRecent)
    sk.recent.erase(sk.recent.begin());
  sk.lastPlan = plan;
}

// The stepped leader: ticks between a strike being decided (its chunks asked
// of the fetch cache) and the bolt (the readback is one tick plus
// kSnapshotLatency, so the cache has answered well inside it).
static constexpr uint32_t kStrikeLeadTicks = 8;

// A spell's strike, planned and emitted. WARDS ARE ASKED AGAIN AT THE STRUCK
// CELL (and the cell the bolt arrives in): FilterStreams refused at the aim,
// but the target search moves the bolt up to kStrikeMaxSearch cells, and a
// bolt aimed outside a ward must not reach a rod inside it. A strike the
// tick's budget refuses REFUNDS its own tariff to the caster's mana (capped at
// the pool's max): CLAUDE.md, a refused op costs nothing. Returns true when
// the bolt went out.
static bool FireSpellStrike(TickAuthorityCtx& w, PlayerSession& s, const StrikeSpec& spec,
                            int32_t tariff, uint32_t tick, std::vector<CellOp>& cellOps) {
  EnsureStrikeMats(w);
  const SpellProbe probe = WorldStrikeProbe(w.world);
  StrikePlan plan = PlanStrike(spec, w.strikes.mats, &probe, w.world);
  w.strikes.spellStrikes++;
  const IVec3 foot =
      spec.atAim ? plan.target : IVec3{plan.target.x, plan.target.y + 1, plan.target.z};
  if (s.spells.StrikeWarded(plan.target.x, plan.target.y, plan.target.z) ||
      s.spells.StrikeWarded(foot.x, foot.y, foot.z)) {
    w.strikes.spellWarded++;
    RecordStrike(w, plan, tick, false, false, spec.atAim);
    return false;
  }
  const bool ok = EmitStrike(plan, w.strikes.budget, cellOps);
  if (!ok && tariff > 0) {
    CasterState& m = s.caster.mana;
    const int32_t before = m.mana;
    m.mana = std::max(before, (int32_t)std::min<int64_t>((int64_t)m.manaMax, (int64_t)m.mana + tariff));
    if (m.mana > before) w.strikes.spellRefunded += (uint64_t)(m.mana - before);
  }
  RecordStrike(w, plan, tick, false, ok, spec.atAim);
  return ok;
}

// A spell's strike -> CellOps. The glyph's strike block is the shape;
// repetition (`lightning lightning`) scales the bolt's height and the splash's
// arcs, inside the caps a plan may never exceed, and the effect's STRENGTH
// scales the same two (a spent carrier strikes weaker).
//
// SEEKING PAST THE MIRROR. When the target search reads a chunk neither the
// mirror nor the fetch cache holds (a bolt called 40 m off, a rod on a tower
// above the mirror's top), the strike does what a storm's does: it asks the
// cache for those chunks and waits a stepped leader (kStrikeLeadTicks), then
// fires on what is known, from this session's phase I. Both stores are
// functions of the tick, so the wait and the target are too (rule 1). A
// search the stores already cover fires at once, exactly as before.
//
// Returns false when this tick refused it (ward or budget) -- the HUD's spell
// overflow counts that. A strike sent to wait is not a refusal.
static bool SpellStrikeToCells(TickAuthorityCtx& w, PlayerSession& s, const GlyphDef& g,
                               const SpellStrike& sk, uint32_t tick,
                               std::vector<CellOp>& cellOps) {
  EnsureStrikeMats(w);
  const StrikeSpec spec =
      StrikeSpecFromGlyph(g.strike, IVec3{sk.x, sk.y, sk.z}, sk.scaleMille,
                          rng::Hash3(sk.salt, tick, 0x5B311u), sk.strengthMille);
  std::vector<IVec3> missing;
  if (!StrikeSearchKnown(spec, w.world, &missing) &&
      w.strikes.spellPending.size() < TickAuthorityCtx::StrikeWorld::kMaxSpellPending) {
    for (const IVec3& wc : missing) w.world.RequestChunkFetch(wc);
    TickAuthorityCtx::StrikeWorld::SpellPending p;
    p.spec = spec;
    p.fireTick = tick + kStrikeLeadTicks;
    p.session = s.index;
    p.tariff = sk.tariff;
    w.strikes.spellPending.push_back(p);
    w.strikes.spellLeaders++;
    return true;
  }
  return FireSpellStrike(w, s, spec, sk.tariff, tick, cellOps);
}

// This session's spell strikes whose leader has run out: fired in push order
// (the order they were cast), from phase I. Returns how many this tick refused.
static int FireDueSpellStrikes(TickAuthorityCtx& w, PlayerSession& s, uint32_t tick,
                               std::vector<CellOp>& cellOps) {
  auto& q = w.strikes.spellPending;
  if (q.empty()) return 0;
  int refused = 0;
  size_t keep = 0;
  for (size_t i = 0; i < q.size(); i++) {
    if (q[i].session != s.index || q[i].fireTick > tick) {
      q[keep++] = q[i];
      continue;
    }
    const auto p = q[i];
    if (!FireSpellStrike(w, s, p.spec, p.tariff, tick, cellOps)) refused++;
  }
  q.resize(keep);
  return refused;
}

// THE STORM'S GROUND STRIKES (owner, 2026-10-03: yes, in the sim, near the
// players only). Two steps, both pure functions of (seed, tick, the player's
// id and position, the weather schedule, the stores):
//
//   DECIDE  per SESSION (wave 2: it was the primary's alone), on a tick whose
//           hash -- keyed by the player's id, StrikeWorld::playerIdBase +
//           index -- rolls under the sky's lightning rate x
//           weather.strikeRate (weather::SimLightningQ, the integer ladder the
//           rain word walks -- never the render flash, which is wall-clock-
//           shaped and frame-paced). The aim is a hashed point in the ring
//           strikeRadius/4 .. strikeRadius round THAT player, on the analytic
//           ground; every chunk the target search will read, up to the real
//           top (kStrikeScanUp), is requested from the fetch cache now.
//   FIRE    kStrikeLeadTicks later -- the stepped leader -- through
//           LightningStrike against the mirror + the cache the request filled.
//           World-level: once per tick, in the primary's slot.
//
// Player id 0 (single player, the host) rolls exactly the hashes the
// primary-only version rolled. FORCED (w.strikes.forceNext: the gate,
// SANDVOX_STRIKE_EVERY=<ticks>) decides a strike this tick whatever the sky,
// at forceAim when it is set -- taken by the first session to roll.
static void FireWeatherStrikes(TickAuthorityCtx& w, uint32_t tick,
                               std::vector<CellOp>& cellOps) {
  auto& sk = w.strikes;
  EnsureStrikeMats(w);
  // A spell leader whose session is gone (it left mid-leader) would wait
  // forever: dropped after a generous grace. Not a refusal anyone sees.
  if (!sk.spellPending.empty())
    sk.spellPending.erase(std::remove_if(sk.spellPending.begin(), sk.spellPending.end(),
                                         [&](const auto& p) {
                                           return p.fireTick + 4 * kStrikeLeadTicks < tick;
                                         }),
                          sk.spellPending.end());
  if (!sk.mats.Ready() || sk.pending.empty()) return;
  const SpellProbe probe = WorldStrikeProbe(w.world);
  std::vector<TickAuthorityCtx::StrikeWorld::Pending> keep;
  for (const auto& p : sk.pending) {
    if (p.fireTick > tick) {
      keep.push_back(p);
      continue;
    }
    const StrikeSpec spec = WeatherStrikeSpec(sk.mats, p.aim, p.key);
    StrikePlan plan;
    const bool ok = LightningStrike(spec, sk.mats, &probe, w.world, sk.budget, cellOps, &plan);
    sk.weatherFired++;
    RecordStrike(w, plan, tick, true, ok, false);
  }
  sk.pending.swap(keep);
}

static void DecideWeatherStrike(TickAuthorityCtx& w, uint32_t playerId, Vec3 playerPos,
                                uint32_t tick) {
  auto& sk = w.strikes;
  EnsureStrikeMats(w);
  if (!sk.mats.Ready()) return;
  const Tuning& tn = CurrentTuning();
  bool decide = false, forced = false;
  static const uint32_t kEvery = [] {
    const char* e = std::getenv("SANDVOX_STRIKE_EVERY");
    return e ? (uint32_t)std::max(0, std::atoi(e)) : 0u;
  }();
  // The player's key: 0 for player 0, so its hashes are the primary-only ones.
  const uint32_t pk = playerId * 0x9E3779B9u;
  if (sk.forceNext) {
    decide = forced = true;
    sk.forceNext = false;
  } else if (kEvery > 0 && tick % kEvery == 0) {
    decide = forced = true;
  } else if (tn.weather.strikeRate > 0.0f) {
    const int64_t rateQ = weather::SimLightningQ(tn, kDefaultSeed, tick);  // flashes/min, Q16
    if (rateQ > 0) {
      // Per-tick probability, Q32: rate/min x share / (60 s x 30 Hz). The
      // share is quantised once (llround of an IEEE float: exact everywhere).
      const int64_t shareQ = std::llround((double)tn.weather.strikeRate * 65536.0);
      const uint64_t pQ32 = (uint64_t)std::min<int64_t>(
          (rateQ * shareQ) / (60 * 30), (int64_t)0xFFFFFFFFll);
      const uint32_t roll = rng::Hash3(kDefaultSeed ^ 0x57121CEu, tick, 0x1u + 0x100u * playerId);
      decide = (uint64_t)roll < pQ32;
    }
  }
  if (!decide || sk.pending.size() >= TickAuthorityCtx::StrikeWorld::kMaxPending) return;

  IVec3 aim{};
  if (forced && sk.forceAimSet) {
    aim = sk.forceAim;
    sk.forceAimSet = false;
  } else {
    // A hashed point in the ring R/4..R round the player: up to eight tries
    // of a uniform square, first that lands in the ring (else the last).
    const int R = std::clamp(tn.weather.strikeRadius, 16, 240);
    const int px = (int)std::floor(playerPos.x), pz = (int)std::floor(playerPos.z);
    int dx = 0, dz = R / 2;
    for (uint32_t k = 0; k < 8; k++) {
      const uint32_t h = rng::Hash3((kDefaultSeed ^ 0x57121D0u) ^ pk, tick, k);
      dx = (int)(h % (uint32_t)(2 * R + 1)) - R;
      dz = (int)((h >> 16) % (uint32_t)(2 * R + 1)) - R;
      const int d2 = dx * dx + dz * dz;
      if (d2 <= R * R && d2 >= (R / 4) * (R / 4)) break;
    }
    aim.x = px + dx;
    aim.z = pz + dz;
    aim.y = World::TerrainHeight(aim.x, aim.z, kDefaultSeed) + 1;
  }
  // Keep the aim in the window (a strike never reaches outside the sim). The
  // scan's own span is clamped to the window in PlanStrike, so the aim's
  // height needs no room above it any more.
  const IVec3 o = w.world.WindowOrigin();
  const int lo = 8, hiN = (int)kWorldN - 9;
  aim.x = std::clamp(aim.x, o.x * (int)kChunk + lo, o.x * (int)kChunk + hiN);
  aim.y = std::clamp(aim.y, o.y * (int)kChunk + lo, o.y * (int)kChunk + hiN);
  aim.z = std::clamp(aim.z, o.z * (int)kChunk + lo, o.z * (int)kChunk + hiN);
  TickAuthorityCtx::StrikeWorld::Pending p;
  p.aim = aim;
  p.fireTick = tick + kStrikeLeadTicks;
  p.key = rng::Hash3(kDefaultSeed ^ 0x57121D1u, tick, (uint32_t)sk.weatherDecided);
  p.forced = forced;
  // Ask for every chunk the target search will read, up to the real top: it
  // lands well inside the leader (the readback is one tick plus
  // kSnapshotLatency). The fetch queue's own cap (kFetchPerTick) bounds what
  // one tick carries.
  std::vector<IVec3> chunks;
  StrikeSearchChunks(WeatherStrikeSpec(sk.mats, aim, p.key), w.world, chunks);
  for (const IVec3& wc : chunks) w.world.RequestChunkFetch(wc);
  sk.pending.push_back(p);
  sk.weatherDecided++;
  if (forced) sk.forced++;
}

// A PEER'S BOLT, ANNOUNCED HERE (wave 2, package E). A strike is decided and
// recorded on the machine that authored it; its CellOps cross in the merge,
// but its Event did not, so the other machine's frame neither flashed nor
// clapped. Phase N reads the peer's kept cells (OpDelayQueue::Merge's
// remoteCellIdx) and turns each run of them that lays `lightning` into an
// Event (remote = true): foot = its lowest lightning cell. Presentation only
// -- the cells are already in the merged batch -- and bounded by kMaxEvents.
// A shock (arcs, no bolt) makes no flash or clap on its author either, so
// arc-only runs are not announced.
static void AnnounceRemoteStrikes(TickAuthorityCtx& w, const std::vector<CellOp>& cells,
                                  const std::vector<uint32_t>& remoteIdx, uint32_t tick) {
  auto& sk = w.strikes;
  EnsureStrikeMats(w);
  if (!sk.mats.Ready() || remoteIdx.empty()) return;
  auto matOf = [&](uint32_t i) { return cells[i].word & 0xFFFu; };
  size_t a = 0;
  while (a < remoteIdx.size()) {
    // A run: consecutive merged indices of lightning / arc cells, split where
    // a new bolt (lightning) follows a splash (arc) -- one strike's cells are
    // bolt first, then splash (game/lightning.h PlanStrike).
    const uint32_t m0 = matOf(remoteIdx[a]);
    if (m0 != sk.mats.lightning) {
      a++;
      continue;
    }
    size_t b = a + 1;
    bool inSplash = false;
    while (b < remoteIdx.size() && remoteIdx[b] == remoteIdx[b - 1] + 1) {
      const uint32_t m = matOf(remoteIdx[b]);
      if (m == sk.mats.arc) inSplash = true;
      else if (m != sk.mats.lightning || inSplash) break;
      b++;
    }
    IVec3 foot{};
    bool have = false;
    uint32_t n = 0;
    for (size_t k = a; k < b; k++) {
      const CellOp& c = cells[remoteIdx[k]];
      n++;
      if (matOf(remoteIdx[k]) != sk.mats.lightning) continue;
      const uint32_t slot = c.cellIdx / kChunkVol, li = c.cellIdx % kChunkVol;
      if (!World::IsWindowSlot(slot)) continue;
      const IVec3 wc = w.world.SlotToWorldChunk(slot);
      const IVec3 at{wc.x * (int)kChunk + (int)(li % kChunk),
                     wc.y * (int)kChunk + (int)((li / kChunk) % kChunk),
                     wc.z * (int)kChunk + (int)(li / (kChunk * kChunk))};
      if (!have || at.y < foot.y) foot = at;
      have = true;
    }
    a = b;
    if (!have || sk.events.size() >= TickAuthorityCtx::StrikeWorld::kMaxEvents) continue;
    TickAuthorityCtx::StrikeWorld::Event ev;
    ev.tick = tick;
    ev.foot = foot;
    ev.target = IVec3{foot.x, foot.y - 1, foot.z};
    ev.emitted = true;
    ev.cells = n;
    ev.remote = true;
    sk.events.push_back(ev);
    sk.remoteEvents++;
  }
}

static void PhaseI(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {

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
            Vec3{player.vel.x - player.slideVel.x, 0,
                 player.vel.z - player.slideVel.z},
            kTickDt, avatar.Spawned() ? avatar.LocoGroundAlign() : 0.0f);

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
          if (avatar.HasEyeLocal())
            player.SetModelEyeHeight(avatar.EyeRestHeight());
          // A new rig wears nothing (Mob::BuildRig clears its shells and
          // DressFromKit's memo), so the armour sync below offers every slot
          // again on its own.
          tpRig.Snap();   // re-entering from fly: don't ease across the gap
        }
        if (!wantAvatar && avatar.Spawned()) avatar.Despawn();
        // ---- melee: drive the swing, then pose the arm (game/melee.h) -------
        // BEFORE PreTick, because PreTick is what flattens the pose and
        // submits the kinematic limb targets — a weapon pose pushed in after
        // it would be a frame late and the blade would trail the mouse.
        {
          // THE CAMERA, FOLDED ONTO THE BODY: yawed so a look behind is
          // reflected to the front and a look past the cone pins at its edge
          // (melee.aimYaw; game/thirdperson.h ResolveSwingBasis). Without
          // this, a third-person camera orbited behind the character made
          // every strike cut backwards at the lens. Resolved once per tick
          // off the heading the policy above just produced. No body (fly
          // mode): the raw camera, as before. `swFwd` is also the AIM RAY a
          // strike program casts (below).
          Vec3 swRight = cam.Right(), swUp = cam.Up(), swFwd = cam.Forward();
          if (avatar.Spawned())
            ResolveSwingBasis(camHeading, avatarHeading, cam.Right(), cam.Up(),
                              cam.Forward(), swRight, swUp, swFwd);
          // THE BASIS A STROKE PROGRAM IS EXPRESSED IN is the BODY's, level —
          // the NPC's (mob.cpp StepStroke's basis), not the camera's. A keyed
          // pose turns the shoulder by the aim's yaw/pitch in the CHEST's
          // frame (Mob ApplyKeyedArm `aimed`), so the aim angles must be
          // measured from the body too. Measured in the pitched camera basis
          // the crosshair always sat at elevation ~0 — looking up or down
          // changed nothing — and azimuth was only right while the body
          // happened to face the camera.
          const Vec3 bodyFwd{std::sin(avatarHeading), 0.0f,
                             std::cos(avatarHeading)};
          const Vec3 bodyUp{0.0f, 1.0f, 0.0f};
          const Vec3 bodyRight = bodyFwd.cross(bodyUp).normalized();
          if (avatar.Spawned()) {
            // Equip/unequip only on a CHANGE. EquipItem builds a body and a
            // joint, so calling it every tick with the same weapon would
            // rebuild the sword 60 times a second; comparing against what is
            // already in the hand keeps this a no-op in the common case.
            //
            // BEFORE melee.Update, not after: the arm read below needs the item
            // in the hand to know WHICH arm is the weapon arm, and the tick the
            // player selects the blade and clicks can be the same tick.
            // A VESSEL in the hands is held the same way, through the same
            // borrowed rig slot: the flask is a real part of the arm while
            // you hold it (game/container.h).
            //
            // BOTH HANDS, FROM THE KIT (dual wielding): each fist holds what
            // its kit slot says (session.h HandsNow), whatever tool is up —
            // an item in your hand is in your hand. The tool and magic mode
            // decide what the BUTTONS do, not what you are carrying.
            // A HAND TOO HURT TO HOLD ON lets go first (melee.injuredArmDrop;
            // Mob::GripFails): the item falls as itself and leaves the kit's
            // hand slot, so the equip below finds the fist empty.
            if (avatar.GripFails(CurrentTuning().melee.injuredArmDrop) > 0) {
              ui.kitMessage = "your grip fails";
              ui.kitMessageAge = 0.0f;
            }
            // AT THE ALCHEMY BENCH the hands hold the bench's vessels instead
            // (session.h BenchHold): the kit is untouched, and the fists go
            // back to it the tick the bench's table is empty or shut.
            const PlayerSession::BenchHold& bench = s.benchHold;
            for (int hk = 0; hk < kHands; hk++) {
              const Hand h = HandAt(hk);
              // Re-read: GripFails may have just emptied it.
              const ItemDef* want =
                  hands.hand[hk].item != nullptr && !kit.equip.InHand(h).Empty()
                      ? hands.hand[hk].item
                      : nullptr;
              // A bench vessel goes only into a hand that can GRIP it (the
              // KitMove rule, melee.injuredArmDrop): GripFails would take it
              // straight back out.
              const float dropBelow = CurrentTuning().melee.injuredArmDrop;
              if (bench.active)
                want = avatar.HandUsable(h) && avatar.HandCondition(h) >= dropBelow
                           ? items.Named(bench.hand[hk].item)
                           : nullptr;
              const std::string wantName = want ? want->name : std::string();
              // AS IT WAS (DESIGN.md §7 "A coat moves on contact"): the kit
              // stack carries the item's lattice -- its coat -- and the fist
              // puts it back on. Re-equipped also when the stack was REPLACED
              // by one of the same name (HeldKitStale): two swords are two
              // swords. A bench prop is not the item and has no stack here.
              if (avatar.HeldItem(h) != wantName ||
                  (want != nullptr && !bench.active && avatar.HeldKitStale(h)))
                avatar.EquipItemAsWas(
                    want, h, bench.active ? nullptr : &kit.equip.InHand(h).damage);
              // ...and it is the BENCH's, shown in the fist while the flask
              // itself stays in the bag (Mob::SetHeldBorrowed): never shed,
              // looted or saved as the item.
              avatar.SetHeldBorrowed(h, bench.active && want != nullptr);
            }
            // THE IDLE STRIKE HAND: between strokes the driver's ready pose
            // is on the hand last used, if it can swing, else on the other.
            // Changed only while the driver is IDLE (its whole memory is one
            // arm's, in one basis), and a change re-seeds it from the new
            // arm's live pose, so nothing jumps. A hand that cannot swing (a
            // flask, a ruined arm) never gets the ready pose, which is what
            // leaves the pour clip the arm.
            // (Not while the bench's pour owns an arm: it sets the strike
            // hand itself, below, and this would hand it straight back.)
            // (Nor while a cast has an arm thrown out: PoseSpellHands holds
            // the claim on the spell hand until it has faded.)
            if (!playerStrike.Active() && melee.Phase() == SwingPhase::Idle &&
                s.benchPourW <= 0.0f && s.spellArmHand < 0) {
              Hand idle = s.lastHand;
              if (!hands[idle].ready && hands[OtherHand(idle)].ready)
                idle = OtherHand(idle);
              const bool mirror = idle == Hand::Left;
              if (idle != avatar.StrikeHand() || mirror != s.strikeMirrored) {
                avatar.SetStrikeHand(idle);
                s.strikeMirrored = mirror;
                melee.Reset();
              }
            }
            // ...and what each vessel in a hand is showing: its contents'
            // colour up to their level (render-only, container.h). The
            // CONTENTS are the kit hand stack's (so a flask knocked from the
            // hand leaves with them, Mob::ShedGearBeforeDetach), and what they
            // look like is derived from it.
            for (int hk = 0; hk < kHands; hk++) {
              const Hand h = HandAt(hk);
              const ItemDef* vd = hands.hand[hk].vessel ? hands.hand[hk].item : nullptr;
              const ItemStack& hs = kit.equip.InHand(h);
              if (bench.active) {
                const PlayerSession::BenchHold::HandView& bv = bench.hand[hk];
                const ItemDef* bd = items.Named(bv.item);
                const bool bheld = bd && avatar.HeldItem(h) == bv.item;
                avatar.SetHeldContents(bheld ? bv.contents : alchemy::Composition{}, h);
                avatar.SetHeldFill(bheld ? ContainerHeldFillFrom(*bd, bv.contents, bv.stoppered)
                                         : ContainerHeldFill{}, h);
                continue;
              }
              const bool held = vd != nullptr && !hs.Empty();
              avatar.SetHeldContents(held ? hs.contents : alchemy::Composition{}, h);
              avatar.SetHeldFill(held ? ContainerHeldFillOf(*vd, hs) : ContainerHeldFill{}, h);
            }
            // WHERE THE BLADE IS, so taking control of it is not a teleport:
            // the stroke seeds itself from the live point AND the live hand,
            // takes its blade length from the pair, and bounds itself by the
            // rig's own reach (game/melee.h SetStroke).
            Vec3 handNow, tipNow, flatNow;
            float reachNow = 0;
            // A MIRRORED driver reads the live blade's flat reflected, the
            // same pseudovector flip its pose goes out with (strokes.h).
            if (avatar.WeaponStrokePose(handNow, tipNow, flatNow, reachNow))
              melee.SetStroke(handNow, tipNow,
                              s.strikeMirrored ? flatNow * -1.0f : flatNow,
                              reachNow);
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
              // ...and the avatar's own CHEST, so no cut across the body drags
              // the arm through it (melee.h SetBodyKeepOut). Same seam, same
              // clamp site, same reason the head sphere is composed by the rig
              // rather than guessed by the driver.
              Vec3 ba, bb;
              float bw = 0, bd = 0;
              if (avatar.BodyKeepOut(ba, bb, bw, bd))
                melee.SetBodyKeepOut(ba, bb, bw, bd);
              else
                melee.ClearBodyKeepOut();
            }
            // The NPC driver always did this and the player's never had —
            // without it the asymmetric azimuth window (azOut on the weapon
            // side) assumes a right-handed rig whatever the avatar holds.
            // Mirrored, the driver lives in the AUTHORED arm's frame, whose
            // weapon side is the right (strokes.h).
            melee.SetHandSign(s.strikeMirrored ? 1.0f : avatar.HandSign());
          } else {
            melee.ClearArm();
          }
          // ---- THE STROKE PROGRAM FEEDS THE DRIVER --------------------------
          // The sweep and the whoosh key on `playerStrike.Cutting()` — the
          // PROGRAM's cut, the NPC's own contract. The driver has no cut of its
          // own (melee.h SwingPhase).
          // The program entered its cut THIS tick (the whoosh edge).
          bool strikeCutEdge = false;
          // THE DRIVER'S BASIS: the body's, reflected when the driver serves
          // the other arm from its stroke's author (s.strikeMirrored).
          // READ AT EACH USE: a stroke that begins below may change it.
          auto strikeRightNow = [&] {
            return s.strikeMirrored ? bodyRight * -1.0f : bodyRight;
          };
          {
            // DISCRETE: consume the press latch, then step the program. Begin
            // and first step land on the SAME tick, exactly as the NPC's
            // BeginStroke/StepStroke pair does.
            // THE STRIKE'S OWN HAND, not "any hand": a swing whose fist
            // stops being able to swing (the sword dropped, the arm ruined,
            // the hands lowered) ends, whatever the other fist is doing.
            const bool strokeHandLive =
                !playerStrike.Active() || hands[playerStrike.hand].ready;
            if (!meleeReady || !avatar.Spawned() || !strokeHandLive) {
              // Weapon stowed (or body gone) mid-swing: drop the claim the
              // same way MobSystem's teardown guard does.
              playerStrike.Reset();
              if (!meleeReady || !avatar.Spawned()) strikeBuffered = -1;
              if (avatar.Spawned()) avatar.ClearStrikeEffector();
            }
            // ---- STRIKE CHAINING (strokes.h StrikeChains) --------------------
            // A swing ends with the weapon on the side it was flicked toward,
            // so the strike flicked BACK the other way (or one compass step
            // either side of that) starts from where the weapon already is:
            // it may begin during the recover, and winds up
            // melee.chainWindupRate times faster. Swinging the mouse back and
            // forth is the fast rhythm. A strike from any other side waits
            // the recover out through the bank below, as it always did.
            const Tuning::Melee& chainT = CurrentTuning().melee;
            // ...AND THE OTHER HAND ALWAYS CHAINS (dual wielding). Its weapon
            // is not the one that just swung, so "already on that side" does
            // not apply: it may start in the recover, at its own windup rate.
            // Left, right, left is the dual wielder's fast rhythm.
            auto chainsNow = [&](int next, Hand nextHand) {
              if (playerStrike.phase != StrokeCursor::Phase::Recover) return false;
              if (nextHand != playerStrike.hand) return true;
              const PlayerStrikeMap& cmap =
                  PlayerCompass(mobs.AttackStyles(), hands[nextHand].weapon);
              // ...AND A SHORT BLADE'S STAB CHAINS INTO ITSELF (`clickRepeat`):
              // the point comes back along the line it went out on, so the
              // next stab starts from there like an opposite cut does.
              const ItemDef* nItem =
                  hands[nextHand].weapon ? hands[nextHand].item : nullptr;
              if (next == strikeBase &&
                  StrikeRepeats(cmap, strikeBase,
                                FormForItem(nItem)))
                return true;
              return StrikeChains(cmap, strikeBase, next,
                                  chainT.chainSectorLeeway);
            };
            // A chaining strike clicked during the CUT was banked; it goes
            // the moment the recover begins rather than at its end.
            if (strikeQueued < 0 && strikeBuffered >= 0 &&
                chainsNow(strikeBuffered, s.strikeBufferedHand)) {
              strikeQueued = strikeBuffered;
              s.strikeQueuedHand = s.strikeBufferedHand;
              strikeBuffered = -1;
            }
            const Hand qHand = s.strikeQueuedHand;
            if (strikeQueued >= 0 && hands[qHand].ready && avatar.Spawned()) {
              const bool chained =
                  playerStrike.Active() && chainsNow(strikeQueued, qHand);
              const bool otherHand = chained && qHand != playerStrike.hand;
              if (chained) {
                // The recover is abandoned where it stands: the new stroke's
                // first keyed frame blends from the live arm (mob.cpp
                // keyLive), so there is no snap. The OTHER hand's chain hands
                // the old arm back on its own short release (Mob::
                // ReleaseOffArm).
                playerStrike.Reset();
                avatar.ClearStrikeEffector();
              }
              if (!playerStrike.Active()) {
                // THE WEAPON'S OWN VERSION of the stroke the flick named
                // (strokes.h WEAPON FORMS), resolved once, here, like the
                // NPC's MobSystem::StartStroke — for the item in THIS hand.
                const ItemDef* qItem = hands[qHand].weapon ? hands[qHand].item : nullptr;
                const int formed = mobs.AttackStyles().ResolveForm(
                    strikeQueued,
                    FormForItem(qItem));
                if (const AttackStyle* sty = mobs.AttackStyles().At(formed)) {
                  // ---- POINT THE DRIVER AT THE PART THIS STYLE SWINGS -----
                  // Exactly what MobSystem::BeginStroke does, and through the
                  // same call: `player_punch_r` names `fist.R`, so the arm
                  // chain is claimed on the fist and `avatar.WeaponEdge`
                  // reports the knuckles. A style whose weapon this body has
                  // not got refuses HERE, before the program begins, so a
                  // strike never runs with no edge on the end of it.
                  if (avatar.ArmForStyle(*sty, qHand)) {
                    // THE ARM, AND WHETHER IT IS THE AUTHORED ONE (strokes.h
                    // "THE LEFT HAND IS THE RIGHT, MIRRORED"). A change of
                    // hand or of frame re-seeds the driver from the new arm's
                    // live pose: its memory is the old arm's, in the old
                    // basis, and carrying it across is a teleport.
                    const bool mirror = StrokeMirrored(*sty, qHand);
                    if (mirror != s.strikeMirrored || otherHand) melee.Reset();
                    s.strikeMirrored = mirror;
                    // Fresh sliders per swing — MobSystem::BeginStroke says
                    // why (MeleeTuning is a copy; F5 must reach the next cut).
                    ApplyMeleeTuning(melee.tuning);
                    // AN INJURED ARM SWINGS SLOWER (melee.injuredArm*): the
                    // same stroke, stretched, and so a weaker cut.
                    const float slow =
                        StyleSideOf(*sty) != StyleSide::None
                            ? InjuredArmSlow(avatar.HandCondition(qHand))
                            : 1.0f;
                    // The seed is (who, when), like the NPC's; the player's
                    // styles author jitter 0, so it only matters if an author
                    // turns jitter back on — and then it still replays.
                    BeginStrokeProgram(playerStrike, *sty, formed,
                                       rng::Hash3(0x504Cu, tick, 0x5747u),
                                       chained && !otherHand
                                           ? chainT.chainWindupRate
                                           : 1.0f,
                                       slow);
                    playerStrike.hand = qHand;
                    playerStrike.mirrored = mirror;
                    strikeBase = strikeQueued;
                    s.strikeBaseHand = qHand;
                    // ...and the style's body animation, exactly as the NPC's
                    // BeginStroke does (strokes.h AttackStyle::clip) — its
                    // mirror, swung with the other arm (Mob::StrokeClip).
                    if (const std::string clip = avatar.StrokeClip(*sty, mirror);
                        !clip.empty())
                      avatar.PlayClip(clip);
                  }
                }
              } else if (playerStrike.phase == StrokeCursor::Phase::Cut ||
                         playerStrike.phase == StrokeCursor::Phase::Recover ||
                         qHand != playerStrike.hand) {
                // ONE strike banks mid-swing (last click wins); a click during
                // the windup is dropped — the windup IS the commitment. The
                // OTHER hand's click banks even then: that arm has committed
                // to nothing yet.
                strikeBuffered = strikeQueued;
                s.strikeBufferedHand = qHand;
              }
              strikeQueued = -1;
            } else if (strikeQueued >= 0) {
              // Pressed with a hand that cannot swing any more (it dropped
              // its sword between the press and this tick): gone, not banked.
              strikeQueued = -1;
            }
            // ---- A CHARGED STRIKE (strokes.h StrokeCursor::holdWindup) ------
            // The strike's own button still down at the end of its windup
            // parks the arm there; letting go throws the cut, harder
            // (melee.chargeDamage, applied at the sweep below). While parked,
            // a flick names a stroke exactly as a press would, and the arm
            // moves over to THAT stroke's windup end (ReaimChargedStroke), so
            // sweeping the mouse from a right horizontal to a diagonal slides
            // the held weapon between the two. The aim keeps tracking the
            // crosshair until the release commits it, as a windup's does.
            if (playerStrike.Active()) {
              const Hand ch = playerStrike.hand;
              playerStrike.holdWindup = ti.Held(ch == Hand::Left ? TB_ALT : TB_ATTACK);
              float fx = 0, fy = 0;
              if (playerStrike.Holding() && strikePicker.Remembered(fx, fy)) {
                const StyleLibrary& lib = mobs.AttackStyles();
                const PlayerStrikeMap& map = PlayerCompass(lib, hands[ch].weapon);
                const int base = FlickStyleFor(lib, map, ch, fx, fy);
                const ItemDef* cItem = hands[ch].weapon ? hands[ch].item : nullptr;
                const int formed = lib.ResolveForm(base, FormForItem(cItem));
                const AttackStyle* was = lib.At(playerStrike.style);
                const AttackStyle* next = lib.At(formed);
                // Only a stroke the SAME arm, the same part and the same
                // basis can swing: anything else would need a new take-over,
                // and a hold is not the place for one.
                if (base >= 0 && base != strikeBase && was && next &&
                    next->weapon == was->weapon &&
                    StrokeMirrored(*next, ch) == playerStrike.mirrored &&
                    ReaimChargedStroke(playerStrike, *was, *next, formed,
                                       chainT.chargeBlendTicks)) {
                  strikeBase = base;
                  s.lastStrikePick.style = base;
                  s.lastStrikePick.fx = fx;
                  s.lastStrikePick.fy = fy;
                  s.lastStrikePick.flicked = true;
                  s.lastStrikePick.serial++;
                }
              }
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
                    // The FOLDED camera line: identical to cam.Forward()
                    // inside the cone (ResolveSwingBasis returns it
                    // bit-for-bit), reflected to the front when the camera
                    // looks at the character's face. Pitch is the camera's
                    // either way.
                    const Vec3 look = swFwd;
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
                    // ...BUT NEVER PAST WHERE THE TIP CAN MEET THE LINE. A
                    // bearing to a point 40 voxels out is all but parallel to
                    // the camera line, so a thrust into open air ran beside
                    // the reticle a shoulder's offset low and wide of it and
                    // never crossed it. The point on the line exactly the
                    // arm's outer reach from the pivot is where a straight-
                    // ahead tip lands ON the reticle; anything hit nearer
                    // still wins.
                    {
                      float reachLo = 0, reachHi = 0;
                      melee.ReachBand(reachLo, reachHi);
                      const Vec3 e = eye - pivot;
                      const float b = e.dot(look);
                      const float disc = b * b - (e.dot(e) - reachHi * reachHi);
                      if (reachHi > 0.0f && disc >= 0.0f) {
                        const float dConv = -b + std::sqrt(disc);
                        if (dConv > 0.0f) hitDist = std::min(hitDist, dConv);
                      }
                    }
                    StrokeAimAt(pivot, eye + look * hitDist, strikeRightNow(),
                                bodyUp, bodyFwd, aimAz, aimEl, aimDist);
                  }
                }
                const StrokeStepResult r = StepStrokeProgram(
                    playerStrike, sty, melee, aimAz, aimEl, aimDist, kTickDt,
                    strikeRightNow(), bodyUp, bodyFwd);
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
                    s.strikeQueuedHand = s.strikeBufferedHand;
                    strikeBuffered = -1;
                  }
                }
              }
            }
            if (!stepped) {
              // No program stepped the driver this tick: it idles/unwinds
              // exactly as a released button always did, so the arm hands
              // back over the usual ramp. ONE advance per tick either way.
              // The body basis, like the program's: a recover the program
              // began in it must unwind in it, not jump into the camera's.
              // ARMED = the idle strike hand can swing (a flask gets no ready
              // pose), in the frame the driver is in.
              melee.Update(kTickDt, false,
                           avatar.Spawned() && hands[avatar.StrikeHand()].ready,
                           strikeRightNow(), bodyUp, bodyFwd);
            }
          }
          // ---- THE SWING WHOOSH, on the EDGE into the program's cut --------
          // A commit is a moment, so this fires once per cut rather than on
          // every tick the slash is live. Latched (not played here) because
          // this is inside the tick loop and audio drains once per frame.
          //
          // EVERY STRIKE WHOOSHES. This used to be gated on the driver's
          // MOUSE speed, which nothing has fed since strikes became clicks
          // (the picker is the only consumer of the look delta), so it sat
          // near zero and a strike whooshed only when some leftover motion
          // happened to clear combatfx.whooshMinSpeed. The volume and pitch
          // come off the TIP speed instead — the number the damage curve
          // reads — measured by the sweep below on this same tick
          // (whooshArmed), so the player still hears "speed is the damage".
          if (strikeCutEdge) {
            const Tuning::CombatFx& fx = CurrentTuning().combatfx;
            {
              s.whooshArmed = true;
              combatWhooshCue.pending = true;
              combatWhooshCue.power = 0.0f;
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
          if (avatar.Spawned()) {
            // Weight rises while the weapon is up and FADES over the releasing
            // recover, so the arm is handed back to the walk cycle across a
            // couple of hundred ms instead of being dropped in one tick from
            // wherever the player left it (melee.h PoseWeight). The whole pose
            // travels as ONE value — hand, blade axis, blade roll and the
            // elbow's bend pole — because the rig needs all four to put the
            // sword where the stroke says it is.
            // The live discrete strike's per-joint brakes (melee.h ArmSmooth)
            // and, for a KEYED style, its frames' joints (strokes.h).
            const AttackStyle* sty =
                playerStrike.style >= 0 ? mobs.AttackStyles().At(playerStrike.style)
                                        : nullptr;
            WeaponPose wp = StrokePoseNow(playerStrike, sty, melee);
            // The other arm from the style's author: reflect what the basis
            // could not (strokes.h MirrorWeaponPose).
            if (s.strikeMirrored) MirrorWeaponPose(wp);
            avatar.SetWeaponPose(wp);
            PoseBenchHands(s, avatar);
            PoseSpellReady(s, avatar, hands);
            PoseSpellHands(s, avatar, hands, ti, swFwd, playerStrike.Active());
          }
        }
        // ---- ARMOUR: the body wears what the kit's equipment says -----------
        //
        // Mob::DressFromKit (W2-M): the kit is the avatar's own, and the rig's
        // shells are derived from it. Here, in the same place in the frame the
        // weapon's seam uses one block up and for the same reason: WearItem
        // builds bodies and joints, so it runs once per CHANGE, and before
        // PreTick flattens the pose and submits the kinematic targets — a
        // shell appended after that would sit at its spawn pose for a tick and
        // visibly snap into place. A respawn clears the rig's shells
        // (BuildRig) and the next call simply puts them back.
        //
        // This used to be a loop HERE that compared the session's PlayerKit
        // with the rig by name every tick through two latch arrays, and read a
        // removed piece's damage into a map keyed by item name — so two robes
        // shared one set of holes. The damage now travels in the stack itself
        // (Mob::KitMove/KitTake flush the shells into it on the way out).
        if (avatar.Spawned() && avatar.DressFromKit(items) > 0) {
          ui.kitMessage = "that does not fit you";
          ui.kitMessageAge = 0.0f;
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
        // The body's live velocity IS this controller's (Mob::BodyVelocity).
        avatar.BindPlayer(&player);
        if (avatar.Spawned())
          avatar.PreTick(tick, player, avatarHeading, kTickDt, world, ops,
                         cellOps,
                         spawns);
        // DEAD AVATAR: HOLD, AND WAIT TO BE ASKED. Nothing rebuilds the body
        // on a timer any more, and NOTHING OPENS A MENU OVER THE DEATH either:
        // the corpse lies where it fell (a dead Mob in mobs_ since the tick
        // after the death, MobSystem::AdoptDeadAvatar, and it burns, bleeds and
        // settles like any other corpse) and the screen comes up when — and
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
          // A SESSION WITH NO WINDOW HAS NOBODY TO PRESS THE BUTTON. Its
          // `ui` is the throw-away sink bound at the top of this phase, so
          // the request the death screen would raise is raised for it once
          // the same hold has elapsed. The primary never runs this line:
          // localView is true there.
          if (!s.localView && respawnTimer >= av.respawnDelay)
            ui.respawnRequest = true;
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
              // ...and only for the window that owns the cursor.
              if (s.localView && w.restoreCursorAfterUi)
                w.restoreCursorAfterUi();
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
      // `spellExps` is the SESSION'S tick scratch (PlayerScratch): filled
      // here, drained into `exps` in phase K.
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
        // demons D2: the dev panel's "learn all demon names" (game/demon_lore.h).
        // Ownership is player state, not hashed sim state; applied here, in
        // the tick, so the next cast this tick already sees the new glyphs.
        if (ui.devLearnDemonNames) {
          ui.devLearnDemonNames = false;
          const int fresh = demon::GrantAllNames(caster.inventory, glyphs);
          const int all = demon::CountNames(glyphs);
          ui.devDemonNamesStatus =
              all == 0 ? std::string("no demon names in glyphs.json yet")
                       : std::to_string(fresh) + " new, " + std::to_string(all) + " known in all";
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
        } bodyCtx{&phys, &mobs, &player, kPlayerCasterId, &avatar};
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
        //
        // MEASURED TO THE BODY'S BOX, NOT TO A POINT ON IT (2026-09-22). This
        // used to compare `from` against `MobOrigin`, which is the collider's
        // min corner in x/z and the FEET in y — i.e. the ground between a
        // creature's ankles. A `float aura projectile` attaches its status
        // within the effect's radius plus 2 (three voxels for a mod), so a
        // bolt that struck a human anywhere above the shins found NO body,
        // the status fell back to being a PLACE, and a sustained mod on a
        // place does nothing at all: the spell worked on you and silently
        // failed on everything else. The player's branch had the whole figure
        // allowed for in a `+9.0f` for exactly this reason; the mobs' did not.
        bodyProbe.bodyIdAt = [](void* c, Vec3 from, float radius, uint64_t& id, Vec3& out) {
          SpellBodyCtx& bc = *(SpellBodyCtx*)c;
          // Distance from the point to a box, zero inside it.
          auto boxDist = [](const Vec3& p, const Vec3& lo, const Vec3& hi) {
            const float dx = std::max(std::max(lo.x - p.x, 0.0f), p.x - hi.x);
            const float dy = std::max(std::max(lo.y - p.y, 0.0f), p.y - hi.y);
            const float dz = std::max(std::max(lo.z - p.z, 0.0f), p.z - hi.z);
            return std::sqrt(dx * dx + dy * dy + dz * dz);
          };
          float best = radius;
          bool found = false;
          for (uint32_t i = 0; i < bc.mobs->MobCount(); i++) {
            const uint64_t mid = bc.mobs->MobIdAt(i);
            // A status rides a CREATURE; a corpse is a dead Mob now but was
            // never a target here (it was debris), and still is not.
            if (!bc.mobs->IsAlive(mid)) continue;
            Vec3 lo, hi;
            if (!bc.mobs->MobBodyBox(mid, lo, hi)) continue;
            const float d = boxDist(from, lo, hi);
            if (d <= best) {
              best = d;
              id = mid;
              // The body's CENTRE, not its corner: this is the point the
              // status rides and the one a place-status would resolve at.
              out = Vec3{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                         (lo.z + hi.z) * 0.5f};
              found = true;
            }
          }
          // The player, as a figure box of her own (~1.7 m) around `pos`, and
          // NEAREST WINS: a self-cast with an enemy standing on top of you
          // still lands on you.
          const Vec3 dp = bc.player->pos - from;
          const float dPlayer =
              std::max(0.0f, std::sqrt(dp.x * dp.x + dp.y * dp.y + dp.z * dp.z) - 9.0f);
          if (dPlayer <= radius && (!found || dPlayer < best)) {
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
            // A seeking spell hunts the LIVING: a corpse is a Mob now
            // (PLAN_corpse_is_a_mob.md), and homing on one is a wasted cast.
            if (!m.IsAlive(m.MobIdAt(i))) continue;
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
          if (part >= 0 && avatar.PartCentreWorld(part, at)) {
            const SpellFxVec selfAt{SpellFxFromFloat(at.x), SpellFxFromFloat(at.y),
                                    SpellFxFromFloat(at.z)};
            const Vec3 body = player.pos;
            const SpellFxVec originFx{SpellFxFromFloat(body.x), SpellFxFromFloat(body.y),
                                      SpellFxFromFloat(body.z)};
            const SpellFxVec dirFx{0, kSpellFxOne, 0};
            const SpellProbe probe = WorldSpellProbe(world);
            CastResult res = spells.Cast(caster.compiled, caster.mana, playerHealth,
                                         kPlayerCasterId, originFx, dirFx, tick, emit, &probe,
                                         &selfAt, &bodyProbe);
            // The spell STAYS readied: a page clicked in the spellbook casts on
            // every limb you click after it, as a spell in the hand does. The
            // blank leaf, Backspace or another selection puts it away.
            caster.lastOutcome = res.outcome;
          }
        }
        // THE POUR BRUSH (game/container.h): with a filled vessel chosen on
        // the FLASKS row (any pack or hotbar slot -- `ps.vessel`), the health
        // panel's portrait pours where the cursor is. main.cpp
        // built the ray from the portrait camera; here it meets the body's own
        // skin cells (MobSystem::PickBody) and the disc round the hit takes a
        // little more coat every tick the button is held (PourOnBody), plus
        // whatever the material's `coat.effects` do to the limbs it touched.
        //
        // THE SPEND IS A RATE THAT FOLLOWS THE BRUSH: PourBrushCellsPerSec
        // (the disc's area, `applyCells` a second at the default size), paid in
        // the flask's own eighths off a milli-eighth accumulator, and only on
        // ticks the ray actually meets skin -- pouring at the air beside you
        // costs nothing, because nothing leaves the flask.
        {
          PlayerSession::PourStroke& ps = s.pourStroke;
          ItemStack* vp = kit.Resolve(ps.vessel);
          // ONE VESSEL'S FILL (ItemInstance::StacksWith): a filled stack left by an old
          // save is split first, else the brush would drain one flask's fill
          // on behalf of the whole stack. Where the rest cannot be set down,
          // the brush does not pour.
          if (vp && !vp->Empty() && vp->count > 1) {
            const bool inHot = vp >= hotbar.slots && vp < hotbar.slots + kItemSlots;
            const bool inBag = vp >= kit.bag.slots && vp < kit.bag.slots + Bag::kSlots;
            // ...or a HAND (dual wielding: a held flask is a kit hand slot).
            const bool inEquip =
                vp >= kit.equip.slots && vp < kit.equip.slots + kEquipSlotCount;
            const bool split =
                inHot ? ContainerIsolateOne(hotbar.slots, kItemSlots,
                                            (int)(vp - hotbar.slots), kit.bag.slots,
                                            Bag::kSlots)
                : inBag ? ContainerIsolateOne(kit.bag.slots, Bag::kSlots,
                                              (int)(vp - kit.bag.slots),
                                              hotbar.slots, kItemSlots)
                : inEquip ? ContainerIsolateOne(kit.equip.slots, kEquipSlotCount,
                                                (int)(vp - kit.equip.slots),
                                                kit.bag.slots, Bag::kSlots)
                        : false;
            if (!split) vp = nullptr;
          }
          const ItemDef* vdef = vp ? items.Of(*vp) : nullptr;
          if (!ps.active || !avatar.Spawned() || !vdef || !vdef->IsContainer() ||
              !vp->Filled() || vp->stoppered) {
            s.pourStrokeTicks = 0;
            s.pourSpendMilli = 0;
          } else {
            const MobSystem::BodyRayHit hit =
                mobs.PickBody(avatar.Id(), ps.ro, ps.rd, 4096.0f);
            if (hit.hit) {
              const uint32_t mat = ContainerTopMat(*vp, &mats);
              // milli-eighths per tick at 30 Hz
              s.pourSpendMilli += (int64_t)std::llround(
                  PourBrushCellsPerSec(*vdef, ps.radius) * kContainerUnitsPerCell *
                  1000.0f / 30.0f);
              const int units = (int)(s.pourSpendMilli / 1000);
              s.pourSpendMilli -= (int64_t)units * 1000;
              if (units > 0) {
                vp->contents.Take((uint16_t)mat, (uint32_t)units);
              }
              uint32_t marked = 0;
              // +3 a tick: a spot held under the brush for five ticks is
              // soaked through (15), a quick pass leaves a light coat.
              const uint32_t did = mobs.PourOnBody(avatar.Id(), hit, ps.rd,
                                                   ps.radius, mat, 3u, tick, &marked);
              if (s.pourStrokeTicks == 0 || did) {
                std::string msg = "you pour " +
                                  (mat < mats.size() ? mats[mat].name : std::string("it")) +
                                  " over your " + avatar.PartName(hit.limb);
                if (did & MobSystem::kRemedyStanch) msg += "; the bleeding stops";
                if (did & MobSystem::kRemedyDisinfect) msg += InfectionStoppedText(mobs, mats);
                if (did & MobSystem::kRemedyRestore) msg += "; the flesh begins to knit";
                ui.kitMessage = msg;
                ui.kitMessageAge = 0.0f;
              }
              s.pourStrokeTicks++;
            }
          }
        }
        // ---- THE ITEM STAGE (game/itemstage.h; DESIGN.md §8b "The item stage")
        //
        // The stage open on a HAND or WORN slot reads the stack, and the rig
        // is that item's truth while it is on (itemcoat.h): capture it every
        // tick so the stage shows the coat drying in the fist. Then the brush:
        // one tick of the stroke main.cpp built from the stage camera, spent
        // from the FLASKS row's vessel at the portrait brush's rate, written
        // through the body-coat writers and pushed back onto the rig slot.
        {
          if (s.itemStageOpen.space == KitSpace::Equip && avatar.Spawned())
            CaptureLimbToItem(mobs, avatar.Id(), s.itemStageOpen);
          const itemstage::Stroke& is = s.itemStroke;
          if (!is.active) {
            s.itemStageTicks = 0;
            s.itemStageSpendMilli = 0;
          } else {
            const itemstage::StrokeResult r = itemstage::ApplyStroke(
                kit, &mobs, avatar.Spawned() ? avatar.Id() : 0, items, mats, is,
                s.itemStageSpendMilli);
            if (r.hit) {
              if (s.itemStageTicks == 0) {
                const ItemStack* st = kit.Resolve(is.item);
                std::string what = st ? st->name : std::string("item");
                std::replace(what.begin(), what.end(), '_', ' ');
                ui.kitMessage = "you pour " +
                                (r.mat < mats.size() ? mats[r.mat].name : std::string("it")) +
                                " over the " + what;
                ui.kitMessageAge = 0.0f;
              }
              s.itemStageTicks++;
            } else if (r.refused && s.itemStageTicks == 0) {
              ui.kitMessage = r.refused;
              ui.kitMessageAge = 0.0f;
            }
          }
        }
        // ---- SPELLS IN HAND (caster.h PlayerCaster::hand) ------------------
        //
        // A hand holding a spell casts it on its own button's press edge — LMB
        // the right hand, RMB the left, the dual-wield rule — and the spell
        // STAYS in the hand, so it fires as often as the button is clicked;
        // mana is the rate limit. ResolveHands decided which hands qualify
        // (empty of items, a usable arm). Both on one tick: right first, then
        // left, each its own Cast() with its own origin.
        //
        // Origin: one step forward of the hand (SpellHandPoint) along the aim,
        // so the flight leaves the fist; the caster's own parts are rejected
        // by ownership in bodyHit above, so an arm in the line is not a hit.
        // Direction is the aim ray. A FATAL cast runs its effect at the
        // CASTER instead, through the same Cast (thesis 2).
        for (int hk = 0; hk < kHands; hk++) {
          const Hand h = HandAt(hk);
          if (!hands[h].spell) continue;
          if (!ti.Pressed(h == Hand::Right ? TB_ATTACK : TB_ALT)) continue;
          const PlayerCaster::HandSpell& hs = caster.hand[hk];
          const Vec3 fwd = cam.Forward();
          const Vec3 muzzle = SpellHandPoint(avatar, player, cam, h) + fwd * 1.5f;
          SpellFxVec originFx{SpellFxFromFloat(muzzle.x), SpellFxFromFloat(muzzle.y),
                              SpellFxFromFloat(muzzle.z)};
          SpellFxVec dirFx{SpellFxFromFloat(fwd.x), SpellFxFromFloat(fwd.y),
                           SpellFxFromFloat(fwd.z)};
          const bool fatal =
              ResolveCast(caster.mana, playerHealth.Get(), hs.compiled.manaCost).outcome ==
              CastOutcome::Fatal;
          if (fatal) {
            const Vec3 body = player.pos;
            originFx = {SpellFxFromFloat(body.x), SpellFxFromFloat(body.y),
                        SpellFxFromFloat(body.z)};
          }
          const SpellProbe probe = WorldSpellProbe(world);
          CastResult res = spells.Cast(hs.compiled, caster.mana, playerHealth,
                                       kPlayerCasterId, originFx, dirFx, tick, emit, &probe,
                                       nullptr, &bodyProbe);
          caster.lastOutcome = res.outcome;
          caster.beamHand = hk;
          // ...and the arm throws out after it (PoseSpellHands, next tick's
          // pose). A cast the VM refused throws nothing.
          if (res.outcome != CastOutcome::Nothing) s.spellCastHand = hk;
        }

        // A held beam follows the aim while the button of the hand that cast
        // it stays down.
        {
          const Hand bh = HandAt(caster.beamHand);
          const Vec3 fwd = cam.Forward();
          const Vec3 muzzle = SpellHandPoint(avatar, player, cam, bh) + fwd * 1.5f;
          spells.HoldBeam(kPlayerCasterId,
                          {SpellFxFromFloat(muzzle.x), SpellFxFromFloat(muzzle.y),
                           SpellFxFromFloat(muzzle.z)},
                          {SpellFxFromFloat(fwd.x), SpellFxFromFloat(fwd.y),
                           SpellFxFromFloat(fwd.z)},
                          ti.Held(bh == Hand::Right ? TB_ATTACK : TB_ALT));
        }
        if (ti.Pressed(TB_DROP)) {
          spells.DropNewestStatus(kPlayerCasterId);
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
            if (sb.casterId == kPlayerCasterId) bill += sb.amount;
          if (bill > 0) {
            const int32_t fromMana = std::min(bill, caster.mana.mana);
            caster.mana.mana -= fromMana;
            bill -= fromMana;
            if (bill > 0) {
              const int32_t hp = playerHealth.Get();
              if (hp > bill) {
                playerHealth.Spend(bill);
              } else {
                spells.DropAll(kPlayerCasterId);   // dry: it all goes out
              }
            }
          }
          // ...plus every bound demon's UPKEEP (demons D5, game/demon_talk.h).
          caster.mana.reserved =
              spells.ReservationFor(kPlayerCasterId) + demon::UpkeepFor(w, s.index);
        }
        // A sustained gravity mod on a body: the caster's own, or anyone
        // else's. The second half was missing until 2026-09-22 — the VM has
        // always emitted the impulse for whatever body the status attached to,
        // and this loop dropped every one that was not the player's, so
        // `float aura` lifted you and left an enemy standing. MobSystem knows
        // which of its two velocity states the body is in (limp or walking);
        // this only has to hand the number over.
        // ONE ROUTE FOR EVERY BODY (Mob::AddBodyVelocity, W2-L): the
        // caster's own lift goes through its body too, which is what reaches a
        // LIMP player's limbs (the controller is not integrating then).
        for (const SpellBodyImpulse& bi : emit.bodyImpulses) {
          if (bi.target == kPlayerCasterId) {
            avatar.BindPlayer(&player);
            avatar.AddBodyVelocity(bi.vps);
          } else {
            mobs.LiftMob(bi.target, bi.vps);
          }
        }
        // GRAFTS: the world half already left as ops; the body half fills the
        // caster's missing anatomy cells with that matter, root-first. The VM
        // cannot reach a body (thesis 4); the owner does it.
        for (const SpellRestore& rs : emit.restores) {
          if (rs.casterId == kPlayerCasterId) {
            if (avatar.Spawned()) avatar.RestoreBody(rs.material, rs.count);
          } else {
            mobs.RestoreMob(rs.casterId, rs.material, rs.count);
          }
        }
        // WARDS filter the spell's OWN emission too (a fire aura inside an
        // anti-fire ward is refused like anyone else's).
        ui.spellRefused = spells.FilterStreams(emit.ops, emit.explosions, emit.spawns, emit.winds,
                                               &emit.strikes, &emit.summons);
        // SUMMONINGS (game/demon.h, docs/PLAN_demons.md D1): the VM reported a
        // demon's name cast at a point; the demon world queues the arrival and
        // asks the stores for what it will read.
        for (const SpellSummon& su : emit.summons)
          DemonQueueSummon(w, std::span<SessionTick>(&st, 1), s.index, su, glyphs, tick);

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
          phys.SetBodyRole(h, Physics::BodyRole::Debris);  // clears the caster first
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
        // STRIKES (the `lightning` / `shock` glyphs, docs/PLAN_electricity.md
        // E3): the VM reported where; the bolt is planned against the stores
        // and emitted as CellOps by the storm's own strike function, charged
        // to the tick's strike budget before a single op is pushed. A refusal
        // (the budget, or a ward at the struck cell) is counted and lands in
        // the HUD's spell overflow, like an op that did not fit; a budget
        // refusal refunds the strike's tariff. A strike whose search leaves
        // the stores waits a stepped leader -- the due ones fire first, in
        // the order they were cast.
        ui.spellOpsDropped += FireDueSpellStrikes(w, s, tick, cellOps);
        for (const SpellStrike& sk : emit.strikes) {
          const GlyphDef* sg = glyphs.At(sk.glyph);
          if (!sg || !sg->strike.has) continue;
          if (SpellStrikeToCells(w, s, *sg, sk, tick, cellOps) == false) ui.spellOpsDropped++;
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
          w.ownerId = kPlayerCasterId;   // the same casterId the projectile carries
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
  }
}

// ---- PHASE J (WORLD) - --fell-tree, debris PreTick and the fluid lab
// Verbatim from the single-body TickAuthority, lines 2007-2033 at 54fe241.
static void PhaseJ(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {

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
  }
}

// ---- PHASE K (PLAYER) - laser kerf, the sword bite, prefab drain, explosions
// Verbatim from the single-body TickAuthority, lines 2034-2334 at 54fe241.
// ---- REACTION EFFECTS -> EXPLOSIONS (docs/PLAN_alchemy_chemistry.md A) ----
//
// The world's handler registry for reactions.json "effects", keyed by kind
// NAME. The world handles `explode` (package A), `flash` (package E: a
// burst of `glare` round the cell) and `shock` (a flash whose default
// material is `spark`: a discharge that lives a tick or two and ignites what
// it touches, through the spark's own rules). `eject`, `burst` and `pop` act
// on a vessel or a player at the bench, which the world has no counterpart
// for, so they are the bench's alone (alchemy_bench.cpp's registry) and are
// ignored here by design (materials.h ReactionEffect: "a consumer that does
// not know a kind ignores it"). A new world kind is one more branch below
// plus JSON.
//
// WHAT IT READS is fixed-latency and ordered: the grid's firings come from
// World::TakeReactFx (published snapshots, slot order within a tick, tick
// order across), the bodies' from MobSystem::TakeBodyReactFx (this tick's
// burn pass, creature order). Everything below is a pure function of those
// lists and the tick, so two runs author the same ExplosionOps (rule 1), and
// every blast is an ExplosionOp through SubmitTick (rule 3).
//
// BOUNDED (rule 2): at most kReactBlastsPerTick blasts a tick from reactions
// (inside the engine's kMaxExplosionsPerTick), and a firing within an
// already-issued blast's radius this tick is merged into it rather than
// blasted again -- a lump of sodium in a lake fires hundreds of rules a
// tick, and what reads right is a few bangs, not a hundred.
static constexpr uint32_t kReactBlastsPerTick = 4;
// FLASH (package E: thermite, phosphorus, the chemical wedding, a
// transmutation) -- the world's second kind. A ball of light-only gas laid
// round the cell the tick after (ReactFxAftermath), at most this many a tick,
// a firing inside one already laid this tick merged into it.
static constexpr size_t kReactFlashesPerTick = 4;

static void ReactFxAftermath(TickAuthorityCtx& w, uint32_t tick,
                             std::vector<CellOp>& cellOps) {
  auto& rf = w.reactFx;
  if (rf.aftermath.empty()) return;
  // Resolved by name, never a hardcoded id (CLAUDE.md conventions).
  uint32_t fireId = 0, smokeId = 0;
  for (size_t i = 1; i < w.mats.size(); i++) {
    if (w.mats[i].name == "fire") fireId = (uint32_t)i;
    if (w.mats[i].name == "smoke") smokeId = (uint32_t)i;
  }
  std::vector<TickAuthorityCtx::ReactFxWorld::Aftermath> keep;
  for (const auto& a : rf.aftermath) {
    // One tick AFTER the blast, so the crater exists and the IfAir ops land
    // in the hole it opened instead of on the rock it was about to remove.
    if (a.tick >= tick) { keep.push_back(a); continue; }
    const uint32_t key = (uint32_t)a.c.x * 73856093u ^ (uint32_t)a.c.y * 19349663u ^
                         (uint32_t)a.c.z * 83492791u;
    const int r = std::max(1, a.radius);
    if (a.mat != 0 && a.arcs) {
      // ARCS (a body burst with `arcs`: a power cell discharging). Four to
      // six jagged walks out of the centre, each step one cell along its
      // heading plus a hashed sideways kick, so the spray reads as lightning
      // rather than a puff. Same purity and bound as the ball below: every
      // position a hash of (cell, tick, arc, step), at most 6 x 2r cells.
      const int nArc = 4 + (int)(rng::Hash3(key, a.tick, 0xA2C50000u) % 3u);
      for (int k = 0; k < nArc; k++) {
        const uint32_t hd = rng::Hash3(key, a.tick, 0xA2C60000u + (uint32_t)k);
        // A heading on the unit cube's surface, from the hash.
        int hx = (int)(hd % 3u) - 1, hy = (int)((hd >> 4) % 3u) - 1,
            hz = (int)((hd >> 8) % 3u) - 1;
        if (!hx && !hy && !hz) hy = 1;
        IVec3 c = a.c;
        for (int st = 0; st < 2 * r; st++) {
          if (cellOps.size() >= kMaxCellOpsPerTick) break;
          const uint32_t hs = rng::Hash3(key ^ (uint32_t)k * 0x9E3779B9u,
                                         a.tick, 0xA2C70000u + (uint32_t)st);
          c.x += hx + ((hs & 3u) == 0 ? 1 : (hs & 3u) == 1 ? -1 : 0);
          c.y += hy + (((hs >> 2) & 3u) == 0 ? 1 : ((hs >> 2) & 3u) == 1 ? -1 : 0);
          c.z += hz + (((hs >> 4) & 3u) == 0 ? 1 : ((hs >> 4) & 3u) == 1 ? -1 : 0);
          if (!w.world.CellInWindow(c)) break;
          cellOps.push_back({World::SlotCellIndex(c),
                             PackVoxNew(a.mat, (hs >> 28) % 3u) | kCellOpIfAir});
          rf.flashCells++;
        }
      }
      continue;
    }
    if (a.mat != 0) {
      // A FLASH (package E): a ball of the flash material -- `glare`, pure
      // light that decays in two or three ticks and reacts with nothing --
      // laid IfAir round the cell. Positions are a hash of (cell, tick, k),
      // so the ops are a pure function of the firing (rule 1); the count is
      // bounded here and the flashes a tick in ReactFxToBlasts (rule 2).
      const int n = std::min(6 + 3 * r, 30);
      for (int k = 0; k < n; k++) {
        if (cellOps.size() >= kMaxCellOpsPerTick) break;
        const uint32_t h = rng::Hash3(key, a.tick, 0xF1A50000u + (uint32_t)k);
        const int dx = (int)(h % (uint32_t)(2 * r + 1)) - r;
        const int dy = (int)((h >> 10) % (uint32_t)(2 * r + 1)) - r;
        const int dz = (int)((h >> 20) % (uint32_t)(2 * r + 1)) - r;
        const IVec3 c{a.c.x + dx, a.c.y + dy, a.c.z + dz};
        if (!w.world.CellInWindow(c)) continue;
        cellOps.push_back({World::SlotCellIndex(c),
                           PackVoxNew(a.mat, (h >> 28) % 3u) | kCellOpIfAir});
        rf.flashCells++;
      }
      continue;
    }
    const int rf3 = std::max(1, (3 * r) / 4);
    // A brief flash of flame inside the crater and a puff of smoke over it.
    // Fire decays in a few ticks and smoke in a few seconds (reactions.json),
    // so this is the whole of the aftermath; nothing here grows.
    const int nFire = std::min(4 + 2 * r, 20);
    const int nSmoke = std::min(3 + r, 12);
    for (int k = 0; k < nFire + nSmoke; k++) {
      if (cellOps.size() >= kMaxCellOpsPerTick) break;
      const uint32_t h = rng::Hash3(key, a.tick, 0xAF7E0000u + (uint32_t)k);
      const bool fire = k < nFire;
      const int span = fire ? rf3 : r;
      const int dx = (int)(h % (uint32_t)(2 * span + 1)) - span;
      const int dz = (int)((h >> 10) % (uint32_t)(2 * span + 1)) - span;
      const int dy = fire ? (int)((h >> 20) % (uint32_t)(2 * span + 1)) - span
                          : (int)((h >> 20) % (uint32_t)(r + 1)) + r / 2;
      const IVec3 c{a.c.x + dx, a.c.y + dy, a.c.z + dz};
      if (!w.world.CellInWindow(c)) continue;
      const uint32_t mat = fire ? fireId : smokeId;
      if (!mat) continue;
      cellOps.push_back({World::SlotCellIndex(c),
                         PackVoxNew(mat, (h >> 28) % 3u) | kCellOpIfAir});
      rf.aftermathCells++;
    }
  }
  rf.aftermath.swap(keep);
}

static void ReactFxToBlasts(TickAuthorityCtx& w, uint32_t tick,
                            std::vector<ExplosionOp>& exps) {
  auto& rf = w.reactFx;
  std::vector<ReactFxEvent> evs = w.world.TakeReactFx();
  std::vector<ReactFxEvent> body = w.mobs.TakeBodyReactFx();
  rf.bodyEvents += body.size();
  // BODY BURSTS (materials.h struck / burst): sparks and steam off a struck
  // machine, a breached boiler, a dead android's discharge. Already capped
  // per tick by MobSystem::PushBodyBurst; laid the NEXT tick as a flash ball
  // (or arcs) through the same aftermath as a reaction flash, so the write
  // goes through the op stream (rule 3) and never merges with a reaction's.
  for (const MobSystem::BodyBurst& b : w.mobs.TakeBodyBursts()) {
    TickAuthorityCtx::ReactFxWorld::Aftermath a;
    a.c = b.cell;
    a.radius = b.radius;
    a.tick = tick;
    a.mat = b.mat;
    a.arcs = b.arcs;
    rf.aftermath.push_back(a);
    rf.bodyBursts++;
  }
  evs.insert(evs.end(), body.begin(), body.end());
  rf.events += evs.size();
  uint32_t issued = 0;
  const size_t first = exps.size();
  // FLASHES (package E): merged and capped exactly as blasts are, into their
  // own list -- a flash is light, so it neither consumes nor is merged into
  // a blast slot.
  struct Flash { IVec3 c; int r; };
  std::vector<Flash> flashes;
  auto matByName = [&](const std::string& n) -> uint32_t {
    for (size_t i = 1; i < w.mats.size(); i++)
      if (w.mats[i].name == n) return (uint32_t)i;
    return 0;
  };
  for (const ReactFxEvent& e : evs) {
    const RuleFx* fx = FindRuleFx(w.mats, e.fxId);
    if (!fx) continue;
    for (const ReactionEffect& eff : fx->effects) {
      if (eff.kind == "flash" || eff.kind == "shock") {
        const int r = std::clamp(eff.radius > 0 ? eff.radius : 2, 1, 6);
        bool merged = false;
        for (const Flash& f : flashes)
          merged = merged || std::max({std::abs(f.c.x - e.cell.x), std::abs(f.c.y - e.cell.y),
                                       std::abs(f.c.z - e.cell.z)}) <= f.r;
        if (merged || flashes.size() >= kReactFlashesPerTick) {
          rf.refused++;
          continue;
        }
        const uint32_t mat = matByName(!eff.what.empty() ? eff.what
                                       : eff.kind == "shock" ? std::string("spark")
                                                             : std::string("glare"));
        if (!mat) continue;
        flashes.push_back({e.cell, r});
        rf.flashes++;
        TickAuthorityCtx::ReactFxWorld::Aftermath a;
        a.c = e.cell;
        a.radius = r;
        a.tick = tick;
        a.mat = mat;
        rf.aftermath.push_back(a);
        continue;
      }
      if (eff.kind != "explode") continue;  // not a world kind (yet)
      bool merged = false;
      for (size_t i = first; i < exps.size() && !merged; i++) {
        const ExplosionOp& o = exps[i];
        merged = std::max({std::abs(o.x - e.cell.x), std::abs(o.y - e.cell.y),
                           std::abs(o.z - e.cell.z)}) <= std::max(o.radius, 1);
      }
      if (merged || issued >= kReactBlastsPerTick ||
          exps.size() >= kMaxExplosionsPerTick) {
        rf.refused++;
        continue;
      }
      const int radius = std::clamp(eff.radius, 1, kMaxExplosionRadius);
      const ExplosionOp op{e.cell.x, e.cell.y, e.cell.z, radius,
                           std::max(1, eff.power), 0, 0, 0};
      exps.push_back(op);
      issued++;
      rf.blasts++;
      rf.recent.push_back(op);
      if (rf.recent.size() > TickAuthorityCtx::ReactFxWorld::kRecent)
        rf.recent.erase(rf.recent.begin());
      rf.aftermath.push_back({e.cell, radius, tick});
    }
  }
}

static void PhaseK(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {
      // laser kerf into a body, deferred from the input block above so it can
      // reach `spawns` (a cut that severs the body sheds the loose bits)
      if (laserCut.body) {
        // A live limb is carved (eject=false: the beam vaporizes, and a held
        // laser spraying gobbets every tick would drain the particle ring);
        // plain debris melts. The two paths are the same operation on the two
        // populations — see game/mob.h.
        if (laserCut.limb)
          mobs.CarveLimbRadial(laserCut.body, laserCut.at, laserCut.radius,
                               false /*ragged*/, false /*eject*/, world, spawns,
                               DamageCtx(DamageCause::Beam));
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
      avatar.SetSwinging(playerStrike.Cutting());
      if (avatar.Spawned() && meleeReady) {
        // THE STRIKING HAND's item (dual wielding) — the one WeaponEdge below
        // reads the blade of. The other fist's is carried, not swung.
        const HandNow& strikeHn = hands[avatar.StrikeHand()];
        const ItemDef* heldItem = strikeHn.weapon ? strikeHn.item : nullptr;
        Vec3 eb, et, ef;
        float ehw = 0;
        if (avatar.WeaponEdge(eb, et, ehw, &ef)) {
          // THE WHOOSH'S LOUDNESS (armed on the cut edge above): this tick's
          // tip travel against the damage curve's own min/full speeds.
          if (s.whooshArmed && lastEdgeValid) {
            const float sp = (et - lastEdgeTip).len() / kTickDt;
            const float lo = melee.tuning.minSpeed;
            const float hi = std::max(melee.tuning.fullSpeed, lo + 1e-3f);
            combatWhooshCue.power = std::clamp((sp - lo) / (hi - lo), 0.0f, 1.0f);
          }
          s.whooshArmed = false;
          // THE PROGRAM'S CUT is the cut — the NPC's own contract:
          // MobSystem::StepStroke sweeps on the CURSOR's cut, and tip speed
          // still scales the damage.
          // ONE STROKE, ONE IMPULSE PER SLOT (melee.h EdgeSweep::struck). The
          // set is cleared on the first tick the program is not cutting, the
          // same test the sweep gates on, so a strike gets exactly one blunt
          // hit per body per swing.
          if (!playerStrike.Cutting()) {
            playerStruck.clear();
            playerBitten = false;
            s.playerHaftStruck.clear();
            s.playerVictimHits.clear();
          }
          if (lastEdgeValid && playerStrike.Cutting()) {
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
            // A CHARGED STRIKE (held past its windup) lands harder: every hp
            // of the profile, so a charged mace and a charged sword are each
            // their own blow, stronger. The haft sweep copies `sw`, so it is
            // charged too. `power` (tip speed x edge) is untouched: the swing
            // is the same swing.
            if (playerStrike.charged) {
              const float cm = CurrentTuning().melee.chargeDamage;
              sw.strike.cut *= cm;
              sw.strike.blunt *= cm;
              sw.strike.bite *= cm;
            }
            sw.tick = tick;
            // BLUNT AND BITE ARE IMPULSES (melee.h EdgeSweep::struck). The
            // set is owned here and cleared on the tick the program is not
            // cutting, the same line the sweep gate above reads.
            sw.struck = &playerStruck;
            sw.bitten = &playerBitten;
            // Strike n on one creature does repeatHitScale^(n-1) of its hp;
            // the haft copies this pointer and shares the count.
            sw.victimHits = &s.playerVictimHits;
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
            // ONE BLOW'S AFTERMATH -- hit-stop tier and impact cues -- for the
            // blade's sweep and for the haft's (weak: a chip dip at most,
            // unless something came off; cues `gainDb` down).
            auto resolveSweep = [&](const EdgeSweep& sw, float gainDb,
                                    bool weak) -> EdgeSweepResult {
              const size_t sev0 = mobs.SeverEvents().size();
              const size_t voi0 = mobs.VoiceEvents().size();
              // The third queue, for the population the other two cannot see:
              // dead flesh (DebrisSystem::GoreEvent).
              const size_t gore0 = debris.GoreEvents().size();
              const EdgeSweepResult res = MeleeSweepDamage(
                  sw, melee.tuning, avatar, phys, mobs, debris, world, spawns);
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
                else if (flesh && !weak)
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
                q.Offer(pw, hitAt, gainDb);
                // Weapon impact layer: the sword's ring or the mace's thud,
                // on ANY body contact regardless of flesh/armor.
                const bool edged = sw.strike.cut > sw.strike.blunt;
                const bool louder = !combatStrikeCue.pending ||
                    pw * std::pow(10.0f, gainDb / 20.0f) >
                        combatStrikeCue.power *
                            std::pow(10.0f, combatStrikeCue.gainDb / 20.0f);
                combatStrikeCue.Offer(pw, hitAt, gainDb);
                if (louder) combatStrikeEdged = edged;
                // Wet cutting layer: edged weapons contacting flesh.
                if (flesh && edged) combatCutCue.Offer(pw, hitAt, gainDb);
              }
              return res;
            };
            const EdgeSweepResult res = resolveSweep(sw, 0.0f, false);
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
            // ---- THE HAFT: the stick behind the head (item.h hasHaft) ---
            // Swept after the head, from its own last position, with its own
            // impulse set; skipped once the head was parried (the blow is
            // over). `powerScale` makes the whole blow that much weaker.
            Vec3 hb, ht;
            float hhw = 0, hPow = 1, hDb = 0;
            if (!res.arrested && s.lastHaftValid &&
                avatar.HaftEdge(hb, ht, hhw, hPow, hDb)) {
              EdgeSweep hs = sw;
              hs.aPrev = s.lastHaftBase;
              hs.bPrev = s.lastHaftTip;
              hs.aNow = hb;
              hs.bNow = ht;
              hs.flatNow = Vec3{};
              hs.halfWidth = hhw;
              hs.carveBonus = 0.0f;
              hs.powerScale = hPow;
              hs.haft = true;
              hs.struck = &s.playerHaftStruck;
              hs.bitten = nullptr;
              const EdgeSweepResult hr = resolveSweep(hs, hDb, true);
              if (hr.arrested) {
                melee.Arrest();
                if (playerStrike.Cutting()) {
                  playerStrike.phase = StrokeCursor::Phase::Recover;
                  playerStrike.phaseTick = 0;
                }
              }
            }
          }
          lastEdgeBase = eb;
          lastEdgeTip = et;
          lastEdgeValid = true;
          {
            Vec3 hb, ht;
            float hhw = 0, hPow = 1, hDb = 0;
            s.lastHaftValid = avatar.HaftEdge(hb, ht, hhw, hPow, hDb);
            s.lastHaftBase = hb;
            s.lastHaftTip = ht;
          }
        } else {
          lastEdgeValid = false;   // sheathed or severed: no segment to sweep
          s.lastHaftValid = false;
        }
      } else {
        lastEdgeValid = false;
        s.lastHaftValid = false;
      }

      // prefab stamps drain after island ops (they win same-cell conflicts)
      placer.PreTick(world, cellOps);

      // explosions: X-detonate at the crosshair + grenade fuses + spell blasts
      // (`exps` is aliased out of the caller's OpBatch.)
      // Spell blasts join here rather than getting their own path, so a
      // firebolt's detonation gets the island checks, body damage, mob carving
      // and impulse a grenade already gets — for free, and consistently.
      // WHERE THIS SESSION'S BLASTS START. Everything below acts on the
      // explosions THIS session added, never on the whole batch - with two
      // players the second pass would otherwise re-crater, re-carve and
      // re-shove every body the first player's grenade already hit. For one
      // session expBegin is 0 and every test below is the one it replaces.
      const size_t expBegin = exps.size();
      // A GATE'S BLAST IS THE PRIMARY'S (session.h section J): it goes off in
      // this slot so it gets the crater scan, the body carve, the debris
      // impulse and the rig launch a grenade gets. Empty in the game.
      if (s.index == 0)
        for (const ExplosionOp& e : w.harness.blasts)
          if (exps.size() < kMaxExplosionsPerTick) exps.push_back(e);
      for (const ExplosionOp& e : spellExps)
        if (exps.size() < kMaxExplosionsPerTick) exps.push_back(e);
      // X-detonate is the DEV PANEL's, so it fires once, with the primary.
      if (s.index == 0 && ui.pendingDetonate) {
        ui.pendingDetonate = false;
        const WorldSnapshot& snap = world.Snap();
        if (snap.valid && snap.pick[0] != 0) {
          exps.push_back({(int)snap.pick[2], (int)snap.pick[3], (int)snap.pick[4],
                          CurrentTuning().tools.detonateRadius,
                          CurrentTuning().tools.detonatePower, 0, 0, 0});
        }
      }
      // THE ALCHEMY BENCH'S EXPLOSIONS (alchemy_bench.h BenchOutcome): a
      // reaction that blew up in the character's hands goes off here, in the
      // grenade's slot, so it craters, carves the hands and arms that held
      // it and shoves exactly as a grenade at the hands would.
      // One that does not fit this tick waits for the next rather than
      // vanishing: the vessels that made it are already gone.
      {
        size_t taken = 0;
        while (taken < s.pendingBlasts.size() && exps.size() < kMaxExplosionsPerTick)
          exps.push_back(s.pendingBlasts[taken++]);
        s.pendingBlasts.erase(s.pendingBlasts.begin(), s.pendingBlasts.begin() + (ptrdiff_t)taken);
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
      // REACTIONS THAT EXPLODE (sodium + water, hydrogen + fire, ...). The
      // world's, so the primary's slot, like a gate's blast; after the
      // player's own blasts so a grenade is never refused for a reaction.
      // Last tick's blasts get their fire and smoke first.
      if (s.index == 0) {
        ReactFxAftermath(w, tick, cellOps);
        ReactFxToBlasts(w, tick, exps);
        // THE STORM'S GROUND STRIKES (docs/PLAN_electricity.md E3): the bolts
        // are the world's, so they FIRE in the primary's slot, beside the
        // reactions' aftermath...
        FireWeatherStrikes(w, tick, cellOps);
      }
      // ...and every session ROLLS for its own player (wave 2), keyed by the
      // player's id, so a storm strikes round each player, not only the first.
      DecideWeatherStrike(w, w.strikes.playerIdBase + (uint32_t)s.index, player.pos, tick);
      if (exps.size() > expBegin) {
        everExploded = true;
        lastExplosionTick = tick;
        for (size_t ei = expBegin; ei < exps.size(); ei++) {
          const ExplosionOp& e = exps[ei];
          // ---- THE CRATER SCAN IS NOT RAISED HERE ANY MORE (M9.4-D) ------
          //
          // It moved to phase N, after the op merge, and it is raised from the
          // MERGED batch instead of from this machine's own explosions. The
          // reason is ownership: under M9.4 a peer's grenade going off in a
          // chunk I hold needs MY island scan (I am the only machine that will
          // step the matter it cuts loose), and this loop can only see the
          // explosions THIS machine authored. Raising it here would scan the
          // author's chunks and nobody would scan the owner's.
          //
          // SINGLE-PLAYER IS UNCHANGED, exactly: with no peer the merged batch
          // IS this vector, `OpLandingTick` is `tick`, phase N is after phase
          // J (the only other producer of destruction events in a tick), so
          // the same boxes are queued at the same tick in the same order.
          //
          // ---- THE BODIES STAY HERE, ON THE AUTHOR'S MACHINE (W1-F) -------
          // Every body this process holds: NPCs, every local player, and a
          // peer's ghost (carved as presentation, never launched) — the same
          // rule a melee hit on a ghost follows. The peer's REAL body is hit
          // on the peer's machine, by the peer, at the landing tick: phase N's
          // RemoteExplosionsHitOwnAvatars applies every REMOTE blast in the
          // merged batch to that machine's own avatars (DESIGN.md §10). The
          // ghost's gore is discarded here (MobSystem::CarveMobsRadial) and
          // authored there, so it enters the shared batch once.
          // VESSELS BURST FIRST (container.h ContainerBlastPass): the carve
          // below would take a flask's body and, through OnBodyGone, the
          // registry entry that is its contents. They drain through
          // w.vesselSpills from next tick's phase H, like a broken flask.
          if (w.ground)
            ContainerBlastPass(*w.ground, items, phys, debris,
                               BlastForceOf(e).center, BlastForceOf(e).craterRadius,
                               w.vesselSpills);
          ExplosionHitsBodies(e, world, phys, debris, mobs, spawns);
          stream.MarkModifiedBox({e.x - e.radius, e.y - e.radius, e.z - e.radius},
                                 {e.x + e.radius, e.y + e.radius, e.z + e.radius});
        }
      }
  }
}

// ---- ONE EXPLOSION AGAINST EVERY BODY (session.h) -------------------------
BlastForce BlastForceOf(const ExplosionOp& e) {
  const Tuning& t = CurrentTuning();
  BlastForce f;
  f.center = Vec3{(float)e.x + 0.5f, (float)e.y + 0.5f, (float)e.z + 0.5f};
  f.debrisCenter = Vec3{(float)e.x, (float)e.y, (float)e.z};
  f.craterRadius = (float)e.radius * t.physics.explosionBodyDamageScale;
  f.pushRadius = (float)e.radius * t.physics.explosionImpulseRadiusScale;
  f.debrisImpulse = (float)e.power * t.physics.explosionImpulseScale;
  f.rigImpulse = (float)e.power * t.ragdoll.blastImpulseScale;
  f.power = (float)e.power;
  return f;
}

void ExplosionHitsBodies(const ExplosionOp& e, World& world, Physics& phys,
                         DebrisSystem& debris, MobSystem& mobs,
                         std::vector<ParticleSpawn>& spawns, BlastBodies who) {
  // Reach and strength come from ONE place (BlastForceOf, session.h).
  const BlastForce f = BlastForceOf(e);
  if (who == BlastBodies::OwnAvatars) {
    // A PEER's blast on this machine: the same carve and the same launch as
    // below, on this process's own avatars only. The debris, the NPCs and the
    // impulse are left alone -- this is the owner-side half of a player hit,
    // not a second application of the whole blast.
    mobs.CarveLocalAvatarsRadial(f.center, f.craterRadius, world, spawns,
                                 f.power);
    mobs.BlastLocalAvatarsRadial(f.center, f.pushRadius, f.rigImpulse);
    return;
  }
  // Blow voxels OFF the bodies in range before shoving what survives:
  // an explosion next to a rigidbody now craters it, and splits it into
  // separate bodies when the crater severs it. Runs first so the
  // impulse below acts on the post-damage bodies (including the new
  // fragments, which is what makes a blown-apart object scatter).
  debris.DamageBodiesRadial(f.center, f.craterRadius, world, spawns);
  // Living flesh craters too: a blast next to a mob tears voxels off
  // its limbs, and takes a limb clean off when it removes enough of it.
  // Same call shape as the debris line above — that parallel is the
  // point (game/mob.h). EVERY PLAYER is in it (W1-F): MobSystem walks
  // its registered avatars after the NPCs, so another player standing
  // in this session's blast is carved. It used to be this session's
  // own `avatar.CarveRadial` beside it, which reached nobody else.
  //
  // WITH ITS POWER (W2-H): the body crater follows the terrain crater's
  // rule, so a weak blast tears less than a strong one of the same radius
  // and a worn shell in the way takes its hardness off the flesh behind it
  // (Mob::CarveLimbRadial, game/shellresponse.h).
  mobs.CarveMobsRadial(f.center, f.craterRadius, world, spawns, f.power);
  // The per-body impulse is for DEBRIS. A living creature's limbs are
  // skipped whether kinematic (standing) or dynamic (already limp
  // from an earlier blast): impulse / limb mass on a 0.3 kg hand is
  // 170 m/s and the joints drag the rest of the rig after it —
  // "bodies zoom across the map". The rig takes ONE launch below.
  std::vector<uint64_t> rigBodies;
  mobs.AppendLiveLimbBodies(rigBodies);  // NPCs + every avatar
  std::sort(rigBodies.begin(), rigBodies.end());
  phys.ApplyRadialImpulse(f.debrisCenter, f.pushRadius, f.debrisImpulse,
                          &rigBodies);
  // ...and the LIVING are knocked flying. A standing creature's limbs
  // are kinematic, so the impulse above never touched them; this is
  // the blast's other half (Mob::BlastRadial): go limp, take a launch
  // velocity of impulse / body mass toward away-from-the-blast,
  // capped at ragdoll.maxLaunchSpeed, and get back up once landed.
  // NPCs + every LOCAL avatar; a peer's ghost is carved above but
  // never launched — its position is the wire's (game/mob.h).
  mobs.BlastMobsRadial(f.center, f.pushRadius, f.rigImpulse);
}

uint32_t RemoteExplosionsHitOwnAvatars(std::span<const ExplosionOp> exps,
                                       std::span<const uint32_t> remoteIdx,
                                       World& world, Physics& phys,
                                       DebrisSystem& debris, MobSystem& mobs,
                                       std::vector<ParticleSpawn>& gore) {
  uint32_t n = 0;
  for (uint32_t i : remoteIdx) {
    if (i >= exps.size()) continue;
    ExplosionHitsBodies(exps[i], world, phys, debris, mobs, gore,
                        BlastBodies::OwnAvatars);
    n++;
  }
  return n;
}

// ---- PHASE L (WORLD) - dirty marks, the celestial clock and the edit layer
// Verbatim from the single-body TickAuthority, lines 2335-2384 at 54fe241.
static void PhaseL(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  // The GameLogic span is the WORLD's (it is opened once, before the
  // first session's phase C) and it is closed here, where the submit
  // sequence starts. Aliased so the body below reads as it always did.
  sandvox::PerfSpan& spanGame = *ws.spanGame;
  {
      // CPU-known writes mark chunks modified now — eviction can't wait for
      // the latent dirty-flag snapshot
      for (const BrushOp& b : ops)
        stream.MarkModifiedBox({b.x - b.radius, b.y - b.radius, b.z - b.radius},
                               {b.x + b.radius, b.y + b.radius, b.z + b.radius});
      // A peer's blast carved my avatar last tick (phase N); its gore joins
      // THIS tick's local batch so it reaches both machines (session.h).
      // Empty unless a peer is connected.
      if (!w.remoteBlastGore.empty()) {
        spawns.insert(spawns.end(), w.remoteBlastGore.begin(),
                      w.remoteBlastGore.end());
        w.remoteBlastGore.clear();
      }
      // ---- far landings coming home (chunk tickets P2, sim/tickets.h) ------
      // Matter that came to rest outside residency and was parked on the CPU
      // is re-thrown, still, on the tick its chunk is resident again (a ticket
      // or the window). On the ordinary spawn stream, bounded by its cap like
      // every other producer on it; what does not fit waits for the next tick.
      // HERE, before the keep-awake latch below, so a re-thrown particle keeps
      // the particle passes alive until it has landed, like any other spawn.
      if (stream.TicketSet().HasLandingSpawns() && spawns.size() < kMaxParticleSpawnsPerTick)
        stream.TicketSet().DrainLandingSpawns(
            spawns, kMaxParticleSpawnsPerTick - (uint32_t)spawns.size());
      // body-shatter spawns keep the particle passes alive exactly like
      // explosions do (a fragment must fly and land on later ticks too)
      if (!spawns.empty()) {
        everExploded = true;
        lastExplosionTick = tick;
      }
      particlesActive =
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

      pc = IVec3{ifloor(player.pos.x) / (int)kChunk,
                 ifloor(player.pos.y) / (int)kChunk,
                 ifloor(player.pos.z) / (int)kChunk};
      // A harness centres the mirror on its FIXTURE (session.h section J).
      if (w.harness.haveSubmitChunk) pc = w.harness.submitChunk;
      t0 = NowSeconds();
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
      //
      // The submit these ops ride is TicksEncoded() + 1 (SubmitTick encodes
      // this tick's readback after the kernels). The layer records which slots
      // it woke on that submit so Stream's modified fold can tell the layer's
      // own wake from a modification (sim/worldedit.h, map-overhaul P7).
      if (WorldEditLayer().HasPending() && cellOps.size() < kMaxCellOpsPerTick)
        WorldEditLayer().Drain(world, cellOps,
                               kMaxCellOpsPerTick - (uint32_t)cellOps.size(),
                               world.TicksEncoded() + 1u);
  }
}

// ---- PHASE M (PLAYER) - the capsule, the grab servo, wards and the swimmer wake
// Verbatim from the single-body TickAuthority, lines 2385-2436 at 54fe241.
static void PhaseM(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {
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
      grab.Tick(phys, debris, &mobs, CurrentTuning().player, player.EyePos(),
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
  }
}

// ---- PHASE N (WORLD) - SubmitTick and the physics step
// Verbatim from the single-body TickAuthority, lines 2437-2468 at 54fe241.
static void PhaseN(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {
      // ---- M9.3-B: THE OP EXCHANGE, AND IT IS ALL OF IT -----------------
      //
      // Everything above built THIS MACHINE's batch for tick `tick`. When a
      // peer is connected that batch is not what gets submitted: it is
      // labelled `tick + kOpLabelAhead` and stored, and what is submitted
      // instead is the MERGE of the batches both machines labelled `tick` —
      // which they authored that many ticks ago and exchanged in the
      // meantime.
      //
      // THE ORDER IS AUTHOR-MAJOR, PUSH-ORDER-MINOR (net/opsync.h), so the
      // two machines submit the same vector and CLAUDE.md rule 3's "the
      // lowest op index owns the cell" names the same cell on both. If the
      // merge ever had to reorder a LOCAL op relative to its push order to be
      // canonical, that would mean a producer pushed after this point and the
      // rule would be unenforceable — the plan's kill criterion. It cannot
      // happen here: phase N is after every producer in the tick.
      //
      // THE METADATA HAS TO TRAVEL WITH THE BATCH. Author ranges
      // (oprecord.h's scopes) are recorded against the vector the PRODUCING
      // tick built and are consumed by the tick that SUBMITS it — the same
      // tick, until the delay put them five ticks apart. So the ranges are resolved to a flat
      // side table here, cleared, and re-noted at the merged indices inside
      // Merge. Without it the record would attribute a client's own brush
      // strokes to the host, whose ops precede them in the canonical order.
      //
      // SINGLE-PLAYER IS THE `else` AND IT IS NOT AN `else`: the whole block
      // is behind one bool. Nothing is stored, nothing is copied, and the
      // submitted vectors are the ones the tick built, in the order it built
      // them — which is what the `--record-ops` oracle comparison proves.
      if (w.opsync && w.opsync->Connected()) {
        namespace opstream = sandvox::opstream;
        net::OpDelayQueue& q = w.opsync->Q();
        const uint32_t label = tick + q.D();
        q.PushLocal(tick, out, world.WindowOrigin());
        q.NoteLocalMeta(
            label, opstream::ResolveAuthors(opstream::Stream::Brush,
                                            (uint32_t)ops.size()),
            opstream::ResolveAuthors(opstream::Stream::Explosion,
                                     (uint32_t)exps.size()));
        // The ranges belong to the batch that has just been stored; leaving
        // them would let them claim indices in the MERGED vector, which is a
        // different vector with different ops in it.
        opstream::ClearAuthorRanges();

        net::MergeStats ms;
        std::vector<uint32_t> remoteBrush, remoteExp, remoteCell;
        OpBatch merged = q.Merge(tick, world, ms, &remoteBrush, &remoteExp, &remoteCell);
        // A peer's lightning bolt flashes and claps here too (presentation).
        AnnounceRemoteStrikes(w, merged.cells, remoteCell, tick);
        // The local ops were marked modified at their own tick (phase L, by
        // the machine that authored them); a REMOTE brush op touches chunks
        // this machine never marked, and an unmarked chunk can be evicted
        // with the edit still in it. Sticky and cheap, so marking here — one
        // tick later than a local op would — is correct rather than merely
        // close enough.
        for (uint32_t i : remoteBrush) {
          const BrushOp& b = merged.ops[i];
          stream.MarkModifiedBox(
              {b.x - b.radius, b.y - b.radius, b.z - b.radius},
              {b.x + b.radius, b.y + b.radius, b.z + b.radius});
        }
        // A peer's crater is an edit to chunks this machine never marked,
        // for the same reason (phase K marks only this machine's blasts).
        for (uint32_t i : remoteExp) {
          const ExplosionOp& e = merged.exps[i];
          stream.MarkModifiedBox(
              {e.x - e.radius, e.y - e.radius, e.z - e.radius},
              {e.x + e.radius, e.y + e.radius, e.z + e.radius});
        }
        // `ops`, `exps`, `cellOps`, `spawns` and `fluidSpawns` are references
        // to out's members (SV_OPS_REFS), so assigning `out` re-points every
        // one of them at the merged vectors and the submit below needs no
        // change at all.
        out = std::move(merged);
        // ---- A PEER'S GRENADE HITS MY BODY, ON MY MACHINE ---------------
        // The owner-side half W1-F left open (DESIGN.md s10): at the landing
        // tick, the tick its crater reaches this GPU, each REMOTE explosion
        // carves and launches this machine's own avatars. The author's
        // machine carved my ghost as presentation and could not launch it;
        // this is the real body. My own blasts are LOCAL in the merge and
        // were applied in phase K when I authored them, so nothing is applied
        // twice. The gore goes to the next local batch (session.h).
        RemoteExplosionsHitOwnAvatars(exps, remoteExp, world, phys, debris,
                                      mobs, w.remoteBlastGore);
      }
      // ---- M9.3-C: THE SMOKE'S DELIBERATE DIVERGENCE --------------------
      //
      // AFTER the merge and before the submit, which is the only window in
      // which a cell op can reach this machine's GPU without reaching the
      // peer's: the outgoing batch was stored by PushLocal above, so appending
      // here cannot change what goes on the wire. Null except under
      // SANDVOX_NET_SMOKE_DRIFT=1 (session.h section I says why it exists at
      // all), so single-player and every harness are untouched.
      if (w.driftCells) w.driftCells(tick, cellOps);
      // ---- M9.4-D: CRATERS BY CHUNK AUTHORITY, FROM THE MERGED BATCH ----
      //
      // Every explosion that is about to be SUBMITTED this tick — mine and the
      // peer's, in the canonical order the merge produced — queues an island
      // scan over its blast box. Three properties, in the order they matter:
      //
      //  1. IT IS THE LANDING TICK BY CONSTRUCTION. `tick` here is the tick
      //     the op actually reaches the GPU, so there is no `OpLandingTick`
      //     shift to get wrong: the crater exists in the grid the scan will
      //     read. Phase K's version had to add D by hand and would have been
      //     wrong the day the delay changed.
      //  2. THE PEER'S EXPLOSIONS GET A SCAN, on the machine that owns the
      //     rock they cut. That is the whole point of the move (M9.4's note):
      //     a grenade thrown by the other player used to crater my chunk and
      //     leave the overhang hanging, because only its author queued a scan
      //     and the author does not step my matter.
      //  3. AUTHORITY IS TESTED ONCE, INSIDE THE DRAIN, not here.
      //     `DebrisSystem::SetChunkOwnedFn` already gates every event at the
      //     point it is consumed (debris.cpp's ChunkOwned test in the event
      //     drain), per-event-chunk rather than per-explosion-centre — which
      //     is strictly better for a blast that straddles a boundary. A second
      //     test on the centre chunk here would be a duplicate rule that could
      //     drift from it, and design guideline 3 is about exactly that.
      //
      // Unconnected this is byte-identical to what phase K did: same vector,
      // same boxes, same tick, and still after phase J's events.
      for (const ExplosionOp& e : exps)
        debris.AddDestructionEvent(
            tick, {e.x - e.radius, e.y - e.radius, e.z - e.radius},
            {e.x + e.radius, e.y + e.radius, e.z + e.radius});
      tSubmit0 = NowSeconds();
      SubmitTick(ctx, world, sim, tick, kDefaultSeed, ops, exps, cellOps,
                 tick % 15 == 0 /*hash occasionally*/, pc, true, particlesActive,
                 spawns, farCount, fluidSpawns, fluidCount,
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
      tSubmit1 = NowSeconds();
      phys.Step(kTickDt);   // CPU physics overlaps the GPU tick
      tPhys1 = NowSeconds();
      debris.PostStep();
      const double tPs1 = NowSeconds();
      mobs.PostStep();
      const double tPs2 = NowSeconds();
      // A THROWN ROCK IS A BLOW (W2-H): this step's new contacts, read while
      // they are this step's. A contact blow neither dents nor breaks plate
      // (MobSystem::ApplyContactDamage), so it authors no particles; the
      // scratch list is where they would go if it ever did.
      {
        std::vector<ParticleSpawn> contactGore;
        mobs.ApplyContactDamage(phys, world, contactGore);
      }
      const double tPs3 = NowSeconds();
      // LAST: after every burn and carve of the tick, so the frame never draws
      // a hooded head with hair a hit just re-packed poking through the hood.
      mobs.SyncHairTuck();
      // SANDVOX_POSTSTEP_PROF=<ms>: name the part of a slow post-step (the
      // `postStep` scope is all four calls above plus the avatar's).
      static const double postProfMs = [] {
        const char* e = std::getenv("SANDVOX_POSTSTEP_PROF");
        return e ? std::atof(e) : 0.0;
      }();
      if (postProfMs > 0.0) {
        const double tPs4 = NowSeconds();
        if ((tPs4 - tPhys1) * 1000.0 >= postProfMs)
          std::printf("[poststep] tick %u: %.2f ms = debris %.2f mobs %.2f contact %.2f "
                      "hair %.2f\n", tick, (tPs4 - tPhys1) * 1000.0,
                      (tPs1 - tPhys1) * 1000.0, (tPs2 - tPs1) * 1000.0,
                      (tPs3 - tPs2) * 1000.0, (tPs4 - tPs3) * 1000.0);
      }
  }
}

// ---- PHASE O (PLAYER) - avatar PostStep, the ragdoll follow and the push-out
// Verbatim from the single-body TickAuthority, lines 2469-2490 at 54fe241.
static void PhaseO(TickAuthorityCtx& w, WorldScratch& ws,
                    SessionTick& st, PlayerScratch& ps, uint32_t tick,
                    OpBatch& out) {
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_OPS_REFS
  SV_PLAYER_REFS
  SV_SEAM_REFS_PLAYER
  {
      avatar.PostStep();
      avatar.SyncHairTuck();   // the player's hair under their own hood
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
  }
}

// ---- PHASE P (WORLD) - the tick's perf accounting
// Verbatim from the single-body TickAuthority, lines 2491-2515 at 54fe241.
static void PhaseP(TickAuthorityCtx& w, WorldScratch& ws,
                    std::span<SessionTick> players,
                    std::span<PlayerScratch> scratch, uint32_t tick,
                    OpBatch& out) {
  // A world phase that needs A player needs THE PRIMARY: the dev panel,
  // the far-plume eye and the submit chunk are all the window's.
  SessionTick& st = players[0];
  PlayerScratch& ps = scratch[0];
  SV_ENGINE_REFS
  SV_SCRATCH_REFS
  SV_TELEMETRY_REFS
  SV_SEAM_REFS_WORLD
  SV_OPS_REFS
  SV_PLAYER_REFS
  {
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

// ---- one tick of authority, for N players ---------------------------------
//
// The alternation, and nothing else. Read it beside the phase comments above:
// a starred line is per session, in index order; an unstarred one runs once.
// ---- THE HANDS THIS TICK (session.h HandsNow) --------------------------------
//
// Can this arm throw an UNARMED style right now? The compass's styles, each
// taken on THIS arm (a style authored on the other side names its mirror's
// weapon, fist.R -> fist.L — Mob::ArmForStyle's rule, asked here without
// arming anything), is any one of them usable. A CONTENT question: author a
// creature with claws instead of fists and nothing here changes.
static bool UnarmedUsable(const Mob& body, const StyleLibrary& lib, Hand h) {
  if (!lib.playerUnarmed.Usable()) return false;
  auto usable = [&](int si) {
    const AttackStyle* sty = lib.At(si);
    if (sty == nullptr) return false;
    // A `held` style is thrown with THIS hand's fist (Mob::ArmForStyle).
    if (sty->weapon.empty() || sty->weapon == "held")
      return body.HandFist(h) != nullptr;
    const std::string name = StrokeMirrored(*sty, h) ? MirrorSideName(sty->weapon)
                                                     : sty->weapon;
    const MobNaturalWeaponDef* nw = body.NaturalWeaponNamed(name);
    return nw != nullptr && body.NaturalWeaponUsable(*nw);
  };
  for (const PlayerStrikeMap::Sector& sec : lib.playerUnarmed.sectors)
    if (usable(sec.style)) return true;
  for (int k = 0; k < 2; k++)
    if (usable(lib.playerUnarmed.neutral[k])) return true;
  return false;
}

static void ResolveHands(TickAuthorityCtx& w, SessionTick& st) {
  PlayerSession& s = *st.s;
  const Kit& kit = s.avatar.GetKit();
  const bool body = s.avatar.Spawned();
  for (int k = 0; k < kHands; k++) {
    const Hand h = HandAt(k);
    HandNow& hn = st.hands.hand[k];
    hn = HandNow{};
    const ItemDef* d = w.items.Of(kit.equip.InHand(h));
    // Only what a hand can grip rides it (EquipSlotAccepts' own rule, asked
    // again because a save or a script can put anything in a slot).
    if (d != nullptr && EquipSlotAccepts(EquipSlotOfHand(h), d->kind)) {
      hn.item = d;
      hn.weapon = d->kind == ItemKind::Melee;
      hn.vessel = d->IsContainer();
    }
    // No body (fly mode, dead) swings nothing; an arm that is gone or
    // useless (Mob::HandCondition 0) swings nothing either.
    // A spell in an empty hand: cast in fly mode too (no body: from the eye),
    // never from an arm that could not grip.
    if (hn.item == nullptr && s.caster.hand[k].Equipped() &&
        (!body || s.avatar.HandUsable(h))) {
      hn.spell = true;
      continue;
    }
    if (!st.intent.handsUp || !body || !s.avatar.HandUsable(h)) continue;
    if (hn.weapon) hn.ready = true;
    else if (hn.item == nullptr)
      hn.ready = UnarmedUsable(s.avatar, w.mobs.AttackStyles(), h);
  }
}

void TickAuthority(TickAuthorityCtx& w, std::span<SessionTick> players,
                   uint32_t tick, OpBatch& out) {
  // A tick with nobody in it is not a tick. Every world phase below reads the
  // primary for the window's questions, so there has to be one.
  if (players.empty()) return;

  // ONE BATCH FOR THE WHOLE TICK, cleared once. Every session appends to the
  // same five vectors in index order, so "op index is push order and push
  // order is deterministic" (CLAUDE.md rule 3) still holds with two players:
  // the order is (session 0's ops in phase order), then session 1's.
  out.Clear();
  // A GATE'S OWN OPS GO FIRST (session.h section J): the lowest op indices,
  // where every hand-rolled harness ticker put them. Null in the game.
  if (w.harness.inject) w.harness.inject(tick, out);

  if (!w.tickScratch) w.tickScratch = std::make_shared<TickScratch>();
  w.tickScratch->Reset(players.size());
  // One strike allowance per tick for every caller (game/lightning.h).
  w.strikes.budget.NewTick();
  WorldScratch& ws = w.tickScratch->ws;
  std::vector<PlayerScratch>& scratch = w.tickScratch->players;
  // A session with no window of its own gets its presentation sink now, once,
  // and keeps it: see SV_SEAM_REFS_PLAYER. The primary never allocates one.
  for (SessionTick& p : players)
    if (!p.s->localView && !p.s->sink)
      p.s->sink = std::make_unique<PresentationSink>();

  // CONVERSATIONS FIRST (game/dialogue.h): a choice carried by this tick's
  // command is applied, and a talking player's command is zeroed in place
  // before anything below reads it — so the controller, the hands and the op
  // record all see a player standing still.
  // ...but FIRST the demon bits of the command (demons D5, game/demon_talk.h):
  // the talk key, a presented contract, a dismissal and the conversation's
  // gaze, which the zeroing below would otherwise drop.
  demon::TalkPreTick(w, players, tick);
  if (w.talk)
    for (SessionTick& p : players)
      dialogue::TickSession(*w.talk, *p.s, p.ti, tick, &w.mobs);
  // SHOCKED (docs/PLAN_electricity.md E4; MobSystem::ApplyShocks): a player
  // whose body is stunned has its command zeroed the same way, before
  // anything reads it -- no walking, no strike, no button -- and keeps only
  // the look (the camera is the player's, not the body's). The stun is the
  // creature's (Mob::Stunned), so an NPC and a player are held by one clock.
  //
  // WHAT A STUN HOLDS (wave 2, package E). Every physical verb the command
  // carries: move, every button (strike, cast, throw, use, drop...), the
  // strike, and the use target. `hotbar` / `tool` are only the SELECTION the
  // command reports (the frame layer owns it; nothing in the tick acts on
  // them), so they are kept as the record of what was in hand; the frame
  // layer's own physical kit verbs (Q / E equip, G take / grab) refuse while
  // stunned (main.cpp). TALK is kept on purpose: a conversation choice is a
  // menu pick, applied above before this runs, and a stun of a second or two
  // must not leave a dialogue panel that silently ignores its keys.
  for (SessionTick& p : players) {
    const bool stunned = p.s->avatar.Stunned(tick);
    if (stunned) {
      p.ti.forward = 0.0f;
      p.ti.strafe = 0.0f;
      p.ti.held = 0;
      p.ti.pressed = 0;
      p.ti.strikeStyle = -1;
      p.ti.useRef = 0;
    }
    // THE HUD CUE (ui/overlay.cpp DrawShockCue): how long the stun has left,
    // and how many ticks ago the body was last shocked. Written through the
    // same seam binding the player phases use: the window's UIState for the
    // session that owns it, the throw-away sink for one that does not.
    UIState& sui = p.s->sink ? p.s->sink->ui : w.ui;
    const Mob::ShockRecord& sr = p.s->avatar.Shock();
    sui.stunTicksLeft = (int)p.s->avatar.StunTicksLeft(tick);
    sui.shockTicksAgo = sr.ticks > 0 && tick >= sr.lastTick ? (int)(tick - sr.lastTick) : -1;
  }
  for (SessionTick& p : players) ResolveHands(w, p);
  for (size_t i = 0; i < players.size(); i++)
    PhaseA(w, ws, players[i], scratch[i], tick, out);
  PhaseB(w, ws, players, scratch, tick, out);
  // ---- GAME LOGIC: the rest of the tick body up to the submit ---------
  // Brush, laser, melee, spells, mob/avatar/debris PreTick, explosions.
  // Closed at `t0` in phase L, where the submit sequence starts. Opened HERE
  // — where the old body declared it, between the stream block and the brush
  // — and opened ONCE: it is the world's row on the Performance tab, not one
  // player's, and N sessions opening N spans would bill the row N times.
  // SANDVOX_TICK_PROF=<ms>: a tick slower than that prints where it went, by
  // phase (C..O; L holds the submit, N the physics step and post-step). The
  // `gameLogic` row is C..K plus the start of L; this names which.
  struct TickProf {
    double ms = 0.0, t0 = 0.0, last = 0.0;
    char buf[640];
    int n = 0;
    void Mark(const char* nm) {
      if (ms <= 0.0) return;
      const double t = NowSeconds();
      if (n < 600) n += std::snprintf(buf + n, sizeof buf - (size_t)n, " %s %.2f", nm,
                                      (t - last) * 1000.0);
      last = t;
    }
  } tprof;
  {
    static const double kTickProfMs = [] {
      const char* e = std::getenv("SANDVOX_TICK_PROF");
      return e ? std::atof(e) : 0.0;
    }();
    tprof.ms = kTickProfMs;
    tprof.t0 = tprof.last = NowSeconds();
    tprof.buf[0] = 0;
  }
  ws.spanGame.emplace(sandvox::PerfScope::GameLogic);
  for (size_t i = 0; i < players.size(); i++)
    PhaseC(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("C");
  PhaseD(w, ws, players, scratch, tick, out);
  tprof.Mark("D");
  for (size_t i = 0; i < players.size(); i++)
    PhaseE(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("E");
  PhaseF(w, ws, players, scratch, tick, out);
  tprof.Mark("F");
  // THE SCOOP LEDGER, read ONCE for every session (container.h
  // ContainerScoopLedger): the landing claims of all of them are counted
  // first, so the pot each PhaseG settle draws on is the whole world's and
  // the last claim to land -- in session order -- takes the remainder.
  {
    const WorldSnapshot& lsnap = w.world.Snap();
    if (lsnap.valid) {
      int landing = 0;
      for (SessionTick& p : players)
        for (const ContainerScoopMemo::Claim& c : p.s->scoopMemo.claims)
          if (c.tick == lsnap.tick) landing++;
      ContainerLedgerObserve(w.scoopLedger, lsnap.tick, lsnap.scoopEighths,
                             landing);
      ContainerSoluteObserve(w.soluteLedger, lsnap.tick, lsnap.solScoopedBy);
    }
  }
  for (size_t i = 0; i < players.size(); i++)
    PhaseG(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("G");
  // THE MAP'S REFERENCES (world/refs_game.h): activation against the window
  // and the use verb. Before phase H so a villager a ref spawns is stepped by
  // this tick's mobs.PreTick. Null in every harness but the refs-* gates.
  if (w.refs != nullptr) refs::TickRefs(w, players, tick, out);
  tprof.Mark("refs");
  // THE DEMONS (game/demon.h): due arrivals spawn, contained demons' circles
  // are re-read, the fence goes up or comes down. Before phase H for the
  // refs' reason: a demon that arrives is stepped by this tick's PreTick.
  DemonTick(w, players, tick);
  // ...and the CONTRACTS (demons D5, game/demon_talk.h): the conversation's
  // demon actions, expiry, duties written onto the brains PhaseH thinks with.
  demon::ContractTick(w, players, tick, out);
  PhaseH(w, ws, players, scratch, tick, out);
  tprof.Mark("H");
  for (size_t i = 0; i < players.size(); i++)
    PhaseI(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("I");
  // THE PEERS' AVATARS, IN PHASE I's SLOT, AFTER THE LOCAL SESSIONS'
  // (M9.2 package B). Here rather than inside PhaseI because a ghost is not a
  // session — it has no TickInput, no camera and no presentation seam — but
  // its AVATAR is an avatar, and an avatar's rig has to be driven before
  // debris.PreTick (phase J) consumes the terrain anchors and before the
  // physics step. The ops it authors are thrown away; game/remoteplayer.h's
  // header says why, and the short version is that its owner is authoring
  // the same ones on its own machine.
  if (w.remotes != nullptr && !w.remotes->list.empty())
    RemotePlayersPreTick(*w.remotes, tick, kTickDt, w.world, w.phys, w.mobs,
                         w.debris, w.mats, players[0].s->avatarDefName);
  PhaseJ(w, ws, players, scratch, tick, out);
  tprof.Mark("J");
  for (size_t i = 0; i < players.size(); i++)
    PhaseK(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("K");
  PhaseL(w, ws, players, scratch, tick, out);
  tprof.Mark("L");
  for (size_t i = 0; i < players.size(); i++)
    PhaseM(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("M");
  PhaseN(w, ws, players, scratch, tick, out);
  tprof.Mark("N");
  for (size_t i = 0; i < players.size(); i++)
    PhaseO(w, ws, players[i], scratch[i], tick, out);
  tprof.Mark("O");
  // The ghosts' post-solver settle, in phase O's slot. No ragdoll follow and
  // no push-out: those write a position, and a ghost's position is the
  // wire's (game/remoteplayer.h, RemotePlayersPostStep).
  if (w.remotes != nullptr && !w.remotes->list.empty())
    RemotePlayersPostStep(*w.remotes);
  PhaseP(w, ws, players, scratch, tick, out);
  if (tprof.ms > 0.0 && (NowSeconds() - tprof.t0) * 1000.0 >= tprof.ms)
    std::printf("[tickprof] tick %u: %.2f ms =%s\n", tick,
                (NowSeconds() - tprof.t0) * 1000.0, tprof.buf);

  // The span is billed by phase L's Close(); releasing it here keeps the
  // optional's lifetime inside the tick that owns it.
  ws.spanGame.reset();
}
