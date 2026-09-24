#pragma once
// mattable.h — MATERIAL NAMES IN SAVES (rule-unification W1-D, 2026-09-24).
//
// A material id is its position in materials.json (materials.h: "index in
// mats == 12-bit material ID"), and every save file stores ids: the region
// files' voxel words, and the entity payloads' lattices (debris, creatures,
// ground items, worn damage, vessel fills, discovered water bodies). Before
// this file a reorder or an insertion in materials.json either REFUSED every
// existing save (meta.svm's table check) or — for anything the check did not
// cover — silently repainted it.
//
// THE CONTRACT NOW. Every persisted blob that carries material ids also
// carries the HASH of the name table it was written under (region header,
// per entity-bucket record, per section container). The tables themselves sit
// beside the save as content-addressed files, `mat_<hash>.svmt`, written
// before the first blob that names them. On read the blob's table is resolved
// against the RUNNING table by NAME and the ids are remapped on the way in,
// so RAM, the GPU and every live system only ever hold running ids.
//
// WHY PER-BLOB AND NOT ONE TABLE PER SAVE DIR. The store is a LIVE world
// (chunkstore.h): regions spill to disk between saves and regions nobody
// visited are never rewritten, and a region bucket keeps dormant creature
// records from earlier sessions next to ones parked this session. One dir can
// therefore hold blobs written under several tables, and a single table in
// meta.svm would decode the older ones as the wrong substances the moment a
// save rewrote it.
//
// UNTAGGED = LEGACY. Files from before this change (SVR2/SVR3 regions, SVX1
// buckets, SVE1 containers) carry no hash; they read under the dir's
// `mat_legacy.svmt`, or, if that does not exist yet, under meta.svm's inline
// name table — which was the table they were written under, because until now
// a load refused anything but an APPEND to it. LoadWorld pins that table into
// `mat_legacy.svmt` before a save could rewrite meta.svm with a different one.
// A legacy table has no stain-slot names (meta.svm never carried them), so
// stain types in untagged files read as identity; a blob with no resolvable
// table at all loads as identity and says so.
//
// STAIN TYPES ARE PART OF THE TABLE. The voxel word's stain type (bits 28..30)
// is a palette SLOT, assigned at load in materials.json file order
// (materials.h stainSlot), so the same reorder that moves an id can move a
// slot. The table records slot -> stain name alongside id -> material name.
//
// THE AMBIENT REMAP. The entity payloads are opaque bytes owned by their
// systems and read through EntitySection callbacks whose signatures many gates
// call directly. Rather than thread a table through every one of them, the
// loader wraps each blob's apply in a ScopedLoadRemap and the few places that
// read a lattice or a material field call ActiveLoadRemap(). Outside a load
// scope it is null — identity — which is exactly right for a network handoff,
// whose peers run the same content.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct MaterialDef;

// The name table one blob was written under.
struct MaterialNameTable {
  std::vector<std::string> mats;    // id order; [0] is "air"
  // Stain palette slot order, [0] = "" (clean). EMPTY = unknown (a legacy
  // table from meta.svm): stain types then read as identity.
  std::vector<std::string> stains;
  bool Empty() const { return mats.empty(); }
  // FNV-1a 32 over both lists (NUL-separated, a marker between). Never 0:
  // 0 is "untagged" in every file header that carries one.
  uint32_t Hash() const;
};
// The running build's table, from the loaded materials.
MaterialNameTable MaterialNameTableOf(const std::vector<MaterialDef>& mats);

// saved id -> running id, and saved stain slot -> running stain slot.
struct MatRemap {
  std::vector<uint16_t> mat;  // indexed by SAVED id; ids past it pass through
  uint8_t stain[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  bool identity = true;
  // Names the running table does not have. Their voxels become air (a stain
  // with no slot becomes clean): the load goes ahead and says so, the same
  // rule PLYR/ITMS follow for an item name that no longer resolves.
  uint32_t unresolvedMats = 0, unresolvedStains = 0;
  uint32_t movedMats = 0, movedStains = 0;
  std::string unresolvedNames;  // for the log line, capped

  uint32_t Mat(uint32_t id) const { return id < mat.size() ? mat[id] : id; }
  // A grid word: material bits 0..11 and stain type bits 28..30 (a stain
  // whose type maps to 0 also loses its amount: "no stain" has one spelling).
  // A material the running table lacks becomes a clean air word (0).
  uint32_t Word(uint32_t w) const;
  // DebrisVoxel::payload (material | state << 12).
  uint16_t Payload(uint16_t p) const {
    return (uint16_t)((p & 0xF000u) | Mat(p & 0xFFFu));
  }
  // A body coat word (voxload.h PackBodyStain: material | amount << 12).
  uint16_t BodyStain(uint16_t s) const {
    return s == 0 ? (uint16_t)0 : (uint16_t)((s & 0xF000u) | Mat(s & 0xFFFu));
  }
  // An RLE (run, word) chunk, in place; adjacent runs that now carry the same
  // word are merged so the result is canonical (RleEncodeChunk's form).
  void Rle(std::vector<uint32_t>& rle) const;
  // One line: what moved, what did not resolve.
  std::string Describe() const;
};
MatRemap BuildMatRemap(const MaterialNameTable& saved,
                       const MaterialNameTable& running);

// Lattice helpers, templated so this header needs neither voxload.h nor
// physics.h. PrefabVoxel: `material` + `stain`; DebrisVoxel: `payload` + `stain`.
template <class V>
void RemapPrefabVoxels(std::vector<V>& v, const MatRemap& r) {
  for (V& x : v) {
    x.material = (uint16_t)r.Mat(x.material);
    x.stain = r.BodyStain(x.stain);
  }
}
template <class V>
void RemapDebrisVoxels(std::vector<V>& v, const MatRemap& r) {
  for (V& x : v) {
    x.payload = r.Payload(x.payload);
    x.stain = r.BodyStain(x.stain);
  }
}

// ---- the ambient load remap (see the header note) --------------------------
// Null outside a load scope and for an identity table.
const MatRemap* ActiveLoadRemap();
class ScopedLoadRemap {
 public:
  explicit ScopedLoadRemap(const MatRemap* r);
  ~ScopedLoadRemap();
  ScopedLoadRemap(const ScopedLoadRemap&) = delete;
  ScopedLoadRemap& operator=(const ScopedLoadRemap&) = delete;

