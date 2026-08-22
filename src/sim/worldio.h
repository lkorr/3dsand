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
//   meta.svm       magic 'SVM4', world constants (kWorldN, kChunk, the exact
//                  bit pattern of kVoxelMeters), the window origin, and the
//                  full material NAME table. Written LAST: its presence marks
//                  a completed save. A load refuses any mismatch and says
//                  exactly which field disagreed — material IDs are baked into
//                  every chunk (world.h "never reorder"), and a world saved at
//                  one voxel size loaded into a build with another would
//                  silently change physical scale.
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

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities = nullptr);
bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities = nullptr);
