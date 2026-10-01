#pragma once
// ============================================================================
// THE SEVER / BLEED POLICY — one table keyed by (cause, tissue)
// (W2-G, docs/PLAN_rule_unification_2026-09-24.md; the cause is
// phys/damagecause.h).
//
// Every question Mob::Damage, HpZeroSevers, JointRuleApplies, CarveLimb and
// Sever used to answer by combining ambient flags (`inBurnFlush_ &&
// !inBluntCarve_`, `!inBluntCarve_ && !inSpawnRot_ && ...`) is a COLUMN here,
// and every combination the flags could express is a ROW. Adding a cause is
// adding two rows and reading them, instead of re-patching each rule.
//
// WHY IN CODE AND NOT IN JSON. Every column is a structural rule of the wound
// model (does a limb come off, does a stump spurt), read on the carve path and
// asserted by a dozen gates that describe it in prose; none of it is a number
// anybody tunes. A data file would buy a loader, a schema row and the ability
// for a content edit to make every blow dismember, and nothing else. What IS
// content — which BODIES are rotten — is data: MobDef::bodyTissue, set by the
// zombie effect's `"bodyTissue": "rotten"` (assets/mobs/effects/zombie.json).
//
// THE PROOF that this table is the old predicates is the `damage-cause` gate
// (src/test/selftest_mob.cpp), which walks every (cause, eaten, tissue) and
// compares each column to the flag expression it replaced, written out there
// verbatim.
//
// TISSUE is a property of the limb, not of the blow:
//   Shell     a worn garment slot (Mob::IsWornSlot)
//   Bloodless a base slot with no blood in it: hair (MobLimbDef::bloodless)
//   Rotten    anatomy of a body whose def says `bodyTissue: rotten` (the undead)
//   Flesh     anatomy of everybody else
// The tissue-dependent answers are the per-tissue columns below the row
// (`shellHeld`, `shellShed`, `rottenVitalDetach`) and the two blanket facts
// "a garment has no blood" and "hair has no blood" (Mob::IsBloodless).
// ============================================================================
#include <cstdint>

#include "phys/damagecause.h"

enum class Tissue : uint8_t { Flesh = 0, Rotten, Bloodless, Shell, Count };

// What a blow's DRIP is scaled by: nothing, a cut's full rate, or trauma's
// gore.bluntBleedScale / gore.unarmedBleedScale (the latter falling back to
// the blunt rate when negative — resolved where the tuning is read).
enum class BleedRate : uint8_t { None = 0, Full, Blunt, Unarmed };

// The joint ("hanging by a thread") rule: never, for every carve, or only on a
// limb an infection is eating (Mob::JointRuleApplies says why fire is exempt
// and rot is not).
enum class JointRule : uint8_t { Never = 0, Always, IfInfected };

// One row per (cause, eaten). Column meanings, and where each is read:
struct CauseRow {
  DamageCause cause;
  bool eaten;
  bool impactSevers;       // Mob::Damage: the extreme-impact-speed sever
  BleedRate bleed;         // Mob::Damage's budget and a carve's drip rate
  bool carveBleeds;        // CarveLimb: does a carve open a drip at all
  bool chargesBrain;       // CarveLimb: brain voxels lost charge hp
  bool collapseSevers;     // CarveLimb: the collapse fraction + straggler floor
  bool carriesChildren;    // CarveLimb: a child seated in a split-off piece leaves with it
  bool cutThrough;         // CarveLimb: the edge came out the other side
  JointRule joint;         // JointRuleApplies (neck + parent-side socket)
  bool hpZeroSeversAny;    // HpZeroSevers: hp 0 severs ANY limb (fire's old answer)
  bool shellHeld;          // a worn shell is exempt from the structural severs
  bool gore;               // Sever: stump wound, gout, thrown blood
  bool shellShed;          // Sever: a severed worn shell leaves as a body
  bool deathBurnt;         // deathCause_ "vital limb burnt/dissolved away"
  bool rottenVitalDetach;  // Damage: a ROTTEN vital limb at hp 0 detaches, then dies
  bool byBlade;            // SeverEvent::byBlade (the wet dismember cue)
};

