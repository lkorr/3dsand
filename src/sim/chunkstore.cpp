#include "sim/chunkstore.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include <zstd.h>

#include "sim/stream.h"  // kPersistMask, RleEncodeChunk

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

bool ReplaceFileAtomic(const std::string& from, const std::string& to) {
#ifdef _WIN32
  // Through fs::path for the UTF-8 -> UTF-16 conversion the rest of the store
  // gets from std::filesystem.
  const fs::path f(from), t(to);
  return MoveFileExW(f.c_str(), t.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  fs::rename(from, to, ec);  // rename(2): replaces `to` atomically
  return !ec;
#endif
}

namespace {
// 'SVR2' — bumped from 'SVR1' when the persisted voxel word widened from
// 16 to 32 bits to carry the stain layer (see RleEncodeChunk). An old
// 'SVR1' file is REJECTED rather than misread: its 16-bit payload would
// decode as garbage runs at the wrong stride. SVR2 is still READ (never
// written): a pre-S3 save loads unchanged, and each region is rewritten as
// SVR3 the next time it goes dirty.
//
//   SVR2: u32 magic, u32 count; per chunk: i32 wc[3], u32 pairs,
//         u32 rle[pairs*2]
constexpr uint32_t kRegionMagicV2 = 0x32525653;  // 'SVR2'
// 'SVR3' (docs/PLAN_save_system.md S3): per-chunk records, each independently
// decodable, so a chunk is still one record away.
//
//   SVR3: u32 magic, u32 count, u32 seed; per chunk: i32 wc[3],
//         u32 len | codec << 28, u8 payload[len]
//
// `seed` is the one the planes' jitter residue was computed with (see
// ChunkStore::EncodeRecord). Codec values are ChunkStore::Codec; an unknown
// one stops the read at that entry — refuse rather than guess.
constexpr uint32_t kRegionMagicV3 = 0x33525653;  // 'SVR3'
constexpr uint32_t kRecLenMask = 0x0FFFFFFFu;
constexpr uint32_t kRecCodecShift = 28;
constexpr size_t kPlaneBytes = (size_t)kChunkVol * 4;  // four byte planes

// One zstd context pair per thread, reused across chunks: creating a CCtx is
// a large allocation that would dominate a 16 KiB compress.
struct ZstdCtx {
  ZSTD_CCtx* c = ZSTD_createCCtx();
  ZSTD_DCtx* d = ZSTD_createDCtx();
  ~ZstdCtx() {
    ZSTD_freeCCtx(c);
    ZSTD_freeDCtx(d);
  }
};
ZstdCtx& Zstd() {
  thread_local ZstdCtx z;
  return z;
}

// region files and the save meta are the only files this code ever deletes
bool IsOurFile(const fs::path& p) {
  std::string name = p.filename().string();
  // manifest.svt joins the list for the reason the wipe exists at all: a
  // BindSave means "this RAM store is the whole world", and a tick manifest
  // left behind by an earlier session would claim freshness for chunks that
  // are no longer there (M9.5-A).
  //
  // The S4 entity files join it for the same reason (PLAN_save_system.md):
  // r_*.sve buckets, world.sve and the pre-S4 entities.sve are this world's
  // entities, and a BindSave of a new world under an old name must not
  // inherit a previous session's creatures beside its terrain. (players/ is
  // wiped separately in BindSave: it is a directory.)
  return (name.rfind("r_", 0) == 0 &&
          (p.extension() == ".svr" || p.extension() == ".sve")) ||
         name == "meta.svm" || name == "manifest.svt" || name == "world.sve" ||
         name == "entities.sve";
}

// 'SVX1': one region's entity bucket (chunkstore.h EntityRecord).
constexpr uint32_t kEntityRegionMagic = 0x31585653;  // 'SVX1'

uint64_t Fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h ? h : 1;  // 0 is reserved for "no file"
}
}  // namespace

std::string ChunkStore::RegionPath(IVec3 rc) const {
  return dir_ + "/r_" + std::to_string(rc.x) + "_" + std::to_string(rc.y) +
         "_" + std::to_string(rc.z) + ".svr";
}

ChunkStore::Region& ChunkStore::Touch(IVec3 rc) {
  // A region someone is about to write into is no longer "known absent".
  absentRegions_.erase(World::PackChunkKey(rc));
  Region& r = regions_[World::PackChunkKey(rc)];
  r.rc = rc;
  r.lastUse = ++useCounter_;
  return r;
}

