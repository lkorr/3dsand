#include "sim/mattable.h"

#include <cstdio>
#include <filesystem>

#include "sim/chunkstore.h"  // ReplaceFileAtomic
#include "sim/materials.h"

namespace fs = std::filesystem;

namespace {
// 'SVMT': u32 magic, u32 hash, u32 matCount, str mats[matCount],
//         u32 stainCount, str stains[stainCount]      (str = u32 len + bytes)
constexpr uint32_t kTableMagic = 0x544D5653;  // 'SVMT'

uint32_t Fnv(const void* p, size_t n, uint32_t h) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; i++) {
    h ^= b[i];
    h *= 16777619u;
  }
  return h;
}

thread_local const MatRemap* g_active = nullptr;
}  // namespace

uint32_t MaterialNameTable::Hash() const {
  uint32_t h = 2166136261u;
  const unsigned char zero = 0, mark = 0xFF;
  for (const std::string& s : mats) {
    h = Fnv(s.data(), s.size(), h);
    h = Fnv(&zero, 1, h);
  }
  h = Fnv(&mark, 1, h);
  for (const std::string& s : stains) {
    h = Fnv(s.data(), s.size(), h);
    h = Fnv(&zero, 1, h);
  }
  // 0 and 0xFFFFFFFF are the tag sentinels (MatTableSet::kUntagged/kLegacy).
  return (h == 0u || h == 0xFFFFFFFFu) ? 1u : h;
}

MaterialNameTable MaterialNameTableOf(const std::vector<MaterialDef>& mats) {
  MaterialNameTable t;
  t.mats.reserve(mats.size());
  t.stains.assign(8, std::string());
  for (const MaterialDef& m : mats) {
    t.mats.push_back(m.name);
    // Slots are shared by stain NAME across materials (materials.cpp), so the
    // first writer and every later one agree.
    if (m.stainSlot > 0 && m.stainSlot < 8 && !m.stain.empty())
      t.stains[m.stainSlot] = m.stain;
  }
  return t;
}

uint32_t MatRemap::Word(uint32_t w) const {
  const uint32_t mat = Mat(w & 0xFFFu);
  // A substance this build does not have becomes CLEAN air: no state nibble,
  // no stain left floating in an empty cell.
  if (mat == 0u && (w & 0xFFFu) != 0u) return 0u;
  w = (w & ~0xFFFu) | mat;
  const uint32_t type = (w >> 28) & 0x7u;
  if (type != 0) {
    const uint32_t nt = stain[type];
    // A stain with no slot any more is washed out entirely (amount too).
    w = nt == 0 ? (w & ~0x7F000000u) : ((w & ~0x70000000u) | (nt << 28));
  }
  return w;
}

void MatRemap::Rle(std::vector<uint32_t>& rle) const {
  if (identity) return;
  size_t o = 0;
  for (size_t i = 0; i + 1 < rle.size(); i += 2) {
    const uint32_t run = rle[i], w = Word(rle[i + 1]);
    if (o > 0 && rle[o - 1] == w) {
      rle[o - 2] += run;
      continue;
    }
    rle[o] = run;
    rle[o + 1] = w;
    o += 2;
  }
  rle.resize(o);
}

std::string MatRemap::Describe() const {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "%u material ids moved, %u unresolved (-> air); %u stain slots "
                "moved, %u unresolved (-> clean)",
                movedMats, unresolvedMats, movedStains, unresolvedStains);
  std::string s = buf;
  if (!unresolvedNames.empty()) s += "; unknown: " + unresolvedNames;
  return s;
}

MatRemap BuildMatRemap(const MaterialNameTable& saved,
                       const MaterialNameTable& running) {
  MatRemap r;
  std::unordered_map<std::string, uint16_t> idOf;
  for (size_t i = 0; i < running.mats.size(); i++)
    idOf.emplace(running.mats[i], (uint16_t)i);
  auto note = [&r](const std::string& n) {
    if (r.unresolvedNames.size() > 160) return;
    if (!r.unresolvedNames.empty()) r.unresolvedNames += ", ";
    r.unresolvedNames += "'" + n + "'";
  };
  r.mat.resize(saved.mats.size());
  for (size_t i = 0; i < saved.mats.size(); i++) {
    auto it = idOf.find(saved.mats[i]);
    uint16_t to = 0;  // air
    if (it != idOf.end()) {
      to = it->second;
    } else {
      r.unresolvedMats++;
      note(saved.mats[i]);
    }
    r.mat[i] = to;
    if (to != i) {
      r.identity = false;
      if (it != idOf.end()) r.movedMats++;
    }
  }
  // Stain slots: only when BOTH sides name them (a legacy table does not).
  if (!saved.stains.empty() && !running.stains.empty()) {
    for (uint32_t s = 1; s < 8; s++) {
      if (s >= saved.stains.size() || saved.stains[s].empty()) continue;
      uint8_t to = 0;
      bool found = false;
      for (uint32_t k = 1; k < 8 && k < running.stains.size(); k++)
        if (running.stains[k] == saved.stains[s]) {
          to = (uint8_t)k;
          found = true;
          break;
        }
      if (!found) {
        r.unresolvedStains++;
        note("stain " + saved.stains[s]);
      } else if (to != s) {
        r.movedStains++;
      }
      r.stain[s] = to;
      if (to != s) r.identity = false;
    }
  }
  if (r.identity) r.mat.clear();  // Mat() passes everything through
  return r;
}

