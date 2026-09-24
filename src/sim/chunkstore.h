#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/world.h"

// Move `from` over `to` in ONE step: `to` names the old file or the new one at
// every instant, never nothing. Every save file (regions, buckets, manifest,
// world/player sections, meta) is written to `<path>.tmp` and landed with this.
// The old `remove(to); rename(from, to)` pair left a window in which a crash
// deleted the file outright. Windows: MoveFileExW(REPLACE_EXISTING |
// WRITE_THROUGH); elsewhere rename(2), which replaces atomically.
bool ReplaceFileAtomic(const std::string& from, const std::string& to);

// Evicted-chunk store (DESIGN.md §3 streaming). Chunks are grouped into
// 16^3-chunk REGIONS. Unbound (the default) it is pure RAM — the v1 behavior.
// Bound to a directory it becomes a region-file store: regions lazy-load on
// Get, dirty regions are written by Flush() and by LRU spill once more than
// kMaxRamRegions sit in RAM, so long journeys stream to disk instead of
// growing RAM without bound, and saves are per-region instead of monolithic.
//
// A bound directory is a LIVE world store (Minecraft-style), not a snapshot:
// LRU spills may persist chunks between explicit saves. Binding is one
// directory per store lifetime — disk-resident chunks can't follow a rebind —
// so BindSave/BindLoad refuse a different directory until Unbind()/Clear().
class ChunkStore {
 public:
  struct Entry {
    IVec3 wc;
    std::vector<uint32_t> rle;
  };
  static constexpr int kRegionShift = 4;  // 16 chunks (256 voxels) per axis
  static constexpr size_t kMaxRamRegions = 64;

  // ---- THE TICK TAG (M9.5-A) ---------------------------------------------
  //
  // `tick` is the sim tick the stored bytes were DECIDED at, not the tick they
  // were written at, and it exists for exactly one question: when two machines
  // both hold a copy of a chunk, whose is newer? The host's store is the
  // truth and a `ChunkPut` that carries an older tick than `TickOf(wc)` is
  // refused rather than applied (PLAN_multiplayer_m9.md M9.5-B).
  //
  // ZERO MEANS UNKNOWN, and that is the single-player default: every existing
  // caller passes no tick, no tag is recorded, `manifest.svt` is never
  // written, and the `.svr` region format is untouched — which is why
  // `save-load` and `region-store` see no change at all. A tagged Put
  // followed by an untagged one DROPS the tag: the untagged bytes are newer
  // and "unknown" is the honest answer for them, where keeping the stale
  // number would claim a freshness the content does not have.
  void Put(IVec3 wc, std::vector<uint32_t> rle, uint32_t tick = 0);
  // The tag, or 0 for an untagged/absent chunk. Deliberately does NOT go
  // through Get(): asking "how old is this?" must not pull the region into
  // RAM and LRU-spill something else out (the ForEachStored argument, applied
  // to a lookup a manifest walk makes thousands of times).
  uint32_t TickOf(IVec3 wc) const;
  // Every stored chunk with its tag, in one pass: the payload of the
  // `ChunkManifest` a late join receives. Built on ForEachStored for the same
  // no-thrash reason; O(store), join-time only.
  void Manifest(std::vector<std::pair<IVec3, uint32_t>>& out);
  // Pointer is valid only until the next Put/Get/Clear: either may LRU-spill
  // the region that owns it. Use immediately.
  const std::vector<uint32_t>* Get(IVec3 wc);

  // Forget everything in RAM and detach from any bound directory. Files are
  // left on disk untouched (a regen must not destroy the last explicit save;
  // the next BindSave overwrites them).
  void Clear() {
    regions_.clear();
    tickTags_.clear();
    entityRegions_.clear();
    chunkCount_ = 0;
    dir_.clear();
  }
  // The entity buckets go with the binding (a dormant record belongs to the
  // world dir it was read from); the RAM chunks stay, as they always have.
  void Unbind() {
    dir_.clear();
    entityRegions_.clear();
  }
  bool Bound() const { return !dir_.empty(); }
  const std::string& Dir() const { return dir_; }
  size_t Count() const { return chunkCount_; }  // chunks resident in RAM

  // Bind for saving: creates the directory; if the store was unbound, any
  // region files from an earlier session are wiped (this RAM store is the
  // whole world) and every RAM region is marked dirty so the next Flush
  // writes a complete store. Binding to the already-bound dir is a no-op.
  bool BindSave(const std::string& dir);
  // Bind for loading: the directory must exist; RAM contents are discarded
  // in favor of the disk's.
  bool BindLoad(const std::string& dir);
  // Write every dirty region. False if any write failed.
  bool Flush(size_t* regionsOut = nullptr, uint64_t* bytesOut = nullptr);

