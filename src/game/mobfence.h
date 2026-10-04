// mobfence.h — a predicate the walk drive and the attack seam ask before a
// creature MOVES or STRIKES somewhere (docs/PLAN_demons.md D1).
//
// WHY A HOOK AND NOT A WALL. A demon summoned into an intact salt circle is
// CONTAINED: it may not walk out of the circle and it may not land a blow
// across it. The circle is not a physical wall -- the player and items cross
// it freely -- so it cannot be geometry, and the rule is the demon system's
// (game/demon.h), not the creature system's. MobSystem asks; the owner
// answers. With no fence installed (every world without a contained demon)
// the cost is one null test per move attempt.
//
// The answer must be a pure function of tick state (rule 1): the demon
// system answers from its own circle masks, which are built from the T-4
// snapshot stores.
#pragma once

#include <cstdint>

struct MobFence {
  // Move / Blow: the walk drive and the attack seam ASK before acting (and a
  // refusal is counted). Reach / Cast (demons D6 item 10) are the SCORER's
  // questions, asked once per think by MobSystem::DecideIntent and never
  // counted: could this body reach a point at all (a blow or a step there),
  // and would a spell it cast at that point get out? A contained demon whose
  // target is past its salt does not score Approach / HoldRange / Circle /
  // Attack -- it would only grind on the ring -- and does not try a cast the
  // seals stop at the ring (ai_behavior.h SelfView::targetFenced, castFenced).
  enum Kind : uint8_t { Move = 0, Blow = 1, Reach = 2, Cast = 3 };
  // May mob `mobId`, at (fromX, fromZ), move its footprint centre to / aim a
  // blow at (toX, toZ)? World voxels. Null = no fence: always yes.
  bool (*allow)(void* ctx, uint64_t mobId, Kind kind, float fromX, float fromZ, float toX,
                float toZ) = nullptr;
  void* ctx = nullptr;
  bool Allows(uint64_t mobId, Kind kind, float fromX, float fromZ, float toX, float toZ) const {
    return allow == nullptr || allow(ctx, mobId, kind, fromX, fromZ, toX, toZ);
  }
};

