#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.h"
#include "sim/materials.h"
#include "sim/world.h"
#include "sim/windprim.h"

// The spell system: a caster VM whose ONLY output is op-stream emissions
// (DESIGN.md §8, docs/PLAN_magic_grammar.md). Four properties are structural —
// everything else here is negotiable.
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
//    -case code. See SpellSystem::Cast().
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
//    reproduce bit-exactly on every machine. Floats appear ONLY at the
//    rendering boundary (lerp to float at draw time) and for Jolt queries.
//
// 4. THE VM IS NOT PLAYER-COUPLED. Compiling and casting is a free function
//    over (glyph list, caster state, origin, direction) -> emitted ops. A mob
//    casts through this same code path, so nothing here may include or reach
//    into Player / PlayerAvatar.
//
// And rule 2 (cost scales with activity) applies to magic with no exception:
// every sustained effect declares a FINITE budget — voxels affected, ticks
// alive, instances, generation — never an open-ended duration. Every lowered
// cast carries those four numbers (SpellCast::ticks/voxels/instances/
// generation) and law L8 in the `spells` gate asserts they are finite.
//
// ---- THE GRAMMAR (docs/PLAN_magic_grammar.md §1–§3) -------------------------
//
// Five SORTS: Matter (a material by name), Effect (something that happens at a
// point), Delivery (how an Effect reaches a point), Mod (a field edit on the
// delivery record), Operator (a word with argument slots that produces one of
// the others). Six PARSE RULES:
//
//   R1 runs merge            fire fire            -> fire×2
//   R2 operators bind        dirt transmute water -> (dirt ⋈ water), greedily,
//                            on their declared side, left to right
//   R3 empty slot = INCOMPLETE  transmute water   -> (_ ⋈ water): charged,
//                            does nothing. No defaults; `anything` and `air`
//                            are words of their own
//   R4 a Delivery closes the clause   explosive projectile fire bomb -> two casts
//   R5 no Delivery -> hand   the payload resolves at reach in front of the caster
//   R6 bag order is irrelevant   explosive shotgun projectile == shotgun
//                            explosive projectile
//
// scripts/magic_grammar.py is the executable reference for these rules; the
// `spells-oracle` gate compares this parser against every pair it generates.
// Nothing in this file knows which words exist: sorts, valence, verbs, axes
// and costs are read from assets/spells/glyphs.json.

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

enum class GlyphSort : uint8_t {
  Matter = 0,
  Effect,
  Delivery,
  Mod,
  Operator,
};
constexpr int kGlyphSortCount = 5;
const char* GlyphSortName(GlyphSort s);   // "matter" | "effect" | ...
bool ParseGlyphSort(const std::string& s, GlyphSort& out);

// A slot's acceptance set, as a bitmask over sorts. `any` accepts everything,
// including a raw operator word (`transmute null` takes `transmute` itself).
constexpr uint8_t kSortBitMatter = 1u << (int)GlyphSort::Matter;
constexpr uint8_t kSortBitEffect = 1u << (int)GlyphSort::Effect;
constexpr uint8_t kSortBitDelivery = 1u << (int)GlyphSort::Delivery;
constexpr uint8_t kSortBitMod = 1u << (int)GlyphSort::Mod;
constexpr uint8_t kSortBitOperator = 1u << (int)GlyphSort::Operator;
constexpr uint8_t kSortAny = 0x1F;

// The primitives an Effect can lower to (plan §5). Hundreds of glyphs, but the
// C++ knows only these verbs — each maps to ONE op type on the MutationQueue or
// to one existing engine seam. A glyph is {sort, verb, args}; new glyphs are
// JSON.
enum class SpellVerb : uint8_t {
  None = 0,
  Spray,     // ParticleSpawn: loose voxels of M flung along the aim
  Place,     // BrushOp mode 0 (into air)
  Convert,   // BrushOp mode 1 with a from-filter; A->air is disintegrate
  Explode,   // ExplosionOp
  Wind,      // WindPrim
  Mend,      // world: convert(cell->air) at the source; body: a restore request
  Trail,     // (operator) an Effect run at each marked voxel of a flight path
  Sustain,   // (operator) attach the inner Effect/Mod to a body as a status
  Filter,    // (operator) an entry in the caster's op filter
  Repeat,    // (operator) the inner Effect again every few ticks, bounded
};
const char* SpellVerbName(SpellVerb v);
bool ParseSpellVerb(const std::string& s, SpellVerb& out);

