#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "sim/materials.h"
#include "sim/world.h"

// The spell system: a caster VM whose ONLY output is op-stream emissions
// (DESIGN.md §8). Four properties are structural — everything else here is
// negotiable and this slice expects to be rewritten around them.
//
// 1. A SPELL IS A PROGRAM WHOSE ONLY OUTPUT IS OP-STREAM EMISSIONS. Every
//    world change a spell makes leaves as a BrushOp / ExplosionOp / CellOp /
//    ParticleSpawn on the MutationQueue (CLAUDE.md rule 3). There is no path
//    from spell code into a voxel buffer, and there must never be one — that
//    is what gives spells save/replay/networking for free.
//
// 2. THE EFFECT PAYLOAD IS POSITION-PARAMETERIZED, SO BACKFIRE IS FREE.
//    ApplySpellEffect() takes the position and direction as arguments, so
//    "cast it at the muzzle" and "cast it at the caster's own body" are the
//    SAME call with different arguments. Backfire is never per-spell special
//    -case code. See CastSpell().
//
// 3. THE VM IS INTEGER, IN FIXED POINT. Projectile position/velocity are 24.8
//    fixed-point voxels — the exact convention ParticleSpawn already uses
//    (world.h) — and mana/health/timers are plain integers.
//
//    IMPORTANT, so nobody "simplifies" this back to float: spell state is
//    CPU-side gameplay state OUTSIDE the hashed grid domain, exactly like mobs
//    and debris. So this is NOT required by CLAUDE.md rule 1 — the world hash
//    cannot see it either way. It is future-proofing for lockstep MP
//    (DESIGN.md §10) and replay debugging, where the projectile's path has to
//    reproduce bit-exactly on every machine. Retrofitting fixed point after the
//    glyph set grows is a rewrite; paying the small awkwardness now is not.
//    Floats appear ONLY at the rendering boundary (lerp to float at draw time)
//    and for Jolt queries.
//
// 4. THE VM IS NOT PLAYER-COUPLED. Compiling and casting is a free function
//    over (glyph list, caster state, origin, direction) -> emitted ops. A mob
//    must be able to cast through this same code path later, so nothing here
//    may include or reach into Player / PlayerAvatar.
//
// And rule 2 (cost scales with activity) applies to magic with no exception:
// every sustained effect declares a FINITE budget — voxels affected, ticks
// alive, total ops — never an open-ended duration. This codebase has hit the
// "permanent condition keeps chunks awake forever" trap three times already
// (light-gated rules, staining, viscous liquids); a trail spell must not be
// the fourth, which is why SpellProjectile carries a voxel budget that only
// ever decreases and kills the projectile when it hits zero.

// ---- fixed point -----------------------------------------------------------
// 24.8 voxels, matching ParticleSpawn (world.h). One unit = 1/256 voxel.
constexpr int32_t kSpellFxShift = 8;
constexpr int32_t kSpellFxOne = 1 << kSpellFxShift;

inline int32_t SpellFxFromFloat(float v) {
  return (int32_t)(v * (float)kSpellFxOne);
}
inline float SpellFxToFloat(int32_t v) {
  return (float)v / (float)kSpellFxOne;
}
// Fixed -> integer voxel cell, floor semantics (correct for negative coords:
// an arithmetic shift right floors, whereas a divide truncates toward zero and
// would put everything in [-1,0) into cell 0).
inline int32_t SpellFxFloor(int32_t v) { return v >> kSpellFxShift; }

struct SpellFxVec {
  int32_t x = 0, y = 0, z = 0;
};

// ---- glyph content (assets/spells/glyphs.json) -----------------------------

enum class GlyphType : uint8_t {
  Element = 0,   // names a material
  Form,          // turns the stack into a live effect (projectile)
  Modifier,      // decorates the spell (trail, transmute_to)
};

enum class SpellForm : uint8_t {
  None = 0,
  Projectile,
};

enum class SpellModifier : uint8_t {
  None = 0,
  Trail,
  TransmuteTo,
};

// How a backfired spell of this element kills its caster. Thematic only — the
// mechanism is always the same call (ApplySpellEffect at the caster).
enum class BackfireKind : uint8_t {
  Generic = 0,
  Burn,       // fire/lava: explode
  Dissolve,   // acid: the caster's own voxels become the element
  Drown,
  Bury,
};

struct GlyphDef {
  std::string id;
  std::string desc;
  GlyphType type = GlyphType::Element;

  int32_t mana = 0;

