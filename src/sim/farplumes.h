#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "math3d.h"
#include "sim/materials.h"
#include "sim/world.h"

// ---- FAR FIRE PLUMES: the CPU index of fires the window has left behind ----
//
// THE GAP THIS CLOSES (world.h's kGasFarEmitMax block states it too, because
// the constant is where a reader meets the feature first). A chunk evicted
// mid-burn is FROZEN. Its `ember` / `lava` / burning-foliage voxels were
// downsampled into the far cascade by the last `fardown`, so the fire is still
// visibly orange at 60 m and stays that way for the rest of the session. Its
// SMOKE is not: a smoke parcel is only ever born at the window face by the
// running CA (sim_step's `gasLeave`), and parcels already in flight die on
// smoke's authored ~9/1000-per-tick decay. Within about four seconds of the
// eviction the distant fire is a silent orange smear.
//
// So this index remembers WHERE the frozen fires are, and the GPU synthesizes
// their plumes into the render-only density box.
//
// WHAT IT IS AND IS NOT
//
//   * It is DERIVED and DISPOSABLE, exactly like FarEdits beside it: it is
//     rebuilt from the same eviction harvest, it is never the authority for a
//     voxel, it is never read by the sim, and it is never hashed or saved.
//   * Its only GPU product is `gasOuter`. It does NOT queue gas parcels. The
//     parcel pool is deterministic sim state — its population is pinned by
//     kGasSpDigest in the determinism gate and a re-entering parcel writes a
//     hashed voxel — so a frozen fire that could spawn parcels would be a
//     frozen fire that moves the world. It cannot: there is no path from here
//     to QueueGasSpawns.
//   * It is DATA-DRIVEN about what "fire" is. A material qualifies if it is
//     tagged `hot`, emits light, and is not a gas (fire itself is CLASS_GAS
//     and is never written to the cascade at all, so it could not be seen out
//     there even if it were listed). No material id appears in this file.
//
// THE AGGREGATION, and why it is per COLUMN. gasOuter's cell is 2^kGasOuterShift
// = 8 fine voxels, and a fine chunk is 16, so one chunk covers exactly 2x2 cells
// in x/z. A burning chunk therefore contributes AT MOST FOUR emitters however
// many hot voxels it holds — which is what keeps the list short enough to cap,
// sort and upload whole, and what makes the strength a number the shader can
// divide by a compile-time constant (kGasFarEmitStrengthMax).
class FarPlumes {
 public:
  // One column's worth of frozen fire.
  struct Emitter {
    int32_t x, y, z;    // world voxel: the column's CENTRE in x/z, the TOPMOST
                        // hot voxel in y (smoke leaves from the top of a fire)
    uint32_t strength;  // hot voxels in the column, 1..kGasFarEmitStrengthMax
  };

  // ---- what counts as frozen fire (process-global, like the material table
  // itself) ------------------------------------------------------------------
  // Latched from the loaded material table by Simulation::UploadTables, which
  // is the one chokepoint the compiled table passes through at boot AND on
  // every R-reload — so the answer is current without any call site having to
  // remember to refresh it. Static because there is exactly one material table
  // per process and every FarPlumes instance would otherwise hold a copy of
  // the same 128 bytes.
  static void SetMaterials(const std::vector<MaterialDef>& mats);
  static bool HotEmissive(uint32_t mat);

  // ---- the eviction harvest -------------------------------------------------
  // `words` is kChunkVol voxel words in chunk-linear order — the same layout
  // FarEdits::NoteChunk takes, from the same call site, because this is the one
  // place the CPU ever sees an edited chunk's content. Re-noting a chunk
  // REPLACES its emitters, so a fire that burned out and was evicted again
  // removes itself.
  void NoteChunk(IVec3 wc, const uint32_t* words);
  // Same, for a page-table sentinel slot whose whole content is one material.
  void NoteUniformChunk(IVec3 wc, uint32_t mat);
  void Clear();