// Deliveries are three MECHANISMS, parameterised (plan §5).
enum class DeliveryMech : uint8_t {
  Instant = 0,   // hand (reach in front), self (caster / chosen part)
  Flight,        // projectile, bolt, lob, orb, bomb
  Continuous,    // beam: held, resolves at the ray hit every tick
};

// A Mod is a field edit on the delivery record. The FIELD vocabulary is fixed
// (it names record fields); which glyph edits which field, by how much, is
// content.
enum class ModField : uint8_t {
  None = 0,
  Count,      // instances (shotgun)
  Children,   // children on resolve (split), generation-capped
  Gravity,    // per-mille of g on the flight, or on the anchored body
  Speed,
  Lifetime,   // ticks; a bomb's fuse too
  Radius,     // resolve radius (wide)
  Bounces,
  Pierce,
  Seek,
  Fuse,       // ticks after impact before resolving
};
enum class ModOp : uint8_t { Mul = 0, Div, Add };

// How repetition acts. Matter and Effects ADD (×N of the axis); Mods COMPOSE
// (applied again: shotgun 3/9/27). Plan §4.
enum class RepeatKind : uint8_t { Add = 0, Compose };

// The wind primitive a glyph authors, read from the glyph's "wind" block.
// CONTENT, not a knob: a modder writes a hurricane by editing glyphs.json, and
// no material id or engine constant is hardcoded on the way (DESIGN.md §6).
struct GlyphWind {
  bool has = false;
  uint32_t kind = kWindPrimCone;   // "cone" | "burst" | "vortex"
  float speedMs = 20.0f;           // core speed, m/s (WindPrimAim converts)
  int32_t radius = 6;              // world cells across
  int32_t reach = 32;              // world cells along the axis
  int32_t ttlTicks = 45;           // hard lifetime bound (rule 2)
  float swirl = 0.0f;              // vortex tangential share of `speed`
  float rise = 0.0f;               // vortex axial share of `speed`
  // Whether this gust may pull SETTLED powder loose inside its footprint.
  bool entrain = false;
};

struct GlyphDef {
  std::string id;
  std::string desc;
  std::string example;
  std::string axis;              // what repeating scales (info box, plan §9)
  GlyphSort sort = GlyphSort::Matter;
  int32_t word = 0;              // the small fixed word cost
  RepeatKind repeat = RepeatKind::Add;

  // ---- matter ----
  std::string materialName;      // authored name; resolved at load
  uint32_t material = 0;         // resolved 12-bit id (0 = air, the void word)
  bool wildcard = false;         // `anything`: matches what is actually there

  // ---- effect / operator ----
  SpellVerb verb = SpellVerb::None;
  // Operator valence: which side(s) it binds and what sort each accepts. Every
  // unary operator takes the word BEFORE it; only an infix operator has both.
  bool hasLeft = false, hasRight = false;
  uint8_t leftMask = 0, rightMask = 0;
  GlyphSort result = GlyphSort::Effect;
  // Effect parameters. Which ones matter depends on the verb.
  int32_t radius = 1;            // convert/place/filter radius, sustain attach radius
  int32_t power = 220;           // explode: hardness budget at the centre
  int32_t voxelBudget = 64;      // trail: hard VOLUME budget (rule 2)
  int32_t everyTicks = 1;        // trail: mark every Nth voxel; repeat: period
  int32_t ticks = 0;             // sustain / repeat / beam lifetime bound
  int32_t repeats = 0;           // repeat: how many times
  int32_t perTick = 1;           // mend: voxels grafted per tick
  GlyphWind wind;