// The state-nibble predictor is the palette variant worldgen gives a non-air
// cell: world.h's JitterStateFor, through its row form JitterRowSeed /
// JitterStateInRow — the pair RleEncodeSentinelChunk uses and page-roundtrip
// checks against the definition. Not a copy of the rule. Planes are in the
// chunk's linear cell order (x fastest, then y, then z), RleEncodeChunk's.
ChunkStore::Codec ChunkStore::EncodeRecord(const uint32_t* rle, size_t pairs,
                                           IVec3 wc, uint32_t seed,
                                           std::vector<uint8_t>& out,
                                           int level) {
  const auto raw = [&]() {
    out.resize(pairs * 8);
    if (pairs) std::memcpy(out.data(), rle, pairs * 8);
    return Codec::RawRle;
  };
  // A single run is 8 bytes raw; no zstd frame beats that.
  if (pairs <= 1) return raw();
  thread_local std::vector<uint32_t> words(kChunkVol);
  thread_local std::vector<uint8_t> planes(kPlaneBytes);
  uint32_t i = 0;
  for (size_t p = 0; p < pairs; p++) {
    const uint32_t run = rle[p * 2], w = rle[p * 2 + 1];
    // Anything the planes cannot carry is stored verbatim, never altered:
    // a malformed run structure, or bits outside kPersistMask.
    if (run == 0 || run > kChunkVol - i || (w & ~kPersistMask) != 0u)
      return raw();
    std::fill_n(words.data() + i, run, w);
    i += run;
  }
  if (i != kChunkVol) return raw();

  uint8_t* matLo = planes.data();
  uint8_t* matHi = matLo + kChunkVol;
  uint8_t* state = matHi + kChunkVol;
  uint8_t* stain = state + kChunkVol;
  const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
            bz = wc.z * (int)kChunk;
  uint32_t k = 0;
  for (int lz = 0; lz < (int)kChunk; lz++)
    for (int ly = 0; ly < (int)kChunk; ly++) {
      const uint32_t rowSeed = JitterRowSeed(by + ly, bz + lz, seed);
      for (int lx = 0; lx < (int)kChunk; lx++, k++) {
        const uint32_t w = words[k];
        const uint32_t mat = w & 0xFFFu;
        const uint32_t pred =
            mat == kMatAir ? 0u : JitterStateInRow(rowSeed, bx + lx, seed);
        matLo[k] = (uint8_t)(mat & 0xFFu);
        matHi[k] = (uint8_t)(mat >> 8);
        state[k] = (uint8_t)(((w >> 12) & 0xFu) ^ pred);
        stain[k] = (uint8_t)(w >> 24);
      }
    }
  const size_t bound = ZSTD_compressBound(kPlaneBytes);
  out.resize(bound);
  const size_t n = ZSTD_compressCCtx(Zstd().c, out.data(), bound,
                                     planes.data(), kPlaneBytes, level);
  if (ZSTD_isError(n) || n >= pairs * 8) return raw();
  out.resize(n);
  return Codec::ZstdPlanes;
}

bool ChunkStore::DecodeRecord(Codec codec, const uint8_t* data, size_t len,
                              IVec3 wc, uint32_t seed,
                              std::vector<uint32_t>& rle) {
  if (codec == Codec::RawRle) {
    // The SVR2 entry rule, unchanged: 1..kChunkVol pairs. The run structure
    // itself is validated where it is consumed (RleDecodeChunk), as before.
    if (len == 0 || len % 8 != 0 || len / 8 > kChunkVol) return false;
    rle.resize(len / 4);
    std::memcpy(rle.data(), data, len);
    return true;
  }
  if (codec != Codec::ZstdPlanes) return false;
  thread_local std::vector<uint32_t> words(kChunkVol);
  thread_local std::vector<uint8_t> planes(kPlaneBytes);
  const size_t n =
      ZSTD_decompressDCtx(Zstd().d, planes.data(), kPlaneBytes, data, len);
  if (ZSTD_isError(n) || n != kPlaneBytes) return false;
  const uint8_t* matLo = planes.data();
  const uint8_t* matHi = matLo + kChunkVol;
  const uint8_t* state = matHi + kChunkVol;
  const uint8_t* stain = state + kChunkVol;
  const int bx = wc.x * (int)kChunk, by = wc.y * (int)kChunk,
            bz = wc.z * (int)kChunk;
  uint32_t k = 0;
  for (int lz = 0; lz < (int)kChunk; lz++)
    for (int ly = 0; ly < (int)kChunk; ly++) {
      const uint32_t rowSeed = JitterRowSeed(by + ly, bz + lz, seed);
      for (int lx = 0; lx < (int)kChunk; lx++, k++) {
        if (matHi[k] > 0xFu || state[k] > 0xFu) return false;
        const uint32_t mat = (uint32_t)matLo[k] | ((uint32_t)matHi[k] << 8);
        const uint32_t pred =
            mat == kMatAir ? 0u : JitterStateInRow(rowSeed, bx + lx, seed);
        words[k] = mat | (((uint32_t)state[k] ^ pred) << 12) |
                   ((uint32_t)stain[k] << 24);
      }
    }
  RleEncodeChunk(words.data(), rle);
  return true;
}

