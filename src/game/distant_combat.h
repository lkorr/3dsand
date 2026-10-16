#pragma once
// distant_combat.h -- THE ABSTRACT HIT MODEL OF THE DISTANT TIER
// (docs/PLAN_distant_npcs.md §2.6, package D2a).
//
// WHY THIS EXISTS. A creature past the residency window has no rig, no Jolt
// bodies and no lattice, and the near tier's melee is all three: the pose is
// the hitbox (MeleeSweepDamage), armour is a cover ray onto worn shells, a
// parry is two blades' segments meeting (FindParry). None of it can run out
// there. So a distant blow is a ROLL against a table, and the table is not
// invented: it is MEASURED from full-fidelity fights by the duel-matrix gate
// `distant-calib-matrix` (src/test/selftest_distant_calib.cpp), which logs
// every stroke STARTED with its outcome (game/stroke_log.h) and reduces the
// log to the numbers below. Re-running that harness after a combat change is
// how this tier keeps up with melee tuning (plan §5, "calibration rot").
//
// WHAT IS HERE, AND WHAT IS NOT. Everything in this header is PURE: a kit is
// cached once from a live creature (BuildDistantKit, at demotion, BEFORE
// ReleaseRig -- the live limbs, the worn shells and the held items are still
// there to read), and every resolver below is a function of its arguments.
// Nothing here knows that `DistantMobs` exists, mutates a global, reads a
// clock or reads a Jolt float. The distant tick (package D2b) owns WHEN a
// stroke starts and lands; this file owns what happens when it does.
//
// DETERMINISM (CLAUDE.md rule 1, plan §2.12). Every draw is
// `rng::Hash3((uint32_t)id ^ salt, tick, i)` with the salts declared beside
// the AI's (ai_behavior.cpp: kSaltDistantStrike / Arrow / Style), compared as
// 24-bit integers (rng::Unit01 is exact), so a resolution is a pure function
// of (kit, kit, hp, style, distance, id, tick). ORDER-INDEPENDENT by
// construction: ResolveStrike / ResolveArrow do not touch the vitals they are
// handed, so resolving two pairs in either order gives the same two answers;
// the caller applies outcomes (ApplyStrike) in whatever order it has defined.
// Floats are the near tier's floats (same binary, same order, same result).
//
// HEALTH IS PER LIMB, NOT PER ZONE (plan §2.2, audit A8). The distant body is
// exactly `Mob::limbs_[i].hp` for the base limbs, so the round trip is exact
// and a snake, a shark, a piranha and a naga need no humanoid zone table. A
// blow picks a limb by its ROLE (LimbRole: head / torso / arm / leg / tail /
// fin), sampled from the measured `roleWeights` of the style; a role the
// defender lacks folds into torso.

#include <cstdint>
#include <string>
#include <vector>

#include "game/impact.h"     // StrikeProfile
#include "phys/damagecause.h"
#include "sim/tuning.h"      // Tuning::Gear for ShellResponseOf

class Mob;
struct ItemLibrary;
struct AttackStyle;

namespace ai {
// THE DISTANT TIER'S DRAW STREAMS, defined in ai_behavior.cpp BESIDE the AI's
// own salts (kSaltAttack, kSaltCircle, ...) so one list holds every per-mob
// stream keyed on `mobId ^ salt` and a collision is visible at a glance; the
// static_asserts there keep them apart.
extern const uint32_t kSaltDistantStrike;  // block, hit, role, limb, spread
extern const uint32_t kSaltDistantArrow;   // hit, role, limb, spread
extern const uint32_t kSaltDistantStyle;   // PickDistantStyle
}  // namespace ai

