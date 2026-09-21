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
// ---- THE GRAMMAR (docs/PLAN_magic_grammar.md; DESIGN.md §8) ------------------
//
// Six SORTS: Matter (a material by name), Effect (something that happens at a
// point), Delivery (how an Effect reaches a point), Mod (a field edit on a
// delivery record), Operator (a word with argument slots that produces one of
// the others), Separator (`lane`: opens a segment of the pile that belongs to
// one instance of the box that closes it).
//
// FOUR RULES, and everything else is a consequence of them:
//
//   1. A NOUN GOES INTO THE PILE. A Matter word, an Effect word, or an
//      operator group whose result sort is Effect is pushed onto the PILE.
//      Order inside the pile does not matter: identical items merge (×N,
//      capped) and the lowering rebuilds the pile in a CANONICAL order.
//
//   2. A DELIVERY BOXES THE PILE, AND SPEAKING CONTINUES. The Delivery word
//      takes the WHOLE pile and wraps it into ONE Effect value of verb
//      `launch`: {that delivery's DeliveryRec, the pile's Effects as its
//      payload, the pile's pending Mods stuck to its record}. The pile becomes
//      exactly that one value. So deliveries NEST and word order matters:
//      `explosive projectile projectile` is a bolt that fires a bolt that
//      explodes.
//
//   3. A MOD STICKS TO THE BOX THAT CLOSES THE PILE. A Mod word (or an
//      operator group of result sort Mod, i.e. `trail`) goes into the pile as
//      PENDING and is applied to the record of the NEXT delivery spoken. If no
//      delivery closes the pile it sticks to `hand`, the implicit outermost
//      delivery, where `shotgun` is three fanned resolve points and `float` is
//      the hop; every other mod on the hand is a charged no-op and the
//      describe line says so.
//
//   4. `lane` OPENS A SEGMENT OF THE PILE THAT BELONGS TO ONE INSTANCE. The
//      first segment is SHARED by every instance of the box that closes the
//      pile; each `lane` word opens a segment that belongs to exactly one of
//      them. The box fires `instances = max(count, L)`: instance i < L carries
//      shared ∪ lane[i+1], instance i >= L carries the shared segment alone.
//      A lane walls binding (an operator after a `lane` cannot take a word
//      spoken before it) and walls merging (`fire lane fire` is two items in
//      two lanes, not `fire×2`) — but a `count` Mod is record-wide wherever it
//      was spoken, because count IS the fan and a lane is one instance of it.
//      COPIES FAN, COLUMNS DO NOT: `SpellFan` spreads only the shared-only
//      instances; an instance that carries a lane fires on the aim itself.
//
// The outermost box is always `hand`, so the whole sentence lowers to ONE cast.
// Lanes on the hand are how two unrelated spells are said at once (`lane
// explosive projectile lane blood mend self`), and law L4 (cost additivity)
// attaches to those root lanes. There is no sentence separator any more:
// `also` was dropped when rule 4 landed.
//
// Unary operators (`trail`, `aura`, `echo`, `null`, `mend`) take the ONE item
// immediately before them, which may itself be a launch box (`explosive
// projectile echo` is a turret). `transmute` is the one infix word. An
// operator with an empty required slot is INCOMPLETE: charged, does nothing.
// Runs merge (`shotgun shotgun` is `shotgun×2`) for Matter, Effects and Mods
// — but NOT for deliveries, because each delivery boxes what is in front of
// it.
//
// scripts/magic_grammar.py is the executable reference for these rules; the
// `spells-oracle` gate compares this parser against every sentence it
// generates. Nothing in this file knows which words exist: sorts, valence,
// verbs, axes and costs are read from assets/spells/glyphs.json.

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
  // `lane` (rule 4): opens a segment of the pile that belongs to ONE instance
  // of the box that closes it. The enum value kept its name; the word it
  // names changed when `also` was dropped.
  Separator,
};
constexpr int kGlyphSortCount = 6;
const char* GlyphSortName(GlyphSort s);   // "matter" | "effect" | ...
bool ParseGlyphSort(const std::string& s, GlyphSort& out);