const MatRemap* ActiveLoadRemap() { return g_active; }

ScopedLoadRemap::ScopedLoadRemap(const MatRemap* r) : prev_(g_active) {
  g_active = (r && !r->identity) ? r : nullptr;
}
ScopedLoadRemap::~ScopedLoadRemap() { g_active = prev_; }

// ---- MatTableSet -------------------------------------------------------------

void MatTableSet::SetRunning(const MaterialNameTable& t) {
  const uint32_t h = t.Empty() ? 0u : t.Hash();
  if (h == runningHash_) return;
  running_ = t;
  runningHash_ = h;
  // Every remap was built TO the old running table.
  remaps_.clear();
}

void MatTableSet::Bind(const std::string& dir) {
  dir_ = dir;
  legacy_ = MaterialNameTable{};
  haveLegacy_ = false;
  legacyPinned_ = false;
  tables_.clear();
  remaps_.clear();
  missing_.clear();
  written_.clear();
  untaggedLogged_ = false;
}

std::string MatTableSet::TablePath(const std::string& dir, uint32_t hash) {
  char name[32];
  std::snprintf(name, sizeof(name), "/mat_%08x.svmt", hash);
  return dir + name;
}
std::string MatTableSet::LegacyPath(const std::string& dir) {
  return dir + "/mat_legacy.svmt";
}
bool MatTableSet::IsTableFile(const std::string& n) {
  return n.rfind("mat_", 0) == 0 && n.size() > 5 &&
         n.compare(n.size() - 5, 5, ".svmt") == 0;
}

bool MatTableSet::WriteTableFile(const std::string& path, const MaterialNameTable& t) {
  std::vector<uint8_t> buf;
  auto u32 = [&buf](uint32_t v) {
    for (int b = 0; b < 4; b++) buf.push_back((uint8_t)(v >> (8 * b)));
  };
  auto str = [&](const std::string& s) {
    u32((uint32_t)s.size());
    buf.insert(buf.end(), s.begin(), s.end());
  };
  u32(kTableMagic);
  u32(t.Hash());
  u32((uint32_t)t.mats.size());
  for (const std::string& s : t.mats) str(s);
  u32((uint32_t)t.stains.size());
  for (const std::string& s : t.stains) str(s);
  const std::string tmp = path + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  if (!fp) return false;
  bool ok = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
  std::fclose(fp);
  if (ok) ok = ReplaceFileAtomic(tmp, path);
  std::error_code ec;
  if (!ok) fs::remove(tmp, ec);
  return ok;
}

bool MatTableSet::ReadTableFile(const std::string& path, MaterialNameTable& t) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::vector<uint8_t> buf;
  std::fseek(fp, 0, SEEK_END);
  const long len = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  buf.resize(len > 0 ? (size_t)len : 0);
  const bool readOk =
      buf.empty() || std::fread(buf.data(), 1, buf.size(), fp) == buf.size();
  std::fclose(fp);
  if (!readOk) return false;
  size_t off = 0;
  bool ok = true;
  auto u32 = [&]() -> uint32_t {
    if (!ok || buf.size() - off < 4) {
      ok = false;
      return 0;
    }
    const uint32_t v = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
                       ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
    off += 4;
    return v;
  };
  auto strs = [&](std::vector<std::string>& out) {
    const uint32_t n = u32();
    // A count the file cannot hold is corruption, not a large table.
    if (!ok || (size_t)n > (buf.size() - off) / 4) {
      ok = false;
      return;
    }
    out.resize(n);
    for (uint32_t i = 0; i < n && ok; i++) {
      const uint32_t l = u32();
      if (!ok || l > buf.size() - off) {
        ok = false;
        return;
      }
      out[i].assign((const char*)buf.data() + off, l);
      off += l;
    }
  };
  const uint32_t magic = u32(), hash = u32();
  if (!ok || magic != kTableMagic) return false;
  MaterialNameTable r;
  strs(r.mats);
  strs(r.stains);
  // The stored hash is checked against the content: a table file that does
  // not hash to what it claims would remap a region as the wrong substances.
  if (!ok || r.Hash() != hash) return false;
  t = std::move(r);
  return true;
}

