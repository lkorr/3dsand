#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gpu/context.h"
#include "sim/bytestream.h"
#include "sim/materials.h"
#include "sim/simulation.h"
#include "sim/stream.h"
#include "sim/world.h"

// Region-directory world persistence (M2 + region files, DESIGN.md §3).
// `path` is a DIRECTORY (e.g. "world.svd") holding:
//
//   meta.svm       magic 'SVM5', world constants (kWorldN, kChunk, the exact
//                  bit pattern of kVoxelMeters), the window origin, the full
//                  material NAME table, and (SVM5) the SIM TICK and the SEED
//                  the world was saved at. Written LAST: its presence marks
//                  a completed save. A load refuses any mismatch and says
//                  exactly which field disagreed — material IDs are baked into
//                  every chunk (world.h "never reorder"), and a world saved at
//                  one voxel size loaded into a build with another would
//                  silently change physical scale.
//
//                  'SVM4' STILL LOADS, and reports tick/seed as UNKNOWN (0).
//                  That is not politeness to old files: the two fields are
//                  APPENDED at the end of the record, so an SVM4 reader's
//                  view of an SVM5 file is identical up to the point it stops
//                  reading, and the only thing a pre-M9.5 save genuinely
//                  lacks is the pair. Refusing it would throw away a world to
//                  gain two numbers nobody had.
//
//                  WHY THE TICK AND THE SEED ARE HERE AT ALL (M9.5-B). The
//                  tick is what `ChunkStore`'s per-chunk tick tags are
//                  measured against: a host that saves, quits and reloads has
//                  to resume its clock ABOVE every tag it persisted, or the
//                  first eviction after the load carries a tick older than
//                  the chunk it replaces and a peer's `ChunkPut` arbitration
//                  ("newer wins") starts answering backwards. The seed is the
//                  other half of the same identity: a world dir and a build
//                  that disagree on the seed regenerate different terrain for
//                  every chunk the store does NOT hold, which is most of
//                  them, and today nothing in the file would catch it.
//
//                  'SVM6' (PLAN_save_system.md S1) appends the WORLDGEN
//                  FINGERPRINT after the pair: u64 value, u32 part count,
//                  then that many u32 per-input hashes (see
//                  WorldgenFingerprint below). SVM5 and SVM4 still load, with
//                  the fingerprint UNKNOWN -- the same append-only argument
//                  SVM5 made for the tick and seed.
//   r_x_y_z.svr    the chunk store's voxel region files (chunkstore.cpp).
//   entities.sve   OPTIONAL entity state — everything that lives OUTSIDE the
//                  voxel grid (rigidbodies, mobs, the avatar). See below.
//
// Save flushes the resident window into the store (draining async evictions)
// then binds the store to the directory and flushes its dirty regions; then
// entities.sve, then meta — so a save is only valid if it completed. Load
// reads meta, points the store at the directory (regions lazy-load from
// disk), re-fills the window from it (procgen for misses), wakes the world,
// and applies the entity sections.
//
// ---- THE SAVE IS A DELTA FROM THE SEED (PLAN_save_system.md S1) ------------
//
// A saved world is `genChunk(seed, generator)` PLUS the store. The resident
// flush puts in only the chunks something wrote since they entered the window
// (Stream::FlushResident says how that is proven, and when it falls back to
// the old write-everything flush); a pristine chunk costs zero bytes and is
// regenerated on load. That is only sound while the GENERATOR is the same one
// that made the pristine chunks the save leans on -- hence the fingerprint.
//
// NO PRUNE of pristine chunks that an OLDER save (or a full-flush fallback)
// already wrote. Proving a stored chunk equals what genChunk would make needs
// the chunk generated and compared, which is a GPU dispatch per chunk the CPU
// cannot shortcut (there is no CPU genChunk). What is provable without that:
// a chunk that entered the window FROM the store and left unmodified is still
// in the store and is simply not re-written -- it may hold a real edit, so it
// stays. Old full saves therefore keep their dead weight until a regen.
//
// ---- THE WORLDGEN FINGERPRINT (SVM6) ---------------------------------------
//
// WHAT IT HASHES: the generator's INPUTS, not its output. Chosen over probing
// genChunk for a handful of chunks because a probe needs scratch slots in a
// live window (every slot is owned by the residency map; the only gen path is
// FillSlots' list dispatch into real slots) and a GPU round trip at every save
// and load, while the inputs are all CPU-readable files. The cost is the file
// reads, dominated by the tree atlases (~8.6 MB); SaveReport::fingerprint.ms
// and the save/load log lines report it. The parts, each FNV-1a 32:
//   [0] worldgen.wgsl AS LoadShader ASSEMBLES IT (world-constant prelude, the
//       tuning constants worldgen references -- TUNE_VEGETATION and
//       TUNE_GROUND_COVER today -- common.wgsl, the body), with every comment
//       stripped and whitespace runs collapsed, so a comment edit does not
//       move it and a code edit does;
//   [1] the world map dir (biomes::EnvironmentStamp::map);
//   [2] biomes + water presets (EnvironmentStamp::biomes);
//   [3] tree species + baked atlases (EnvironmentStamp::trees);
//   [4] the authored edit layer, if tuning names one (it re-applies to every
//       chunk genChunk makes, so it is part of the generator in effect);
//   [5] the material NAME table (worldgen writes ids; meta already refuses a
//       reordered table, this is belt and braces for the value).
// The u64 is FNV-1a 64 over a recipe version and the parts.
//
// BLIND SPOTS, stated: C++ that packs the environment tables (PackWorldMap,
// the tree atlas remap) can change generation without any input file moving;
// and the fingerprint describes the files ON DISK when it is computed, not
// what was uploaded -- a session that edited a biome and never applied it
// fingerprints the edit. A probe hash would close both; it is the upgrade if
// either bites.
//
// POLICY ON MISMATCH AT LOAD: LOAD, LOUDLY. The file's parts are compared
// with this build's and the differing ones are named; `WorldStamp::
// worldgenMismatch` is set for the caller and the save-load gate reports it
// into build/last_run.json. NOT a refusal: dev iteration changes worldgen
// daily and refusing would kill every dev save. The consequence is visible,
// not silent: pristine chunks regenerate from the CURRENT generator and can
// seam against stored ones.
//
// OPEN QUESTION (deferred, must be decided before a shipped save format): what
// a SHIPPED game does when a patch changes the generator under an existing
// save. Options on the table: (a) freeze generator versions -- every save names
// its generator and the build keeps each old one it ever shipped; (b) new
// generation only in never-visited chunks -- requires recording which chunks a
// save has ever generated (a visited set per region), so a changed generator
// can be applied only beyond it; (c) bake-on-upgrade -- regenerate the visited
// set with the OLD generator once and store it, turning the implicit delta
// into an explicit one. None is implemented; today's answer is the dev policy
// above.
//
// Because the bound store also LRU-spills to the same directory while
// streaming, the directory is a live world store; a store already bound to a
// DIFFERENT directory refuses to save/load (one world dir per session).
// Stamp bytes are stripped on save.
//
// ---- entities.sve: versioned, sectioned entity state ------------------------
//
// A flat TLV container:
//
//   u32 magic 'SVE1', u32 sectionCount, then per section:
//   u32 id (FourCC), u32 version, u32 byteLen, payload[byteLen]
//
// Rules, all load-bearing:
//   - Each section carries its OWN version, independent of the others. A
//     system evolves its payload by bumping only its own number.
//   - An UNKNOWN section id is skipped (logged, never fatal): a save written
//     by a newer build still loads, minus what this build cannot know about.
//   - Adding a persistable system means ADDING A SECTION, never changing this
//     container. That is the extension path — worldio.cpp knows nothing about
//     any system's contents; systems register an EntitySection and own their
//     bytes end to end.
//
// Entity state is CPU-float gameplay state OUTSIDE the hashed sim domain
// (debris.h determinism note), so none of this touches rule 1; the voxel grid
// itself round-trips through the region files exactly as before.
//
// Jolt rigidbodies are recreated on load at their saved transform with ZERO
// velocity, DEACTIVATED. Velocities and sleep flags are deliberately not
// serialized: a settled pile reloads identically (and stays asleep — rule 2),
// while something saved mid-flight lands where it was rather than continuing
// its arc. That trade is accepted.