  // element
  std::string materialName;      // authored name; resolved at load
  uint32_t material = 0;         // resolved 12-bit id (never hardcoded anywhere)
  BackfireKind backfire = BackfireKind::Generic;

  // form
  SpellForm form = SpellForm::None;
  int32_t speed = 48;            // voxels/tick, whole voxels (scaled to fx at cast)
  int32_t lifetimeTicks = 150;   // hard bound (rule 2)
  int32_t impactRadius = 3;

  // modifier
  SpellModifier modifier = SpellModifier::None;
  int32_t radius = 1;
  int32_t voxelBudget = 64;      // hard VOLUME budget for trail (rule 2)
  // Lay a mark every Nth voxel of travel. Named for what it does rather than
  // for ticks: the trail marks per whole voxel crossed, not per tick, because
  // the sweep is subdivided for anti-tunneling and a per-tick gate would make
  // trail spacing depend on projectile speed.
  int32_t everyTicks = 1;
};

// A conjoined glyph is nothing but a saved list of glyph ids that pushes the
// same result as speaking them in order (§G stub). No new VM opcode: Speak()
// expands it. This is the whole data-model cost of conjoining.
struct ConjoinedGlyph {
  std::string id;
  std::string desc;
  std::vector<int> glyphs;   // indices into GlyphLibrary::glyphs
};

// STUB (§G). Wards are not implemented; this fixes the shape only. Intended
// semantics, recorded here because it is the load-bearing design decision:
// a ward filters the incoming OP STREAM (cheap, CPU-side, sim untouched), NOT
// the CA. So a ward stops someone casting acid at your feet but does NOT stop
// acid already flowing toward you. That is deliberate — it keeps the
// falling-sand game underneath and makes "cast next to them and let physics do
// it" the counterplay.
struct WardDef {
  std::string id;
  std::string desc;
  std::string filter;        // glyph/modifier id this ward refuses
  // Reduces EFFECTIVE MAX MANA by a FRACTION, per-mille. Fractional rather
  // than flat on purpose: a flat drain means a big late-game pool buys
  // invulnerability, while a fraction keeps a ward a real choice at any scale.
  int32_t drainPerMille = 0;
};

// Engine-wide hard ceilings (rule 2). Authored in the "budgets" block and
// clamped against these at load.
struct SpellBudgets {
  int32_t maxLiveProjectiles = 32;
  int32_t maxGeneration = 3;
  int32_t maxTrailVoxels = 256;
  int32_t maxLifetimeTicks = 300;
};

struct GlyphLibrary {
  std::vector<GlyphDef> glyphs;
  std::vector<ConjoinedGlyph> conjoined;
  std::vector<WardDef> wards;
  SpellBudgets budgets;

  int Find(const std::string& id) const {
    for (size_t i = 0; i < glyphs.size(); i++)
      if (glyphs[i].id == id) return (int)i;
    return -1;
  }
};

// Loads assets/spells/glyphs.json and resolves every material NAME against the
// compiled material list. Returns false with `errors` filled on bad input —
// modders get diagnostics, not silent breakage (DESIGN.md §6). An unresolvable
// material name is an error rather than a fallback to id 0, because a spell
// that silently conjures air is far harder to diagnose than one that refuses
// to load.
bool LoadGlyphs(const std::string& path, const std::vector<MaterialDef>& mats,
                GlyphLibrary& out, std::string& errors);

// ---- the compiled spell ----------------------------------------------------

// What the VM made of the spoken glyph sequence. Deliberately a tiny plain
// struct: this is what a projectile carries, what backfire runs, and what the
// HUD reads.
struct Spell {
  uint32_t element = 0;                 // material id (0 = none spoken)
  BackfireKind backfire = BackfireKind::Generic;
  SpellForm form = SpellForm::None;
  std::vector<int> modifiers;           // glyph indices, in spoken order
  int32_t manaCost = 0;

  // Form parameters, folded down from the form glyph.
  int32_t speed = 48;
  int32_t lifetimeTicks = 150;
  int32_t impactRadius = 3;

  // Generation counter — the SUBCRITICALITY guarantee (rule 2). Nothing
  // triggers anything yet, but anything a trigger spawns later must carry
  // gen = parent.gen + 1 and be refused past budgets.maxGeneration. Wiring it
  // now costs one field; adding it after triggers exist means auditing every
  // spawn site.
  int32_t gen = 0;

  bool HasForm() const { return form != SpellForm::None; }
  bool HasElement() const { return element != 0; }
};

