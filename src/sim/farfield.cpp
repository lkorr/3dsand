#include "sim/farfield.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {
constexpr int kHyst = 2;  // level-chunk hysteresis, same feel as Stream
// Player fine-chunk coord -> this level's chunk coord (arithmetic shift =
// floor division; one level-k chunk = 2^k fine chunks).
IVec3 LevelChunk(IVec3 fineChunk, uint32_t k) {
  int s = (int)k + 1;
  return {fineChunk.x >> s, fineChunk.y >> s, fineChunk.z >> s};
}
// centered window origin for the player's level-chunk coord
IVec3 DesiredOrigin(IVec3 fineChunk, uint32_t k) {
  IVec3 lc = LevelChunk(fineChunk, k);
  int h = (int)kNChunk / 2;
  return {lc.x - h, lc.y - h, lc.z - h};
}
}  // namespace

void FarField::EnqueuePlane(uint32_t k, int axis, int wcoord) {
  int m = (int)kNChunk - 1;
  int sa = wcoord & m;
  for (int b = 0; b < (int)kNChunk; b++) {
    for (int a = 0; a < (int)kNChunk; a++) {
      int s[3];
      s[axis] = sa;
      s[(axis + 1) % 3] = a;
      s[(axis + 2) % 3] = b;
      uint32_t slot = ((uint32_t)s[2] * kNChunk + (uint32_t)s[1]) * kNChunk +
                      (uint32_t)s[0];
      queue_.push_back((k << 12) | slot);
    }
  }
}

void FarField::ResetLevel(uint32_t k, IVec3 desired) {
  origins_[k] = desired;
  uboDirty_ = true;
  for (uint32_t slot = 0; slot < kNumChunks; slot++)
    queue_.push_back((k << 12) | slot);
}

void FarField::FullRefill(IVec3 playerChunk) {
  queue_.clear();
  // coarsest first: the horizon band appears before the near bands refine
  for (int k = (int)kFarLevels - 1; k >= 0; k--)
    ResetLevel((uint32_t)k, DesiredOrigin(playerChunk, (uint32_t)k));
}

void FarField::Update(IVec3 playerChunk) {
  for (uint32_t k = 0; k < kFarLevels; k++) {
    IVec3 desired = DesiredOrigin(playerChunk, k);
    int d[3] = {desired.x - origins_[k].x, desired.y - origins_[k].y,
                desired.z - origins_[k].z};
    if (std::abs(d[0]) >= (int)kNChunk || std::abs(d[1]) >= (int)kNChunk ||
        std::abs(d[2]) >= (int)kNChunk) {
      ResetLevel(k, desired);  // whole window stale (teleport / load)
      continue;
    }
    for (int axis = 0; axis < 3; axis++) {
      if (std::abs(d[axis]) < kHyst) continue;
      int dir = d[axis] > 0 ? 1 : -1;
      int* o = axis == 0 ? &origins_[k].x : axis == 1 ? &origins_[k].y
                                                      : &origins_[k].z;
      *o += dir;
      uboDirty_ = true;
      // incoming plane: the window's leading face after the shift
      int wcoord = dir > 0 ? *o + (int)kNChunk - 1 : *o;
      EnqueuePlane(k, axis, wcoord);
    }
  }
}

uint32_t FarField::PrepareTick(const wgpu::Queue& queue) {
  if (!world_) return 0;
  if (uboDirty_) {
    FarParams fp{};
    for (uint32_t k = 0; k < kFarLevels; k++) {
      fp.origins[k][0] = origins_[k].x;
      fp.origins[k][1] = origins_[k].y;
      fp.origins[k][2] = origins_[k].z;
      fp.origins[k][3] = 0;
    }
    queue.WriteBuffer(world_->farUBO, 0, &fp, sizeof(fp));
    uboDirty_ = false;
  }
  if (queue_.empty()) return 0;
  uint32_t count = (uint32_t)std::min(queue_.size(), (size_t)kFarListCap);
  std::vector<uint32_t> list(count);
  for (uint32_t i = 0; i < count; i++) {
    list[i] = queue_.front();
    queue_.pop_front();
  }
  queue.WriteBuffer(world_->farList, 0, list.data(), count * 4);
  return count;
}