  size_t Chunks() const { return byChunk_.size(); }
  // Burning chunks the index refused because it was full. Diagnostics: a
  // nonzero value is "the INDEX was the bound", which is a different fact from
  // "the UPLOAD cap was", and the gate prints both.
  uint64_t RefusedChunks() const { return refusedChunks_; }

  // Rebuild BOTH upload sections for the window at `windowOriginChunks`,
  // dropping every emitter that is back inside the residency window (in-window
  // fire makes REAL smoke through the CA, and counting it twice would put a
  // second plume on top of the first). What survives is then split by distance,
  // in the MAX NORM from the window centre — the same metric the voxel/parcel
  // crossfade uses, and for the same reason: it is exactly the half-extent at
  // every point of all six faces, so the handover distance does not depend on
  // which way the camera looks.
  //
  //   < kWorldN voxels        the FINE section (gasOuter, 0.8 m cells)
  //   >= kWorldN, <= rangeVox the WIDE section (gasFarOuter, 6.4 m cells),
  //                           aggregated again per COARSE column
  //
  // The split is STRICT — no emitter is in both — which is what makes "no
  // double-brightening" a property of the data rather than of a blend weight
  // the renderer has to get right. `rangeVox` is render.farPlumeRange in
  // voxels; 0 produces an empty wide section, which is the feature's exact off
  // switch (no emitters, no row, no clear, no sampling).
  //
  // Cheap to call every tick: it returns immediately unless the index changed,
  // the window moved, or the range changed — the only three things that can
  // change the answer.
  void Build(IVec3 windowOriginChunks, int32_t rangeVox);

  // Emitters in each section of the list Build() last produced.
  uint32_t Count() const { return count_; }
  uint32_t CountWide() const { return countWide_; }

  // Hand the caller the words to upload, ONCE per version. Returns false when
  // the buffer on the GPU already holds this list — which is every tick of a
  // world whose frozen fires have not moved, i.e. almost all of them. Shaped
  // like World::TakeGasSpawns: a read that also records that the read happened,
  // because the alternative is a "last uploaded" integer kept by the caller and
  // a second place to get wrong.
  bool TakeUpload(const uint32_t** words, uint32_t* wordCount);

 private:
  struct Key {
    int32_t x, y, z;
    bool operator==(const Key& o) const {
      return x == o.x && y == o.y && z == o.z;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](uint32_t v) { h ^= v; h *= 1099511628211ull; };
      mix((uint32_t)k.x);
      mix((uint32_t)k.y);
      mix((uint32_t)k.z);
      return (size_t)(h ^ (h >> 32));
    }
  };

  // Replace `wc`'s emitters with `e` (possibly empty, which erases the entry).
  void Replace(IVec3 wc, const Emitter* e, uint32_t n);

  std::unordered_map<Key, std::vector<Emitter>, KeyHash> byChunk_;
  // THE INDEX IS CAPPED TOO, not only the upload. Without this a session that
  // set half a forest alight and flew away would grow an entry per burning
  // chunk forever. Past the cap new burning chunks are refused (counted, so
  // "the index was the bound" is a printed number); chunks already in it keep
  // updating, so a fire that goes out still removes itself.
  static constexpr size_t kChunkCap = 4096;
  uint64_t refusedChunks_ = 0;

  std::vector<uint32_t> words_;      // the upload image, header + both sections
  uint32_t count_ = 0;               // emitters in the fine section
  uint32_t countWide_ = 0;           // emitters in the wide section
  uint32_t version_ = 0;             // bumped by every rebuild
  uint32_t uploadedVersion_ = 0;     // the version the GPU buffer holds
  bool dirty_ = true;                // the index changed since the last Build
  IVec3 builtOrigin_{INT32_MIN, INT32_MIN, INT32_MIN};
  int32_t builtRange_ = -1;
};