// One persistable entity system: a section in entities.sve.
struct EntitySection {
  uint32_t id;       // FourCC, e.g. 'DBRS'
  uint32_t version;  // the version this build WRITES
  // Called on EVERY load once meta validates, before any payload applies —
  // including when the file lacks this section (an older save must not leave
  // this session's entities standing in the loaded world). Load callbacks may
  // therefore assume a clean system and must NOT reset again themselves.
  std::function<void()> reset;
  // Append this system's payload bytes.
  std::function<void(std::vector<uint8_t>& out)> save;
  // Apply a payload read from the file. `fileVersion` is the version the FILE
  // carries, which may be older than `version`. Returning false logs a loud
  // warning but does not fail the load (the grid is already restored).
  std::function<bool(const uint8_t* data, size_t len, uint32_t fileVersion)>
      load;
};

// The entity systems a save carries, bundled so SaveWorld/LoadWorld don't
// grow one parameter per system. Nullable at the call: a grid-only caller
// passes nullptr and gets exactly the old behaviour.
struct EntityIO {
  std::vector<EntitySection> sections;
};

// The two facts meta.svm gained in SVM5 (M9.5-B). A VALUE TYPE with a
// `known` flag rather than a pair of magic zeros, because tick 0 and seed 0
// are both legal and "the file did not say" has to be distinguishable from
// "the file said zero" — that distinction is the whole reason an SVM4 file is
// allowed to load at all.
struct WorldStamp {
  uint32_t tick = 0;
  uint32_t seed = 0;
  bool known = false;
  // ---- SVM6 (PLAN_save_system.md S1) ----
  // LOAD: what the file carried, and whether it disagreed with this build.
  // SAVE: ignored on input -- SaveWorld computes the fingerprint itself, with
  // the same function LoadWorld compares against, so the two cannot drift.
  uint64_t worldgenFingerprint = 0;
  bool fingerprintKnown = false;   // false for an SVM5/SVM4 file
  bool worldgenMismatch = false;   // known AND different from this build's
};