  // ---- delivery record defaults ----
  DeliveryMech mech = DeliveryMech::Instant;
  int32_t carryMille = 1000;     // premium on the payload tariff
  int32_t speed = 48;            // voxels/tick, whole voxels
  int32_t lifetimeTicks = 150;   // hard bound (rule 2)
  int32_t impactRadius = 3;      // kinetic impact of an empty payload
  int32_t gravityMille = 0;      // per-mille of g
  int32_t fuseTicks = 0;         // 0 = resolve on impact
  int32_t reach = 3;             // instant: voxels in front of the caster
  bool body = false;             // flight as a rigid body (bomb)
  bool resolveOnExpiry = false;  // orb: life running out is a resolve, not a fizzle

  // ---- mod ----
  ModField field = ModField::None;
  ModOp op = ModOp::Mul;
  int32_t amount = 1;
};

// A conjoined glyph / grimoire starter page: a saved list of glyph names that
// speaks as if you had spoken them in order (plan §12b). Indices here are
// resolved at load; the authoring surface is names.
struct ConjoinedGlyph {
  std::string id;
  std::string desc;
  std::vector<int> glyphs;   // indices into GlyphLibrary::glyphs
};

// Engine-wide hard ceilings (rule 2). Authored in the "budgets" block and
// clamped against these at load.
struct SpellBudgets {
  int32_t maxLiveProjectiles = 32;
  int32_t maxGeneration = 3;
  int32_t maxTrailVoxels = 256;
  int32_t maxLifetimeTicks = 300;
  // R1's cap: past it an extra utterance is free and does nothing.
  int32_t maxMultiplicity = 6;
  // shotgun's cap: instances per cast (3^N grows fast).
  int32_t maxInstances = 27;
  // ---- spray, the coercion of free Matter ---------------------------------
  int32_t sprayVoxels = 3;
  int32_t maxSprayVoxels = 96;
  int32_t spraySpeed = 14;      // voxels/tick, whole voxels
  int32_t sprayConeMille = 130; // lateral scatter, per-mille of forward speed
  // ---- the tariff (plan §4), P1 ---------------------------------------------
  int32_t ratePlace = 1;        // per voxel × arcane(M)
  int32_t rateConvert = 1;      // per voxel, plus max(0, arcane(B) - arcane(A))
  int32_t rateGraft = 2;        // per voxel × arcane(M)
  int32_t rateExplode = 1;      // × power × r³ / 1000
  int32_t rateWind = 1;         // × footprint × ticks / 1000
  int32_t anythingSurchargeMille = 500;   // the wildcard, on top of the conversion
  // ---- sustained (plan §7), P3 ----------------------------------------------
  int32_t maxStatusTicks = 900;
  int32_t maxStatusPerCaster = 4;
  // ---- the grimoire (plan §12b), P5 -----------------------------------------
  int32_t maxGrimoirePages = 32;
  int32_t maxMacroWords = 16;
  int32_t maxMacroDepth = 4;
};

struct GlyphLibrary {
  std::vector<GlyphDef> glyphs;
  std::vector<ConjoinedGlyph> conjoined;
  SpellBudgets budgets;
  // Per-material arcane value (materials.json), copied at load so pricing
  // needs only the library. Index == 12-bit material id.
  std::vector<int32_t> arcane;
  int32_t Arcane(uint32_t mat) const {
    return mat < arcane.size() ? arcane[mat] : 0;
  }
  // The implicit delivery (R5). Not in `glyphs`, so it cannot be spoken; its
  // record fields come from the "hand" block.
  GlyphDef hand;