void MatTableSet::AdoptLegacy(const MaterialNameTable& metaTable) {
  remaps_.erase(kLegacy);
  untaggedLogged_ = false;
  MaterialNameTable pinned;
  if (!dir_.empty() && ReadTableFile(LegacyPath(dir_), pinned)) {
    legacy_ = std::move(pinned);
    haveLegacy_ = true;
    legacyPinned_ = true;
    return;
  }
  legacy_ = metaTable;
  haveLegacy_ = !metaTable.Empty();
  legacyPinned_ = false;
  // Pin it only when it could be lost: a save rewrites meta.svm with the
  // RUNNING table, and until then meta.svm still holds this one. Equal tables
  // (the common case) cost no file.
  if (haveLegacy_ && !dir_.empty() && runningHash_ != 0 &&
      metaTable.mats != running_.mats) {
    if (WriteTableFile(LegacyPath(dir_), legacy_)) {
      legacyPinned_ = true;
      std::printf("mattable: %s's untagged files pinned to mat_legacy.svmt "
                  "(%zu names) before a save can rewrite meta.svm's table\n",
                  dir_.c_str(), legacy_.mats.size());
    } else {
      std::fprintf(stderr, "mattable: cannot write %s -- untagged files in "
                   "this dir decode correctly until the next save only\n",
                   LegacyPath(dir_).c_str());
    }
  }
}

const MaterialNameTable* MatTableSet::TableFor(uint32_t hash) {
  if (hash == kLegacy) return haveLegacy_ ? &legacy_ : nullptr;
  if (hash == kRunning || hash == runningHash_)
    return runningHash_ != 0 ? &running_ : nullptr;
  auto it = tables_.find(hash);
  if (it != tables_.end()) return &it->second;
  if (dir_.empty() || missing_.count(hash)) return nullptr;
  MaterialNameTable t;
  if (!ReadTableFile(TablePath(dir_, hash), t)) {
    missing_[hash] = true;
    std::fprintf(stderr,
                 "mattable: *** %s names material table %08x but %s is missing "
                 "or corrupt -- its blobs load with their ids AS STORED "
                 "(identity), which is only right if materials.json did not "
                 "move ***\n",
                 dir_.c_str(), hash, TablePath(dir_, hash).c_str());
    return nullptr;
  }
  return &tables_.emplace(hash, std::move(t)).first->second;
}

const MatRemap* MatTableSet::RemapFor(uint32_t hash) {
  if (runningHash_ == 0) return nullptr;             // nothing to map TO
  if (hash == kRunning || hash == runningHash_) return nullptr;  // this table
  auto it = remaps_.find(hash);
  if (it != remaps_.end()) return it->second.identity ? nullptr : &it->second;
  const MaterialNameTable* t = TableFor(hash);
  if (!t) {
    if (hash == kLegacy && !untaggedLogged_) {
      untaggedLogged_ = true;
      std::printf("mattable: %s has files with no material table and no "
                  "legacy table to read them under -- loading them as "
                  "identity (their ids as stored)\n",
                  dir_.empty() ? "(unbound store)" : dir_.c_str());
    }
    remaps_[hash] = MatRemap{};  // identity, remembered
    return nullptr;
  }
  MatRemap r = BuildMatRemap(*t, running_);
  if (!r.identity)
    std::printf("mattable: blobs under table %08x (%s) remap to the running "
                "table %08x by name: %s\n",
                hash, hash == kLegacy ? "legacy" : "tagged", runningHash_,
                r.Describe().c_str());
  const MatRemap& stored = remaps_.emplace(hash, std::move(r)).first->second;
  return stored.identity ? nullptr : &stored;
}

bool MatTableSet::EnsureRunningWritten(uint64_t* bytesOut) {
  if (runningHash_ == 0 || dir_.empty() || written_.count(runningHash_)) return true;
  const std::string path = TablePath(dir_, runningHash_);
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (!WriteTableFile(path, running_)) {
      std::fprintf(stderr, "mattable: cannot write %s\n", path.c_str());
      return false;
    }
    if (bytesOut) *bytesOut += (uint64_t)fs::file_size(path, ec);
  }
  written_[runningHash_] = true;
  return true;
}
