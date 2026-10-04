#include "sim/worldio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>

#include "gpu/resources.h"   // AssembleShaderSource: the fingerprint's WGSL
#include "sim/biomes.h"      // EnvironmentStamp: map / biomes / trees
#include "sim/solutes.h"     // CurrentSolutes: the species name table of solutes.svs
#include "sim/worldmap.h"    // ActiveMapName
#include "sim/tuning.h"      // CurrentTuning().world.mapLayer
#include "sim/tuningstamp.h" // HashOneFile, the same FNV the stamps use
#include "test/support.h"    // AssetDir(): the one asset-path chokepoint

namespace {
// 'SVM5': appended the sim TICK and the SEED (M9.5-B; worldio.h says why the
// two are worth a magic bump). 'SVM4' before it grew the voxel-size bit
// pattern + the material name table.
//
// UNLIKE THE SVM3 -> SVM4 BUMP, THIS ONE IS BACKWARD-READABLE. SVM4 changed
// the meaning of bytes an SVM3 reader would have misread, so refusing was the
// only safe answer; SVM5 only APPENDS, so every field an SVM4 file carries is
// at the same offset with the same meaning, and reading one costs nothing but
// leaving `tick`/`seed` unknown. The loader therefore accepts both magics and
// records which it saw.
//
// 'SVM6' (PLAN_save_system.md S1) appends the worldgen fingerprint after the
// pair, by the same argument: an SVM5 file is an SVM6 file that stops early.
constexpr uint32_t kMetaMagic = 0x364D5653;    // 'SVM6'
constexpr uint32_t kMetaMagicV5 = 0x354D5653;  // 'SVM5' - still loads
constexpr uint32_t kMetaMagicV4 = 0x344D5653;  // 'SVM4' - still loads
constexpr uint32_t kEntMagicV1 = 0x31455653;  // 'SVE1' - still loads (untagged)
// 'SVE2' (rule-unification W1-D): SVE1 plus, after the count, the hash of the
// material name table every section in the file was written under
// (sim/mattable.h). The sections' own bytes are unchanged.
constexpr uint32_t kEntMagic = 0x32455653;    // 'SVE2'

std::string MetaPath(const std::string& dir) { return dir + "/meta.svm"; }

// ---- solutes.svs: THE SOLUTE LAYER'S FILE (docs/PLAN_solutes.md §7.1) -------
//
// Dissolved mass is authoritative world state (hashed), and it is NOT in the
// voxel word, so it cannot ride the region files: it has its own file beside
// them, holding every chunk's solute the save knows -- the resident window's
// (captured at save time) and every chunk the window has evicted (the
// stream's kept map). One file rather than per-region records because the
// layer is sparse by design: a settled world holds a handful of entries.
//
//   u32 'SVS1', u32 speciesCount, then per species (u32 id, str name) -- the
//   NAME TABLE the cell values were written under, so a load remaps species
//   ids by name exactly as region files remap material ids; then u32 count,
//   then per chunk: u64 PackChunkKey(wc), u32 entry, u32 stall, and for a
//   page entry its kSolWordsPerPage u32 words.
//
// A save with no solute writes no file (and removes a stale one); a world dir
// without the file loads with an empty layer.
constexpr uint32_t kSolFileMagic = 0x31535653;  // 'SVS1'
std::string SolPath(const std::string& dir) { return dir + "/solutes.svs"; }
// The pre-S4 monolithic file: read (legacy load), never written, deleted by
// the first S4 save.
std::string EntPath(const std::string& dir) { return dir + "/entities.sve"; }
std::string WorldEntPath(const std::string& dir) { return dir + "/world.sve"; }
std::string PlayerDirPath(const std::string& dir) { return dir + "/players"; }
std::string PlayerPath(const std::string& dir, const std::string& id) {
  return PlayerDirPath(dir) + "/" + id + ".svp";
}

bool IsRegionSection(const EntitySection& s) {
  return s.scope == EntityScope::Region && s.saveRecords && s.loadRecord;
}
// Which container file a section's whole payload goes to. A Region section
// without the per-record pair is written whole into world.sve rather than
// silently dropped.
EntityScope FileScopeOf(const EntitySection& s) {
  if (s.scope == EntityScope::Player) return EntityScope::Player;
  if (IsRegionSection(s)) return EntityScope::Region;
  return EntityScope::World;
}

std::string FourCCStr(uint32_t id) {
  char s[5] = {(char)(id & 0xFF), (char)((id >> 8) & 0xFF),
               (char)((id >> 16) & 0xFF), (char)((id >> 24) & 0xFF), 0};
  for (char& c : s)
    if (c && (c < 32 || c > 126)) c = '?';
  return s;
}

// Read a whole file into `out`. False (with no complaint) when absent.
bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::fseek(fp, 0, SEEK_END);
  long len = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  out.resize(len > 0 ? (size_t)len : 0);
  bool ok = out.empty() || std::fread(out.data(), 1, out.size(), fp) == out.size();
  std::fclose(fp);
  return ok;
}

// ---- the SVE1 section containers (world.sve, players/<id>.svp) ------------

// tmp + rename, the region files' rule: a crash mid-write leaves the previous
// file, never half of a new one.
bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& buf) {
  const std::string tmp = path + ".tmp";
  FILE* fp = std::fopen(tmp.c_str(), "wb");
  if (!fp) return false;
  bool ok = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
  std::fclose(fp);
  std::error_code ec;
  if (ok) ok = ReplaceFileAtomic(tmp, path);
  if (!ok) std::filesystem::remove(tmp, ec);
  return ok;
}