  int Find(const std::string& id) const {
    for (size_t i = 0; i < glyphs.size(); i++)
      if (glyphs[i].id == id) return (int)i;
    return -1;
  }
  const GlyphDef* At(int i) const {
    return (i >= 0 && i < (int)glyphs.size()) ? &glyphs[i] : nullptr;
  }
  // The delivery glyph for a record: `hand` for -1.
  const GlyphDef& Delivery(int i) const {
    const GlyphDef* g = At(i);
    return g ? *g : hand;
  }
};

// Loads assets/spells/glyphs.json and resolves every material NAME against the
// compiled material list. Returns false with `errors` filled on bad input —
// modders get diagnostics, not silent breakage (DESIGN.md §6).
bool LoadGlyphs(const std::string& path, const std::vector<MaterialDef>& mats,
                GlyphLibrary& out, std::string& errors);

// ---- the spoken stack -------------------------------------------------------

// The typed stack the player speaks onto. Glyphs are functions on this stack;
// the cast key applies "cast" to what is on top.
struct SpellStack {
  std::vector<int> spoken;   // glyph indices, in spoken order

  void Clear() { spoken.clear(); }
  bool Empty() const { return spoken.empty(); }
};
// Bound so a stuck key cannot grow the stack without limit (rule 2 applies to
// UI state too — an unbounded stack is an unbounded mana cost).
constexpr int kSpellStackMax = 16;

// ---- the parse tree (plan §2, §6) --------------------------------------------

// One item of the tree: a raw word with multiplicity, or an operator group
// with its bound children. The HUD draws the brackets straight off this, so
// what you see is what bound.
struct SpellNode {
  int glyph = -1;        // library index
  int32_t n = 1;         // multiplicity (R1 / R6), capped
  bool group = false;    // an operator application
  int left = -1;         // child node, -1 = empty slot (or no slot)
  int right = -1;
  bool complete = true;  // false: a required slot is empty (R3)
  // Spoken span [first, last] of this item and everything under it, for the
  // HUD highlight and the L6 law; `at` is the operator word's own position.
  int first = -1, last = -1;
  int at = -1;
};

struct SpellClause {
  int delivery = -1;     // glyph index of the Delivery, -1 = hand
  int32_t weight = 1;    // Delivery multiplicity
  std::vector<int> bag;  // node indices after R6 merge, first-seen order
};

struct SpellTree {
  std::vector<SpellNode> nodes;
  std::vector<SpellClause> clauses;
  bool Empty() const { return clauses.empty(); }
};

// R1 → R2/R3 → R4/R6. Total: any sequence of valid glyph indices parses.
SpellTree ParseSpell(const GlyphLibrary& lib, const SpellStack& stack);

// The sort an item denotes: a raw word's sort, or an operator's result sort.
GlyphSort NodeSort(const GlyphLibrary& lib, const SpellTree& t, int node);
// The R6 identity of an item, multiplicity excluded: `fire`,
// `(dirt|transmute|water)`, `(|trail|)`. What the reference script calls key().
std::string NodeKey(const GlyphLibrary& lib, const SpellTree& t, int node);

enum class BracketStyle : uint8_t {
  Oracle = 0,   // the reference script's exact spelling (× ‖ ⋈ ◂ and **bold**)
  Hud,          // ASCII for the pixel font: x2, |, ><, <, DELIVERY in caps
};
// An item with its brackets: `fire×2`, `(dirt ⋈ water)`, `(_ ◂trail)`.
std::string ShowNode(const GlyphLibrary& lib, const SpellTree& t, int node,
                     BracketStyle style = BracketStyle::Oracle);
// The whole sentence: bag items, then the delivery, clauses joined by ‖.
std::string BracketSpell(const GlyphLibrary& lib, const SpellTree& t,
                         BracketStyle style = BracketStyle::Oracle);

// ---- the lowered cast (plan §5, §6) -------------------------------------------