namespace distant {

// ---- LIMB ROLES ---------------------------------------------------------------
//
// What a blow can be AIMED AT, independent of the rig's names. Derived from
// the sidecar's `tag` first (the same tags gait and the IK chains already
// query: "head", "spine", "arm", "hand", "leg", "foot", "tail", "fin", "jaw"),
// then from the limb's NAME for a tagless part, so the human, the snake, the
// shark, the piranha and the naga's twelve tail segments all answer without a
// per-creature table. Hair and other bloodless parts are `Other`: never
// sampled by a blow, never health.
enum class LimbRole : uint8_t { Head = 0, Torso, Arm, Leg, Tail, Fin, Other, Count };
constexpr int kRoles = (int)LimbRole::Count;
const char* LimbRoleName(LimbRole r);
// "head" -> Head, ...; Count when the name is not a role.
LimbRole LimbRoleByName(const std::string& n);
// The role of one limb def. `bloodless` limbs are Other whatever their tag.
LimbRole LimbRoleOf(const std::string& tag, const std::string& name,
                    bool bloodless = false);

// ---- WHAT A DEFENDER HOLDS UP ------------------------------------------------
// None: nothing in either fist (a zombie, a snake, a bare fist). Blade: a
// weapon that can PARRY (FindParry: blade on blade). Shield: a shield in a
// fist (FindShieldBlock). Measured as a block chance per class.
enum class GuardClass : uint8_t { None = 0, Blade, Shield, Count };
constexpr int kGuards = (int)GuardClass::Count;
const char* GuardClassName(GuardClass g);

// ---- WHAT A BLOW IS ------------------------------------------------------------
// The DOMINANT part of a StrikeProfile (game/impact.h): a sword is a Cut, a
// mace and a fist are Blunt, jaws are a Bite. Pierce is an arrow. Decides the
// shell row (ShellResponseOf), the armour exposure and the bleed per hp.
enum class StrikeKind : uint8_t { Cut = 0, Blunt, Bite, Pierce, Count };
constexpr int kKinds = (int)StrikeKind::Count;
const char* StrikeKindName(StrikeKind k);
StrikeKind DominantKind(const StrikeProfile& s);
// The DamageCause the near tier books this kind under. A natural weapon's
// blunt part is Unarmed (its own bleed rate), a held one's Blunt.
DamageCause CauseOf(StrikeKind k, bool natural);

// =============================================================================
// THE CALIBRATION TABLES (assets/mobs/distant_combat.json)
// =============================================================================

struct WeaponCalib {
  std::string weapon;      // item name ("sword") or natural weapon ("jaws")
  int n = 0, hits = 0, blocked = 0;
  float hitRate = 0.0f;    // P(contact | not blocked)
  float meanPower = 0.0f;  // see StyleCalib::meanPower
};

// One authored attack style (attack_styles.json `name`; a weapon FORM folds
// into its base style's row, so "horizontal_r" covers the dagger's short form).
struct StyleCalib {
  std::string name;
  int n = 0;               // strokes started
  int hits = 0;            // ...that met the TARGET's body (flesh or shell)
  int blocked = 0;         // ...that a shield or a blade arrested
  // P(contact | not blocked): hits / (n - blocked). The resolver multiplies
  // it by (1 - the defender's block chance), which reproduces hits / n.
  float hitRate = 0.45f;
  // Where a hit lands, by role (sums to 1 over the roles present).
  float roleWeights[kRoles] = {0.12f, 0.46f, 0.24f, 0.18f, 0.0f, 0.0f, 0.0f};
  // THE EFFECTIVE MULTIPLIER on the weapon's StrikeProfile::Total(), measured
  // on hits against UNARMOURED flesh: life hp charged per hit / strike total.
  // It folds in everything the near tier does to a blow -- the speed ramp, the
  // edge alignment, the carve's volume hp, repeat strikes in one stroke -- so
  // the resolver reproduces the observed mean without re-deriving any of it.
  float meanPower = 1.5f;
  // Coefficient of variation of that hp (sd / mean). The resolver spreads its
  // hp by an exponential mixture with this CV (mean preserved exactly), so a
  // sever-sized blow still happens now and then at distance.
  float powerCv = 0.6f;
  // Mean ticks from the stroke's start to its first contact, measured. 0 = use
  // the authored impact (DistantStyle::impactTicks).
  float impactTicks = 0.0f;
  std::vector<WeaponCalib> byWeapon;
};

struct ArmourCalib {
  // THE GAP RATE per kind: the share of a blow on a shell-covered limb that
  // still reaches the flesh beyond what ShellResponseOf's `passed` already
  // transmits (a slit chipped through, a cuirass that does not cover the side
  // the edge arrived from). f = passed + (1 - passed) * exposure.
  float exposure[kKinds] = {0.15f, 0.5f, 0.1f, 0.3f};
  int n[kKinds] = {};
};

struct BleedCalib {
  // HP OF BLOOD OWED per hp of a blow of this kind (Mob::Damage's bleed budget
  // x gore.bleedHpPerVoxel, times the share of it that actually drains).
  float perHp[kKinds] = {0.9f, 0.15f, 0.7f, 0.9f};
  // The bleed RATE (hp/tick) is multiplied by this every tick: the total a
  // wound pays out is rate / (1 - decay), so a blow owing B adds
  // B * (1 - decay) to the rate.
  float decay = 0.985f;
  // Measured drained / owed (cauterised and healed wounds pay less).
  float drainShare = 1.0f;
};

// THE SIZE OF A HIT, BY ROLE (D2b). The multiplier on strike total x the
// style's meanPower that a hit on this role carried, as an inverse CDF:
// knots (p, x) with p ascending in [0, 1]. Measured from the same unarmoured
// hits as meanPower, each normalised by its own style's mean, so the knots'
// mean is ~1 and the style's meanPower still sets the scale. Where powerCv
// (clamped at 1) could only spread a blow round its mean, this keeps the
// TAIL: a skull's brain, a chest's heart -- organs charge a flat hp per voxel
// (Mob::CarveLimb), so a hit on the head or torso now and then carries 5-30x
// the mean, and that is what ends most full-fidelity fights early. Empty =
// the CV spread (a table written before D2b).
struct HitSpread {
  std::vector<float> p, x;
};

// DISMEMBERMENT (D2b). Near, a cut that carves through a limb's neck takes it
// off (Mob::Sever) -- the hp rule never does (Mob::HpZeroSevers) -- and an
// open stump bleeds until the creature is dead (gore.stumpBleedsOpen). So a
// sword fight is decided by limbs coming off as much as by hp. Measured from
// the log: P(the struck limb comes off | a hit of this kind on this role,
// unarmoured); a shell scales it by what it lets through.
struct SeverCalib {
  float rate[kKinds][kRoles] = {};
  int hits[kKinds][kRoles] = {};
  // At the sever, the gout and the stump budget (gore.severStumpBudget, hp
  // owed); then each open stump drains this many hp a tick until death: the
  // near stump's top-up (one bleed clump x min(1, bleedVoxelGain x
  // stumpBleedGain) per gore.bleedDripTicks, x gore.bleedHpPerVoxel) -- the
  // TUNING's law, copied at load like the burn cap.
  float owedHp = 24.0f;
  float stumpHpPerTick = 0.15f;
};

struct BurnCalib {
  float fracPerTick = 0.002f;  // burnt surface fraction gained per burning tick
  float hpPerTick = 0.3f;      // hp the flames eat per burning tick (whole body)
  int meanTicks = 300;         // how long a body that caught keeps burning
  // The cap curve (Mob::BurnHealthCapFor), copied from gore.* at load so the
  // resolver is a pure function of the table.
  float capMidFraction = 0.40f;
  float capMidHealth = 0.333f;
  float deathFraction = 0.70f;
};

struct RangedCalib {
  float arrowSpeed = 480.0f;   // world voxels/s at the loose (flight time)
  // P(hit) at distance d: linear between these knots (world voxels), flat
  // past the ends.
  std::vector<float> distKnots = {20.0f, 60.0f, 120.0f, 170.0f};
  std::vector<float> rateKnots = {0.75f, 0.55f, 0.30f, 0.15f};
  // ...times (1 - speedPenalty * min(1, targetSpeed / speedRef)).
  float speedRef = 40.0f;      // world voxels/s
  float speedPenalty = 0.4f;
  float damageFrac = 1.0f;     // life hp per hit / the ammo's flat damage
  float roleWeights[kRoles] = {0.1f, 0.5f, 0.2f, 0.2f, 0.0f, 0.0f, 0.0f};
  int n = 0, hits = 0;
};

// An ASYMMETRIC matchup at full fidelity (plan §2.6/§4 `distant-calibrate`):
// what D2b's distant run of the same setup must agree with.
struct MatchupCalib {
  std::string name;
  std::string a, b;            // side descriptions ("2x duelist+sword+plate")
  int aCount = 1, bCount = 1;
  float gapVox = 14.0f;        // start distance between the two lines
  int n = 0;                   // bouts measured
  float winA = 0.0f, winB = 0.0f, draw = 0.0f;
  float ttkTicks = 0.0f;       // mean ticks from the start to the last death
  float ttkSd = 0.0f;
  int timeoutTicks = 0;
  // THE DAMAGE RACE, decided or not: each side's summed TotalHp when the bout
  // ended (the last death, or the timeout) over what it started with, mean
  // over bouts. Full-fidelity fights often end in mutual incapacitation (both
  // swords out of their hands, both bleeding slowly), so the win rate alone
  // leaves most bouts unscored; this scores every one.
  float hpLeftA = 0.0f, hpLeftB = 0.0f;
};

struct DistantCalib {
  int version = 1;
  std::string about;
  std::string source = "builtin";    // the file it came from, or "builtin"
  int minSamples = 8;                // a byWeapon row is used from this n up
  float reachSlack = 2.0f;           // voxels past reach a blow still lands
  std::vector<StyleCalib> styles;
  StyleCalib fallback;               // a style the table has no row for
  float blockChance[kGuards] = {0.0f, 0.06f, 0.3f};
  int blockN[kGuards] = {};
  ArmourCalib armour;
  BleedCalib bleed;
  BurnCalib burn;
  RangedCalib ranged;
  HitSpread spread[kRoles];   // D2b: the hit-size inverse CDF per role
  SeverCalib sever;           // D2b: dismemberment
  std::vector<MatchupCalib> matchups;