bool ChunkStore::ReadRegionFile(
    const std::string& path,
    const std::function<void(IVec3 wc, std::vector<uint32_t>& rle)>& fn) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return true;  // absent: nothing stored there
  // Whole file in one read: regions are small once compressed, and parsing a
  // buffer makes every bounds check an explicit size compare.
  std::vector<uint8_t> buf;
  std::fseek(fp, 0, SEEK_END);
  const long size = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  if (size > 0) {
    buf.resize((size_t)size);
    if (std::fread(buf.data(), 1, buf.size(), fp) != buf.size()) buf.clear();
  }
  std::fclose(fp);
  size_t off = 0;
  const auto u32 = [&](uint32_t& v) {
    if (buf.size() - off < 4) return false;
    std::memcpy(&v, buf.data() + off, 4);
    off += 4;
    return true;
  };
  uint32_t magic = 0, count = 0, seed = 0;
  if (!u32(magic) || !u32(count) ||
      (magic != kRegionMagicV2 && magic != kRegionMagicV3) ||
      (magic == kRegionMagicV3 && !u32(seed)))
    return false;
  for (uint32_t c = 0; c < count; c++) {
    uint32_t w[3] = {}, len = 0;
    bool ok = u32(w[0]) && u32(w[1]) && u32(w[2]) && u32(len);
    const IVec3 wc{(int32_t)w[0], (int32_t)w[1], (int32_t)w[2]};
    std::vector<uint32_t> rle;
    if (ok && magic == kRegionMagicV2) {
      // SVR2: `len` is the pair count
      ok = len != 0 && len <= kChunkVol && buf.size() - off >= (size_t)len * 8;
      if (ok) {
        rle.resize((size_t)len * 2);
        std::memcpy(rle.data(), buf.data() + off, (size_t)len * 8);
        off += (size_t)len * 8;
      }
    } else if (ok) {
      const uint32_t bytes = len & kRecLenMask;
      ok = buf.size() - off >= bytes &&
           DecodeRecord((Codec)(len >> kRecCodecShift), buf.data() + off,
                        bytes, wc, seed, rle);
      off += bytes;
    }
    if (!ok) {
      std::fprintf(stderr, "chunkstore: %s truncated or corrupt at entry %u\n",
                   path.c_str(), c);
      break;
    }
    fn(wc, rle);
  }
  return true;
}