// A slot's acceptance set, as a bitmask over sorts. `any` accepts everything,
// including a raw operator word (`transmute null` takes `transmute` itself).
constexpr uint8_t kSortBitMatter = 1u << (int)GlyphSort::Matter;
constexpr uint8_t kSortBitEffect = 1u << (int)GlyphSort::Effect;
constexpr uint8_t kSortBitDelivery = 1u << (int)GlyphSort::Delivery;
constexpr uint8_t kSortBitMod = 1u << (int)GlyphSort::Mod;
constexpr uint8_t kSortBitOperator = 1u << (int)GlyphSort::Operator;
constexpr uint8_t kSortBitSeparator = 1u << (int)GlyphSort::Separator;
constexpr uint8_t kSortAny = 0x3F;

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
  Launch,    // (rule 2) a BOX: a DeliveryRec carrying `inner` as its payload
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
const char* ModFieldName(ModField f);

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

// How a CARRIER looks in flight. Render-only content, never authoritative:
// the delivery glyph's "look" block in glyphs.json. main.cpp draws sprites
// from it at the drawing boundary; the VM never reads it, so a modder can
// make a bolt a comet without touching a number the sim depends on.
enum class LookShape : uint8_t {
  Bolt = 0,   // a bright core with a streak of sprites back along the velocity
  Ball,       // a rounded blob (core + six face lobes), short tail
  Orb,        // a large slow core with a ring of motes orbiting the flight axis
  Spark,      // a small flickering point (a fuse, a resting bolt)
};
struct GlyphLook {
  LookShape shape = LookShape::Ball;
  float size = 0.6f;               // core half-size, voxels
  int32_t tail = 0;                // trailing sprites behind the core
  float tailStep = 0.5f;           // voxels between them, back along the velocity
  float glow = 1.0f;               // emission of the core
  uint32_t color = 0xFFFFFFFFu;    // 0xAABBGGRR, used when the cast carries no matter
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
  // Also: what a `place` effect lays, and what a rigid-body delivery (bomb)
  // is made of.
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
  // mend: the anatomy's own materials (no foreign penalty), by name in the
  // glyph's "native" list, and the penalty for anything else: extra
  // arcane(M) × foreignPenaltyMille / 1000 per voxel.
  std::vector<uint32_t> nativeMats;
  int32_t foreignPenaltyMille = 1000;
  GlyphWind wind;

