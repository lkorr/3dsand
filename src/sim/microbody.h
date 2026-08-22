#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/voxload.h"
#include "sim/world.h"

// Dynamic microvoxel BODIES (docs/PLAN_voxel_editor.md §C, DESIGN.md §9).
//
// A mob def may declare `"skinScale": 2|4|8: its limb .vox models are authored
// at that many voxels per WORLD voxel, so a limb whose .vox box is 6x2x2 at
// skinScale 2 occupies 3x1x1 world voxels. Physics builds the same limb at a
// pitch of 1/physScale — a SEPARATE, coarser resolution derived from the art
// (mob.h) — so the creature is the same physical size as a scale-1 one with a
// coarser skin: the extra resolution buys detail, not bulk, and does not drag
// the collider up with it.
//
// RENDERING is where they diverge. The cube path (debris.wgsl `vsBody`) emits
// one 36-vertex cube per voxel; at scale 4 that is 64x the instances for the
// same silhouette, which is exactly the wrong trade. Instead each micro body
// rasterizes ONE box — its oriented bounding box, 36 vertices total — and the
// fragment shader marches the limb's brick in object space. Cost then scales
// with SCREEN AREA rather than with voxel count, which is what makes 4x
// resolution affordable (rule 2: cost tracks activity, not content size).
//
// DETERMINISM (rule 1): everything here is render-only, exactly like the static
// micro pool. The pool and the tables are bound to the microbody render
// pipeline and to nothing else; the sim never sees them. Damage edits micro
// payloads (see the COW section below), but only ever as presentation state —
// a body's authoritative voxels live in DebrisSystem, and nothing here feeds
// back into the hashed grid.

// ---- GPU layout ------------------------------------------------------------

// One limb model, 16 bytes — must match struct MicroBodyModel in common.wgsl.
struct MicroBodyModelGpu {
  // Word index into the pool where this model's payload starts. The payload is
  // dims.x*dims.y*dims.z micro voxels, 2 packed 16-bit voxels per word,
  // x-major then y then z (idx = (z*dy + y)*dx + x), padded to a whole word.
  //
  // Each 16-bit voxel is:
  //   bits 0..7   material ID — what the voxel IS. Palette index == material ID
  //               (the project-wide .vox convention), so it shades through the
  //               ordinary material table. 8 bits caps a micro body at material
  //               ids 1..255, checked at load.
  //   bits 8..15  ART COLOUR slot — what the voxel LOOKS like, 0 meaning "use
  //               the material's own colour". A creature is one material all
  //               over and painted per voxel, so colour cannot share the
  //               material's channel. Resolved against the art palette
  //               (kArtPaletteBase in voxload.h); render-only, never hashed.
  uint32_t base;
  // bits 0..9 dims.x, bits 10..19 dims.y, bits 20..29 dims.z (micro voxels).
  // 10 bits each is 1023 per axis, far past the +-127 DebrisVoxel bound that
  // limits a limb anyway; packing them keeps the record at 16 bytes.
  uint32_t dims;
  uint32_t scale;  // micro voxels per world voxel: 2 or 4
  uint32_t _pad;   // padding to 16 bytes; no flag bits are defined
};
static_assert(sizeof(MicroBodyModelGpu) == 16,
              "must match common.wgsl MicroBodyModel");

// One micro body to draw this frame, 16 bytes — must match MicroBodyInst in
// microbody.wgsl. COMPACTED: the draw's instance count is exactly the number
// of micro bodies on screen, so a world with none dispatches no vertices at
// all (rule 2). Slot indexes the shared bodyXforms buffer, which both the cube
// pass and this one read — the two passes partition the slots, never duplicate.
struct MicroBodyInstGpu {
  uint32_t slot;
  uint32_t model;
  uint32_t pad0 = 0, pad1 = 0;
};
static_assert(sizeof(MicroBodyInstGpu) == 16,
              "must match microbody.wgsl MicroBodyInst");

// ---- CPU side --------------------------------------------------------------

// The pool plus the per-model records.
//
// Two populations share one pool. **Shared** models are packed at mob-def load
// (or on first sphere spawn), indexed by every instance of that def, and never
// freed. **Owned** models are copy-on-write clones created the first time a
// particular BODY is damaged — from then on that body has its own payload and
// edits are local to it (MicroBodyOwn / MicroBodyEdit below).
//
// Freed owned blocks return to `freeList` keyed by word count and are reused
// verbatim, which is what keeps "shoot the same sphere for ten minutes" from
// walking the pool's high-water mark to the ceiling. Blocks are never
// coalesced or compacted: a compaction would have to rewrite every live
// model's `base` while the GPU may still be reading last frame's upload, and
// exact-size reuse already handles the dominant case (a body re-editing its
// own block in place, which does not reallocate at all).
struct MicroBodySet {
  std::vector<MicroBodyModelGpu> models;
  std::vector<uint32_t> pool;
  // model index -> true when this record is an owned COW clone (freeable).
  // Shared models must never be freed: other instances still point at them.
  std::vector<uint8_t> owned;
  // model index -> words actually reserved at `base`. This is NOT derivable
  // from dims: an edited body reuses its block in place while shrinking, so
  // the block stays the size it was allocated at and must be freed at that
  // size or the surplus leaks out of the pool permanently.
  std::vector<uint32_t> blockWords;
  // word count -> list of free block bases of exactly that size.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> freeList;
  // Retired owned model records, reusable so a long fight does not exhaust
  // kMaxMicroBodyModels even though the pool words are recycled.
  std::vector<uint32_t> freeModels;
  // Set by any mutation; the caller re-uploads and clears. Batching one upload
  // per tick rather than one per edit is rule 2 applied to PCIe traffic.
  bool dirty = false;