  // Visit every stored chunk exactly once — RAM first, then whatever else is
  // on disk under a bound directory. `rle`/`pairs` are the chunk's encoded
  // words; the pointer is valid only for the duration of the callback.
  //
  // DELIBERATELY DOES NOT GO THROUGH Get(): a full walk of a large save would
  // pull every region into RAM and then LRU-spill it straight back out, which
  // is both slow and a write amplification on a read-only traversal. Disk
  // regions are streamed with their own FILE handle and never enter regions_.
  //
  // For rebuilding DERIVED indexes over the persisted world (the far-field
  // edit index — see FarEdits::RebuildFromStore). O(store); load-time only.
  using Visitor = std::function<void(IVec3 wc, const uint32_t* rle, size_t pairs)>;
  void ForEachStored(const Visitor& fn);

  // ---- THE ON-DISK CHUNK CODEC (SVR3, docs/PLAN_save_system.md S3) ---------
  //
  // Disk only. RAM keeps the (run, word) RLE above — Put/Get/ForEachStored,
  // the M9.5 exchange and FarEdits all see exactly what they saw before; the
  // codec runs at WriteRegion (encode) and at the two disk readers (decode).
  //
  // A record is the chunk's 4,096 persisted words split into four byte
  // planes — material low byte, material high nibble, state nibble, stain
  // byte (bits 24..31) — with the state nibble XOR'd against the palette
  // variant worldgen would have given that cell (world.h JitterStateFor, the
  // one CPU mirror of worldgen.wgsl's rule; row form, as the sentinel RLE
  // uses). Untouched terrain then reads as all-zero state, and zstd takes the
  // rest. Air is predicted as state 0 (worldgen never jitters air); nothing
  // else is special-cased, because a predictor that consulted the MATERIAL
  // TABLE (liquids are born full) would make a region file's meaning depend
  // on materials.json — the save would decode differently after an edit to
  // it. The predictor reads only the decoded material plane, the cell
  // position and the seed.
  //
  // THE SEED IS IN THE FILE HEADER, and decode uses that one, never the
  // store's. A wrong or stale SetSeed can therefore only cost compression
  // (the residue stops being zero), never correctness.
  //
  // A record is the SMALLER of the plane codec and the raw RLE pairs, so a
  // one-run chunk stays 8 bytes and anything the planes cannot represent
  // (stamp bits set, an RLE that does not cover exactly kChunkVol) is kept
  // verbatim rather than refused or silently altered.
  enum class Codec : uint32_t { RawRle = 0, ZstdPlanes = 1 };
  static constexpr int kZstdLevel = 3;  // measured 1/3/9 — see region-codec
  // Seed used to ENCODE (recorded in each region header it writes). Set once
  // by Stream::Init, next to PageTable::SetWorldSeed.
  void SetSeed(uint32_t seed) { seed_ = seed; }
  uint32_t Seed() const { return seed_; }
  // One chunk's record payload (no wc/length framing). `level` is exposed for
  // the gate's level sweep; the store always writes kZstdLevel.
  static Codec EncodeRecord(const uint32_t* rle, size_t pairs, IVec3 wc,
                            uint32_t seed, std::vector<uint8_t>& out,
                            int level = kZstdLevel);
  // Inverse. False on anything malformed — never a partial chunk.
  static bool DecodeRecord(Codec codec, const uint8_t* data, size_t len,
                           IVec3 wc, uint32_t seed, std::vector<uint32_t>& rle);

  // ---- REGION ENTITY BUCKETS: r_x_y_z.sve (PLAN_save_system.md S4) ---------
  //
  // The entity half of a region. World-anchored entities (mobs, ground items)
  // are saved into the region that contains their position, in a file beside
  // that region's `.svr`, so they stream with the terrain instead of living in
  // one monolithic file that is rewritten and loaded whole.
  //
  // THE STORE HOLDS OPAQUE RECORDS AND NOTHING ELSE. A record is a section
  // FourCC, that section's payload version, the position that bucketed it and
  // the owning system's bytes. Which system a FourCC names, and what its bytes
  // mean, is sim/worldio.h's and game/persist.cpp's business; this class only
  // keeps them per region and gets them to and from disk.
  //
  // WHAT THE STORE KEEPS IN RAM is each region's DORMANT records: the ones
  // read from disk that have not been handed to a live system. A record that
  // IS live (applied at load, or a creature spawned this session) is owned by
  // its system and is written back from there at save time
  // (WriteEntityRegion's `live`). A region's file is therefore always
  // dormant + live, and a region that was never read and holds nothing live
  // is never opened, rewritten or deleted -- which is what lets S5b park an
  // NPC in one region without touching any other.
  //
  //   r_x_y_z.sve: u32 magic 'SVX1', u32 count; per record:
  //                u32 section, u32 version, f32 pos[3], u32 len, u8 bytes[len]
  //
  // Flat and individually framed so one record can be appended or removed
  // without understanding any other. Lifecycle follows the chunk half:
  // BindLoad and Clear/Unbind forget every bucket; BindSave from unbound wipes
  // the files with the region files (IsOurFile).
  struct EntityRecord {
    uint32_t section = 0;  // FourCC of the owning EntitySection
    uint32_t version = 0;  // the payload version the bytes were written at
    Vec3 pos{};            // world voxels: decides the bucket
    std::vector<uint8_t> bytes;
  };
  static IVec3 RegionOfChunk(IVec3 wc) { return RegionOf(wc); }
  static IVec3 RegionOfVoxel(Vec3 p);
  // The dormant records of region `rc`, read from its file the first time
  // they are asked for (unbound, or no file: empty). Mutable on purpose --
  // taking a record out of this vector IS making it live, and appending one
  // is parking it (S5b); either changes what the next save writes.
  std::vector<EntityRecord>& DormantEntities(IVec3 rc);
  // Every region the entity half has opened or written under this binding.
  void EntityRegionsKnown(std::vector<IVec3>& out) const;
  // Write region `rc`'s file as its dormant records + `live`. Skipped when the
  // bytes equal what the file already holds (so an unchanged bucket costs a
  // hash, not a write); the file is removed when the result is empty.
  // `wrote` / `bytesOut` (added to) report what happened. Unbound: false.
  bool WriteEntityRegion(IVec3 rc, const std::vector<const EntityRecord*>& live,
                         bool* wrote = nullptr, uint64_t* bytesOut = nullptr);
  // Remove every r_*.sve under the bound dir and forget every bucket: a
  // grid-only save must not leave a previous save's creatures on disk.
  void RemoveAllEntityFiles();
  std::string EntityRegionPath(IVec3 rc) const;