  // ---- delivery record defaults ----
  DeliveryMech mech = DeliveryMech::Instant;
  int32_t carryMille = 1000;     // premium on the payload tariff
  // Speed in 24.8 FIXED voxels per tick (kSpellFxOne = one voxel), so the
  // authored number may be fractional: "speed": 4.8 in glyphs.json is 4.8
  // voxels a tick, 144 voxels a second at 30 Hz. Whole-voxel speeds were the
  // reason every bolt crossed the CPU mirror in one tick.
  int32_t speedFx = 48 * kSpellFxOne / 10;
  int32_t lifetimeTicks = 150;   // hard bound (rule 2)
  int32_t impactRadius = 3;      // kinetic impact of an empty payload
  int32_t gravityMille = 0;      // per-mille of g
  int32_t fuseTicks = 0;         // 0 = resolve on impact
  int32_t reach = 3;             // instant: voxels in front of the caster
  bool body = false;             // flight as a rigid body (bomb)
  bool resolveOnExpiry = false;  // orb: life running out is a resolve, not a fizzle
  GlyphLook look;                // how it is drawn (render-only)
  // What the describe line calls one of these ("a bolt", "a bomb"). Content,
  // so a modder's new carrier reads as itself; defaults to the glyph id.
  std::string noun;

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
  int32_t maxMacroWords = 24;
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
// UI state too — an unbounded stack is an unbounded mana cost). 24 since rule
// 4: three lanes of two effects, a fan, a delivery and a mod is a sentence a
// player will want to say, and 16 could not hold one.
constexpr int kSpellStackMax = 24;

// ---- the parse tree (the three rules) ----------------------------------------

// One item of the tree: a raw word with multiplicity, an operator group with
// its bound children, or a BOX (rule 2) — a delivery holding the pile it
// closed. The HUD draws the brackets straight off this, so what you see is
// what bound.
struct SpellNode {
  int glyph = -1;        // library index; on a box, the delivery (-1 = hand)
  int32_t n = 1;         // multiplicity (runs merge), capped
  bool group = false;    // an operator application
  bool box = false;      // a delivery box: `glyph` is the delivery, `items`
                         // the pile it closed (nouns AND pending mods)
  int left = -1;         // operator child, -1 = empty slot (or no slot)
  int right = -1;
  std::vector<int> items;   // box: the closed pile, first-seen order, merged
  // RULE 4: which segment of the pile this item was spoken in. 0 = the shared
  // segment; 1..L = the segment opened by the Nth `lane` word, which belongs
  // to instance N-1 of the box that closes the pile. Part of NodeKey, so two
  // identical words in different lanes never merge.
  int32_t lane = 0;
  // Box only: the spoken position of each `lane` word this box closed, in
  // order, for the HUD highlight and the linearizer. Size == L.
  std::vector<int> laneAt;
  bool complete = true;  // false: a required slot is empty
  // Spoken span [first, last] of this item and everything under it, for the
  // HUD highlight and the L6 law; `at` is the operator/delivery word's own
  // position.
  int first = -1, last = -1;
  int at = -1;
};

// THE sentence. Since `also` was dropped for rule 4, a spoken sequence is
// exactly ONE clause (`clauses` holds 0 entries for silence, 1 otherwise) —
// the vector survives so the UI and the gates keep their shape, and so that a
// future second root is a change of one number rather than of every loop. Its
// root is ALWAYS the implicit `hand` box (rule 3), so `delivery`/`weight`
// below are the hand's and the nesting lives inside `bag`.
struct SpellClause {
  int root = -1;         // the outermost box node
  int delivery = -1;     // == nodes[root].glyph; -1 = hand, and always is
  int32_t weight = 1;
  std::vector<int> bag;  // == nodes[root].items
};

struct SpellTree {
  std::vector<SpellNode> nodes;
  std::vector<SpellClause> clauses;
  bool Empty() const { return clauses.empty(); }
};

// The three rules, left to right. Total: any sequence of valid glyph indices
// parses.
SpellTree ParseSpell(const GlyphLibrary& lib, const SpellStack& stack);

// The sort an item denotes: a raw word's sort, or an operator's result sort.
GlyphSort NodeSort(const GlyphLibrary& lib, const SpellTree& t, int node);
// The identity of an item under rule 1, multiplicity excluded: `fire`,
// `(dirt|transmute|water)`, `(|trail|)`, `[fire#1|projectile]`. What the
// reference script calls key().
std::string NodeKey(const GlyphLibrary& lib, const SpellTree& t, int node);

enum class BracketStyle : uint8_t {
  Oracle = 0,   // the reference script's exact spelling (× ‖ ⋈ ◂ and **bold**)
  Hud,          // ASCII for the pixel font: x2, |, ><, <, DELIVERY in caps
};
// An item with its brackets: `fire×2`, `(dirt ⋈ water)`, `(_ ◂trail)`, and a
// box as `[ ... **projectile**]`. A lane boundary inside a box prints as
// ` / ` in BOTH styles (`[explosive / gunpowder / fire **projectile**]`) —
// one spelling, because the oracle JSON and this parser compare strings.
std::string ShowNode(const GlyphLibrary& lib, const SpellTree& t, int node,
                     BracketStyle style = BracketStyle::Oracle);
// The whole sentence: the hand box's items, lanes separated by ` / `.
std::string BracketSpell(const GlyphLibrary& lib, const SpellTree& t,
                         BracketStyle style = BracketStyle::Oracle);

// ---- the lowered cast (plan §5, §6) -------------------------------------------

struct DeliveryRec;

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
  // The wrapped effect(s) for trail / sustain / repeat — and, for a Launch,
  // the payload the box carries.
  std::vector<EffectInst> inner;
  // A LAUNCH's delivery record (rule 2). Exactly one entry when
  // `verb == SpellVerb::Launch`, empty otherwise. A vector rather than a
  // by-value member because DeliveryRec holds EffectInsts of its own — the
  // same knot `inner` ties, tied the same way.
  std::vector<DeliveryRec> launch;
  // A sustained MOD (float aura ...): the field edit the status applies.
  ModField modField = ModField::None;
  ModOp modOp = ModOp::Mul;
  int32_t modAmount = 0;
  int32_t modN = 1;
  int node = -1;             // tree node, for the readout
};

