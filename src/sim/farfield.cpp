#include "sim/farfield.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {
constexpr int kHyst = 2;  // level-chunk hysteresis, same feel as Stream
// Player fine-chunk coord -> this level's chunk coord (arithmetic shift =
// floor division; one level-k chunk = 16 cells of 2^(k+kFarShiftBase) fine
// voxels = 2^(k+kFarShiftBase) fine chunks; k here is the 0-based index).
IVec3 LevelChunk(IVec3 fineChunk, uint32_t k) {
  int s = (int)(k + 1 + kFarShiftBase);
  return {fineChunk.x >> s, fineChunk.y >> s, fineChunk.z >> s};
}
// centered window origin for the player's level-chunk coord
IVec3 DesiredOrigin(IVec3 fineChunk, uint32_t k) {
  IVec3 lc = LevelChunk(fineChunk, k);
  int h = (int)kFarNChunk / 2;
  return {lc.x - h, lc.y - h, lc.z - h};
}
}  // namespace

void FarField::Enqueue(uint32_t k, uint32_t slot) {
  queue_.push_back((k << 12) | slot);
  pending_[k]++;
}

void FarField::EnqueuePlane(uint32_t k, int axis, int wcoord) {
  int m = (int)kFarNChunk - 1;
  int sa = wcoord & m;
  for (int b = 0; b < (int)kFarNChunk; b++) {
    for (int a = 0; a < (int)kFarNChunk; a++) {
      int s[3];
      s[axis] = sa;
      s[(axis + 1) % 3] = a;
      s[(axis + 2) % 3] = b;
      uint32_t slot = ((uint32_t)s[2] * kFarNChunk + (uint32_t)s[1]) * kFarNChunk +
                      (uint32_t)s[0];
      Enqueue(k, slot);
    }
  }
}

void FarField::ResetLevel(uint32_t k, IVec3 desired) {
  origins_[k] = desired;
  uboDirty_ = true;
  for (uint32_t slot = 0; slot < kFarNumChunks; slot++) Enqueue(k, slot);
}

void FarField::FullRefill(IVec3 playerChunk) {
  queue_.clear();
  for (uint32_t k = 0; k < kFarLevels; k++) pending_[k] = 0;
  // coarsest first: the horizon band appears before the near bands refine
  for (int k = (int)kFarLevels - 1; k >= 0; k--)
    ResetLevel((uint32_t)k, DesiredOrigin(playerChunk, (uint32_t)k));
}

float FarField::SafeRadiusMeters() const {
  // Half-extent of cascade level k (1-based) in meters: the level's box edge
  // is kFarN << (k + kFarShiftBase) = kWorldN << k fine voxels (the shift base
  // pins box size to WINDOW edges), so half of it is
  // (kWorldN / 2) * 2^k * kVoxelMeters. Derived from world.h, never hardcoded.
  auto halfExtent = [](uint32_t k) {
    return (float)(kWorldN >> 1) * (float)(1u << k) * kVoxelMeters;
  };
  for (uint32_t k = 0; k < kFarLevels; k++) {
    if (pending_[k] == 0) continue;
    // level k+1 (1-based) is incomplete; trust out to level k's half-extent,
    // or — if even level 1 is incomplete — only the residency window itself
    // (half of kWorldN fine voxels), which is the pre-cascade draw distance.
    return k == 0 ? (float)(kWorldN >> 1) * kVoxelMeters : halfExtent(k);
  }
  return halfExtent(kFarLevels);   // everything filled: the full horizon
}

void FarField::Update(IVec3 playerChunk) {
  for (uint32_t k = 0; k < kFarLevels; k++) {
    IVec3 desired = DesiredOrigin(playerChunk, k);
    int d[3] = {desired.x - origins_[k].x, desired.y - origins_[k].y,
                desired.z - origins_[k].z};
    if (std::abs(d[0]) >= (int)kFarNChunk || std::abs(d[1]) >= (int)kFarNChunk ||
        std::abs(d[2]) >= (int)kFarNChunk) {
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
      int wcoord = dir > 0 ? *o + (int)kFarNChunk - 1 : *o;
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
    // The dispatch is encoded in THIS tick's submit, so the entry counts as
    // filled from here on — SafeRadiusMeters is read on the render path of the
    // same frame, one submit behind at worst.
    pending_[list[i] >> 12]--;
  }
  queue.WriteBuffer(world_->farList, 0, list.data(), count * 4);
  return count;
}
