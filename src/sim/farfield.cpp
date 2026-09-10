#include "sim/farfield.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "sim/faredits.h"

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
  queue_.push_back((k << kFarSlotShift) | slot);
  pending_[k]++;
}

void FarField::EnqueuePlane(uint32_t k, int axis, int wcoord) {
  int m = (int)kFarNChunk - 1;
  int sa = wcoord & m;
  // The incoming plane is the box's high face when the origin stepped toward
  // +axis (wcoord = o + N - 1) and its low face otherwise (wcoord = o).
  const int oa = axis == 0 ? origins_[k].x : axis == 1 ? origins_[k].y
                                                     : origins_[k].z;
  const int side = wcoord == oa + (int)kFarNChunk - 1 ? 1 : 0;
  recs_.push_back({k, axis, side, kFarNChunk * kFarNChunk, epoch_[k]});
  faces_[k][axis][side]++;
  uboDirty_ = true;
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
  bulkPending_[k] += kFarNumChunks;
  // Every plane record queued for this level is now meaningless — the whole
  // level is invalid until the reset lands — so it stops owning a face.
  epoch_[k]++;
  for (int a = 0; a < 3; a++) faces_[k][a][0] = faces_[k][a][1] = 0;
  recs_.push_back({k, -1, 0, kFarNumChunks, epoch_[k]});
}

void FarField::FullRefill(IVec3 playerChunk) {
  queue_.clear();
  recs_.clear();
  for (uint32_t k = 0; k < kFarLevels; k++) {
    pending_[k] = 0;
    bulkPending_[k] = 0;
    for (int a = 0; a < 3; a++) faces_[k][a][0] = faces_[k][a][1] = 0;
  }
  // coarsest first: the horizon band appears before the near bands refine
  for (int k = (int)kFarLevels - 1; k >= 0; k--)
    ResetLevel((uint32_t)k, DesiredOrigin(playerChunk, (uint32_t)k));
}

float FarField::SafeRadiusMeters() const {
  // Half-extent of cascade level k (1-based) comes from world.h
  // (kFarHalfExtentMeters), so this tracks kFarN / kFarShiftBase / kFarLevels
  // automatically instead of restating the box-size relation.
  for (uint32_t k = 0; k < kFarLevels; k++) {
    if (pending_[k] == 0) continue;
    // level k+1 (1-based) is incomplete; trust out to level k's half-extent,
    // or — if even level 1 is incomplete — only the residency window itself,
    // which is the pre-cascade draw distance.
    const float inner = k == 0 ? kWindowHalfExtentMeters : kFarHalfExtentMeters(k);
    if (bulkPending_[k] > 0) return inner;
    // Only PLANES outstanding: the level was complete and what is missing is
    // its incoming face, one level chunk thick, on the side the player is
    // moving toward. Everything inside that face is filled, so the trusted
    // radius is the level's own half-extent less the face — under the play
    // cap a plane takes ~16 ticks to land, and fogging the whole level out
    // for that long would pulse the horizon on every chunk boundary crossed.
    const float face =
        (float)(kChunk << (k + 1 + kFarShiftBase)) * kVoxelMeters;
    return std::max(inner, kFarHalfExtentMeters(k + 1) - face);
  }
  return kFarHalfExtentMeters(kFarLevels);   // everything filled: full horizon
}