struct SpellLane;

// The delivery record a live cast carries. Mods are field edits on it.
struct DeliveryRec {
  int glyph = -1;            // -1 = hand
  DeliveryMech mech = DeliveryMech::Instant;
  int32_t weight = 1;        // Delivery×N: speed, lifetime, kinetic impact ×N
  int32_t carryMille = 1000;
  int32_t speedFx = 0;       // 24.8 fixed voxels/tick (GlyphDef::speedFx)
  int32_t lifetimeTicks = 1;
  int32_t impactRadius = 1;
  int32_t gravityMille = 0;  // per-mille of g (flight), or on the anchored body
  int32_t fuseTicks = 0;
  int32_t reach = 0;
  int32_t count = 1;         // instances (shotgun)
  int32_t bounces = 0, pierce = 0, seek = 0;
  int32_t radiusMille = 1000;   // wide: resolve radius multiplier
  bool body = false;         // rigid body flight (bomb)
  bool resolveOnExpiry = false;
  // Trail mods: effects run at each marked voxel of the flight path, under a
  // hard voxel budget that only decreases (rule 2).
  std::vector<EffectInst> trail;
  int32_t trailBudget = 0;
  int32_t trailEvery = 1;    // mark every Nth voxel of travel
  // RULE 4: one entry per `lane` segment of the pile this record's box closed,
  // in SPOKEN order. Lane i belongs to instance i. A vector rather than a
  // by-value member for the same reason `EffectInst::launch` is one: a lane
  // holds a record, and a record holds effects that hold records.
  // INVARIANT: a lane's own `rec.lanes` is always empty (one level only).
  std::vector<SpellLane> lanes;
};

// One instance's private segment of the pile (rule 4): the shared record with
// this lane's Mods applied on top and this lane's trail merged in, plus the
// nouns only this instance carries. `InstanceCast` is the one place that puts
// the two halves back together.
struct SpellLane {
  DeliveryRec rec;
  std::vector<EffectInst> extra;
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
  // The nesting (rule 2): how deep the launch boxes go under this one, and
  // how many LEAF instances the whole tree can produce (the product of the
  // counts down each path, summed over paths). `instancesClamped` says the
  // leaf cap cut the fan back, which the describe line reports.
  int32_t depth = 0;
  int32_t leaves = 1;
  bool instancesClamped = false;
  // Mod words that landed on a record with nothing to edit (rule 3: the hand
  // has no speed, no fuse, nothing to bounce). Charged; named in the readout.
  std::vector<int> wastedMods;
  int clause = -1;

  int32_t Cost() const { return wordCost + tariff + carryCost; }
};

// THE PRICE OF ONE BOX, kept beside the cast list so the graph page can draw
// a subtotal under every join (PLAN_spell_graph §4). Derived data: filled by
// `LowerBox` on the way out, never read by the VM.
struct BoxPrice {
  int node = -1;             // the SpellTree node of the box
  int32_t wordCost = 0, tariff = 0, carryCost = 0;
  int32_t instances = 1, leaves = 1;
  bool instancesClamped = false;
};