 private:
  struct Region {
    IVec3 rc{};                                  // region coord (file name)
    std::unordered_map<uint64_t, Entry> chunks;  // packed chunk key
    bool dirty = false;
    bool loaded = false;  // disk contents merged (or known absent)
    uint64_t lastUse = 0;
  };
  static IVec3 RegionOf(IVec3 wc) {
    return {wc.x >> kRegionShift, wc.y >> kRegionShift, wc.z >> kRegionShift};
  }
  std::string RegionPath(IVec3 rc) const;
  Region& Touch(IVec3 rc);
  // Merge the region's disk file into RAM (RAM entries win: they are newer).
  void EnsureLoaded(IVec3 rc, Region& r);
  bool WriteRegion(IVec3 rc, Region& r, uint64_t* bytesOut);
  void SpillOverBudget();

  // manifest.svt: the tick tags, as a flat (wc, tick) list beside the region
  // files. A SIDE MAP rather than a field on Entry, for two reasons that are
  // both about not paying for it in single player: the `.svr` format stays
  // byte-identical (no save-format change, §M9.5-A), and reading a tag costs
  // no region load — which is what lets a manifest of a disk-resident world
  // be answered without dragging every region through RAM. Only NON-ZERO tags
  // are held, so an untagged world's map is empty and its file is never
  // written.
  static constexpr uint32_t kManifestMagic = 0x31545653;  // 'SVT1'
  std::string ManifestPath() const { return dir_ + "/manifest.svt"; }
  void LoadManifest();
  bool WriteManifest();

  // Reads one region file (SVR2 or SVR3) and hands every chunk to `fn`.
  // The single reader both EnsureLoaded and ForEachStored use, so the two
  // cannot disagree about the format. Returns false if the file is not a
  // region file; a bad entry stops the walk (the entries before it stand).
  static bool ReadRegionFile(
      const std::string& path,
      const std::function<void(IVec3 wc, std::vector<uint32_t>& rle)>& fn);

  std::string dir_;
  uint32_t seed_ = 0;
  std::unordered_map<uint64_t, Region> regions_;  // packed region key
  // Value carries the COORDINATE as well as the tick, because the manifest
  // writer has to emit (wc, tick) and World::PackChunkKey has no inverse —
  // writing one here would be a second copy of that packing to keep in step
  // with the original, which is the "two places must agree" bug this repo
  // keeps paying for. Twelve bytes per tagged chunk, and only tagged ones.
  struct Tag {
    IVec3 wc;
    uint32_t tick = 0;
  };
  std::unordered_map<uint64_t, Tag> tickTags_;  // packed chunk key -> tag

  // One region's entity bucket (see EntityRecord). `diskHash` is FNV-1a 64 of
  // the file bytes as last read or written, 0 = no file: it is how an
  // unchanged bucket skips its write.
  struct EntityRegion {
    IVec3 rc{};
    std::vector<EntityRecord> dormant;
    uint64_t diskHash = 0;
    bool loaded = false;
    // The file exists but did not parse (DormantEntities). It is set aside
    // as `<path>.corrupt` before anything is written over or deleted in its
    // place, so a bucket this build cannot read is never silently destroyed.
    bool unreadable = false;
  };
  EntityRegion& TouchEntityRegion(IVec3 rc);
  bool SetAsideUnreadable(EntityRegion& e, const std::string& path);
  std::unordered_map<uint64_t, EntityRegion> entityRegions_;  // packed region key
  size_t chunkCount_ = 0;
  uint64_t useCounter_ = 0;
};