// What the VM thinks the spoken sequence is, for the HUD. A clear on-screen
// readout of this is worth more than validation — an illegal sequence must
// still be CASTABLE and misfire, because "the ancient language punishes
// imprecision" is the design thesis and a hard parse error would make
// experimentation frustrating.
struct SpellReadout {
  std::string text;         // "lava + trail + projectile"
  std::string verdict;      // "firebolt", "no form — will misfire", ...
  bool wellFormed = false;  // has an element AND a form
};

// The typed stack the player speaks onto. Glyphs are functions on this stack;
// the cast key applies "cast" to what is on top.
struct SpellStack {
  std::vector<int> spoken;   // glyph indices, in spoken order

  void Clear() { spoken.clear(); }
  bool Empty() const { return spoken.empty(); }
};

// Fold the spoken stack into a Spell. Never fails: an ill-formed sequence
// compiles to a Spell that will misfire, and the cost is still computable so
// the HUD can show it draining live BEFORE the cast.
Spell CompileSpell(const GlyphLibrary& lib, const SpellStack& stack);
SpellReadout DescribeSpell(const GlyphLibrary& lib, const SpellStack& stack,
                           const Spell& spell);

// ---- live projectiles ------------------------------------------------------

// One in-flight spell. All authoritative state is integer.
struct SpellProjectile {
  Spell spell;
  SpellFxVec pos{};      // 24.8 fixed voxels
  SpellFxVec vel{};      // 24.8 fixed voxels per tick
  int32_t ticksLeft = 0;
  int32_t trailBudget = 0;   // voxels the trail may still lay (rule 2)
  int32_t trailPhase = 0;
  // Last cell the trail marked. The sweep is subdivided for anti-tunneling, so
  // without this the trail would emit one op per SUB-STEP and burn its whole
  // budget on a handful of cells in a single tick.
  int32_t markedX = 0, markedY = 0, markedZ = 0;
  bool markedValid = false;
  bool alive = true;
  // Casters are identified by an opaque integer, so a mob can own a projectile
  // without the VM knowing what a mob is (thesis 4).
  uint64_t casterId = 0;
};

// ---- caster state ----------------------------------------------------------

// The result of resolving a cast against the caster's mana and health. This is
// the whole tension mechanic, so it is one small enum rather than scattered
// booleans.
enum class CastOutcome : uint8_t {
  Normal = 0,     // cost <= mana
  Unstable,       // mana < cost <= mana + health: cast, but IMPRECISE
  Fatal,          // cost > mana + health: the spell goes off AT the caster
  Nothing,        // nothing spoken
};

struct CastResult {
  CastOutcome outcome = CastOutcome::Nothing;
  int32_t manaSpent = 0;
  int32_t healthSpent = 0;
  // 0..1000 per-mille — how deep into health the cast went. Drives the
  // trajectory wobble, so the mana bar reads as a PRECISION meter rather than
  // a second HP bar.
  int32_t instability = 0;
};

// Integer mana pool with slow regen. Health is NOT stored here: the player's
// health effectively lives on PlayerAvatar's per-part hp + alive_, and mobs
// have their own, so the caster reads it through a callback rather than
// inventing a parallel number that would immediately drift.
struct CasterState {
  int32_t mana = 100;
  int32_t manaMax = 100;
  // Per-mille of manaMax reserved by active wards (§G stub). Fractional so a
  // large pool cannot buy invulnerability.
  int32_t wardDrainPerMille = 0;
  // Regen accumulator in per-mille of a mana point, so a slow rate is
  // expressible without floats.
  int32_t regenAccum = 0;
  int32_t regenPerMillePerTick = 220;

  // Effective max after ward drain — what the HUD draws and what mana refills
  // toward.
  int32_t EffectiveMax() const {
    int32_t drop = (int32_t)((int64_t)manaMax * wardDrainPerMille / 1000);
    int32_t m = manaMax - drop;
    return m < 0 ? 0 : m;
  }
  void Tick();
};

// Resolve a cast's cost against mana and the caster's CURRENT health, without
// applying anything. `health` is whatever the caller's health model reports
// (see CasterHealth below). Split out from casting so the HUD can show the
// crossover point live.
CastResult ResolveCast(const CasterState& caster, int32_t health,
                       int32_t manaCost);

// How a caster's health is read and spent. A callback rather than a field
// precisely so the player's health can stay on PlayerAvatar (per-part hp +
// alive_) and a mob's can stay on MobSystem, with no parallel number to drift.
struct CasterHealth {
  // Current health in the caller's own units, clamped >= 0.
  int32_t (*get)(void* ctx) = nullptr;
  // Spend `amount` of health. Never kills — a fatal cast is signalled through
  // the Fatal outcome and resolved by the effect payload running at the
  // caster, not by this call.
  void (*spend)(void* ctx, int32_t amount) = nullptr;
  void* ctx = nullptr;