// Every section whose FILE scope is `scope`, as one SVE1 container. False on
// a write failure. With no such section and `always` false, no file is
// written (a save with no player writes no player file).
bool WriteSectionFile(const std::string& path, const EntityIO& io,
                      EntityScope scope, bool always, uint64_t* bytes,
                      MatTableSet& tables) {
  std::vector<const EntitySection*> picked;
  for (const EntitySection& s : io.sections)
    if (FileScopeOf(s) == scope) picked.push_back(&s);
  if (picked.empty() && !always) return true;
  // The table file before the container that names it (the region rule).
  if (!tables.EnsureRunningWritten(bytes)) return false;
  std::vector<uint8_t> buf;
  ByteWriter w{buf};
  w.U32(kEntMagic);
  w.U32((uint32_t)picked.size());
  w.U32(tables.RunningHash());
  std::vector<uint8_t> payload;
  for (const EntitySection* s : picked) {
    payload.clear();
    if (s->save) s->save(payload);
    w.U32(s->id);
    w.U32(s->version);
    w.U32((uint32_t)payload.size());
    w.Bytes(payload.data(), payload.size());
  }
  if (!WriteFileAtomic(path, buf)) {
    std::fprintf(stderr, "save: failed to write %s\n", path.c_str());
    return false;
  }
  if (bytes) *bytes += buf.size();
  return true;
}

// Apply every section of one SVE1 container through the sections' WHOLE-
// payload loaders, whatever scope they are registered at (so the pre-S4
// entities.sve, which holds MOBS and ITMS whole, loads through this too).
// False when the file is absent.
bool ApplySectionFile(const std::string& path, const EntityIO& entities,
                      MatTableSet& tables) {
  std::vector<uint8_t> buf;
  if (!ReadFileBytes(path, buf)) return false;  // absent: nothing to apply
  ByteReader r{buf.data(), buf.size()};
  uint32_t magic = 0, count = 0, table = MatTableSet::kUntagged;
  if (!r.U32(magic) || (magic != kEntMagic && magic != kEntMagicV1) ||
      !r.U32(count) || (magic == kEntMagic && !r.U32(table))) {
    std::fprintf(stderr, "load: %s is corrupt (bad header)\n", path.c_str());
    return true;
  }
  // Every section's lattices read under the file's table (mattable.h).
  const ScopedLoadRemap remapScope(tables.RemapFor(MatTableSet::FromDisk(table)));
  for (uint32_t i = 0; i < count; i++) {
    uint32_t id = 0, version = 0, len = 0;
    if (!r.U32(id) || !r.U32(version) || !r.U32(len) || r.off + len > r.n) {
      std::fprintf(stderr, "load: %s truncated at section %u\n", path.c_str(), i);
      return true;
    }
    const uint8_t* payload = r.p + r.off;
    r.off += len;  // the table lets us seek past ANY section, known or not
    const EntitySection* match = nullptr;
    for (const EntitySection& s : entities.sections)
      if (s.id == id) match = &s;
    if (!match || !match->load) {
      // Forward compatibility: a newer build's section is skipped, loudly.
      std::printf("load: skipping unknown entity section '%s' (v%u, %u bytes)\n",
                  FourCCStr(id).c_str(), version, len);
      continue;
    }
    if (!match->load(payload, len, version))
      std::fprintf(stderr,
                   "load: entity section '%s' (v%u) failed to apply; that "
                   "system starts empty\n",
                   FourCCStr(id).c_str(), version);
  }
  return true;
}

// ---- the whole entity save / load (worldio.h layout) ----------------------

bool SaveEntities(const std::string& dir, const EntityIO& io, ChunkStore& store,
                  EntityFileReport& rep) {
  // world.sve ALWAYS, even with no World-scope section: its presence is what
  // marks a dir as S4-layout, so a load never falls back to a stale
  // entities.sve beside it.
  if (!WriteSectionFile(WorldEntPath(dir), io, EntityScope::World, true, &rep.bytes,
                        store.Tables()))
    return false;

  bool anyPlayer = false;
  for (const EntitySection& s : io.sections)
    if (FileScopeOf(s) == EntityScope::Player) anyPlayer = true;
  if (anyPlayer) {
    std::error_code ec;
    std::filesystem::create_directories(PlayerDirPath(dir), ec);
    if (!WriteSectionFile(PlayerPath(dir, io.playerId), io, EntityScope::Player,
                          false, &rep.bytes, store.Tables()))
      return false;
  }

  // ---- the region buckets ----
  // Every live record, keyed by the region holding its position NOW.
  struct Bucket {
    IVec3 rc{};
    std::vector<EntityRecord> live;
  };
  std::unordered_map<uint64_t, Bucket> buckets;
  std::vector<EntityRecord> recs;
  for (const EntitySection& s : io.sections) {
    if (!IsRegionSection(s)) continue;
    recs.clear();
    s.saveRecords(recs);
    for (EntityRecord& r : recs) {
      r.section = s.id;
      r.version = s.version;
      const IVec3 rc = ChunkStore::RegionOfVoxel(r.pos);
      Bucket& b = buckets[World::PackChunkKey(rc)];
      b.rc = rc;
      b.live.push_back(std::move(r));
      rep.regionRecords++;
    }
  }
  // ...plus every region the store already knows a bucket for: one whose
  // records were all applied (or all walked away) must be rewritten without
  // them, or the next load would resurrect them beside their live selves.
  // A region neither live nor known is NOT visited -- its file, if any, holds
  // parked records nobody has asked for, and they stay exactly as they are.
  std::vector<IVec3> known;
  store.EntityRegionsKnown(known);
  for (const IVec3& rc : known) {
    Bucket& b = buckets[World::PackChunkKey(rc)];
    b.rc = rc;
  }
  bool ok = true;
  std::vector<const EntityRecord*> ptrs;
  for (auto& [key, b] : buckets) {
    ptrs.clear();
    for (const EntityRecord& r : b.live) ptrs.push_back(&r);
    bool wrote = false;
    if (!store.WriteEntityRegion(b.rc, ptrs, &wrote, &rep.bytes)) {
      std::fprintf(stderr, "save: failed to write entity bucket %s\n",
                   store.EntityRegionPath(b.rc).c_str());
      ok = false;
      continue;
    }
    if (wrote)
      rep.regionFilesWritten++;
    else
      rep.regionFilesKept++;
  }
  if (!ok) return false;

  // The pre-S4 file is now fully superseded: everything it held is live and
  // has just been written into the three files above.
  std::error_code ec;
  if (std::filesystem::exists(EntPath(dir), ec)) {
    std::filesystem::remove(EntPath(dir), ec);
    std::printf("save: pre-S4 entities.sve distributed into world.sve / players / "
                "region buckets and removed\n");
  }
  std::printf("save: entities -> world.sve, %s, %u live records in region buckets "
              "(%u files written, %u unchanged), %.1f KB\n",
              anyPlayer ? PlayerPath(dir, io.playerId).c_str() : "no player file",
              rep.regionRecords, rep.regionFilesWritten, rep.regionFilesKept,
              rep.bytes / 1e3);
  return true;
}

