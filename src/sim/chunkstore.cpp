#include "sim/chunkstore.h"

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace {
// 'SVR2' — bumped from 'SVR1' when the persisted voxel word widened from
// 16 to 32 bits to carry the stain layer (see RleEncodeChunk). An old
// 'SVR1' file is REJECTED rather than misread: its 16-bit payload would
// decode as garbage runs at the wrong stride.
constexpr uint32_t kRegionMagic = 0x32525653;  // 'SVR2'

// region files and the save meta are the only files this code ever deletes
bool IsOurFile(const fs::path& p) {
  std::string name = p.filename().string();
  return (name.rfind("r_", 0) == 0 && p.extension() == ".svr") ||
         name == "meta.svm";
}
}  // namespace

std::string ChunkStore::RegionPath(IVec3 rc) const {
  return dir_ + "/r_" + std::to_string(rc.x) + "_" + std::to_string(rc.y) +
         "_" + std::to_string(rc.z) + ".svr";
}

ChunkStore::Region& ChunkStore::Touch(IVec3 rc) {
  Region& r = regions_[World::PackChunkKey(rc)];
  r.rc = rc;
  r.lastUse = ++useCounter_;
  return r;
}

void ChunkStore::EnsureLoaded(IVec3 rc, Region& r) {
  if (r.loaded) return;
  r.loaded = true;  // even on miss/corrupt: don't retry the disk every Get
  if (dir_.empty()) return;
  FILE* fp = std::fopen(RegionPath(rc).c_str(), "rb");
  if (!fp) return;
  uint32_t hdr[2] = {};
  if (std::fread(hdr, 4, 2, fp) != 2 || hdr[0] != kRegionMagic) {
    std::fprintf(stderr, "chunkstore: %s is not a region file\n",
                 RegionPath(rc).c_str());
    std::fclose(fp);
    return;
  }
  std::vector<uint32_t> rle;
  for (uint32_t c = 0; c < hdr[1]; c++) {
    int32_t wc[3];
    uint32_t pairs = 0;
    if (std::fread(wc, 4, 3, fp) != 3 || std::fread(&pairs, 4, 1, fp) != 1 ||
        pairs == 0 || pairs > kChunkVol) {
      std::fprintf(stderr, "chunkstore: %s truncated at entry %u\n",
                   RegionPath(rc).c_str(), c);
      break;
    }
    rle.resize((size_t)pairs * 2);
    if (std::fread(rle.data(), 4, rle.size(), fp) != rle.size()) {
      std::fprintf(stderr, "chunkstore: %s truncated at entry %u\n",
                   RegionPath(rc).c_str(), c);
      break;
    }
    uint64_t key = World::PackChunkKey({wc[0], wc[1], wc[2]});
    if (r.chunks.count(key)) continue;  // RAM copy is newer
    r.chunks[key] = {{wc[0], wc[1], wc[2]}, rle};
    chunkCount_++;
  }
  std::fclose(fp);
}

bool ChunkStore::WriteRegion(IVec3 rc, Region& r, uint64_t* bytesOut) {
  // merge disk-only chunks first, or rewriting the file would drop them
  EnsureLoaded(rc, r);
  std::string path = RegionPath(rc);
  std::error_code ec;
  if (r.chunks.empty()) {
    fs::remove(path, ec);
    r.dirty = false;
    return true;
  }
  std::string tmp = path + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  if (!fp) {
    std::fprintf(stderr, "chunkstore: cannot write %s\n", tmp.c_str());
    return false;
  }
  uint32_t hdr[2] = {kRegionMagic, (uint32_t)r.chunks.size()};
  uint64_t bytes = 8;
  bool ok = std::fwrite(hdr, 4, 2, fp) == 2;
  for (const auto& [key, e] : r.chunks) {
    int32_t wc[3] = {e.wc.x, e.wc.y, e.wc.z};
    uint32_t pairs = (uint32_t)(e.rle.size() / 2);
    ok = ok && std::fwrite(wc, 4, 3, fp) == 3 &&
         std::fwrite(&pairs, 4, 1, fp) == 1 &&
         std::fwrite(e.rle.data(), 4, e.rle.size(), fp) == e.rle.size();
    bytes += 16 + e.rle.size() * 4;
  }
  std::fclose(fp);
  if (ok) {
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    ok = !ec;
  }
  if (!ok) {
    std::fprintf(stderr, "chunkstore: failed writing %s\n", path.c_str());
    fs::remove(tmp, ec);
    return false;
  }
  if (bytesOut) *bytesOut += bytes;
  r.dirty = false;
  return true;
}

void ChunkStore::SpillOverBudget() {
  if (dir_.empty()) return;  // unbound: nowhere to spill
  while (regions_.size() > kMaxRamRegions) {
    auto lru = regions_.begin();
    for (auto it = regions_.begin(); it != regions_.end(); ++it)
      if (it->second.lastUse < lru->second.lastUse) lru = it;
    if (lru->second.dirty &&
        !WriteRegion(lru->second.rc, lru->second, nullptr))
      break;  // disk full/unwritable: keep it in RAM rather than lose data
    chunkCount_ -= lru->second.chunks.size();
    regions_.erase(lru);
  }
}

void ChunkStore::Put(IVec3 wc, std::vector<uint32_t> rle) {
  Region& r = Touch(RegionOf(wc));
  uint64_t key = World::PackChunkKey(wc);
  auto [it, inserted] = r.chunks.try_emplace(key);
  if (inserted) chunkCount_++;
  it->second.wc = wc;
  it->second.rle = std::move(rle);
  r.dirty = true;
  SpillOverBudget();
}

const std::vector<uint32_t>* ChunkStore::Get(IVec3 wc) {
  IVec3 rc = RegionOf(wc);
  Region& r = Touch(rc);
  EnsureLoaded(rc, r);
  auto it = r.chunks.find(World::PackChunkKey(wc));
  if (it == r.chunks.end()) return nullptr;
  return &it->second.rle;
}

bool ChunkStore::BindSave(const std::string& dir) {
  if (Bound()) return dir_ == dir;
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    std::fprintf(stderr, "chunkstore: cannot create %s\n", dir.c_str());
    return false;
  }
  // stale region files under this name belong to some earlier session's
  // world — this RAM store is the complete current world, so wipe them
  for (const auto& de : fs::directory_iterator(dir, ec))
    if (IsOurFile(de.path())) fs::remove(de.path(), ec);
  dir_ = dir;
  for (auto& [key, r] : regions_) {
    r.dirty = true;
    r.loaded = true;  // nothing on disk to merge anymore
  }
  return true;
}

bool ChunkStore::BindLoad(const std::string& dir) {
  if (Bound() && dir_ != dir) return false;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  regions_.clear();  // the disk's copy wins wholesale
  chunkCount_ = 0;
  dir_ = dir;
  return true;
}

bool ChunkStore::Flush(size_t* regionsOut, uint64_t* bytesOut) {
  if (regionsOut) *regionsOut = 0;
  if (bytesOut) *bytesOut = 0;
  if (!Bound()) return false;
  bool ok = true;
  for (auto& [key, r] : regions_) {
    if (!r.dirty) continue;
    if (WriteRegion(r.rc, r, bytesOut)) {
      if (regionsOut) (*regionsOut)++;
    } else {
      ok = false;
    }
  }
  return ok;
}