uint32_t FarField::FaceWord(uint32_t k) const {
  // Layout documented at the declaration (farfield.h) and at the reader
  // (raymarch.wgsl farBox): nibble (axis * 2 + side) holds the planes queued on
  // that face, saturated at 15; bit 24 says the whole level is pending.
  if (bulkPending_[k] > 0) return 1u << 24;
  uint32_t w = 0;
  for (int a = 0; a < 3; a++)
    for (int side = 0; side < 2; side++)
      w |= std::min(faces_[k][a][side], 15u) << (4 * (a * 2 + side));
  return w;
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

uint32_t FarField::PrepareTick(const rhi::Queue& queue) {
  if (!world_) return 0;
  if (uboDirty_) {
    FarParams fp{};
    for (uint32_t k = 0; k < kFarLevels; k++) {
      fp.origins[k][0] = origins_[k].x;
      fp.origins[k][1] = origins_[k].y;
      fp.origins[k][2] = origins_[k].z;
      fp.origins[k][3] = (int32_t)FaceWord(k);   // the render's valid box
    }
    queue.WriteBuffer(world_->farUBO, 0, &fp, sizeof(fp));
    uboDirty_ = false;
  }
  if (queue_.empty()) return 0;
  // A reset (teleport, load, the startup horizon) drains at the list cap; in
  // play, incoming planes drain at kPlayFillCap (see farfield.h).
  bool bulk = false;
  for (uint32_t k = 0; k < kFarLevels; k++) bulk = bulk || bulkPending_[k] > 0;
  const uint32_t cap = (uint32_t)std::min(
      queue_.size(), (size_t)(bulk ? kFarListCap : kPlayFillCap));
  std::vector<uint32_t> list;
  list.reserve(cap);
  patchHeader_.clear();
  patchPayload_.clear();

  FarEdits* edits = world_->farEdits;
  for (uint32_t i = 0; i < cap; i++) {
    const uint32_t packed = queue_.front();
    const uint32_t k = packed >> kFarSlotShift;        // 0-based level index
    const uint32_t slot = packed & kFarSlotMask;

    // ---- this entry's edit patches (far-field edit persistence) ----------
    // The queue names SLOTS, so the world level chunk resident in that slot is
    // resolved here under the SAME origins the kernel will read this tick —
    // the mirror of common.wgsl's farSlotToChunk. Resolving it any earlier
    // (at enqueue time) would name the chunk that used to live there.
    const std::vector<uint32_t>* patch = nullptr;
    if (edits && !edits->Empty()) {
      const int m = (int)kFarNChunk - 1;
      const IVec3 sc{(int)(slot % kFarNChunk),
                     (int)((slot / kFarNChunk) % kFarNChunk),
                     (int)(slot / (kFarNChunk * kFarNChunk))};
      const IVec3 o = origins_[k];
      const IVec3 lc{o.x + ((sc.x - o.x) & m), o.y + ((sc.y - o.y) & m),
                     o.z + ((sc.z - o.z) & m)};
      patch = edits->Lookup(k + 1, lc);
    }
    const uint32_t n = patch ? (uint32_t)patch->size() : 0u;
    // BUDGET BEFORE EMISSION (CLAUDE.md's rule for every other queue here): an
    // entry that will not fit stays queued rather than being filled with a
    // truncated patch, which would leave the horizon showing half an edit and
    // no record that it did. One entry can hold at most kChunkVol = 4096
    // words, far under the cap, so this can never deadlock.
    if (patchPayload_.size() + n > kFarPatchCap) break;

    patchHeader_.push_back((uint32_t)patchPayload_.size());
    patchHeader_.push_back(n);
    if (n) patchPayload_.insert(patchPayload_.end(), patch->begin(), patch->end());

    list.push_back(packed);
    queue_.pop_front();
    // The dispatch is encoded in THIS tick's submit, so the entry counts as
    // filled from here on — SafeRadiusMeters is read on the render path of the
    // same frame, one submit behind at worst.
    pending_[k]--;
    if (bulkPending_[k] > 0) bulkPending_[k]--;  // FIFO: resets pop first
    // The plane this entry belongs to is the front record (same FIFO). Its
    // face is released when its last entry is dispatched — published next
    // tick, after this tick's sieve is in the queue.
    PlaneRec& r = recs_.front();
    if (--r.remaining == 0) {
      if (r.axis >= 0 && r.epoch == epoch_[r.level] &&
          faces_[r.level][r.axis][r.side] > 0) {
        faces_[r.level][r.axis][r.side]--;
        uboDirty_ = true;
      }
      recs_.pop_front();
    }
  }
  const uint32_t count = (uint32_t)list.size();
  if (count == 0) return 0;   // first entry alone blew the budget: cannot happen
  queue.WriteBuffer(world_->farList, 0, list.data(), count * 4);
  // The header is written for EVERY dispatched entry, always — the kernel
  // indexes it by dispatch index and a stale pair points the patch loop at
  // another entry's payload. 8 bytes per entry, so <= 32 KiB in the fullest
  // tick a full refill can produce, and zero bytes in a tick with no fills.
  queue.WriteBuffer(world_->farPatch, 0, patchHeader_.data(), count * 8);
  if (!patchPayload_.empty())
    queue.WriteBuffer(world_->farPatch, (uint64_t)kFarPatchBase * 4,
                      patchPayload_.data(), patchPayload_.size() * 4);
  lastPatchWords_ = (uint32_t)patchPayload_.size();
  return count;
}
