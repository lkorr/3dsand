#include "game/bodyreg.h"

// The walk order — debris, then mob limbs, then avatar parts — appears exactly
// three times in this file and nowhere else in the codebase. Each system's
// Append* takes the base its slots start at; the bases are derived here from
// the same counts every time, so the three arrays cannot disagree.

void BodyRegistry::BuildXforms(std::vector<BodyXformGpu>& out) const {
  debris_.BuildXforms(out);  // clears + fills [0, D)
  mobs_.AppendXforms(out);
  if (avatar_) avatar_->AppendXforms(out);
}

void BodyRegistry::BuildInstances(std::vector<BodyVoxInst>& out) {
  debris_.BuildInstances(out);  // clears + fills, slots [0, D)
  const uint32_t mobBase = debris_.BodyCount();
  mobs_.AppendInstances(out, mobBase);
  if (avatar_) avatar_->AppendInstances(out, mobBase + mobs_.LimbBodyCount());
}

void BodyRegistry::BuildMicroInsts(std::vector<MicroBodyInstGpu>& out) const {
  out.clear();
  debris_.AppendMicroInsts(out);
  const uint32_t mobBase = debris_.BodyCount();
  mobs_.AppendMicroInsts(out, mobBase);
  if (avatar_)
    avatar_->AppendMicroInsts(out, mobBase + mobs_.LimbBodyCount());
}

uint32_t BodyRegistry::TotalSlots() const {
  return debris_.BodyCount() + mobs_.LimbBodyCount() +
         (avatar_ ? avatar_->LimbBodyCount() : 0u);
}

bool BodyRegistry::AnyInstancesDirty() const {
  return debris_.InstancesDirty() || mobs_.InstancesDirty() ||
         (avatar_ && avatar_->InstancesDirty());
}