void LoadEntities(const std::string& dir, const EntityIO& entities,
                  ChunkStore& store, IVec3 windowOrigin, EntityFileReport& rep) {
  // Reset EVERY registered system first, whether or not the file (or its
  // section) exists: entities from the session being replaced must not stand
  // in the loaded world, and an older grid-only save must load into an empty
  // entity state rather than a haunted one.
  for (const EntitySection& s : entities.sections)
    if (s.reset) s.reset();

  // S4 layout: world, then the player, then the regions the window touches.
  // World first because it carries the state the others are applied against
  // (the mob id counter every loaded creature draws from).
  if (ApplySectionFile(WorldEntPath(dir), entities, store.Tables())) {
    ApplySectionFile(PlayerPath(dir, entities.playerId), entities, store.Tables());
    ApplyRegionEntities(store, windowOrigin, entities, &rep);
    return;
  }
  // Pre-S4: one file, every section whole. Distributed by the next save.
  if (ApplySectionFile(EntPath(dir), entities, store.Tables())) {
    rep.legacy = true;
    std::printf("load: %s holds a pre-S4 entities.sve; loaded whole, the next "
                "save splits it into world.sve / players / region buckets\n",
                dir.c_str());
  }
}

// ---- meta.svm ---------------------------------------------------------------

// ---- the worldgen fingerprint (worldio.h) -----------------------------------

// Strip WGSL comments (line, and NESTED block comments -- WGSL allows them)
// and collapse every whitespace run to one space, so the hash sees code and
// not prose. String literals do not exist in WGSL, so no quoting to respect.
std::string NormalizeWgsl(const std::string& src) {
  std::string out;
  out.reserve(src.size() / 2);
  bool pendingSpace = false;
  const size_t n = src.size();
  for (size_t i = 0; i < n;) {
    const char c = src[i];
    if (c == '/' && i + 1 < n && src[i + 1] == '/') {
      while (i < n && src[i] != '\n') i++;
      pendingSpace = true;
      continue;
    }
    if (c == '/' && i + 1 < n && src[i + 1] == '*') {
      int depth = 1;
      i += 2;
      while (i < n && depth > 0) {
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') { depth++; i += 2; }
        else if (src[i] == '*' && i + 1 < n && src[i + 1] == '/') { depth--; i += 2; }
        else i++;
      }
      pendingSpace = true;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      pendingSpace = true;
      i++;
      continue;
    }
    if (pendingSpace && !out.empty()) out.push_back(' ');
    pendingSpace = false;
    out.push_back(c);
    i++;
  }
  return out;
}

uint32_t Fnv32(const void* p, size_t n, uint32_t h = 2166136261u) {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  return h;
}

uint32_t VoxelMetersBits() {
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(kVoxelMeters), "float32 expected");
  std::memcpy(&bits, &kVoxelMeters, sizeof(bits));
  return bits;
}

// solutes.svs (the layout is at SolPath). Returns false only on a write error.
bool WriteSoluteFile(const std::string& dir, const Stream::SoluteMap& m) {
  if (m.empty()) {
    std::error_code ec;
    std::filesystem::remove(SolPath(dir), ec);
    return true;
  }
  std::vector<uint8_t> buf;
  ByteWriter w{buf};
  w.U32(kSolFileMagic);
  const std::vector<SoluteDef>& sp = CurrentSolutes();
  w.U32((uint32_t)sp.size());
  for (const SoluteDef& d : sp) {
    w.U32(d.species);
    w.Str(d.name);
  }
  w.U32((uint32_t)m.size());
  for (const auto& [key, sc] : m) {
    w.Pod(key);
    const bool page = (sc.entry & kSolPageBit) != 0 && sc.words.size() == kSolWordsPerPage;
    w.U32(page ? kSolPageBit : (sc.entry & ~kSolPageBit));
    w.U32(sc.stall);
    if (page) w.Bytes(sc.words.data(), kSolWordsPerPage * 4);
  }
  return WriteFileAtomic(SolPath(dir), buf);
}

// A 16-bit cell value with its species re-keyed through `remap` (old id ->
// new id; a species the running table no longer has maps to 0 and the cell
// is dropped, counted in `lost`).
uint32_t RemapSoluteValue(uint32_t v, const std::vector<uint32_t>& remap, uint64_t& lost) {
  if (v == 0) return 0;
  const uint32_t s = v >> 8;
  const uint32_t ns = s < remap.size() ? remap[s] : 0u;
  if (ns == 0) {
    lost += v & 0xFFu;
    return 0;
  }
  return (ns << 8) | (v & 0xFFu);
}