struct CastList {
  SpellTree tree;
  std::vector<SpellCast> casts;
  // One entry per box in `tree`, the hand root included, in lowering order.
  std::vector<BoxPrice> boxPrice;
  const BoxPrice* PriceOf(int node) const {
    for (const BoxPrice& p : boxPrice)
      if (p.node == node) return &p;
    return nullptr;
  }
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
// Fills a cast's tariff and carry from its payload, trail and instances. With
// lanes (rule 4) the tariff is the SUM over instances of tariff(shared ∪
// lane_i) rather than tariff(shared) × instances, which is the same number
// when there are no lanes.
void PriceCast(const GlyphLibrary& lib, SpellCast& cast);
// HOW MANY INSTANCES a record fires: max(count, lanes), clamped. One place,
// because the fan loops, the pricing and the volume bound must agree.
int32_t RecInstances(const GlyphLibrary& lib, const DeliveryRec& d);
// THE CAST INSTANCE i ACTUALLY FLIES WITH (rule 4): the shared cast for
// i >= L, and for i < L the lane's record with the lane's nouns appended to
// the shared payload. Called in every fan loop; returns `cast` unchanged when
// the cast has no lanes, so a sentence without `lane` behaves bit-for-bit as
// it did.
SpellCast InstanceCast(const SpellCast& cast, int32_t i);
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
  bool piercing = false;     // inside the wall it is passing through
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
  // A per-launch serial, for the renderer only (live_ is swap-removed, so an
  // index is not an identity): phases a bolt's flicker and an orb's ring.
  uint32_t seq = 0;
};

// A RIGID-BODY CARRIER (bomb): flight with a fuse, as a real body through the
// existing debris path. The VM cannot create one (that is physics, and the
// owner's business), so it REPORTS the request and the owner answers with
// SpellSystem::AdoptBody(token, handle). From then on the bomb is ordinary
// debris — it falls, rolls, settles, catches fire — and the VM only asks where
// it is (SpellBodyProbe) until the fuse runs out.
struct SpellBodyRequest {
  uint32_t token = 0;    // matches the AdoptBody call
  Vec3 pos{};            // world voxels, the ball's centre
  Vec3 vel{};            // voxels per second
  float radius = 1.5f;   // voxels
  uint32_t material = 0; // what the ball is made of (the delivery glyph's)
};

struct SpellBomb {
  SpellCast cast;
  uint32_t token = 0;
  uint64_t body = 0;         // 0 until adopted
  int32_t fuseLeft = 0;
  int32_t ticksLeft = 0;     // hard bound even if the fuse never runs (rule 2)
  SpellFxVec lastPos{};
  int32_t trailBudget = 0;
  int32_t trailPhase = 0;
  int32_t markedX = 0, markedY = 0, markedZ = 0;
  bool markedValid = false;
  int32_t instability = 0;
  int32_t gen = 0;
  uint64_t casterId = 0;
};

// What the VM may ask the owner about bodies, one tick latent, without knowing
// what a mob or a rigid body is (thesis 4). Nullable: without it a bomb goes
// off where it was thrown and nothing seeks.
struct SpellBodyProbe {
  // The current centre of an adopted body; false when it no longer exists.
  bool (*bodyAt)(void* ctx, uint64_t handle, Vec3& outCentre) = nullptr;
  // The nearest body worth turning toward, from `from`, for `casterId`'s own
  // bolt (so a caster's bolts do not seek the caster). False when none.
  bool (*nearestTarget)(void* ctx, Vec3 from, uint64_t casterId, Vec3& outCentre) = nullptr;
  // The body (a mob, the player) within `radius` of `from`, by the owner's
  // opaque id, for a status to attach to. False when the point is a PLACE.
  bool (*bodyIdAt)(void* ctx, Vec3 from, float radius, uint64_t& outId,
                   Vec3& outCentre) = nullptr;
  // Where a body is now; false when it is gone (the status drops).
  bool (*bodyPos)(void* ctx, uint64_t id, Vec3& outCentre) = nullptr;
  // THE BODY A FLIGHT RUNS INTO. The first body (a mob's limb, a debris
  // chunk, another caster) on the segment `from`..`to` that is not
  // `casterId`'s own; `outHit` is where the segment meets it. False when the
  // segment is clear. Bodies are not in the voxel grid, so without this a
  // bolt flies straight through a creature and resolves on the wall behind.
  bool (*bodyHit)(void* ctx, Vec3 from, Vec3 to, uint64_t casterId, Vec3& outHit) = nullptr;
  void* ctx = nullptr;
};