  int32_t Get() const { return get ? get(ctx) : 0; }
  void Spend(int32_t amount) const {
    if (spend && amount > 0) spend(ctx, amount);
  }
};

// ---- emission --------------------------------------------------------------

// Everything a spell may emit, in one bundle. The VM appends here and NOWHERE
// else — this struct IS thesis 1. The caller splices these onto the per-tick
// MutationQueue streams, subject to the op reservation in SpellSystem.
struct SpellEmission {
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> explosions;
  std::vector<ParticleSpawn> spawns;
  // Set when the effect should carve the caster's own body (a Fatal cast).
  // The VM cannot do this itself without reaching into the avatar/mob systems
  // (thesis 4), so it reports the intent and the owner performs it.
  bool carveCaster = false;
  Vec3 carveAt{};
  float carveRadius = 0;
};

// THE POSITION-PARAMETERIZED EFFECT PAYLOAD (thesis 2).
//
// This is the single function that turns a spell into world changes. It takes
// the position and direction as arguments, which is exactly why backfire needs
// no special-case code anywhere: casting at the muzzle and casting into the
// caster's own chest are the same call.
//
//   ApplySpellEffect(spell, muzzlePos, aimDir, ...)   // normal impact
//   ApplySpellEffect(spell, casterPos, anyDir, ...)   // backfire
//
// `atFx` is 24.8 fixed voxels. `strength` scales the effect 0..1000 per-mille
// (a fatal backfire runs at full strength; an impact may run softer).
void ApplySpellEffect(const GlyphLibrary& lib, const Spell& spell,
                      SpellFxVec atFx, SpellFxVec dirFx, int32_t strengthMille,
                      SpellEmission& out);

// ---- the system ------------------------------------------------------------

// Owns the live projectiles and the per-tick op reservation. Not player-
// coupled: Cast() takes an origin and a direction, so a mob can drive it.
class SpellSystem {
 public:
  // OP BUDGET FAIRNESS (§F). kMaxOpsPerTick is 64 BrushOps and MobSystem
  // already reserves kBleedOpsPerTick = 6 for bleeding, with the avatar
  // reserving 6 more. Trails and transmutes would otherwise starve against
  // ambient bleeding and a spell would "sometimes not fire", which is
  // miserable to diagnose. So magic gets its own explicit reservation in the
  // same spirit, rather than silently sharing.
  static constexpr int kSpellOpsPerTick = 24;

  void SetLibrary(const GlyphLibrary* lib) { lib_ = lib; }
  const GlyphLibrary* Library() const { return lib_; }

  // Cast a compiled spell. `originFx`/`dirFx` are 24.8 fixed voxels; `dirFx`
  // need not be normalized. Returns the resolved outcome. Emissions (a fatal
  // backfire's explosion, an instant effect) land in `out`.
  //
  // NOT player-coupled (thesis 4): everything about the caster arrives through
  // CasterState + CasterHealth + casterId.
  CastResult Cast(const Spell& spell, CasterState& caster,
                  const CasterHealth& health, uint64_t casterId,
                  SpellFxVec originFx, SpellFxVec dirFx, uint32_t tick,
                  SpellEmission& out);

  // Advance every live projectile one tick. `kindAt` is the one-tick-latent
  // voxel mirror probe (the same callback the player and grenade use).
  void Tick(uint32_t tick, const World& world,
            const std::vector<uint32_t>& classOf, SpellEmission& out);

  void Clear() { live_.clear(); }
  const std::vector<SpellProjectile>& Live() const { return live_; }
  int LiveCount() const { return (int)live_.size(); }

  // Diagnostics for the HUD/selftest: ops dropped this tick because the
  // reservation was full. A spell that silently does nothing is the exact
  // failure §F exists to prevent, so it is counted rather than ignored.
  int OpsDroppedLastTick() const { return opsDropped_; }

 private:
  const GlyphLibrary* lib_ = nullptr;
  std::vector<SpellProjectile> live_;
  int opsDropped_ = 0;
};

// Trajectory wobble from an unstable cast. Single small function, deliberately
// easy to tune: casting into health makes the spell IMPRECISE, not merely
// costly, which is what makes the mana bar a precision meter instead of a
// second HP bar. Integer + counter-based hash, so it reproduces in a replay.
SpellFxVec SpellWobble(SpellFxVec dirFx, int32_t instabilityMille,
                       uint32_t tick, uint64_t casterId);