// The generator's identity (see the header comment). `parts` exists so a
// mismatch names WHICH input moved instead of printing one opaque number.
struct WorldgenFingerprint {
  static constexpr uint32_t kParts = 6;
  static constexpr uint32_t kRecipe = 1;  // bump if the recipe itself changes
  uint64_t value = 0;
  uint32_t parts[kParts] = {};
  double ms = 0;  // what computing it cost
};
WorldgenFingerprint ComputeWorldgenFingerprint(const std::string& assetDir,
                                               const std::vector<MaterialDef>& mats);
// Short name of part i ("wgsl", "map", "biomes", "trees", "editLayer",
// "materials"), for log lines.
const char* WorldgenFingerprintPartName(uint32_t i);

// Optional in/out record of one SaveWorld. `forceFullFlush` is the only input
// (the pre-S1 write-every-resident-chunk behaviour, kept for measurement);
// everything else is filled in.
struct SaveReport {
  bool forceFullFlush = false;
  Stream::FlushReport flush;
  size_t regions = 0;
  uint64_t bytes = 0;  // ChunkStore::Flush's bytesOut: region files written
  WorldgenFingerprint fingerprint;
};

// `stamp` is what to WRITE (pass {} and the file records tick 0 / seed 0 with
// `known` true — which is honest for a caller that genuinely has no clock).
// Defaulted so every existing call site is untouched and byte-compatible
// apart from the two appended words.
bool SaveWorld(GpuContext& ctx, World& world, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities = nullptr, WorldStamp stamp = {},
               SaveReport* report = nullptr);
// `stampOut`, when non-null, receives what the file carried. `known` is false
// for an SVM4 file; the two numbers are then 0 and MUST NOT be used to set a
// clock (see the SVM4 paragraph at the top).
bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities = nullptr,
               WorldStamp* stampOut = nullptr);