 private:
  const MatRemap* prev_;
};

// ---- the save dir's tables -------------------------------------------------
//
// Owned by ChunkStore (one per store binding). Knows the RUNNING table, the
// dir's untagged (legacy) table, and caches every table a blob has named.
class MatTableSet {
 public:
  // TWO SPELLINGS OF "WHICH TABLE", one per side of the file boundary:
  //   ON DISK (region/container headers, bucket records): 0 = untagged, a
  //     pre-W1-D blob; anything else = the hash of the table it names.
  //   IN RAM (EntityRecord::matTable, and what RemapFor/TableFor take):
  //     0 = THIS PROCESS's running table -- the default, so a record built
  //     from live state (a save, a creature parked this session) needs no
  //     one to remember to tag it; kLegacy = untagged on disk; else a hash.
  // Hash() is never 0 or kLegacy. FromDisk/ToDisk are the only conversions.
  static constexpr uint32_t kUntagged = 0;
  static constexpr uint32_t kRunning = 0;
  static constexpr uint32_t kLegacy = 0xFFFFFFFFu;
  static uint32_t FromDisk(uint32_t t) { return t == kUntagged ? kLegacy : t; }
  uint32_t ToDisk(uint32_t t) const {
    return t == kRunning ? runningHash_ : t == kLegacy ? kUntagged : t;
  }

  void SetRunning(const MaterialNameTable& t);
  const MaterialNameTable& Running() const { return running_; }
  // 0 when no running table was ever set (a bare ChunkStore in a gate): then
  // every write is untagged and every read is identity, the pre-W1-D shape.
  // Otherwise never 0 (Hash()).
  uint32_t RunningHash() const { return runningHash_; }

  // A new binding: forget what was cached and written for the old dir.
  void Bind(const std::string& dir);
  // The dir's files were all just deleted (BindSave from unbound).
  void OnWiped() { written_.clear(); legacyPinned_ = false; }

  // LoadWorld, after reading meta.svm: the table untagged files read under.
  // `mat_legacy.svmt` wins if it exists (it pinned the table in force when a
  // W1-D build first opened this dir); otherwise `metaTable` is adopted and,
  // when it differs from the running table, pinned to that file so a save
  // that rewrites meta.svm cannot lose it.
  void AdoptLegacy(const MaterialNameTable& metaTable);

  // The remap for a blob whose RAM tag is `hash`; null for identity (the
  // running table), for a legacy blob with no known legacy table, and for a
  // table file that is missing or corrupt (each logged once per binding).
  const MatRemap* RemapFor(uint32_t hash);
  // The table a hash names, if known (running, legacy or read from the dir).
  const MaterialNameTable* TableFor(uint32_t hash);

  // Write mat_<RunningHash>.svmt into the dir if this binding has not already.
  // Called before every tagged write. False on a write failure.
  bool EnsureRunningWritten(uint64_t* bytesOut = nullptr);

  static std::string TablePath(const std::string& dir, uint32_t hash);
  static std::string LegacyPath(const std::string& dir);
  static bool WriteTableFile(const std::string& path, const MaterialNameTable& t);
  static bool ReadTableFile(const std::string& path, MaterialNameTable& t);
  // True for every file name this class writes (ChunkStore's wipe list).
  static bool IsTableFile(const std::string& fileName);

 private:
  std::string dir_;
  MaterialNameTable running_;
  uint32_t runningHash_ = 0;
  MaterialNameTable legacy_;
  bool haveLegacy_ = false;
  bool legacyPinned_ = false;
  std::unordered_map<uint32_t, MaterialNameTable> tables_;  // read from dir
  std::unordered_map<uint32_t, MatRemap> remaps_;
  std::unordered_map<uint32_t, bool> missing_;  // logged already
  std::unordered_map<uint32_t, bool> written_;
  bool untaggedLogged_ = false;
};