// One thing that happens at a point. ApplySpellEffect switches on `verb` —
// the only switch in the system.
struct EffectInst {
  SpellVerb verb = SpellVerb::None;
  int glyph = -1;            // the glyph that authored it (parameters)
  int32_t n = 1;             // multiplicity: the verb's declared scale axis ×n
  bool complete = true;      // false: incomplete operator, charged, does nothing
  // Materials. spray/place/mend use matA; convert is matA -> matB.
  uint32_t matA = 0, matB = 0;
  bool anyA = false, anyB = false;   // the wildcard on either side
  int glyphA = -1, glyphB = -1;      // the Matter glyphs those came from (names)
  int32_t radius = 1;        // resolve radius, after `wide`
  // The wrapped effect(s) for trail / sustain / repeat.
  std::vector<EffectInst> inner;
  // A sustained MOD (float aura ...): the field edit the status applies.
  ModField modField = ModField::None;
  ModOp modOp = ModOp::Mul;
  int32_t modAmount = 0;
  int32_t modN = 1;
  int node = -1;             // tree node, for the readout
};

// The delivery record a live cast carries. Mods are field edits on it.
struct DeliveryRec {
  int glyph = -1;            // -1 = hand
  DeliveryMech mech = DeliveryMech::Instant;
  int32_t weight = 1;        // Delivery×N: speed, lifetime, kinetic impact ×N
  int32_t carryMille = 1000;
  int32_t speed = 0;         // voxels/tick
  int32_t lifetimeTicks = 1;
  int32_t impactRadius = 1;
  int32_t gravityMille = 0;  // per-mille of g (flight), or on the anchored body
  int32_t fuseTicks = 0;
  int32_t reach = 0;
  int32_t count = 1;         // instances (shotgun)
  int32_t children = 0;      // split
  int32_t bounces = 0, pierce = 0, seek = 0;
  int32_t radiusMille = 1000;   // wide: resolve radius multiplier
  bool body = false;         // rigid body flight (bomb)
  bool resolveOnExpiry = false;
  // Trail mods: effects run at each marked voxel of the flight path, under a
  // hard voxel budget that only decreases (rule 2).
  std::vector<EffectInst> trail;
  int32_t trailBudget = 0;
  int32_t trailEvery = 1;    // mark every Nth voxel of travel
};

// One clause, lowered: what Cast() runs, what a projectile carries, what
// backfire runs.
struct SpellCast {
  DeliveryRec delivery;
  std::vector<EffectInst> payload;
  int32_t instances = 1;     // == delivery.count, capped
  // Price, split the way the HUD shows it (plan §9).
  int32_t wordCost = 0;
  int32_t tariff = 0;        // payload tariff × instances (P1)
  int32_t carryCost = 0;     // the delivery premium on that tariff (P1)
  bool priceUnknown = false; // `anything`: billed on resolve
  // Rule 2 budgets, all finite (law L8).
  int32_t ticks = 0;
  int32_t voxels = 0;
  int32_t generation = 0;
  int clause = -1;

  int32_t Cost() const { return wordCost + tariff + carryCost; }
};

struct CastList {
  SpellTree tree;
  std::vector<SpellCast> casts;
  int32_t wordCost = 0, tariff = 0, carryCost = 0;
  int32_t manaCost = 0;      // the total the HUD shows and ResolveCast charges
  bool priceUnknown = false;
  int32_t gen = 0;           // generation of the caster (split children: +1)
  bool Empty() const { return casts.empty(); }
};

// Parse + lower + price. Never fails: every sequence lowers to a definite cast
// list. Silence lowers to an empty one.
CastList CompileSpell(const GlyphLibrary& lib, const SpellStack& stack);
CastList LowerSpell(const GlyphLibrary& lib, const SpellTree& tree);