// A RESOLVE HAPPENED HERE: reported so the renderer can draw a flash. Render
// information only — the world change itself is the ops beside it.
struct SpellImpactFx {
  SpellFxVec at{};
  uint32_t tint = 0;        // CastTintMaterial of the cast, 0 = none
  int32_t radius = 1;       // the largest radius the resolve worked at
  int deliveryGlyph = -1;   // for the look's colour fallback
};

// A SUSTAINED STATUS (plan §7): what the `aura` operator produces. Attached to
// a body by the owner's opaque id, or to a place; runs every tick, billed
// every tick to the caster as a reservation out of their max mana, until
// dropped, dry, or the hard tick cap (rule 2; also a per-caster cap).
struct SpellStatus {
  EffectInst effect;         // the Sustain instance: inner effect, or a mod
  uint64_t target = 0;       // body id, 0 = a place
  SpellFxVec at{};           // the place, or the body's last centre
  SpellFxVec dir{0, kSpellFxOne, 0};
  uint64_t casterId = 0;
  int32_t ticksLeft = 0;
  int32_t perTick = 0;       // tariff per tick
  int32_t instability = 0;
  uint32_t id = 0;           // for the drop UI (newest = highest)
};

// A HELD BEAM: continuous delivery. Resolves at the ray hit every tick the
// owner keeps it held (HoldBeam), billed per tick, ended by release, by
// running dry, or by the tick cap.
struct SpellBeam {
  SpellCast cast;
  uint64_t casterId = 0;
  SpellFxVec origin{}, dir{kSpellFxOne, 0, 0};
  int32_t ticksLeft = 0;
  int32_t perTick = 0;
  int32_t instability = 0;
  bool held = true;
  bool prepaid = true;       // the cast paid for the first resolve
  // A NESTED beam (`explosive beam projectile`) is not the caster's aim: it
  // burns from where the parent resolved, along the direction it was given,
  // until its tick cap. HoldBeam leaves it alone.
  bool anchored = false;
};

// An ECHO: the inner effect again at the point every `period` ticks, a
// bounded number of times (priced up front: repeats × the effect).
struct SpellEcho {
  std::vector<EffectInst> inner;
  SpellFxVec at{}, dir{};
  int32_t left = 0, period = 1, phase = 0;
  uint64_t casterId = 0;
  int32_t instability = 0;
};

// A FILTER (plan §7 wards): `W null`. Refuses incoming ops of W's kind within
// `radius` of `at`, on the OP STREAM at the MutationQueue splice — never the
// CA, so acid already flowing still flows and the counterplay is physics.
// One tick unless sustained by aura, which re-issues it every tick.
struct SpellFilter {
  int glyph = -1;            // the refused word
  SpellFxVec at{};
  int32_t radius = 1;
  uint64_t casterId = 0;
  int32_t ticksLeft = 1;
};

// A GRAFT: `count` voxels of `material` to fill into the caster's missing
// anatomy cells (Mob::RestoreBody), nearest-to-root first. The world half —
// the source voxels leaving as convert(cell->air) ops — is already in `ops`.
struct SpellRestore {
  uint64_t casterId = 0;
  uint32_t material = 0;
  int32_t count = 0;
};

// A NESTED LAUNCH ASKING TO BE BORN (rule 2). `ApplySpellEffect` stays
// op-stream-only: a Launch effect appends one of these rather than reaching
// into the system, and `SpellSystem` adopts them at the end of the call that
// produced them, exactly the way it adopts statuses, echoes and filters.
//
// `at`/`dir` are filled in by whoever emitted the payload, not by the payload:
// a child launches from its parent's LAST FREE position, along the parent's
// direction reflected off the surface it hit (the same per-axis reflection
// `bounce` uses), or straight up when there was no surface. `generation` < 0
// means "not stamped yet".
struct SpellLaunchReq {
  DeliveryRec delivery;
  std::vector<EffectInst> payload;
  SpellFxVec at{};
  SpellFxVec dir{0, kSpellFxOne, 0};
  int32_t generation = -1;
  uint64_t casterId = 0;
  int32_t instability = 0;
  uint32_t salt = 0;
};