// Read solutes.svs into `m` (cleared first). An absent file is an empty layer
// and true; a malformed one is reported and loads nothing.
bool ReadSoluteFile(const std::string& dir, Stream::SoluteMap& m) {
  m.clear();
  std::vector<uint8_t> bytes;
  if (!ReadFileBytes(SolPath(dir), bytes)) return true;
  ByteReader r{bytes.data(), bytes.size()};
  uint32_t magic = 0, nsp = 0;
  r.U32(magic);
  r.U32(nsp);
  if (!r.ok || magic != kSolFileMagic || nsp > kSolSpeciesMax) {
    std::fprintf(stderr, "load: %s is not a solute file (magic %08x)\n",
                 SolPath(dir).c_str(), magic);
    return false;
  }
  // BY NAME, like the material table: solutes.json may have been reordered or
  // extended since the save.
  std::vector<uint32_t> remap(kSolSpeciesMax + 1, 0u);
  bool identity = true;
  for (uint32_t i = 0; i < nsp && r.ok; i++) {
    uint32_t id = 0;
    std::string name;
    r.U32(id);
    r.Str(name);
    const SoluteDef* d = CurrentSoluteNamed(name.c_str());
    if (id <= kSolSpeciesMax) remap[id] = d ? d->species : 0u;
    if (!d || d->species != id) identity = false;
  }
  uint32_t count = 0;
  r.U32(count);
  uint64_t lost = 0;
  for (uint32_t i = 0; i < count && r.ok; i++) {
    uint64_t key = 0;
    Stream::SoluteChunk sc;
    r.Pod(key);
    r.U32(sc.entry);
    r.U32(sc.stall);
    if (sc.entry & kSolPageBit) {
      sc.words.resize(kSolWordsPerPage);
      r.Bytes(sc.words.data(), kSolWordsPerPage * 4);
      if (!identity)
        for (uint32_t& wd : sc.words)
          wd = RemapSoluteValue(wd & 0xFFFFu, remap, lost) |
               (RemapSoluteValue(wd >> 16, remap, lost) << 16);
    } else if (!identity) {
      const uint32_t v = RemapSoluteValue(sc.entry & 0xFFFFu, remap, lost);
      sc.entry = v ? (kSolUniformBit | v) : 0u;
    }
    if (!r.ok) break;
    if (sc.entry != 0) m[key] = std::move(sc);
  }
  if (!r.ok) {
    std::fprintf(stderr, "load: %s is truncated; its solute was not loaded\n",
                 SolPath(dir).c_str());
    m.clear();
    return false;
  }
  if (lost)
    std::printf("load: %llu units of dissolved mass belonged to species the "
                "running solutes.json no longer has; dropped\n",
                (unsigned long long)lost);
  return true;
}

}  // namespace

const char* WorldgenFingerprintPartName(uint32_t i) {
  static const char* kNames[WorldgenFingerprint::kParts] = {
      "wgsl", "map", "biomes", "trees", "editLayer", "materials"};
  return i < WorldgenFingerprint::kParts ? kNames[i] : "?";
}

WorldgenFingerprint ComputeWorldgenFingerprint(const std::string& assetDir,
                                               const std::vector<MaterialDef>& mats) {
  const auto t0 = std::chrono::steady_clock::now();
  WorldgenFingerprint fp;
  // [0] the generator's code, exactly as the GPU would be handed it. A read
  // failure hashes as 0 rather than failing the save: the fingerprint is a
  // diagnostic about the generator, and a missing worldgen.wgsl has already
  // stopped this process from generating anything.
  std::string wgsl;
  if (AssembleShaderSource(assetDir + "/shaders", "worldgen.wgsl", wgsl)) {
    const std::string norm = NormalizeWgsl(wgsl);
    fp.parts[0] = Fnv32(norm.data(), norm.size());
  }
  // [1..3] the authored environment, with the stamp the tuner already mirrors.
  // The map part covers map.json, map.svmap and the sculpt layer (P5).
  const std::string mapName = worldmap::ActiveMapName(CurrentTuning().world.mapLayer);
  const biomes::EnvironmentStamp env = biomes::StampEnvironment(assetDir, mapName);
  fp.parts[1] = env.map;
  fp.parts[2] = env.biomes;
  fp.parts[3] = env.trees;
  // [4] the edit layer genChunk's output is always patched with (worldedit.h):
  // the one the loaded map names (map.json `editLayer`, P7). Chunks the layer
  // alone touched are not stored, so they regenerate from this on load.
  const std::string& edit = worldmap::CurrentWorldMap().editLayer;
  if (!edit.empty())
    fp.parts[4] = sandvox::HashOneFile(assetDir + "/worldedits/" + edit + ".svedit",
                                       edit + ".svedit");
  // [5] the material NAME table, in id order, NUL-separated.
  {
    uint32_t h = 2166136261u;
    const unsigned char zero = 0;
    for (const MaterialDef& m : mats) {
      h = Fnv32(m.name.data(), m.name.size(), h);
      h = Fnv32(&zero, 1, h);
    }
    fp.parts[5] = h;
  }
  // The value: FNV-1a 64 over the recipe version and the parts.
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint32_t v) {
    for (int b = 0; b < 4; b++) {
      h ^= (v >> (8 * b)) & 0xFFu;
      h *= 1099511628211ull;
    }
  };
  mix(WorldgenFingerprint::kRecipe);
  for (uint32_t i = 0; i < WorldgenFingerprint::kParts; i++) mix(fp.parts[i]);
  fp.value = h;
  fp.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
              .count();
  return fp;
}