// The predicted world footprint of one effect at one point, in voxels: what
// the trail budget charges per mark and what the tariff prices.
int32_t EffectVolume(const GlyphLibrary& lib, const EffectInst& e);
// THE TARIFF (plan §4): what one effect does to the world, priced per voxel by
// the material's arcane value and the budgets' rates. `anything` prices as 0
// here and is billed when it resolves (SpellEmission::billOnResolve).
int32_t EffectTariff(const GlyphLibrary& lib, const EffectInst& e);
// Fills a cast's tariff and carry from its payload, trail and instances.
void PriceCast(const GlyphLibrary& lib, SpellCast& cast);
// The first material a cast carries (a spray, a convert's product, a trail
// mark), for drawing the bolt; 0 when it carries none.
uint32_t CastTintMaterial(const SpellCast& cast);

// What the VM thinks the spoken sequence is, for the HUD.
struct SpellReadout {
  std::string text;         // the bracket readout (HUD style)
  std::string verdict;      // what it will do, in words
  bool wellFormed = false;  // anything spoken at all
};
SpellReadout DescribeSpell(const GlyphLibrary& lib, const CastList& list);
// What one clause does, in words (plan §8 describe()).
std::string DescribeCast(const GlyphLibrary& lib, const SpellCast& cast);

// ---- live projectiles ------------------------------------------------------

// One in-flight instance. All authoritative state is integer.
struct SpellProjectile {
  SpellCast cast;
  SpellFxVec pos{};      // 24.8 fixed voxels
  SpellFxVec vel{};      // 24.8 fixed voxels per tick
  int32_t ticksLeft = 0;
  int32_t trailBudget = 0;   // voxels the trail may still lay (rule 2)
  int32_t trailPhase = 0;
  // Last cell the trail marked. The sweep is subdivided for anti-tunneling, so
  // without this the trail would emit one op per SUB-STEP.
  int32_t markedX = 0, markedY = 0, markedZ = 0;
  bool markedValid = false;
  int32_t bouncesLeft = 0, pierceLeft = 0;
  int32_t fuseLeft = -1;     // >= 0: resting, counting down to resolve
  bool resting = false;
  bool alive = true;
  int32_t gen = 0;
  // The cast's instability, carried to the impact: an unstable convert lands
  // a melt-mode share wherever it resolves, not only at the hand.
  int32_t instability = 0;
  // Casters are identified by an opaque integer, so a mob can own a projectile
  // without the VM knowing what a mob is (thesis 4).
  uint64_t casterId = 0;
  uint64_t body = 0;         // rigid-body handle for a `bomb` (P2), 0 = none
};

// ---- caster state ----------------------------------------------------------

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
  // trajectory wobble and the melt share of an unstable convert.
  int32_t instability = 0;
};

// Integer mana pool with slow regen. Health is NOT stored here: the player's
// health effectively lives on PlayerAvatar's per-part hp + alive_, and mobs
// have their own, so the caster reads it through a callback.
struct CasterState {
  int32_t mana = 100;
  int32_t manaMax = 100;
  // Reserved out of the max by live sustained statuses (plan §7): the per-tick
  // tariff × the regen horizon. What the HUD draws as the shortened bar.
  int32_t reserved = 0;
  // Regen accumulator in per-mille of a mana point.
  int32_t regenAccum = 0;
  int32_t regenPerMillePerTick = 220;

  int32_t EffectiveMax() const {
    int32_t m = manaMax - reserved;
    return m < 0 ? 0 : m;
  }
  void Tick();
};

// Resolve a cast's cost against mana and the caster's CURRENT health, without
// applying anything.
CastResult ResolveCast(const CasterState& caster, int32_t health,
                       int32_t manaCost);

// How a caster's health is read and spent. A callback rather than a field so
// the player's health can stay on PlayerAvatar and a mob's on MobSystem.
struct CasterHealth {
  int32_t (*get)(void* ctx) = nullptr;
  void (*spend)(void* ctx, int32_t amount) = nullptr;
  void* ctx = nullptr;

  int32_t Get() const { return get ? get(ctx) : 0; }
  void Spend(int32_t amount) const {
    if (spend && amount > 0) spend(ctx, amount);
  }
};

// ---- emission --------------------------------------------------------------