// A per-tick charge the owner applies to a caster (statuses, beams).
struct SpellBill {
  uint64_t casterId = 0;
  int32_t amount = 0;
};
// A per-tick impulse a sustained mod puts on a body (float aura ...).
struct SpellBodyImpulse {
  uint64_t target = 0;       // body id
  Vec3 vps{};                // voxels per second
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
  // Rigid-body carriers to create (bomb) and ones whose fuse has run out.
  std::vector<SpellBodyRequest> bodyRequests;
  std::vector<uint64_t> bodyDone;
  // A Mod on a body-anchored delivery acts on the CASTER's body where that
  // means anything: `float self` is a hop, `heavy self` a shove down. Voxels
  // per second, reported for the owner to apply (the VM cannot touch a
  // controller). Zero when no anchored cast carried a gravity edit.
  Vec3 casterImpulseVps{};
  // Sustained things the payload asked for. The SYSTEM adopts these at the
  // end of the call that produced them (statuses, echoes, filters are its
  // state); the owner reads bills and impulses.
  std::vector<SpellStatus> statuses;
  std::vector<SpellEcho> echoes;
  std::vector<SpellFilter> filters;
  // Nested launches (rule 2). Adopted by the system; empty after Cast/Tick.
  std::vector<SpellLaunchReq> launches;
  std::vector<SpellBill> bills;
  std::vector<SpellBodyImpulse> bodyImpulses;
  std::vector<SpellRestore> restores;
  // Where flights and bombs resolved this tick (render-only).
  std::vector<SpellImpactFx> impacts;
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
//
// `flatten` is what a Fatal cast is: every Launch resolves its own payload IN
// PLACE, recursively, instead of asking for a carrier. So an overcast
// `explosive projectile projectile bomb` goes off in the caster's chest with
// everything it was ever going to do, and nothing leaves the body.
void ApplySpellEffect(const GlyphLibrary& lib, const std::vector<EffectInst>& payload,
                      SpellFxVec atFx, SpellFxVec dirFx, int32_t strengthMille,
                      SpellEmission& out, const SpellProbe* probe = nullptr,
                      int32_t instabilityMille = 0, uint32_t salt = 0,
                      bool flatten = false);

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
                  const SpellFxVec* selfAt = nullptr,
                  const SpellBodyProbe* bodies = nullptr);

  // Advance every live projectile and bomb one tick. `bodies` answers where
  // adopted bodies are, what a seeking bolt should turn toward and what a
  // flight runs into. The world is non-const because a flight beyond the
  // 3x3x3 CPU mirror asks for the chunks ahead of it (World::RequestChunkFetch,
  // bounded and coalesced) so it has something to hit when it gets there.
  void Tick(uint32_t tick, World& world,
            const std::vector<uint32_t>& classOf, SpellEmission& out,
            const SpellBodyProbe* bodies = nullptr);

  // The owner's answer to a SpellBodyRequest: the handle it made for `token`.
  // A token the VM no longer holds (the bomb already went off) is ignored and
  // the owner keeps the body.
  bool AdoptBody(uint32_t token, uint64_t handle);

  // ---- sustained (plan §7) --------------------------------------------------
  // A held beam follows the caster's aim: the owner reports it every tick and
  // whether the cast key is still down. Not held = the beam ends.
  void HoldBeam(uint64_t casterId, SpellFxVec originFx, SpellFxVec dirFx, bool held);
  // Drop the newest status this caster is paying for (the drop UI), or all of
  // them (the caster ran dry, or died).
  bool DropNewestStatus(uint64_t casterId);
  void DropAll(uint64_t casterId);
  // What this caster's live statuses reserve out of their max mana: the
  // per-tick tariff × the regen horizon (plan §7: "lowers your maximum mana"
  // without a second number).
  int32_t ReservationFor(uint64_t casterId) const;
  static constexpr int32_t kReserveHorizonTicks = 30;
  int StatusCountFor(uint64_t casterId) const;
  const std::vector<SpellStatus>& Statuses() const { return statuses_; }
  const std::vector<SpellBeam>& Beams() const { return beams_; }
  const std::vector<SpellEcho>& Echoes() const { return echoes_; }
  const std::vector<SpellFilter>& Filters() const { return filters_; }

  // ---- the op-stream filter (plan §7 wards) ----------------------------------
  // Applied by the owner at the MutationQueue splice: every op within a
  // filter's radius whose kind the filter's word names is REMOVED. Returns
  // how many were refused. Carriers (projectiles, bombs) are absorbed inside
  // Tick() the same way.
  int FilterStreams(std::vector<BrushOp>& ops, std::vector<ExplosionOp>& exps,
                    std::vector<ParticleSpawn>& spawns, std::vector<WindPrim>& winds) const;

  void Clear() {
    live_.clear();
    bombs_.clear();
    statuses_.clear();
    beams_.clear();
    echoes_.clear();
    filters_.clear();
  }
  const std::vector<SpellProjectile>& Live() const { return live_; }
  int LiveCount() const { return (int)live_.size(); }
  const std::vector<SpellBomb>& Bombs() const { return bombs_; }
  int BombCount() const { return (int)bombs_.size(); }
  int OpsDroppedLastTick() const { return opsDropped_; }
  int RefusedLastTick() const { return refused_; }

 private:
  void Launch(const SpellCast& cast, SpellFxVec originFx, SpellFxVec aim,
              uint64_t casterId, uint32_t tick, int32_t instance,
              int32_t instability, SpellEmission& out, const SpellProbe* probe);
  void RequestBody(const SpellCast& cast, SpellFxVec originFx, SpellFxVec aim,
                   uint64_t casterId, int32_t instability, SpellEmission& out);

  // Take the sustained things a payload asked for into the system's state,
  // attaching statuses to the body at their point (per-caster cap applied).
  // Nested launches are adopted FIRST, and an instant one (`self`) resolves
  // its own payload here, which may ask for more of everything — so the
  // launch drain loops, bounded by the generation cap.
  void Adopt(SpellEmission& out, const SpellBodyProbe* bodies, uint32_t tick,
             const SpellProbe* probe);
  void AdoptLaunches(SpellEmission& out, const SpellBodyProbe* bodies, uint32_t tick,
                     const SpellProbe* probe);
  bool FilterRefuses(const SpellFilter& f, int32_t x, int32_t y, int32_t z,
                     uint32_t material, int kindMode) const;

  const GlyphLibrary* lib_ = nullptr;
  std::vector<SpellProjectile> live_;
  std::vector<SpellBomb> bombs_;
  std::vector<SpellStatus> statuses_;
  std::vector<SpellBeam> beams_;
  std::vector<SpellEcho> echoes_;
  std::vector<SpellFilter> filters_;
  // Where a nested `self` resolves during a Cast() that named a body part
  // (the character screen). Valid only for the duration of that call.
  SpellFxVec selfAt_{};
  bool selfAtValid_ = false;
  uint32_t nextToken_ = 0;
  uint32_t nextStatusId_ = 0;
  uint32_t nextSeq_ = 0;
  int opsDropped_ = 0;
  int refused_ = 0;
};

// Trajectory wobble from an unstable cast. Integer + counter-based hash, so it
// reproduces in a replay.
SpellFxVec SpellWobble(SpellFxVec dirFx, int32_t instabilityMille,
                       uint32_t tick, uint64_t casterId);
// A fanned copy of `dir` for instance `i` of `count` (shotgun). Instance 0 is
// the aim itself. COPIES FAN, COLUMNS DO NOT (rule 4): the callers only fan an
// instance whose payload is the shared-only copy, so a projectile in its own
// lane flies at the crosshair rather than a few degrees off it.
SpellFxVec SpellFan(SpellFxVec dirFx, int32_t i, int32_t count, uint32_t tick,
                    uint64_t casterId);
