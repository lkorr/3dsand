#pragma once
// ============================================================================
// WHAT DID THIS — one cause value for every body that can be damaged
// (W2-G, docs/PLAN_rule_unification_2026-09-24.md).
//
// Until 2026-09-24 the living and the dead answered this question two ways.
// DebrisSystem carried a `DamageCause` through its carve (so a severed part
// could tell a sword from a fire), while Mob answered it with six AMBIENT
// FLAGS (`inBurnFlush_`, `inSpawnRot_`, `inBladeCut_`, `inBluntCarve_`,
// `inUnarmedBlunt_`, `inBite_`) plus MobSystem's audio `bladeCut_`, each set by
// a scope guard around a damage entry point and read several calls deep by
// whichever sever rule happened to care. Every new cause re-patched every rule
// (b4daa6e, fd90f11, 904dd30, 719df2d, 9d1701d), because a flag nobody set
// read as "not me" to every rule it was not written for.
//
// Now the cause is a VALUE, passed explicitly from the entry point down
// through Mob::Damage / CarveLimb / Sever, and what each cause may do to each
// kind of tissue is one table (game/severpolicy.h). Debris uses the same enum.
//
// The first six values are debris's original enum IN ORDER; nothing persists
// or transmits a cause, but keeping them stable costs nothing.
// ============================================================================
#include <cstdint>

enum class DamageCause : uint8_t {
  Other = 0,  // no cause stated: a test, a console sever, the caster's cost
  Blade,      // an edge: the melee cut (Mob::CutLimb) and its Damage
  Blunt,      // trauma from a weapon: Mob::BluntHit and the pulp it arms
  Bite,       // a tear: Mob::BiteHit
  Beam,       // the laser
  Blast,      // an explosion's crater
  Unarmed,    // trauma from a NATURAL weapon: Blunt, at gore.unarmedBleedScale
  Burn,       // the body reaction evaluator ate it: fire, acid, rot, joint twins
  SpawnRot,   // the holes a creature was BORN with (Mob::RotAtSpawn)
  Fall,       // landing too hard (PlayerAvatar::ApplyFallDamage)
  Count
};

constexpr bool IsBluntCause(DamageCause c) {
  return c == DamageCause::Blunt || c == DamageCause::Unarmed;
}

// The cause plus the two facts that travel with it.
//
// `eaten`: this carve is Mob::FlushBurn expressing removals that were made one
// voxel at a time and batched (a burn tick, an infection step, a blunt pulp
// tick, or the pending tombstones a strike flushes before it reads the
// lattice). It is NOT a cause — a beating that dissolves over the next few
// ticks is still Blunt — and it is what the old `inBurnFlush_` meant: the
// matter was consumed, not struck, so it cauterises and it keeps fire's
// account of how a limb comes apart. Set only by FlushBurn.
//
// `severity`: 0..1, the audio's blade intensity (SeverEvent::severity). What
// MobSystem::BladeCutScope used to carry beside `bladeCut_`.
struct DamageCtx {
  DamageCause cause = DamageCause::Other;
  bool eaten = false;
  float severity = 1.0f;

  constexpr DamageCtx() = default;
  constexpr DamageCtx(DamageCause c, float sev = 1.0f)
      : cause(c), severity(sev) {}
  constexpr DamageCtx Eaten() const {
    DamageCtx e = *this;
    e.eaten = true;
    return e;
  }
};