void ChunkStore::EnsureLoaded(IVec3 rc, Region& r) {
  if (r.loaded) return;
  r.loaded = true;  // even on miss/corrupt: don't retry the disk every Get
  if (dir_.empty()) return;
  const bool isRegion = ReadRegionFile(
      RegionPath(rc), [&](IVec3 wc, std::vector<uint32_t>& rle) {
        const uint64_t key = World::PackChunkKey(wc);
        if (r.chunks.count(key)) return;  // RAM copy is newer
        r.chunks[key] = {wc, std::move(rle)};
        chunkCount_++;
      });
  if (!isRegion)
    std::fprintf(stderr, "chunkstore: %s is not a region file\n",
                 RegionPath(rc).c_str());
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
  // SVR3, always (SVR2 is read-only). The seed in the header is the one the
  // records are encoded against, so decode never depends on SetSeed.
  uint32_t hdr[3] = {kRegionMagicV3, (uint32_t)r.chunks.size(), seed_};
  uint64_t bytes = sizeof(hdr);
  bool ok = std::fwrite(hdr, 4, 3, fp) == 3;
  std::vector<uint8_t> rec;
  for (const auto& [key, e] : r.chunks) {
    int32_t wc[3] = {e.wc.x, e.wc.y, e.wc.z};
    const Codec codec =
        EncodeRecord(e.rle.data(), e.rle.size() / 2, e.wc, seed_, rec);
    const uint32_t lenCodec =
        (uint32_t)rec.size() | ((uint32_t)codec << kRecCodecShift);
    ok = ok && std::fwrite(wc, 4, 3, fp) == 3 &&
         std::fwrite(&lenCodec, 4, 1, fp) == 1 &&
         (rec.empty() ||
          std::fwrite(rec.data(), 1, rec.size(), fp) == rec.size());
    bytes += 16 + rec.size();
  }
  std::fclose(fp);
  if (ok) ok = ReplaceFileAtomic(tmp, path);
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
  // NOT the per-Put linear scan PLAN_surface_flight_perf.md B5 describes:
  // the loop guard runs first, so a Put that is under budget costs one size()
  // compare and nothing else. The O(regions) scan happens only on an actual
  // eviction, and each pass of it evicts one region — so it is O(64) per
  // spill, not per Put. Left alone deliberately after re-reading it; the copy
  // at the two call sites was the real find in that item and is fixed.
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

void ChunkStore::Put(IVec3 wc, std::vector<uint32_t> rle, uint32_t tick) {
  Region& r = Touch(RegionOf(wc));
  uint64_t key = World::PackChunkKey(wc);
  // See the tag contract in the header: 0 is "unknown", and an untagged write
  // over a tagged one ERASES the tag rather than inheriting it.
  if (tick != 0)
    tickTags_[key] = {wc, tick};
  else
    tickTags_.erase(key);
  auto [it, inserted] = r.chunks.try_emplace(key);
  if (inserted) chunkCount_++;
  it->second.wc = wc;
  it->second.rle = std::move(rle);
  r.dirty = true;
  SpillOverBudget();
}

const std::vector<uint32_t>* ChunkStore::Get(IVec3 wc) {
  const IVec3 rc = RegionOf(wc);
  const uint64_t rkey = World::PackChunkKey(rc);
  auto rit = regions_.find(rkey);
  if (rit == regions_.end()) {
    // A MISS DOES NOT CREATE A REGION (see absentRegions_). Unbound, there is
    // no disk to ask; bound, the disk is asked ONCE per region per binding,
    // and only a region that actually holds chunks enters RAM.
    if (dir_.empty() || absentRegions_.count(rkey)) return nullptr;
    Region probe;
    probe.rc = rc;
    EnsureLoaded(rc, probe);   // counts what it loads into chunkCount_
    if (probe.chunks.empty()) {
      absentRegions_.insert(rkey);
      return nullptr;
    }
    rit = regions_.emplace(rkey, std::move(probe)).first;
  }
  Region& r = rit->second;
  r.lastUse = ++useCounter_;
  EnsureLoaded(rc, r);
  auto it = r.chunks.find(World::PackChunkKey(wc));
  if (it == r.chunks.end()) return nullptr;
  return &it->second.rle;
}

uint32_t ChunkStore::TickOf(IVec3 wc) const {
  auto it = tickTags_.find(World::PackChunkKey(wc));
  return it == tickTags_.end() ? 0u : it->second.tick;
}

void ChunkStore::Manifest(std::vector<std::pair<IVec3, uint32_t>>& out) {
  out.clear();
  out.reserve(chunkCount_);
  // ForEachStored visits RAM then disk and de-duplicates by chunk key, which
  // is exactly the "every stored chunk exactly once" a manifest needs — and it
  // never touches the LRU. A chunk with no entry in tickTags_ (a pre-M9.5
  // world, or a world whose manifest.svt is absent) reports 0, per the header.
  ForEachStored([&](IVec3 wc, const uint32_t*, size_t) {
    out.push_back({wc, TickOf(wc)});
  });
}

void ChunkStore::LoadManifest() {
  tickTags_.clear();
  if (dir_.empty()) return;
  FILE* fp = std::fopen(ManifestPath().c_str(), "rb");
  if (!fp) return;  // absent file = tag 0 for everything (M9.5-A)
  uint32_t hdr[2] = {};
  if (std::fread(hdr, 4, 2, fp) != 2 || hdr[0] != kManifestMagic) {
    std::fprintf(stderr, "chunkstore: %s is not a tick manifest\n",
                 ManifestPath().c_str());
    std::fclose(fp);
    return;
  }
  for (uint32_t i = 0; i < hdr[1]; i++) {
    int32_t wc[3];
    uint32_t tick = 0;
    if (std::fread(wc, 4, 3, fp) != 3 || std::fread(&tick, 4, 1, fp) != 1) {
      // Truncation is survivable here in a way it is not for a region file:
      // the tags are metadata over content that is still intact, so the
      // entries that DID read are kept and the rest fall back to "unknown".
      std::fprintf(stderr, "chunkstore: %s truncated at entry %u\n",
                   ManifestPath().c_str(), i);
      break;
    }
    const IVec3 cc{wc[0], wc[1], wc[2]};
    if (tick != 0) tickTags_[World::PackChunkKey(cc)] = {cc, tick};
  }
  std::fclose(fp);
}

bool ChunkStore::WriteManifest() {
  if (dir_.empty()) return true;
  std::error_code ec;
  if (tickTags_.empty()) {
    // Nothing tagged: remove any file from a previous session rather than
    // writing a zero-entry one, so a single-player save leaves the directory
    // exactly as it was before M9.5-A.
    fs::remove(ManifestPath(), ec);
    return true;
  }
  const std::string tmp = ManifestPath() + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  if (!fp) {
    std::fprintf(stderr, "chunkstore: cannot write %s\n", tmp.c_str());
    return false;
  }
  uint32_t hdr[2] = {kManifestMagic, (uint32_t)tickTags_.size()};
  bool ok = std::fwrite(hdr, 4, 2, fp) == 2;
  for (const auto& [key, tag] : tickTags_) {
    int32_t c[3] = {tag.wc.x, tag.wc.y, tag.wc.z};
    const uint32_t tick = tag.tick;
    ok = ok && std::fwrite(c, 4, 3, fp) == 3 &&
         std::fwrite(&tick, 4, 1, fp) == 1;
  }
  std::fclose(fp);
  if (ok) ok = ReplaceFileAtomic(tmp, ManifestPath());
  if (!ok) {
    std::fprintf(stderr, "chunkstore: failed writing %s\n",
                 ManifestPath().c_str());
    fs::remove(tmp, ec);
  }
  return ok;
}

void ChunkStore::ForEachStored(const Visitor& fn) {
  // RAM wins: a RAM entry is newer than the disk copy of the same chunk (the
  // same rule EnsureLoaded applies), so it is visited first and the disk pass
  // below skips keys already seen.
  std::unordered_map<uint64_t, char> seen;
  for (auto& [rkey, r] : regions_)
    for (const auto& [ckey, e] : r.chunks) {
      seen[ckey] = 1;
      fn(e.wc, e.rle.data(), e.rle.size() / 2);
    }
  if (dir_.empty()) return;

  std::error_code ec;
  for (const auto& de : fs::directory_iterator(dir_, ec)) {
    if (!IsOurFile(de.path())) continue;
    if (de.path().extension() != ".svr") continue;  // meta.svm is not a region
    // Same reader as EnsureLoaded (SVR2 and SVR3), its own buffer: nothing
    // here enters regions_.
    ReadRegionFile(de.path().string(),
                   [&](IVec3 wc, std::vector<uint32_t>& rle) {
                     if (seen.count(World::PackChunkKey(wc))) return;
                     fn(wc, rle.data(), rle.size() / 2);
                   });
  }
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
  // ...and the player files (S4): a new world's save must not hand a later
  // load somebody else's pack. Only *.svp, only in players/.
  for (const auto& de : fs::directory_iterator(dir + "/players", ec))
    if (de.path().extension() == ".svp") fs::remove(de.path(), ec);
  dir_ = dir;
  absentRegions_.clear();
  for (auto& [key, r] : regions_) {
    r.dirty = true;
    r.loaded = true;  // nothing on disk to merge anymore
  }
  // Entity buckets held unbound (S5b's parked NPCs in a never-saved world)
  // have no file now; the next WriteEntityRegion writes them.
  for (auto& [key, e] : entityRegions_) {
    e.diskHash = 0;
    e.loaded = true;
  }
  return true;
}

// ---- region entity buckets (chunkstore.h, PLAN_save_system.md S4) ---------

IVec3 ChunkStore::RegionOfVoxel(Vec3 p) {
  auto chunkOf = [](float v) {
    return (int)std::floor((double)v / (double)kChunk);
  };
  return RegionOf({chunkOf(p.x), chunkOf(p.y), chunkOf(p.z)});
}

std::string ChunkStore::EntityRegionPath(IVec3 rc) const {
  return dir_ + "/r_" + std::to_string(rc.x) + "_" + std::to_string(rc.y) +
         "_" + std::to_string(rc.z) + ".sve";
}

ChunkStore::EntityRegion& ChunkStore::TouchEntityRegion(IVec3 rc) {
  EntityRegion& e = entityRegions_[World::PackChunkKey(rc)];
  e.rc = rc;
  return e;
}

std::vector<ChunkStore::EntityRecord>& ChunkStore::DormantEntities(IVec3 rc) {
  EntityRegion& e = TouchEntityRegion(rc);
  if (e.loaded) return e.dormant;
  e.loaded = true;
  if (dir_.empty()) return e.dormant;
  FILE* fp = std::fopen(EntityRegionPath(rc).c_str(), "rb");
  if (!fp) return e.dormant;  // no file: nothing parked here
  std::vector<uint8_t> buf;
  std::fseek(fp, 0, SEEK_END);
  const long len = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  buf.resize(len > 0 ? (size_t)len : 0);
  const bool readOk =
      buf.empty() || std::fread(buf.data(), 1, buf.size(), fp) == buf.size();
  std::fclose(fp);
  // The hash is of what is ON DISK even when the parse below refuses it, so a
  // save with nothing to add leaves a file it could not read exactly as it
  // was -- a refused file is kept for a better build, never replaced by an
  // empty bucket.
  e.diskHash = Fnv64(buf.data(), buf.size());
  std::vector<EntityRecord> recs;
  bool ok = readOk && buf.size() >= 8;
  size_t off = 8;
  uint32_t magic = 0, count = 0;
  if (ok) {
    std::memcpy(&magic, buf.data(), 4);
    std::memcpy(&count, buf.data() + 4, 4);
    ok = magic == kEntityRegionMagic;
  }
  for (uint32_t i = 0; ok && i < count; i++) {
    if (off + 24 > buf.size()) {
      ok = false;
      break;
    }
    EntityRecord r;
    uint32_t n = 0;
    std::memcpy(&r.section, buf.data() + off, 4);
    std::memcpy(&r.version, buf.data() + off + 4, 4);
    std::memcpy(&r.pos.x, buf.data() + off + 8, 4);
    std::memcpy(&r.pos.y, buf.data() + off + 12, 4);
    std::memcpy(&r.pos.z, buf.data() + off + 16, 4);
    std::memcpy(&n, buf.data() + off + 20, 4);
    off += 24;
    if (n > buf.size() - off) {
      ok = false;
      break;
    }
    r.bytes.assign(buf.begin() + (ptrdiff_t)off,
                   buf.begin() + (ptrdiff_t)(off + n));
    off += n;
    recs.push_back(std::move(r));
  }
  if (!ok) {
    // ALL OR NOTHING: a half-read bucket would apply some of a region's
    // creatures and then, at the next save, write the survivors back as if
    // they were the whole bucket.
    std::fprintf(stderr,
                 "chunkstore: %s is not a readable entity bucket (magic %08x); "
                 "its records are ignored, and if anything live lands in that "
                 "region the file is renamed to .corrupt, never overwritten\n",
                 EntityRegionPath(rc).c_str(), magic);
    e.unreadable = true;
    return e.dormant;
  }
  e.dormant = std::move(recs);
  return e.dormant;
}

void ChunkStore::EntityRegionsKnown(std::vector<IVec3>& out) const {
  for (const auto& [key, e] : entityRegions_) out.push_back(e.rc);
}

// An unreadable bucket (EntityRegion::unreadable) is renamed to the first
// free `<path>.corrupt[N]` before a write or a delete would take its place.
// False only if the rename itself failed -- then the caller must not write
// either, or the file it could not move would be destroyed after all.
bool ChunkStore::SetAsideUnreadable(EntityRegion& e, const std::string& path) {
  if (!e.unreadable) return true;
  std::error_code ec;
  if (!fs::exists(path, ec)) {  // gone since the read: nothing to protect
    e.unreadable = false;
    return true;
  }
  std::string dst = path + ".corrupt";
  for (int i = 1; fs::exists(dst, ec) && i < 1000; i++)
    dst = path + ".corrupt" + std::to_string(i);
  fs::rename(path, dst, ec);
  if (ec) {
    std::fprintf(stderr,
                 "chunkstore: cannot set aside unreadable %s (%s); not "
                 "writing over it\n",
                 path.c_str(), ec.message().c_str());
    return false;
  }
  std::fprintf(stderr,
               "chunkstore: unreadable entity bucket %s preserved as %s\n",
               path.c_str(), dst.c_str());
  e.unreadable = false;
  return true;
}

bool ChunkStore::WriteEntityRegion(IVec3 rc,
                                   const std::vector<const EntityRecord*>& live,
                                   bool* wrote, uint64_t* bytesOut) {
  if (wrote) *wrote = false;
  if (dir_.empty()) return false;
  // Merge the file first: a region that was never read may hold dormant
  // records, and writing it from `live` alone would delete them.
  std::vector<EntityRecord>& dormant = DormantEntities(rc);
  EntityRegion& e = TouchEntityRegion(rc);
  std::vector<const EntityRecord*> all;
  all.reserve(dormant.size() + live.size());
  for (const EntityRecord& r : dormant) all.push_back(&r);
  all.insert(all.end(), live.begin(), live.end());

  const std::string path = EntityRegionPath(rc);
  std::error_code ec;
  if (all.empty()) {
    if (e.unreadable) {
      // Nothing live and nothing readable here: set the file aside rather
      // than delete it, and leave no bucket behind.
      if (!SetAsideUnreadable(e, path)) return false;
      e.diskHash = 0;
      if (wrote) *wrote = true;
      return true;
    }
    if (e.diskHash != 0) {
      fs::remove(path, ec);
      e.diskHash = 0;
      if (wrote) *wrote = true;
    }
    return true;
  }
  std::vector<uint8_t> buf;
  auto put = [&buf](const void* p, size_t n) {
    buf.insert(buf.end(), (const uint8_t*)p, (const uint8_t*)p + n);
  };
  const uint32_t hdr[2] = {kEntityRegionMagic, (uint32_t)all.size()};
  put(hdr, 8);
  for (const EntityRecord* r : all) {
    const uint32_t n = (uint32_t)r->bytes.size();
    put(&r->section, 4);
    put(&r->version, 4);
    put(&r->pos.x, 4);
    put(&r->pos.y, 4);
    put(&r->pos.z, 4);
    put(&n, 4);
    if (n) put(r->bytes.data(), n);
  }
  const uint64_t h = Fnv64(buf.data(), buf.size());
  if (h == e.diskHash) return true;  // the bucket did not change
  const std::string tmp = path + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  bool ok = fp && std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
  if (fp) std::fclose(fp);
  if (ok && !SetAsideUnreadable(e, path)) ok = false;
  if (ok) ok = ReplaceFileAtomic(tmp, path);
  if (!ok) {
    std::fprintf(stderr, "chunkstore: failed writing %s\n", path.c_str());
    fs::remove(tmp, ec);
    return false;
  }
  e.diskHash = h;
  if (wrote) *wrote = true;
  if (bytesOut) *bytesOut += buf.size();
  return true;
}

void ChunkStore::RemoveAllEntityFiles() {
  entityRegions_.clear();
  if (dir_.empty()) return;
  std::error_code ec;
  for (const auto& de : fs::directory_iterator(dir_, ec)) {
    const std::string name = de.path().filename().string();
    if (name.rfind("r_", 0) == 0 && de.path().extension() == ".sve")
      fs::remove(de.path(), ec);
  }
}

bool ChunkStore::BindLoad(const std::string& dir) {
  if (Bound() && dir_ != dir) return false;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return false;
  regions_.clear();  // the disk's copy wins wholesale
  entityRegions_.clear();
  absentRegions_.clear();
  chunkCount_ = 0;
  dir_ = dir;
  LoadManifest();  // ...and so does its manifest; absent file = all tags 0
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
  // LAST, and unconditionally (not gated on any region being dirty): the tags
  // can move without any region moving — a re-Put of identical bytes with a
  // newer tick is a legal thing for the host to do — and a manifest written
  // before the regions could name content a failed region write never
  // persisted. Same ordering argument meta.svm uses in worldio.cpp.
  if (!WriteManifest()) ok = false;
  return ok;
}