void ApplyRegionEntities(ChunkStore& store, IVec3 wo, const EntityIO& io,
                         EntityFileReport* rep) {
  const int n = (int)(kWorldN / kChunk);  // window edge, chunks
  auto inWindow = [&](const Vec3& p) {
    const int cx = (int)std::floor((double)p.x / (double)kChunk);
    const int cy = (int)std::floor((double)p.y / (double)kChunk);
    const int cz = (int)std::floor((double)p.z / (double)kChunk);
    return cx >= wo.x && cx < wo.x + n && cy >= wo.y && cy < wo.y + n &&
           cz >= wo.z && cz < wo.z + n;
  };
  const IVec3 lo = ChunkStore::RegionOfChunk(wo);
  const IVec3 hi = ChunkStore::RegionOfChunk({wo.x + n - 1, wo.y + n - 1, wo.z + n - 1});
  for (int rz = lo.z; rz <= hi.z; rz++)
    for (int ry = lo.y; ry <= hi.y; ry++)
      for (int rx = lo.x; rx <= hi.x; rx++) {
        std::vector<EntityRecord>& dormant = store.DormantEntities({rx, ry, rz});
        if (dormant.empty()) continue;
        // Take what this window can hold OUT of the bucket first, then apply:
        // a loader that spawned something must never see (or re-enter) a
        // bucket that still lists the record it is applying.
        std::vector<EntityRecord> take, keep;
        for (EntityRecord& r : dormant) {
          const EntitySection* match = nullptr;
          for (const EntitySection& s : io.sections)
            if (s.id == r.section && IsRegionSection(s)) match = &s;
          if (!match) {
            // Unknown (or not registered by this caller): stays parked, and so
            // survives this build's next save untouched.
            std::printf("load: region (%d,%d,%d) keeps a '%s' record this build "
                        "does not apply\n",
                        rx, ry, rz, FourCCStr(r.section).c_str());
            keep.push_back(std::move(r));
          } else if (!inWindow(r.pos)) {
            keep.push_back(std::move(r));
          } else {
            take.push_back(std::move(r));
          }
        }
        dormant = std::move(keep);
        if (take.empty()) {
          if (rep) rep->recordsDormant += (uint32_t)dormant.size();
          continue;
        }
        if (rep) rep->regionsApplied++;
        // A record that fails for GOOD (content gone, truncated) is logged and
        // NOT re-parked: the same rule the whole-payload loaders follow for a
        // name that no longer resolves. A record that could not be made live
        // YET (`Retry`: the creature cap, a pool momentarily full) goes back
        // into the bucket -- MobParking::Unpark's rule. Parked records beyond
        // the cap are a normal state, and a load that dropped them would
        // delete every NPC past the living cap from the world file on the next
        // save.
        std::vector<EntityRecord> retry;
        for (EntityRecord& r : take) {
          const EntitySection* match = nullptr;
          for (const EntitySection& s : io.sections)
            if (s.id == r.section && IsRegionSection(s)) match = &s;
          // The record's ids are in the table IT was written under, which
          // in one bucket can differ record to record (chunkstore.h).
          const ScopedLoadRemap remapScope(store.Tables().RemapFor(r.matTable));
          const RecordLoad got =
              match->loadRecord(r.bytes.data(), r.bytes.size(), r.version);
          if (got == RecordLoad::Applied) {
            if (rep) rep->recordsApplied++;
          } else if (got == RecordLoad::Retry) {
            if (rep) rep->recordsRetried++;
            retry.push_back(std::move(r));
          } else {
            if (rep) rep->recordsDropped++;
            std::fprintf(stderr,
                         "load: a '%s' record (v%u) in region (%d,%d,%d) failed "
                         "to apply and is dropped\n",
                         FourCCStr(r.section).c_str(), r.version, rx, ry, rz);
          }
        }
        // Re-fetch the bucket rather than trust the reference taken above: a
        // loader that spawned may have touched the store.
        std::vector<EntityRecord>& back = store.DormantEntities({rx, ry, rz});
        for (EntityRecord& r : retry) back.push_back(std::move(r));
        if (rep) rep->recordsDormant += (uint32_t)back.size();
      }
}

bool SavePlayerFile(const std::string& dir, const EntityIO& io, ChunkStore& store) {
  std::error_code ec;
  std::filesystem::create_directories(PlayerDirPath(dir), ec);
  return WriteSectionFile(PlayerPath(dir, io.playerId), io, EntityScope::Player,
                          false, nullptr, store.Tables());
}