  const StyleCalib& Style(const std::string& name) const;
  const MatchupCalib* Matchup(const std::string& name) const;
};

// The table every tier falls back to when the file is absent or unreadable:
// the numbers above (round, plausible, a first calibration's order of
// magnitude), the burn curve from the current tuning.
DistantCalib DistantCalibBuiltin();

// Load assets/mobs/distant_combat.json. Follows every loader here: a bad
// value is clamped or skipped LOUDLY into `log`, an unknown key is ignored,
// and nothing is fatal. Returns false (with `out` = the builtin table, source
// "builtin") when the file is missing or does not parse.
bool LoadDistantCombat(const std::string& path, DistantCalib& out,
                       std::string& log);
// The writer (the calibration harness's output). Deterministic text: ordered
// keys, rounded numbers, no timestamps.
std::string DistantCalibToJson(const DistantCalib& c);

// =============================================================================
// THE KIT: everything the distant tier needs to know about a creature's fighting
// =============================================================================

struct DistantStyle {
  int style = -1;              // StyleLibrary index of the BASE style
  int form = -1;               // ...resolved to the held weapon's form
  std::string name;            // base style name (the table row)
  std::string weapon;          // item name or natural weapon name ("fist.R")
  bool natural = false;
  // A `held` style (AttackStyle::weapon "" / "held"): swung with whatever is
  // in the fist -- the item, or the fist itself once it is empty. The distant
  // tier re-arms these (and only these) when a grip fails or an archer draws
  // its sidearm (game/distant.cpp RearmHeld); a named natural weapon (jaws)
  // never changes.
  bool held = false;
  bool fallback = false;       // AttackStyle::fallback (the PickAttackStyle group rule)
  float reach = 0.0f;          // StyleReachOn at demotion, centre-to-centre
  int impactTicks = 1;         // authored: windup frames + half the cut
  int totalTicks = 1;          // authored: every frame + the release
  StrikeProfile strike;        // what it lands with
  StrikeKind kind = StrikeKind::Cut;
};

struct DistantLimb {
  std::string name;
  LimbRole role = LimbRole::Other;
  bool vital = false;          // vital OR the root: zero hp here is a death
  bool bloodless = false;      // hair: not health, never struck
  bool alive = true;           // still attached (at demotion; D2b: until severed)
  bool severable = false;      // MobLimbDef::severable (and not the root)
  int parent = -1;             // base limb index of its parent (-1: the root)
  float maxHp = 0.0f;          // the AUTHORED hp (the burn cap's base)
  float shellHardness = 0.0f;  // hardest worn shell over it (0 = bare)
  // D9: WHERE IT WAS on the body at the kit's capture (Mob::LimbCentreWorld),
  // in the BODY'S FRAME: x right, y up from the feet, z forward, from the body
  // box's centre column -- what an arrow that met the statue's box is laid
  // against (ResolveArrowContact). `placed` false: the limb had no body then.
  float at[3] = {0.0f, 0.0f, 0.0f};
  bool placed = false;
};

struct DistantKit {
  uint64_t id = 0;
  std::string def;
  std::string profile;
  std::vector<DistantStyle> styles;  // usable, in the profile's order
  int closeStyle = -1;               // index into `styles` (MobDef::closeStyle)
  float closeRange = 0.0f;
  float slack = 1.0f;                // half the body's depth (BeginStroke's slack)
  GuardClass guard = GuardClass::None;
  std::vector<DistantLimb> limbs;    // the BASE limbs, index-parallel to the rig
  bool bleeds = true;                // MobDef::bleedMat != 0
  // A RUINED LIMB IS STILL IN THE WAY (D2b). Near, an arm hacked to hp 0
  // hangs there ruined (Mob::HpZeroSevers: hp alone never removes a limb) and
  // the next blow aimed at it still lands ON it -- `limb.hp -= amount` takes
  // it below zero and the stroke log counts that as the hit's life hp, which
  // is what `meanPower` and `roleWeights` were measured from. So with this
  // set, a blow's limb is drawn over the ATTACHED limbs of its role whatever
  // their hp (a role with none attached still folds into torso), and hp may
  // go negative exactly as near; DeathTest / TotalHp are unchanged (a ruined
  // limb is not health). Off (the default, and D2a's synthetic kits) = the
  // first resolver's rule: only limbs with hp left are struck. The distant
  // tier sets it on every kit it builds (game/distant.cpp DemoteToDistant):
  // without it every blow the table aimed at a ruined arm would move onto the
  // trunk, and a body would reach its vital limbs faster than the fights the
  // table was measured from.
  bool absorbRuined = false;
  // A BOW (game/mob_archery.h): the arrow's flat damage and its full-draw
  // speed; shots = arrows in the kit at demotion.
  bool ranged = false;
  float arrowDamage = 0.0f;
  float arrowSpeed = 0.0f;
  int shots = 0;
  std::string ammo;
};

// The hardest LIVE worn shell over each base limb (0 = bare), by each shell's
// dominant material's hardness (a cuirass is iron with a few leather straps;
// the straps do not decide whether a sword gets through). One lattice scan per
// worn slot: a demotion's cost, not a tick's.
std::vector<float> ShellHardnessOfLimbs(const Mob& m);

// A style's authored timing in ticks: the impact (the windup with
// BeginStrokeProgram's two-tick floor, then half the cut) and the whole stroke
// (every frame plus the release). What a kit's DistantStyle::impactTicks /
// totalTicks hold, from a rig or (D5+D6) from a record.
void StyleTimingOf(const AttackStyle& sty, int& impact, int& total);

// Cache the kit from a LIVE creature (the demotion, before ReleaseRig).
// `items` may be null (no held-weapon profiles then); a null-system or
// def-less mob yields an empty kit. `withBody` false (D3: a NEAR attacker's
// kit at its cut, game/distant.h NearEdgeBlow) stops after the styles and the
// bow -- no limb roles, no shell scan -- which is all an attacker's kit is
// read for (ResolveStrike reads the defender's body, the attacker's styles).
DistantKit BuildDistantKit(const Mob& m, const ItemLibrary* items, bool withBody = true);

// =============================================================================
// THE ABSTRACT BODY'S STATE (what the distant tier steps)
// =============================================================================

// Other = already dead when it was captured (a corpse demotes too, plan §2.1).
enum class DistantDeath : uint8_t { None = 0, VitalLimb, BloodLoss, Burns, Other };
const char* DistantDeathName(DistantDeath d);

struct DistantVitals {
  std::vector<float> limbHp;   // Mob::limbs_[i].hp of the base limbs, exactly
  float bleedRate = 0.0f;      // hp/tick, decays by BleedCalib::decay
  int stumps = 0;              // D2b: open stumps, each draining SeverCalib::stumpHpPerTick
  float burnFrac = 0.0f;       // burnt surface fraction (Mob::BurnFraction)
  int burnTicksLeft = 0;       // > 0 while the body is on fire
  DistantDeath dead = DistantDeath::None;
};

// The live creature's numbers, for the demotion (limb hp exactly; the open
// wounds' owed blood turned into a rate; a burning body keeps burning for the
// table's meanTicks).
DistantVitals CaptureDistantVitals(const Mob& m, const DistantCalib& cal);

// =============================================================================
// THE RESOLVERS (pure)
// =============================================================================

// Every draw keys off this: rng::Hash3((uint32_t)id ^ salt, tick, i).
struct DistantRngKey {
  uint64_t id = 0;     // the ATTACKER's (the shooter's) id
  uint32_t tick = 0;   // the impact tick
};

enum class StrikeResult : uint8_t { Hit = 0, Miss, Blocked, OutOfReach, NoTarget };
const char* StrikeResultName(StrikeResult r);

struct StrikeOutcome {
  StrikeResult result = StrikeResult::Miss;
  bool hit = false;
  int limb = -1;               // base limb index on the defender
  LimbRole role = LimbRole::Other;
  float hp = 0.0f;             // life hp to charge (after the shell)
  StrikeKind kind = StrikeKind::Cut;
  DamageCause cause = DamageCause::Other;
  float bleedAdded = 0.0f;     // hp/tick to ADD to DistantVitals::bleedRate
  float shellFactor = 1.0f;    // what the shell let through (1 = bare)
  bool sever = false;          // D2b: the struck limb comes off (SeverLimb)
};

// A STYLE FROM THE KIT, as PickAttackStyle draws one (strokes.cpp): usable
// styles only (the kit holds no other), the reach filter with the body's
// slack, the fallback group dropped while a real style survives, and the
// close-quarters style first inside its range. -1 = nothing reaches.
int PickDistantStyle(const DistantKit& k, float distance, uint64_t id,
                     uint32_t tick);

// ONE BLOW at its impact tick. `style` indexes `att.styles`. Out of
// `reach + cal.reachSlack` it is OutOfReach; then the defender's block roll,
// the style's hit roll, the role and limb draw (a live limb of the sampled
// role, else torso, else the healthiest live limb), the hp (strike total x
// meanPower x the CV spread x the shell factor) and the bleed. Does not touch
// `defHp`; apply with ApplyStrike.
StrikeOutcome ResolveStrike(const DistantKit& att, const DistantKit& def,
                            const std::vector<float>& defHp, int style,
                            float distance, const DistantRngKey& key,
                            const DistantCalib& cal, const Tuning::Gear& gear);

struct ArrowOutcome {
  bool hit = false;
  int flightTicks = 1;         // impact = loose tick + this
  int limb = -1;
  LimbRole role = LimbRole::Other;
  float hp = 0.0f;
  float bleedAdded = 0.0f;
  float pHit = 0.0f;           // the probability rolled against (diagnostic)
};

// ONE ARROW, loosed now: flight time from the distance and the speed (the
// kit's bow speed, else the table's), hit chance from the distance knots and
// the target's speed, damage = the ammo's flat damage x damageFrac x the
// shell factor (Pierce prices plate by the cut-hardness ratio, as the channel
// does: phys/kerf.h PierceChannel).
ArrowOutcome ResolveArrow(const DistantKit& shooter, const DistantKit& def,
                          const std::vector<float>& defHp, float distance,
                          float targetSpeed, const DistantRngKey& key,
                          const DistantCalib& cal, const Tuning::Gear& gear);

// Apply a resolved blow: hp off the limb, bleed onto the rate, then the death
// test. Returns the death (None while it lives). A blow that SEVERS is applied
// by the caller through SeverLimb (it edits the kit, which is the caller's own
// copy), after this.
DistantDeath ApplyStrike(DistantVitals& v, const DistantKit& k,
                         const StrikeOutcome& o, const DistantCalib& cal);
// THE LIMB COMES OFF (D2b): `limb` and every base limb under it leave the
// body (kit.limbs[i].alive = false: no longer struck, no longer health), the
// stump opens (v.stumps + 1, draining every tick until death) and the gout's
// owed blood joins the bleed rate. A VITAL limb off -- a head -- is a death.
// Writes the subtree's indices to `off` (optional). Pure on its arguments; the
// caller owns the kit it passes.
DistantDeath SeverLimb(DistantVitals& v, DistantKit& k, int limb,
                       const DistantCalib& cal, std::vector<int>* off = nullptr);
DistantDeath ApplyArrow(DistantVitals& v, const DistantKit& k,
                        const ArrowOutcome& o, const DistantCalib& cal);

// D9: AN ARROW THAT MET THE BODY. The player's arrows (and every flight in the
// window) are flown for real and judged GEOMETRICALLY against a distant
// creature's body box, so CONTACT is already decided -- there is no hit roll
// (ResolveArrow's knots are a shot's odds; this is the shot). `at` / `dir` in
// the body's frame (x right, y up from the feet, z forward, from the box's
// centre column -- DistantLimb::at's frame).
struct ArrowContact {
  float at[3] = {0.0f, 0.0f, 0.0f};
  float dir[3] = {0.0f, 0.0f, 1.0f};
  float height = 0.0f;         // the body box's height (the height fallback's scale)
  float halfWidth = 0.0f;      // half the box across
};
// THE LIMB is the live one whose captured centre lies NEAREST THE ARROW'S LINE
// through the body (lowest index on a tie) -- the hit's height and side on the
// body pick it, and its role is that limb's; a kit captured with no limb
// positions falls back to the height alone (head over 0.82 of the body, legs
// under 0.45, an arm past 0.6 of the half width, else torso; a role the body
// lacks folds into torso: PickLimb). Then ResolveArrow's pricing exactly:
// `damage` (the ammo's flat damage) x damageFrac x the shell factor over that
// limb (Pierce prices plate by the cut-hardness ratio), and the bleed it owes.
// No sever (an arrow has no strokes in the log). `hit` false only when the
// body has no live limb to take it. Pure; the draw (the fallback's limb among
// several of a role) is Hash3(key.id ^ kSaltDistantArrow, key.tick, 12).
ArrowOutcome ResolveArrowContact(const DistantKit& def, const std::vector<float>& defHp,
                                 const ArrowContact& c, float damage,
                                 const DistantRngKey& key, const DistantCalib& cal,
                                 const Tuning::Gear& gear);

// One tick of bleeding: the rate in hp (+ every open stump's drain, D2b),
// spread over the live non-bloodless limbs in proportion to what each has
// (Mob::DrainBlood's rule), then the rate decays. A total at or under the
// bill is a death by blood loss, as in DrainBlood. Returns the hp drained.
float StepBleed(DistantVitals& v, const DistantKit& k, const DistantCalib& cal);
// `ticks` ticks of OPEN STUMPS ONLY (D2b; bleedRate must be 0): what
// StepBleed would drain over them, in one sum. Exact, not an approximation:
// a constant drain c a tick spent proportionally leaves every limb at the
// same share of a total that falls by c a tick, whatever the steps. A total
// that would reach the bill inside the span is a death by blood loss, and
// `diedAfter` (optional) gets how many of the ticks it took. Returns the hp
// drained. What lets a creature whose only status is a stump SLEEP (its
// clock is the tick it bleeds out) instead of being stepped until it dies.
float StepStumps(DistantVitals& v, const DistantKit& k, const DistantCalib& cal, int ticks,
                 int* diedAfter = nullptr);
// The number of StepBleed ticks an open-stumps-only body has left before
// the one that kills it (INT32_MAX when nothing drains).
int StumpTicksLeft(const DistantVitals& v, const DistantKit& k, const DistantCalib& cal);
// One tick of burning (while burnTicksLeft > 0): the burnt fraction grows,
// the flames eat hpPerTick, every limb is clamped to maxHp x the cap
// (Mob::ApplyBurnCap), and a cap of 0 is a death by burns. Returns hp lost.
float StepBurn(DistantVitals& v, const DistantKit& k, const DistantCalib& cal);
// The burn cap curve from the table (== Mob::BurnHealthCapFor at the same
// knots; the gate asserts it).
float BurnCapFor(float burntFraction, const BurnCalib& b);
// THE NEAR RULES on the abstract body: a vital or root limb at zero, the total
// gone (blood loss), the burn cap at zero.
DistantDeath DeathTest(const DistantVitals& v, const DistantKit& k,
                       const DistantCalib& cal);
// Sum of live non-bloodless limb hp (Mob::TotalHp's set).
float TotalHp(const DistantVitals& v, const DistantKit& k);

}  // namespace distant