  // ---- art palette ----
  // Skin colours from every loaded prefab, merged, indexed from
  // kArtPaletteBase (sim/voxload.h). It rides here rather than on any one
  // Prefab because a frame draws limbs from several defs at once and they all
  // resolve against ONE reserved run of the material table — so the merge has
  // to happen where the models are pooled, which is here. Render-only, exactly
  // like the pool itself.
  //
  // Packed 0x00RRGGBB. Entry i is art slot kArtPaletteBase + i.
  std::vector<uint32_t> artColors;
};

// Merge one prefab's art palette into `set`, remapping its slots if needed.
// Returns a 256-entry table mapping the prefab's .vox palette slot -> the
// merged slot, so the caller can rewrite its voxels' `color` before packing.
// Colours beyond kArtPaletteSlots are dropped to 0 (unpainted) and reported.
std::vector<uint8_t> MicroBodyMergeArt(MicroBodySet& set,
                                       const std::vector<uint32_t>& artColors,
                                       const std::string& label,
                                       std::string& log);

// Packs one limb's voxel model into `set` and returns its model index, or -1 on
// failure (pool full, out-of-range material, empty model). `voxels` are in
// MICRO units with a min corner already at 0 (PrefabModel-local coords).
//
// `log` collects diagnostics. A limb that fails to pack does NOT fall back to
// the cube path — cube instances are one WORLD voxel each, so a scale-2 limb
// drawn that way would be twice its real size — it simply does not render,
// which the loader says out loud. A broken limb must not stop the mob from
// loading (DESIGN.md §6).
int MicroBodyPack(MicroBodySet& set, const std::vector<PrefabVoxel>& voxels,
                  IVec3 dims, uint32_t scale, const std::string& label,
                  std::string& log);

// ---- copy-on-write: destructible micro bodies -------------------------------
//
// A shared model cannot be edited — every instance of the def points at it, so
// carving a crater in one sphere would crater all of them. Damage therefore
// goes through MicroBodyOwn first: it clones the model into a fresh block and
// returns a NEW model index that exactly one body holds. Calling it on a model
// that is already owned is a no-op returning the same index, so the damage path
// can call it unconditionally and only the first hit pays the copy.
//
// Returns -1 if the pool or the model table is full, in which case the caller
// must fall back to leaving the body's rendering alone (the body's authoritative
// voxels still change; only the skin stops keeping up). Losing detail under
// memory pressure beats refusing to be destructible.
int MicroBodyOwn(MicroBodySet& set, uint32_t model);

// Rewrites an OWNED model's payload from `voxels` (micro units, coordinates
// relative to the body origin, i.e. the same frame DebrisSystem stores). The
// model's dims/base are re-derived: a body that lost its top half gets a
// smaller brick, so the OBB the raster pass draws shrinks with it and the march
// does not wade through empty space.
//
// `originShift` receives the min corner the new brick was rebased to, in MICRO
// units. The caller must shift the body transform by that (rotated into world
// space) or the art will drift off the collider — the brick march runs [0..dims)
// from the body origin, so moving the min corner moves the art.
//
// Returns false and leaves the model untouched when `voxels` is empty or the
// re-pack does not fit; `model` must be owned (MicroBodyOwn first).
bool MicroBodyEdit(MicroBodySet& set, uint32_t model,
                   const std::vector<PrefabVoxel>& voxels, IVec3& originShift);

// Returns an owned model's words to the free list and retires its record.
// Safe (no-op) on shared models and on kMicroBodyNoModel, so body teardown can
// call it blindly.
void MicroBodyFree(MicroBodySet& set, uint32_t model);

// A body's micro rendering, passed along when ownership of the body moves.
//
// This travels as an explicit argument (MobSystem hands it to
// DebrisSystem::AdoptBody) rather than living in a side table keyed by physics
// handle. A side table would have to be kept in sync at every spawn, sever,
// death, cull and reset — five agreements enforced by nothing — and a recycled
// Jolt BodyID could then paint unrelated debris as somebody's leg. As a
// parameter the invariant "this description belongs to that body" is not
// merely maintained, it is unrepresentable-if-violated.
//
// The routing key is still the BODY, not "is a mob limb": that is what makes a
// severed micro limb keep its detail with no special case at the adoption site.
//
// RENDER ONLY. This struct describes the SKIN and nothing else. It used to
// carry a single `scale` that also defined the units of the body's collider
// voxels, which is what capped micro bodies at 4: the collider lives in
// DebrisVoxel (int8, +-120), so a finer skin bought a proportionally smaller
// creature. The two now move independently — `skinScale` here, `physScale` on
// the body — because their costs are unrelated. The brick march is a fragment
// shader over one OBB, so skin cost tracks SCREEN AREA; the collider is Jolt
// boxes, so physics cost tracks VOXEL COUNT. Coupling them held the cheap axis
// hostage to the expensive one.
struct MicroBodyRef {
  // kMicroBodyNoModel = ordinary cube path (plain debris, scale-1 limbs).
  uint32_t model = kMicroBodyNoModel;
  // Micro voxels per world voxel in the BRICK. The body's `skinVoxels` are in
  // these units; its collider `voxels` are in `Body::physScale` units, which
  // may be coarser. Equal values are the ordinary case and mean the two
  // lattices coincide exactly as they did before the split.
  uint32_t skinScale = 1;
  bool Valid() const { return model != kMicroBodyNoModel; }
};