bool LoadPlayerFile(const std::string& dir, const EntityIO& io, ChunkStore& store) {
  return ApplySectionFile(PlayerPath(dir, io.playerId), io, store.Tables());
}

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities, WorldStamp stamp, SaveReport* report) {
  SaveReport localReport;
  SaveReport& rep = report ? *report : localReport;
  ChunkStore& store = stream.Store();
  // Before ANY write: the resident flush below can LRU-spill a region, and
  // every region written is tagged with this table (mattable.h).
  store.Tables().SetRunning(MaterialNameTableOf(mats));
  if (store.Bound() && store.Dir() != path) {
    std::fprintf(stderr, "save: store is bound to %s (one world dir per session)\n",
                 store.Dir().c_str());
    return false;
  }

  // ---- KEEP THE TICK TAGS ACROSS THE FLUSH (M9.5-B) ---------------------
  //
  // `FlushResident` Puts every chunk it stores UNTAGGED (every modified
  // resident chunk; all of them on the full-flush fallback) — it calls
  // `ChunkStore::Put(wc, rle)` with no tick, because M9.5-A deliberately left
  // every existing caller's signature alone and `Stream` has no clock to pass
  // — and an untagged Put ERASES an existing tag (chunkstore.h: "unknown" is
  // the honest answer for bytes nobody vouched for).
  //
  // For a single-player save that is harmless: there are no tags. For a HOST
  // it silently deletes the persistence this whole package exists for. A
  // chunk a peer sent with `ChunkPut` and that the host happens to still hold
  // resident is exactly an edit somebody made, and after the flush it would
  // be indistinguishable from procgen.
  //
  // MEASURED, the first M9.5-B two-process smoke: the client sent 2,193 puts
  // and the saved store reported 2,161 tagged chunks. Thirty-two of them were
  // the ones near the spawn that the standing host still had in its window.
  //
  // So the tags are snapshotted first and re-asserted after. The PREVIOUS
  // tick is kept rather than `stamp.tick`, and that is the conservative
  // choice on purpose: the flushed bytes really are newer than the old tag,
  // but "this chunk is somebody's edit, at least this old" is a true
  // statement, while stamping the save tick onto it would claim a freshness
  // decided by when the player pressed F9. Chunks that had NO tag get none:
  // inventing one for all 32,768 resident slots would put the entire
  // pristine window into `manifest.svt` and into every late join's manifest
  // message.
  std::vector<std::pair<IVec3, uint32_t>> tagsBefore;
  store.Manifest(tagsBefore);
  tagsBefore.erase(std::remove_if(tagsBefore.begin(), tagsBefore.end(),
                                  [](const std::pair<IVec3, uint32_t>& e) {
                                    return e.second == 0;
                                  }),
                   tagsBefore.end());

  // resident window -> store: the DELTA when Stream can prove its modified
  // set complete, the old everything-flush (with the reason) when it cannot.
  // Drains in-flight async evictions either way. See Stream::FlushResident.
  rep.flush = stream.FlushResident(rep.forceFullFlush);
  if (rep.flush.delta)
    std::printf("save: delta flush -- %u of %u resident chunks stored (%u of them "
                "only via the %u-tick unpublished snapshot tail), %u pristine left "
                "to genChunk/the store\n",
                rep.flush.stored, kNumChunks, rep.flush.tailOnly, rep.flush.tailTicks,
                rep.flush.skipped);
  else
    std::printf("save: FULL flush of all %u resident chunks -- %s\n", kNumChunks,
                rep.flush.why.c_str());

  size_t retagged = 0;
  for (const auto& [wc, tickTag] : tagsBefore) {
    if (store.TickOf(wc) != 0) continue;  // survived; nothing to restore
    const std::vector<uint32_t>* cur = store.Get(wc);
    if (!cur) continue;                   // gone from the store entirely
    // Copy first: `Put` may LRU-spill the region `cur` points into.
    std::vector<uint32_t> copy = *cur;
    store.Put(wc, std::move(copy), tickTag);
    retagged++;
  }
  if (retagged)
    std::printf("save: re-asserted %zu tick tags the resident flush cleared\n",
                retagged);

  if (!store.BindSave(path)) return false;
  size_t regions = 0;
  uint64_t bytes = 0;
  if (!store.Flush(&regions, &bytes)) return false;
  rep.regions = regions;
  rep.bytes = bytes;
  // THE SOLUTE LAYER (solutes.svs): every chunk the stream has kept plus the
  // resident window, captured now. Capture first -- it completes any eviction
  // still in flight, which is what makes the kept map complete.
  {
    Stream::SoluteMap resident;
    stream.CaptureResidentSolutes(resident);
    Stream::SoluteMap sol = stream.Solutes();
    for (auto& kv : resident) sol[kv.first] = std::move(kv.second);
    if (!WriteSoluteFile(path, sol)) {
      std::fprintf(stderr, "save: failed to write %s\n", SolPath(path).c_str());
      return false;
    }
    if (!sol.empty())
      std::printf("save: solute layer -- %zu chunks carry dissolved mass (%zu "
                  "resident)\n", sol.size(), resident.size());
  }
  rep.fingerprint = ComputeWorldgenFingerprint(sandvox::AssetDir(), mats);

  // Entities BEFORE meta, for the same reason meta comes last at all: meta's
  // presence marks a completed save, so everything it vouches for must already
  // be on disk. A grid-only save removes the WORLD's stale entity files
  // (entities.sve, world.sve, every r_*.sve) — pairing an old entity file with
  // a new grid would resurrect bodies over terrain that no longer matches.
  // Player files are not the world's and are left alone.
  if (entities) {
    rep.entities = EntityFileReport{};
    if (!SaveEntities(path, *entities, store, rep.entities)) {
      std::fprintf(stderr, "save: failed to write the entity files\n");
      return false;
    }
  } else {
    std::remove(EntPath(path).c_str());
    std::remove(WorldEntPath(path).c_str());
    store.RemoveAllEntityFiles();
  }

  // meta last: its presence marks a completed save. Through the tmp+rename
  // writer like every other save file: written in place, a crash mid-write
  // left a torn meta.svm that refuses the whole world on the next load.
  {
    std::vector<uint8_t> meta;
    ByteWriter w{meta};
    w.U32(kMetaMagic);
    w.U32(kWorldN);
    w.U32(kChunk);
    // Exact bit pattern, compared bitwise on load: a float compare with any
    // tolerance would wave through "close" voxel sizes, and there is no such
    // thing — the grid is authored in voxels, so any change rescales it.
    w.U32(VoxelMetersBits());
    IVec3 o = world.WindowOrigin();
    w.Pod(o.x);
    w.Pod(o.y);
    w.Pod(o.z);
    // The full material NAME table, not just a hash: material IDs are baked
    // into every saved chunk (world.h: append, never reorder), and storing the
    // names lets a refused load say exactly WHICH id changed meaning instead
    // of "hash mismatch". ~50 names is a few hundred bytes.
    w.U32((uint32_t)mats.size());
    for (const MaterialDef& m : mats) w.Str(m.name);
    // ---- SVM5's two appended words (M9.5-B) -----------------------------
    //
    // LAST IN THE RECORD, AFTER THE VARIABLE-LENGTH MATERIAL TABLE, and that
    // position is the whole compatibility argument: an SVM4 reader stops here
    // having read every field it knows, at the offset it expects. Anywhere
    // earlier would shift the table and make SVM4 unreadable, for no gain.
    //
    // The TICK is the clock a reload has to resume above — the per-chunk tick
    // tags in manifest.svt are measured against it, and a host that reloads
    // below its own tags would have every eviction refused as "older" by a
    // peer's arbitration. The SEED is what regenerates every chunk the store
    // does NOT hold, which on any real world is most of them.
    w.U32(stamp.tick);
    w.U32(stamp.seed);
    // ---- SVM6's appended fingerprint (PLAN_save_system.md S1) -----------
    // After the pair for the pair's reason: an SVM5 reader stops before it.
    // The parts ride along so a mismatch at load names the input that moved.
    w.U32((uint32_t)(rep.fingerprint.value & 0xFFFFFFFFull));
    w.U32((uint32_t)(rep.fingerprint.value >> 32));
    w.U32(WorldgenFingerprint::kParts);
    for (uint32_t i = 0; i < WorldgenFingerprint::kParts; i++)
      w.U32(rep.fingerprint.parts[i]);
    if (!WriteFileAtomic(MetaPath(path), meta)) {
      std::fprintf(stderr, "save: failed to write %s\n", MetaPath(path).c_str());
      return false;
    }
  }
  // The TAGGED-chunk count is printed beside the byte total because the two
  // answer different questions and M9.5-B's smoke reads the second: "how much
  // of this world is somebody's EDIT, carried across the save". A tag is only
  // ever written by an authority's eviction or an accepted `ChunkPut`, so this
  // is exactly the persisted multiplayer state.
  size_t tagged = 0;
  {
    std::vector<std::pair<IVec3, uint32_t>> rows;
    store.Manifest(rows);
    for (const auto& [wc, t] : rows)
      if (t != 0) tagged++;
    std::printf("save: store manifest %zu chunks, %zu tick-tagged\n",
                rows.size(), tagged);
  }
  std::printf("saved %s (%.2f MB across %zu regions, tick %u seed %u, "
              "worldgen %016llx in %.1f ms)\n",
              path.c_str(), bytes / 1e6, regions, stamp.tick, stamp.seed,
              (unsigned long long)rep.fingerprint.value, rep.fingerprint.ms);
  return true;
}

bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities, WorldStamp* stampOut,
               EntityFileReport* entReport) {
  std::vector<uint8_t> meta;
  if (!ReadFileBytes(MetaPath(path), meta)) {
    std::fprintf(stderr, "load: %s has no meta.svm\n", path.c_str());
    return false;
  }
  ByteReader r{meta.data(), meta.size()};
  uint32_t magic = 0, worldN = 0, chunk = 0, vmBits = 0;
  int32_t origin[3] = {};
  r.U32(magic);
  r.U32(worldN);
  r.U32(chunk);
  r.U32(vmBits);
  r.Pod(origin[0]);
  r.Pod(origin[1]);
  r.Pod(origin[2]);
  // BOTH MAGICS ARE ACCEPTED (M9.5-B). SVM5 only APPENDED, so an SVM4 file is
  // an SVM5 file that stops two words early and every field below is at the
  // offset this reader expects. SVM3-and-earlier is still refused: refusing
  // beats guessing at a header we cannot verify (the old format recorded
  // neither voxel size nor materials, exactly the two silent-corruption axes
  // this header exists to close).
  const bool haveStamp = (magic == kMetaMagic || magic == kMetaMagicV5);
  const bool haveFingerprint = (magic == kMetaMagic);
  if (!r.ok ||
      (magic != kMetaMagic && magic != kMetaMagicV5 && magic != kMetaMagicV4)) {
    std::fprintf(stderr,
                 "load: %s is not a compatible world dir (bad or pre-SVM4 "
                 "meta.svm magic %08x, want %08x, %08x or %08x)\n",
                 path.c_str(), magic, kMetaMagic, kMetaMagicV5, kMetaMagicV4);
    return false;
  }
  if (worldN != kWorldN || chunk != kChunk) {
    std::fprintf(stderr,
                 "load: %s world constants mismatch (saved N=%u chunk=%u, "
                 "build has N=%u chunk=%u)\n",
                 path.c_str(), worldN, chunk, kWorldN, kChunk);
    return false;
  }
  if (vmBits != VoxelMetersBits()) {
    float savedVm = 0;
    std::memcpy(&savedVm, &vmBits, sizeof(savedVm));
    // Bit patterns as well as values: two floats can round-trip to the same
    // %g text and still differ, and the compare is bitwise on purpose.
    std::fprintf(stderr,
                 "load: %s was saved at kVoxelMeters=%.9g (bits %08x), this "
                 "build is %.9g (bits %08x) — the world would load at the "
                 "wrong physical scale\n",
                 path.c_str(), savedVm, vmBits, kVoxelMeters, VoxelMetersBits());
    return false;
  }
  // ---- the material NAME table (rule-unification W1-D, sim/mattable.h) ---
  //
  // This used to REFUSE any change but an append: saved chunks reference
  // materials by id, so a reorder would have loaded stone as blood. Now every
  // blob in the dir names the table it was written under and is remapped BY
  // NAME as it is read, so a reordered or grown materials.json loads the same
  // substances. What this table still decides is the files that name NO table
  // -- everything written before W1-D -- which read under it (or under the
  // mat_legacy.svmt a previous W1-D load pinned). The mismatch is logged, not
  // refused; an unknown name loads as air and is named in the log.
  MaterialNameTable metaTable;
  {
    uint32_t savedCount = 0;
    r.U32(savedCount);
    // A count the file cannot hold is a corrupt header, not a big table.
    if (!r.ok || (size_t)savedCount > (r.n - r.off) / 4) {
      std::fprintf(stderr, "load: %s meta.svm material table is truncated\n",
                   path.c_str());
      return false;
    }
    metaTable.mats.resize(savedCount);
    for (uint32_t i = 0; i < savedCount && r.ok; i++) r.Str(metaTable.mats[i]);
    if (!r.ok) {
      std::fprintf(stderr, "load: %s meta.svm material table is truncated\n",
                   path.c_str());
      return false;
    }
    const MaterialNameTable running = MaterialNameTableOf(mats);
    if (metaTable.mats != running.mats) {
      // Stain names are not in meta.svm, so this compares materials only.
      MaterialNameTable runMatsOnly;
      runMatsOnly.mats = running.mats;
      const MatRemap m = BuildMatRemap(metaTable, runMatsOnly);
      std::printf("load: %s was last saved under a different materials.json "
                  "(%u names, this build %zu): %s -- loading BY NAME, not "
                  "refusing\n",
                  path.c_str(), savedCount, mats.size(),
                  m.identity ? "an append only, every id keeps its meaning"
                             : m.Describe().c_str());
    }
  }

  // ---- SVM5's appended pair, read AFTER the material table (M9.5-B) ------
  //
  // An SVM4 file has nothing here and `known` stays false — which is the
  // distinction a caller needs, because "the file said tick 0" and "the file
  // did not say" both read as 0 and only the first may be used to set a
  // clock. A TRUNCATED SVM5 pair is treated the same way rather than failing
  // the load: the grid is already valid and two missing diagnostics are not
  // worth throwing a world away for.
  WorldStamp stamp{};
  if (haveStamp) {
    uint32_t savedTick = 0, savedSeed = 0;
    if (r.U32(savedTick) && r.U32(savedSeed)) {
      stamp.tick = savedTick;
      stamp.seed = savedSeed;
      stamp.known = true;
    } else {
      std::fprintf(stderr,
                   "load: %s meta.svm claims SVM5 but its tick/seed pair is "
                   "truncated; treating them as unknown\n",
                   path.c_str());
    }
  }

  // ---- SVM6's worldgen fingerprint (PLAN_save_system.md S1) --------------
  //
  // LOAD, LOUDLY, on a mismatch -- never refuse (worldio.h says why). The
  // mismatch is computed here, before the window refills, because the refill
  // is exactly where a different generator shows: every chunk the store does
  // not hold comes from genChunk as it is NOW.
  if (haveFingerprint) {
    uint32_t lo = 0, hi = 0, nParts = 0;
    if (r.U32(lo) && r.U32(hi)) {
      stamp.worldgenFingerprint = ((uint64_t)hi << 32) | lo;
      stamp.fingerprintKnown = true;
      std::vector<uint32_t> fileParts;
      if (r.U32(nParts) && nParts <= 64) {
        fileParts.resize(nParts);
        for (uint32_t i = 0; i < nParts && r.ok; i++) r.U32(fileParts[i]);
        if (!r.ok) fileParts.clear();
      }
      const WorldgenFingerprint now = ComputeWorldgenFingerprint(sandvox::AssetDir(), mats);
      stamp.worldgenMismatch = now.value != stamp.worldgenFingerprint;
      if (stamp.worldgenMismatch) {
        std::string which;
        for (uint32_t i = 0; i < WorldgenFingerprint::kParts; i++) {
          if (i < fileParts.size() && fileParts[i] == now.parts[i]) continue;
          if (!which.empty()) which += ", ";
          which += WorldgenFingerprintPartName(i);
          if (i >= fileParts.size()) which += " (not in file)";
        }
        std::fprintf(stderr,
                     "load: *** WORLDGEN MISMATCH *** %s was saved by generator "
                     "%016llx, this build is %016llx (differs: %s). Loading "
                     "anyway: every chunk the save does not store regenerates "
                     "from the CURRENT generator and may seam against stored "
                     "ones.\n",
                     path.c_str(), (unsigned long long)stamp.worldgenFingerprint,
                     (unsigned long long)now.value, which.empty() ? "?" : which.c_str());
      } else {
        std::printf("load: worldgen fingerprint %016llx matches (%.1f ms)\n",
                    (unsigned long long)now.value, now.ms);
      }
    } else {
      std::fprintf(stderr,
                   "load: %s meta.svm claims SVM6 but its worldgen fingerprint is "
                   "truncated; treating it as unknown\n",
                   path.c_str());
    }
  } else {
    std::printf("load: %s predates SVM6 -- worldgen fingerprint UNKNOWN, pristine "
                "chunks regenerate from this build's generator unchecked\n",
                path.c_str());
  }
  if (stampOut) *stampOut = stamp;

  ChunkStore& store = stream.Store();
  store.Tables().SetRunning(MaterialNameTableOf(mats));
  if (!store.BindLoad(path)) {
    std::fprintf(stderr, "load: store is bound to %s (one world dir per session)\n",
                 store.Bound() ? store.Dir().c_str() : "?");
    return false;
  }
  // Before the window refill reads a single region: the table untagged files
  // decode under (see the material-table block above).
  store.Tables().AdoptLegacy(metaTable);

  // The saved solute layer (solutes.svs) becomes the stream's kept map; the
  // GPU layer is emptied first so the refill below restores ONLY the saved
  // one (ReloadWindow's capture then finds nothing of the replaced world).
  stream.DiscardSoluteState();
  world.ResetSolutes(ctx.queue);
  ReadSoluteFile(path, stream.Solutes());

  {
    // Snapshot restore (worldgen-equivalent), not a live mutation: the direct
    // upload path in FillSlots is sanctioned the same way worldgen's direct
    // writes are. All GAMEPLAY writes still flow through the MutationQueue.
    // The held readback snapshot describes the PRE-LOAD world and must not be
    // consumable afterwards (see World::InvalidateSnapshot).
    world.InvalidateSnapshot();
    stream.ReloadWindow({origin[0], origin[1], origin[2]});
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeLoadReset(enc);
    rhi::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
  }

  // Entities after the grid: their load paths (Jolt bodies, terrain anchors)
  // read the world that is now in place. With no EntityIO the entity file is
  // ignored entirely — grid-only callers keep their exact old behaviour.
  //
  // S4: world.sve, then players/<id>.svp, then every region bucket the
  // window intersects (records inside the window only). The window was just
  // reloaded at the saved origin, so that is every region a live entity could
  // have been saved into.
  if (entities) {
    EntityFileReport entRep;
    LoadEntities(path, *entities, store, {origin[0], origin[1], origin[2]}, entRep);
    if (entReport) *entReport = entRep;
    if (!entRep.legacy)
      std::printf("load: entities -> %u records applied from %u region buckets, "
                  "%u left parked outside the window\n",
                  entRep.recordsApplied, entRep.regionsApplied, entRep.recordsDormant);
  }

  // ---- far-field EDIT PERSISTENCE across a load (src/sim/faredits.h) -------
  // Everything above restores the SIMULATED world; the cascades are rebuilt
  // from scratch by FarField::FullRefill right after this returns, and the
  // sieve only knows procgen. Without this rebuild a reloaded world's horizon
  // is the pristine hillside again — every crater, tower and blast the save
  // faithfully kept is invisible past the residency window until the player
  // happens to walk each chunk back into it.
  //
  // O(store), once, off the frame path. The index is DERIVED: this is the
  // reconstruction, which is exactly why nothing about the cascades has to be
  // saved (DESIGN.md §9).
  {
    const size_t n = stream.Edits().RebuildFromStore(store);
    std::printf("far-field edit index: %zu stored chunks -> %zu cells in %zu "
                "level chunks\n",
                n, stream.Edits().Cells(), stream.Edits().LevelChunks());
  }

  // The same tagged-chunk count SaveWorld prints, on the way back in. It is
  // the one number the M9.5-B smoke's third launch reads: the tick tags live
  // in `manifest.svt` beside the region files, so "the host saved N tagged
  // chunks, a fresh process loaded N tagged chunks" is the end-to-end claim
  // that the persistence actually persists.
  {
    std::vector<std::pair<IVec3, uint32_t>> rows;
    stream.Store().Manifest(rows);
    size_t tagged = 0;
    for (const auto& [wc, t] : rows)
      if (t != 0) tagged++;
    std::printf("load: store manifest %zu chunks, %zu tick-tagged\n",
                rows.size(), tagged);
  }
  std::printf("loaded %s (%zu chunks in RAM after window fill, meta tick %u "
              "seed %u%s)\n",
              path.c_str(), stream.Store().Count(), stamp.tick, stamp.seed,
              stamp.known ? "" : " [SVM4: unknown]");
  return true;
}