// clang-format off
// Indexed [cause * 2 + eaten]; the static_asserts below pin the order.
//
//   impact  the extreme-speed sever        cut    the cut-through rule
//   bleed   drip rate                       joint  the joint rule
//   cBleed  a carve drips                   hp0    hp 0 severs any limb
//   brain   brain loss charges hp          sHeld  worn shell exempt from severs
//   coll    collapse severs                 gore   a stump spurts
//   kids    split carries seated children   shed   a severed shell drops as a body
//   burnt   "burnt/dissolved away"          rotV   rotten vital detaches at hp 0
//   blade   the wet dismember cue
//
// Reading the rows: BLUNT and UNARMED never amputate (no impact sever, no
// collapse, no carry, no joint) and a rotten body is the one exception, at a
// vital limb; an EATEN carve cauterises (no drip, no gore), consumes a
// garment (no shell exemption, nothing shed) and keeps fire's account (hp 0
// severs, joint only on infection); SPAWN ROT refuses blood, brain charge,
// the joint rule and carried children because those holes are old; only a
// BLADE cuts through and only a blade is heard as one. Bite, Beam, Blast,
// Fall and Burn are Other's rows: they differ in what callers do, not here.
// (Burn is only ever eaten — FlushBurn is its one producer — and its struck
// row is Other's struck row, which is what the old flags said.) Infection
// (PLAN_weapon_coats B1) is Burn's rows under its own ledger name: an
// infection other than the rot is only ever eaten, through FlushBurn.
inline constexpr CauseRow kCauseRows[] = {
  // cause                eaten  impact bleed               cBleed brain  coll   kids   cut    joint                  hp0    sHeld  gore   shed   burnt  rotV   blade
  {DamageCause::Other,    false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Other,    true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
  {DamageCause::Blade,    false, true,  BleedRate::Full,    true,  true,  true,  true,  true,  JointRule::Always,     false, true,  true,  true,  false, false, true},
  {DamageCause::Blade,    true,  true,  BleedRate::Full,    false, true,  true,  true,  true,  JointRule::IfInfected, true,  false, false, false, true,  false, true},
  {DamageCause::Blunt,    false, false, BleedRate::Blunt,   true,  true,  false, false, false, JointRule::Never,      false, true,  true,  true,  false, true,  false},
  {DamageCause::Blunt,    true,  false, BleedRate::Blunt,   false, true,  false, false, false, JointRule::Never,      false, false, false, false, true,  true,  false},
  {DamageCause::Bite,     false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Bite,     true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
  {DamageCause::Beam,     false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Beam,     true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
  {DamageCause::Blast,    false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Blast,    true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
  {DamageCause::Unarmed,  false, false, BleedRate::Unarmed, true,  true,  false, false, false, JointRule::Never,      false, true,  true,  true,  false, true,  false},
  {DamageCause::Unarmed,  true,  false, BleedRate::Unarmed, false, true,  false, false, false, JointRule::Never,      false, false, false, false, true,  true,  false},
  {DamageCause::Burn,     false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Burn,     true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
  {DamageCause::SpawnRot, false, true,  BleedRate::Full,    false, false, true,  false, false, JointRule::Never,      false, true,  true,  true,  false, false, false},
  {DamageCause::SpawnRot, true,  true,  BleedRate::Full,    false, false, true,  false, false, JointRule::Never,      true,  false, false, false, true,  false, false},
  {DamageCause::Fall,     false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Fall,     true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
  {DamageCause::Infection,false, true,  BleedRate::Full,    true,  true,  true,  true,  false, JointRule::Always,     false, true,  true,  true,  false, false, false},
  {DamageCause::Infection,true,  true,  BleedRate::Full,    false, true,  true,  true,  false, JointRule::IfInfected, true,  false, false, false, true,  false, false},
};
// clang-format on

constexpr bool CauseRowsInOrder() {
  if (sizeof(kCauseRows) / sizeof(kCauseRows[0]) !=
      (size_t)DamageCause::Count * 2)
    return false;
  for (size_t i = 0; i < (size_t)DamageCause::Count * 2; i++)
    if ((size_t)kCauseRows[i].cause != i / 2 || kCauseRows[i].eaten != (i & 1))
      return false;
  return true;
}
static_assert(CauseRowsInOrder(),
              "kCauseRows must hold (cause, eaten) for every DamageCause, in "
              "enum order, struck before eaten");

constexpr const CauseRow& CauseRowOf(const DamageCtx& ctx) {
  const size_t c = (size_t)ctx.cause < (size_t)DamageCause::Count
                       ? (size_t)ctx.cause
                       : (size_t)DamageCause::Other;
  return kCauseRows[c * 2 + (ctx.eaten ? 1 : 0)];
}

// The row resolved against one limb's tissue: every answer a sever rule reads.
struct SeverPolicy {
  bool impactSevers = false;
  BleedRate hitBleed = BleedRate::None;  // Mob::Damage's bleed budget
  bool carveBleeds = false;              // a carve opens a drip (at row.bleed)
  BleedRate carveBleed = BleedRate::None;
  bool chargesBrain = false;
  bool collapseSevers = false;
  bool carriesChildren = false;
  bool cutThrough = false;
  JointRule joint = JointRule::Never;
  bool hpZeroSeversAny = false;
  bool shellStaysOn = false;  // this limb is a garment the structural severs skip
  bool rooted = false;        // hair: never leaves whole, only piece by piece
  bool gore = false;
  bool adopt = true;          // a severed slot leaves as a body
  bool deathBurnt = false;
  bool vitalHpZeroDetaches = false;
  bool byBlade = false;
};

constexpr bool TissueHasBlood(Tissue t) {
  return t == Tissue::Flesh || t == Tissue::Rotten;
}

constexpr SeverPolicy SeverPolicyOf(const DamageCtx& ctx, Tissue t) {
  const CauseRow& r = CauseRowOf(ctx);
  const bool blood = TissueHasBlood(t);
  SeverPolicy p;
  // A STRAP DOES NOT SNAP BECAUSE THE BLOW WAS FAST (Mob::Damage).
  p.impactSevers = r.impactSevers && t != Tissue::Shell;
  p.hitBleed = blood ? r.bleed : BleedRate::None;
  p.carveBleeds = r.carveBleeds && blood;
  p.carveBleed = p.carveBleeds ? r.bleed : BleedRate::None;
  p.chargesBrain = r.chargesBrain;
  p.collapseSevers = r.collapseSevers;
  p.carriesChildren = r.carriesChildren;
  p.cutThrough = r.cutThrough;
  p.joint = r.joint;
  p.hpZeroSeversAny = r.hpZeroSeversAny;
  p.shellStaysOn = t == Tissue::Shell && r.shellHeld;
  // HAIR IS ROOTED ALL OVER THE SCALP, NOT AT A JOINT (2026-09-25). A hair
  // piece is `severable` only so that losing it is not a death (HpZeroSevers),
  // and that flag used to opt it into every whole-limb sever written for an
  // arm: the one-point "hanging by a thread" test at its anchor cell, the
  // cut-through, the impact-speed snap and fire's "hp 0 severs". So a sword
  // nick at the anchor, or a burn, dropped the whole mass like a wig (the
  // owner's report). Rooted, it is cut and burnt PIECE BY PIECE: the carve
  // chips it, a piece a blade parts from the rest falls on its own
  // (CarveLimb's split keeps the LARGEST component for a rooted limb), and
  // what stays keeps riding the head. Only the Jolt floor
  // (kMinFragmentVoxels) and its parent leaving still take it off.
  p.rooted = t == Tissue::Bloodless;
  if (p.rooted) {
    p.impactSevers = false;
    p.cutThrough = false;
    p.joint = JointRule::Never;
    p.hpZeroSeversAny = false;
  }
  p.gore = r.gore && blood;
  p.adopt = t != Tissue::Shell || r.shellShed;
  p.deathBurnt = r.deathBurnt;
  p.vitalHpZeroDetaches = t == Tissue::Rotten && r.rottenVitalDetach;
  p.byBlade = r.byBlade;
  return p;
}

// sidecar `tissue` <-> Tissue, for the two values a BODY may author (a slot's
// Shell/Bloodless come from the rig, not from this key).
inline const char* TissueName(Tissue t) {
  switch (t) {
    case Tissue::Flesh: return "flesh";
    case Tissue::Rotten: return "rotten";
    case Tissue::Bloodless: return "bloodless";
    case Tissue::Shell: return "shell";
    default: return "?";
  }
}