// Everything a spell may emit, in one bundle. The VM appends here and NOWHERE
// else — this struct IS thesis 1.
struct SpellEmission {
  std::vector<BrushOp> ops;
  std::vector<ExplosionOp> explosions;
  std::vector<ParticleSpawn> spawns;
  std::vector<WindPrim> winds;
  // Set when the effect should carve the caster's own body (a Fatal cast).
  bool carveCaster = false;
  Vec3 carveAt{};
  float carveRadius = 0;
  // `anything` resolved: what the wildcard turned out to be worth, to bill the
  // caster now (plan §4). Summed by the owner into the caster's pool.
  int32_t billOnResolve = 0;
};

// A probe into the world the VM may consult while resolving (the CPU mirror,
// one tick latent). Nullable: without it `anything` sees air.
struct SpellProbe {
  // Material at a world cell; `known` false when the cell is not mirrored.
  uint32_t (*matAt)(void* ctx, int32_t x, int32_t y, int32_t z, bool& known) = nullptr;
  void* ctx = nullptr;
};
// A probe over the World's async snapshot mirror (World::Snap()).
SpellProbe WorldSpellProbe(const World& world);

// THE POSITION-PARAMETERIZED EFFECT PAYLOAD (thesis 2). Runs every EffectInst
// of `payload` at `atFx` along `dirFx`. `strength` is 0..1000 per-mille;
// `instability` (0..1000) is the melt share of an unstable convert (plan §4).
void ApplySpellEffect(const GlyphLibrary& lib, const std::vector<EffectInst>& payload,
                      SpellFxVec atFx, SpellFxVec dirFx, int32_t strengthMille,
                      SpellEmission& out, const SpellProbe* probe = nullptr,
                      int32_t instabilityMille = 0, uint32_t salt = 0);

// ---- the system ------------------------------------------------------------

class SpellSystem {
 public:
  // OP BUDGET FAIRNESS (§F): magic's explicit share of the 64-op tick budget.
  static constexpr int kSpellOpsPerTick = 24;

  void SetLibrary(const GlyphLibrary* lib) { lib_ = lib; }
  const GlyphLibrary* Library() const { return lib_; }

  // Cast a compiled spell. `originFx`/`dirFx` are 24.8 fixed voxels; `dirFx`
  // need not be normalized. `selfAt`, when given, is where `self` resolves
  // (the character screen's clicked part) instead of the origin.
  CastResult Cast(const CastList& list, CasterState& caster,
                  const CasterHealth& health, uint64_t casterId,
                  SpellFxVec originFx, SpellFxVec dirFx, uint32_t tick,
                  SpellEmission& out, const SpellProbe* probe = nullptr,
                  const SpellFxVec* selfAt = nullptr);

  // Advance every live projectile one tick.
  void Tick(uint32_t tick, const World& world,
            const std::vector<uint32_t>& classOf, SpellEmission& out);

  void Clear() { live_.clear(); }
  const std::vector<SpellProjectile>& Live() const { return live_; }
  int LiveCount() const { return (int)live_.size(); }
  int OpsDroppedLastTick() const { return opsDropped_; }

 private:
  void Launch(const SpellCast& cast, SpellFxVec originFx, SpellFxVec aim,
              uint64_t casterId, uint32_t tick, int32_t instance,
              int32_t instability, SpellEmission& out, const SpellProbe* probe);

  const GlyphLibrary* lib_ = nullptr;
  std::vector<SpellProjectile> live_;
  int opsDropped_ = 0;
};

// Trajectory wobble from an unstable cast. Integer + counter-based hash, so it
// reproduces in a replay.
SpellFxVec SpellWobble(SpellFxVec dirFx, int32_t instabilityMille,
                       uint32_t tick, uint64_t casterId);
// A fanned copy of `dir` for instance `i` of `count` (shotgun). Instance 0 is
// the aim itself.
SpellFxVec SpellFan(SpellFxVec dirFx, int32_t i, int32_t count, uint32_t tick,
                    uint64_t casterId);
